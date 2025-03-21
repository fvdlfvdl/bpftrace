#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_set>

#include <bcc/bcc_elf.h>
#include <bcc/bcc_syms.h>
#include <bcc/bcc_usdt.h>
#include <elf.h>
#include <fcntl.h>
#include <gelf.h>
#include <glob.h>
#include <libelf.h>
#include <link.h>
#include <linux/version.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "debugfs.h"
#include "log.h"
#include "scopeguard.h"
#include "tracefs.h"
#include "utils.h"

namespace {

std::vector<int> read_cpu_range(std::string path)
{
  std::ifstream cpus_range_stream{ path };
  std::vector<int> cpus;
  std::string cpu_range;

  while (std::getline(cpus_range_stream, cpu_range, ',')) {
    std::size_t rangeop = cpu_range.find('-');
    if (rangeop == std::string::npos) {
      cpus.push_back(std::stoi(cpu_range));
    } else {
      int start = std::stoi(cpu_range.substr(0, rangeop));
      int end = std::stoi(cpu_range.substr(rangeop + 1));
      for (int i = start; i <= end; i++)
        cpus.push_back(i);
    }
  }
  return cpus;
}

std::vector<std::string> expand_wildcard_path(const std::string &path)
{
  glob_t glob_result;
  memset(&glob_result, 0, sizeof(glob_result));

  if (glob(path.c_str(), GLOB_NOCHECK, nullptr, &glob_result)) {
    globfree(&glob_result);
    throw bpftrace::FatalUserException("glob() failed");
  }

  std::vector<std::string> matching_paths;
  for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
    matching_paths.emplace_back(glob_result.gl_pathv[i]);
  }

  globfree(&glob_result);
  return matching_paths;
}

std::vector<std::string> expand_wildcard_paths(
    const std::vector<std::string> &paths)
{
  std::vector<std::string> expanded_paths;
  for (const auto &p : paths) {
    auto ep = expand_wildcard_path(p);
    expanded_paths.insert(expanded_paths.end(), ep.begin(), ep.end());
  }
  return expanded_paths;
}

} // namespace

namespace bpftrace {

//'borrowed' from libbpf's bpf_core_find_kernel_btf
// from Andrii Nakryiko
const struct vmlinux_location vmlinux_locs[] = {
  { .path = "/sys/kernel/btf/vmlinux", .raw = true },
  { .path = "/boot/vmlinux-%1$s", .raw = false },
  { .path = "/lib/modules/%1$s/vmlinux-%1$s", .raw = false },
  { .path = "/lib/modules/%1$s/build/vmlinux", .raw = false },
  { .path = "/usr/lib/modules/%1$s/kernel/vmlinux", .raw = false },
  { .path = "/usr/lib/debug/boot/vmlinux-%1$s", .raw = false },
  { .path = "/usr/lib/debug/boot/vmlinux-%1$s.debug", .raw = false },
  { .path = "/usr/lib/debug/lib/modules/%1$s/vmlinux", .raw = false },
  { .path = nullptr, .raw = false },
};

std::optional<std::string> find_vmlinux(struct vmlinux_location const *locs,
                                        struct symbol *sym)
{
  struct utsname uts;
  if (uname(&uts) != 0)
    return std::nullopt;

  for (size_t i = 0; locs[i].path; ++i) {
    auto &loc = locs[i];
    if (loc.raw)
      continue; // This file is for BTF. skip

    // Format the loc.path with the current release.
    char path[PATH_MAX] = {};
    int bytes_written = std::snprintf(
        path, sizeof(path), loc.path, uts.release);
    if (bytes_written < 0) {
      LOG(ERROR) << "Failed to format vmlinux path '" << loc.path << "' using "
                 << uts.release;
      continue;
    } else if (static_cast<size_t>(bytes_written) > sizeof(path)) {
      LOG(WARNING) << "Truncated format for vmlinux path '" << loc.path
                   << "' using " << uts.release;
      continue;
    }

    if (access(path, R_OK))
      continue;

    if (sym == nullptr) {
      return path;
    } else {
      bcc_elf_symcb callback = !sym->name.empty() ? sym_name_cb
                                                  : sym_address_cb;
      struct bcc_symbol_option options = {
        .use_debug_file = 0,
        .check_debug_file_crc = 0,
        .lazy_symbolize = 0,
        .use_symbol_type = BCC_SYM_ALL_TYPES ^ (1 << STT_NOTYPE),
      };
      if (bcc_elf_foreach_sym(path, callback, &options, sym) == -1) {
        LOG(ERROR) << "Failed to iterate over symbols in " << path;
        continue;
      }

      if (sym->start) {
        LOG(V1) << "vmlinux: using " << path;
        return path;
      }
    }
  }

  return std::nullopt;
}

// find vmlinux file.
// if sym is not null, check vmlinux contains the given symbol information
std::optional<std::string> find_vmlinux(struct symbol *sym)
{
  struct vmlinux_location locs_env[] = {
    { .path = std::getenv("BPFTRACE_VMLINUX"), .raw = false },
    { .path = nullptr, .raw = false },
  };
  return find_vmlinux(locs_env[0].path ? locs_env : vmlinux_locs, sym);
}

static bool pid_in_different_mountns(int pid);
static std::vector<std::string> resolve_binary_path(const std::string &cmd,
                                                    const char *env_paths,
                                                    std::optional<int> pid);

void StdioSilencer::silence()
{
  auto syserr = [](std::string msg) {
    return std::system_error(errno, std::generic_category(), msg);
  };

  try {
    int fd = fileno(ofile);
    if (fd < 0)
      throw syserr("fileno()");

    fflush(ofile);

    if ((old_stdio_ = dup(fd)) < 0)
      throw syserr("dup(fd)");

    int new_stdio = -1;
    if ((new_stdio = open("/dev/null", O_WRONLY)) < 0)
      throw syserr("open(\"/dev/null\")");

    if (dup2(new_stdio, fd) < 0)
      throw syserr("dup2(new_stdio_, fd)");

    close(new_stdio);
  } catch (const std::system_error &e) {
    if (errno == EMFILE)
      throw bpftrace::FatalUserException(std::string(e.what()) +
                                         ": please raise NOFILE");
    else
      LOG(BUG) << e.what();
  }
}

StdioSilencer::~StdioSilencer()
{
  if (old_stdio_ == -1)
    return;

  auto syserr = [](std::string msg) {
    return std::system_error(errno, std::generic_category(), msg);
  };

  try {
    int fd = fileno(ofile);
    if (fd < 0)
      throw syserr("fileno()");

    fflush(ofile);
    if (dup2(old_stdio_, fd) < 0)
      throw syserr("dup2(old_stdio_)");
    close(old_stdio_);
    old_stdio_ = -1;
  } catch (const std::system_error &e) {
    LOG(BUG) << e.what();
  }
}

KConfig::KConfig()
{
  std::vector<std::string> config_locs;

  // Try to get the config from BPFTRACE_KCONFIG_TEST env
  // If not set, use the set of default locations
  const char *path_env = std::getenv("BPFTRACE_KCONFIG_TEST");
  if (path_env)
    config_locs = { std::string(path_env) };
  else {
    struct utsname utsname;
    if (uname(&utsname) < 0)
      return;
    config_locs = {
      "/proc/config.gz",
      "/boot/config-" + std::string(utsname.release),
    };
  }

  for (auto &path : config_locs) {
    // gzopen/gzgets handle both uncompressed and compressed files
    gzFile file = gzopen(path.c_str(), "r");
    if (!file)
      continue;

    char buf[4096];
    while (gzgets(file, buf, sizeof(buf))) {
      std::string option(buf);
      if (option.starts_with("CONFIG_")) {
        // trim trailing '\n'
        if (option[option.length() - 1] == '\n')
          option = option.substr(0, option.length() - 1);

        auto split = option.find("=");
        if (split == std::string::npos)
          continue;

        config.emplace(option.substr(0, split), option.substr(split + 1));
      }
    }
    gzclose(file);
    break;
  }
}

void get_uint64_env_var(const ::std::string &str,
                        const std::function<void(uint64_t)> &cb)
{
  uint64_t dest;
  if (const char *env_p = std::getenv(str.c_str())) {
    std::istringstream stringstream(env_p);
    if (!(stringstream >> dest)) {
      throw bpftrace::FatalUserException(
          "Env var '" + str +
          "' did not contain a valid uint64_t, or was zero-valued.");
      return;
    }
    cb(dest);
  }
}

void get_bool_env_var(const ::std::string &str,
                      const std::function<void(bool)> &cb)
{
  if (const char *env_p = std::getenv(str.c_str())) {
    bool dest;
    std::string s(env_p);
    if (s == "1")
      dest = true;
    else if (s == "0")
      dest = false;
    else {
      throw bpftrace::FatalUserException(
          "Env var '" + str + "' did not contain a valid value (0 or 1).");
    }
    cb(dest);
  }
  return;
}

std::optional<std::filesystem::path> find_in_path(std::string_view name)
{
  std::error_code ec;

  const char *path_env = std::getenv("PATH");
  if (!path_env)
    return std::nullopt;

  auto paths = split_string(path_env, ':', true);
  for (const auto &path : paths) {
    auto fpath = std::filesystem::path(path) / name;
    if (std::filesystem::exists(fpath, ec))
      return fpath;
  }

  return std::nullopt;
}

std::optional<std::filesystem::path> find_near_self(std::string_view filename)
{
  std::error_code ec;
  auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) {
    LOG(WARNING) << "Failed to resolve /proc/self/exe: " << ec;
    return std::nullopt;
  }

  exe.replace_filename(filename);
  bool exists = std::filesystem::exists(exe, ec);
  if (!exists) {
    if (ec)
      LOG(WARNING) << "Failed to resolve stat " << exe << ": " << ec;
    return std::nullopt;
  }

  return exe;
}

std::string get_pid_exe(const std::string &pid)
{
  std::error_code ec;
  std::filesystem::path proc_path{ "/proc" };
  proc_path /= pid;
  proc_path /= "exe";

  try {
    return std::filesystem::read_symlink(proc_path).string();
  } catch (const std::filesystem::filesystem_error &e) {
    auto err = e.code().value();
    if (err == ENOENT || err == EINVAL)
      return {};
    else
      throw e;
  }
}

std::string get_pid_exe(pid_t pid)
{
  return get_pid_exe(std::to_string(pid));
}

std::string get_proc_maps(const std::string &pid)
{
  std::error_code ec;
  std::filesystem::path proc_path{ "/proc" };
  proc_path /= pid;
  proc_path /= "maps";

  if (!std::filesystem::exists(proc_path, ec))
    return "";

  return proc_path.string();
}

std::string get_proc_maps(pid_t pid)
{
  return get_proc_maps(std::to_string(pid));
}

std::vector<std::string> get_mapped_paths_for_pid(pid_t pid)
{
  static std::map<pid_t, std::vector<std::string>> paths_cache;

  auto it = paths_cache.find(pid);
  if (it != paths_cache.end()) {
    return it->second;
  }

  std::vector<std::string> paths;

  // start with the exe
  std::string pid_exe = get_pid_exe(pid);
  if (!pid_exe.empty() && pid_exe.find("(deleted)") == std::string::npos)
    paths.push_back(get_pid_exe(pid));

  // get all the mapped libraries
  std::string maps_path = get_proc_maps(pid);
  if (maps_path.empty()) {
    LOG(WARNING) << "Maps path is empty";
    return paths;
  }

  std::fstream fs(maps_path, std::ios_base::in);
  if (!fs.is_open()) {
    LOG(WARNING) << "Unable to open procfs mapfile: " << maps_path;
    return paths;
  }

  std::unordered_set<std::string> seen_mappings;

  std::string line;
  // Example mapping:
  // 7fc8ee4fa000-7fc8ee4fb000 r--p 00000000 00:1f 27168296 /usr/libc.so.6
  while (std::getline(fs, line)) {
    char buf[PATH_MAX + 1];
    buf[0] = '\0';
    auto res = std::sscanf(line.c_str(), "%*s %*s %*x %*s %*u %[^\n]", buf);
    // skip [heap], [vdso], and non file paths etc...
    if (res == 1 && buf[0] == '/') {
      std::string name = buf;
      if (name.find("(deleted)") == std::string::npos &&
          seen_mappings.count(name) == 0) {
        seen_mappings.emplace(name);
        paths.push_back(std::move(name));
      }
    }
  }

  paths_cache.emplace(pid, paths);
  return paths;
}

std::vector<std::string> get_mapped_paths_for_running_pids()
{
  std::unordered_set<std::string> unique_paths;
  for (auto pid : get_all_running_pids()) {
    for (auto &path : get_mapped_paths_for_pid(pid)) {
      unique_paths.insert(std::move(path));
    }
  }
  std::vector<std::string> paths;
  for (auto &path : unique_paths) {
    paths.emplace_back(std::move(path));
  }
  return paths;
}

bool has_wildcard(const std::string &str)
{
  return str.find("*") != std::string::npos ||
         (str.find("[") != std::string::npos &&
          str.find("]") != std::string::npos);
}

std::vector<std::string> split_string(const std::string &str,
                                      char delimiter,
                                      bool remove_empty)
{
  std::vector<std::string> elems;
  std::stringstream ss(str);
  std::string value;
  while (std::getline(ss, value, delimiter)) {
    if (remove_empty && value.empty())
      continue;

    elems.push_back(value);
  }
  return elems;
}

/// Erase prefix up to the first colon (:) from str and return the prefix
std::string erase_prefix(std::string &str)
{
  std::string prefix = str.substr(0, str.find(':'));
  str.erase(0, prefix.length() + 1);
  return prefix;
}

void erase_parameter_list(std::string &demangled_name)
{
  size_t args_start = std::string::npos;
  ssize_t stack = 0;
  // Look for the parenthesis closing the parameter list, then find
  // the matching parenthesis at the start of the parameter list...
  for (ssize_t it = demangled_name.find_last_of(')'); it >= 0; --it) {
    if (demangled_name[it] == ')')
      stack++;
    if (demangled_name[it] == '(')
      stack--;
    if (stack == 0) {
      args_start = it;
      break;
    }
  }

  // If we found the start of the parameter list,
  // remove the parameters from the match line.
  if (args_start != std::string::npos)
    demangled_name.resize(args_start);
}

bool wildcard_match(std::string_view str,
                    const std::vector<std::string> &tokens,
                    bool start_wildcard,
                    bool end_wildcard)
{
  size_t next = 0;

  if (!start_wildcard)
    if (str.find(tokens[0], next) != next)
      return false;

  for (const std::string &token : tokens) {
    size_t found = str.find(token, next);
    if (found == std::string::npos)
      return false;

    next = found + token.length();
  }

  if (!end_wildcard)
    if (str.length() != next)
      return false;

  return true;
}

// Splits input string by '*' delimiter and return the individual parts.
// Sets start_wildcard and end_wildcard if input starts or ends with '*'.
std::vector<std::string> get_wildcard_tokens(const std::string &input,
                                             bool &start_wildcard,
                                             bool &end_wildcard)
{
  if (input.empty())
    return {};

  start_wildcard = input[0] == '*';
  end_wildcard = input[input.length() - 1] == '*';

  std::vector<std::string> tokens = split_string(input, '*');
  auto it = std::ranges::remove(tokens, "");
  tokens.erase(it.begin(), it.end());
  return tokens;
}

std::vector<int> get_online_cpus()
{
  return read_cpu_range("/sys/devices/system/cpu/online");
}

std::vector<int> get_possible_cpus()
{
  return read_cpu_range("/sys/devices/system/cpu/possible");
}

int get_max_cpu_id()
{
  // When booting, the kernel ensures CPUs are ordered from 0 -> N so there
  // are no gaps in possible CPUs. CPU ID is also u32 so this cast is safe
  const auto num_possible_cpus = static_cast<uint32_t>(
      get_possible_cpus().size());
  assert(num_possible_cpus > 0);
  // Using global scratch variables for big string usage looks like:
  //   bounded_cpu_id = bpf_get_smp_processor_id() & MAX_CPU_ID
  //   buf = global_var[bounded_cpu_id][slot_id]
  // We bound CPU ID to satisfy the BPF verifier on older kernels. We use an AND
  // instruction vs. LLVM umin function to reduce the number of jumps in BPF to
  // ensure we don't hit the complexity limit of 8192 jumps
  //
  // To bound using AND, we need to ensure NUM_POSSIBLE_CPUS is rounded up to
  // next nearest power of 2.
  return round_up_to_next_power_of_two(num_possible_cpus) - 1;
}

std::vector<std::string> get_kernel_cflags(const char *uname_machine,
                                           const std::string &ksrc,
                                           const std::string &kobj,
                                           const KConfig &kconfig)
{
  std::vector<std::string> cflags;
  std::string arch = uname_machine;
  const char *archenv;

  if (!strncmp(uname_machine, "x86_64", 6)) {
    arch = "x86";
  } else if (uname_machine[0] == 'i' && !strncmp(&uname_machine[2], "86", 2)) {
    arch = "x86";
  } else if (!strncmp(uname_machine, "arm", 3)) {
    arch = "arm";
  } else if (!strncmp(uname_machine, "sa110", 5)) {
    arch = "arm";
  } else if (!strncmp(uname_machine, "s390x", 5)) {
    arch = "s390";
  } else if (!strncmp(uname_machine, "parisc64", 8)) {
    arch = "parisc";
  } else if (!strncmp(uname_machine, "ppc", 3)) {
    arch = "powerpc";
  } else if (!strncmp(uname_machine, "mips", 4)) {
    arch = "mips";
  } else if (!strncmp(uname_machine, "sh", 2)) {
    arch = "sh";
  } else if (!strncmp(uname_machine, "aarch64", 7)) {
    arch = "arm64";
  } else if (!strncmp(uname_machine, "loongarch", 9)) {
    arch = "loongarch";
  }

  // If ARCH env is defined, use it over uname
  archenv = getenv("ARCH");
  if (archenv)
    arch = std::string(archenv);

  cflags.emplace_back("-nostdinc");
  cflags.emplace_back("-isystem");
  cflags.emplace_back("/virtual/lib/clang/include");

  // see linux/Makefile for $(LINUXINCLUDE) + $(USERINCLUDE)
  cflags.push_back("-I" + ksrc + "/arch/" + arch + "/include");
  cflags.push_back("-I" + kobj + "/arch/" + arch + "/include/generated");
  cflags.push_back("-I" + ksrc + "/include");
  cflags.push_back("-I" + kobj + "/include");
  cflags.push_back("-I" + ksrc + "/arch/" + arch + "/include/uapi");
  cflags.push_back("-I" + kobj + "/arch/" + arch + "/include/generated/uapi");
  cflags.push_back("-I" + ksrc + "/include/uapi");
  cflags.push_back("-I" + kobj + "/include/generated/uapi");

  cflags.emplace_back("-include");
  cflags.push_back(ksrc + "/include/linux/kconfig.h");
  cflags.emplace_back("-D__KERNEL__");
  cflags.emplace_back("-D__BPF_TRACING__");
  cflags.emplace_back("-D__HAVE_BUILTIN_BSWAP16__");
  cflags.emplace_back("-D__HAVE_BUILTIN_BSWAP32__");
  cflags.emplace_back("-D__HAVE_BUILTIN_BSWAP64__");
  cflags.emplace_back("-DKBUILD_MODNAME=\"bpftrace\"");

  // If ARCH env variable is set, pass this along.
  if (archenv)
    cflags.push_back("-D__TARGET_ARCH_" + arch);

  if (arch == "arm") {
    // Required by several header files in arch/arm/include
    cflags.emplace_back("-D__LINUX_ARM_ARCH__=7");
  }

  if (arch == "arm64") {
    // arm64 defines KASAN_SHADOW_SCALE_SHIFT in a Makefile instead of defining
    // it in a header file. Since we're not executing make, we need to set the
    // value manually (values are taken from arch/arm64/Makefile).
    if (kconfig.has_value("CONFIG_KASAN", "y")) {
      if (kconfig.has_value("CONFIG_KASAN_SW_TAGS", "y"))
        cflags.emplace_back("-DKASAN_SHADOW_SCALE_SHIFT=4");
      else
        cflags.emplace_back("-DKASAN_SHADOW_SCALE_SHIFT=3");
    }
  }

  return cflags;
}

std::string get_cgroup_path_in_hierarchy(uint64_t cgroupid,
                                         std::string base_path)
{
  static std::map<std::pair<uint64_t, std::string>, std::string> path_cache;
  struct stat path_st;

  auto cached_path = path_cache.find({ cgroupid, base_path });
  if (cached_path != path_cache.end() &&
      stat(cached_path->second.c_str(), &path_st) >= 0 &&
      path_st.st_ino == cgroupid)
    return cached_path->second;

  // Check for root cgroup path separately, since recursive_directory_iterator
  // does not iterate over base directory
  if (stat(base_path.c_str(), &path_st) >= 0 && path_st.st_ino == cgroupid) {
    path_cache[{ cgroupid, base_path }] = "/";
    return "/";
  }

  for (auto &path_iter :
       std::filesystem::recursive_directory_iterator(base_path)) {
    if (stat(path_iter.path().c_str(), &path_st) < 0)
      return "";
    if (path_st.st_ino == cgroupid) {
      // Base directory is not a part of cgroup path
      path_cache[{ cgroupid, base_path }] = path_iter.path().string().substr(
          base_path.length());
      return path_cache[{ cgroupid, base_path }];
    }
  }

  return "";
}

std::array<std::vector<std::string>, 2> get_cgroup_hierarchy_roots()
{
  // Get all cgroup mounts and their type (cgroup/cgroup2) from /proc/mounts
  std::ifstream mounts_file("/proc/mounts");
  std::array<std::vector<std::string>, 2> result;

  const std::regex cgroup_mount_regex("(cgroup[2]?) (\\S*)[ ]?.*");
  for (std::string line; std::getline(mounts_file, line);) {
    std::smatch match;
    if (std::regex_match(line, match, cgroup_mount_regex)) {
      if (std::filesystem::is_directory(match[2].str())) {
        if (match[1].str() == "cgroup") {
          result[0].push_back(match[2].str());
        } else if (match[1].str() == "cgroup2") {
          result[1].push_back(match[2].str());
        }
      }
    }
  }

  mounts_file.close();
  return result;
}

std::vector<std::pair<std::string, std::string>> get_cgroup_paths(
    uint64_t cgroupid,
    std::string filter)
{
  auto roots = get_cgroup_hierarchy_roots();

  // Replace cgroup version with cgroup mount point directory name for cgroupv1
  // roots and "unified" for cgroupv2 roots
  auto types_v1 = roots[0] |
                  std::views::transform(
                      [&](auto &root) -> std::pair<std::string, std::string> {
                        return {
                          std::filesystem::path(root).filename().string(), root
                        };
                      });
  auto types_v2 = roots[1] |
                  std::views::transform(
                      [&](auto &root) -> std::pair<std::string, std::string> {
                        return { "unified", root };
                      });

  // Filter roots
  bool start_wildcard, end_wildcard;
  auto tokens = get_wildcard_tokens(filter, start_wildcard, end_wildcard);
  auto filter_func = std::views::filter([&](auto pair) -> bool {
    return wildcard_match(pair.first, tokens, start_wildcard, end_wildcard);
  });
  auto filtered_v1 = types_v1 | filter_func;
  auto filtered_v2 = types_v2 | filter_func;

  // Get cgroup path for each root
  auto get_path_func = std::views::transform(
      [&](auto pair) -> std::pair<std::string, std::string> {
        return { pair.first,
                 get_cgroup_path_in_hierarchy(cgroupid, pair.second) };
      });
  auto paths_v1 = filtered_v1 | get_path_func;
  auto paths_v2 = filtered_v2 | get_path_func;

  // Return paths with v2 first, then v1 sorted lexically by name.
  std::vector<std::pair<std::string, std::string>> sorted(paths_v2.begin(),
                                                          paths_v2.end());
  std::vector<std::pair<std::string, std::string>> sorted_v1(paths_v1.begin(),
                                                             paths_v1.end());
  std::ranges::sort(sorted_v1);
  sorted.insert(sorted.end(), sorted_v1.begin(), sorted_v1.end());
  return sorted;
}

bool is_module_loaded(const std::string &module)
{
  if (module == "vmlinux") {
    return true;
  }

  // This file lists all loaded modules
  std::ifstream modules_file("/proc/modules");

  for (std::string line; std::getline(modules_file, line);) {
    if (line.compare(0, module.size() + 1, module + " ") == 0) {
      modules_file.close();
      return true;
    }
  }

  modules_file.close();
  return false;
}

bool is_dir(const std::string &path)
{
  std::error_code ec;
  std::filesystem::path buf{ path };
  return std::filesystem::is_directory(buf, ec);
}

// get_kernel_dirs fills {ksrc, kobj} - directories for pristine and
// generated kernel sources - and returns if they were found.
//
// When the kernel was built in its source tree ksrc == kobj, however when
// the kernel was build in a different directory than its source, ksrc != kobj.
//
// A notable example is Debian, which places pristine kernel headers in
//
//   /lib/modules/`uname -r`/source/
//
// and generated kernel headers in
//
//   /lib/modules/`uname -r`/build/
//
// false is returned if no trace of kernel headers was found at all, with the
// guessed location set anyway for later warning.
//
// Both ksrc and kobj are guaranteed to be != ""
bool get_kernel_dirs(const struct utsname &utsname,
                     std::string &ksrc,
                     std::string &kobj)
{
  ksrc = kobj = std::string(KERNEL_HEADERS_DIR);
  if (!ksrc.empty())
    return true;

  const char *kpath_env = ::getenv("BPFTRACE_KERNEL_SOURCE");
  if (kpath_env) {
    ksrc = std::string(kpath_env);
    const char *kpath_build_env = ::getenv("BPFTRACE_KERNEL_BUILD");
    if (kpath_build_env) {
      kobj = std::string(kpath_build_env);
    } else {
      kobj = ksrc;
    }
    return true;
  }

  std::string kdir = std::string("/lib/modules/") + utsname.release;
  ksrc = kdir + "/source";
  kobj = kdir + "/build";

  // if one of source/ or build/ is not present - try to use the other one for
  // both.
  auto has_ksrc = is_dir(ksrc);
  auto has_kobj = is_dir(kobj);
  if (!has_ksrc && !has_kobj) {
    return false;
  }
  if (!has_ksrc) {
    ksrc = kobj;
  } else if (!has_kobj) {
    kobj = ksrc;
  }

  return true;
}

const std::string &is_deprecated(const std::string &str)
{
  for (auto &item : DEPRECATED_LIST) {
    if (!item.matches(str)) {
      continue;
    }

    if (item.show_warning) {
      LOG(WARNING) << item.old_name
                   << " is deprecated and will be removed in the future. Use "
                   << item.new_name << " instead.";
      item.show_warning = false;
    }

    if (item.replace_by_new_name) {
      return item.new_name;
    } else {
      return str;
    }
  }

  return str;
}

bool is_unsafe_func(const std::string &func_name)
{
  return std::ranges::any_of(UNSAFE_BUILTIN_FUNCS,

                             [&](const auto &cand) {
                               return func_name == cand;
                             });
}

bool is_compile_time_func(const std::string &func_name)
{
  return std::ranges::any_of(COMPILE_TIME_FUNCS,

                             [&](const auto &cand) {
                               return func_name == cand;
                             });
}

bool is_supported_lang(const std::string &lang)
{
  return std::ranges::any_of(UPROBE_LANGS,

                             [&](const auto &cand) { return lang == cand; });
}

bool is_type_name(std::string_view str)
{
  return str.starts_with("struct ") || str.starts_with("union ") ||
         str.starts_with("enum ");
}

std::string exec_system(const char *cmd)
{
  std::array<char, 128> buffer;
  std::string result;
  std::shared_ptr<FILE> pipe(popen(cmd, "r"), pclose);
  if (!pipe)
    throw bpftrace::FatalUserException("popen() failed!");
  while (!feof(pipe.get())) {
    if (fgets(buffer.data(), 128, pipe.get()) != nullptr)
      result += buffer.data();
  }
  return result;
}

// Original resolve_binary_path API defaulting to bpftrace's mount namespace
std::vector<std::string> resolve_binary_path(const std::string &cmd)
{
  const char *env_paths = getenv("PATH");
  return resolve_binary_path(cmd, env_paths, std::nullopt);
}

// If a pid is specified, the binary path is taken relative to its own PATH if
// it is in a different mount namespace. Otherwise, the path is resolved
// relative to the local PATH env var for bpftrace's own mount namespace if it
// is set
std::vector<std::string> resolve_binary_path(const std::string &cmd,
                                             std::optional<int> pid)
{
  std::string env_paths = "";
  std::ostringstream pid_environ_path;

  if (pid.has_value() && pid_in_different_mountns(*pid)) {
    pid_environ_path << "/proc/" << *pid << "/environ";
    std::ifstream environ(pid_environ_path.str());

    if (environ) {
      std::string env_var;
      std::string pathstr = ("PATH=");
      while (std::getline(environ, env_var, '\0')) {
        if (env_var.find(pathstr) != std::string::npos) {
          env_paths = env_var.substr(pathstr.length());
          break;
        }
      }
    }
    return resolve_binary_path(cmd, env_paths.c_str(), pid);
  } else {
    return resolve_binary_path(cmd, getenv("PATH"), pid);
  }
}

// Check whether 'path' refers to a ELF file. Errors are swallowed silently and
// result in return of 'nullopt'. On success, the ELF type (e.g., ET_DYN) is
// returned.
static std::optional<int> is_elf(const std::string &path)
{
  int fd;
  Elf *elf;
  GElf_Ehdr ehdr;

  if (elf_version(EV_CURRENT) == EV_NONE) {
    return std::nullopt;
  }

  fd = open(path.c_str(), O_RDONLY, 0);
  if (fd < 0) {
    return std::nullopt;
  }
  SCOPE_EXIT
  {
    ::close(fd);
  };

  elf = elf_begin(fd, ELF_C_READ, nullptr);
  if (elf == nullptr) {
    return std::nullopt;
  }
  SCOPE_EXIT
  {
    ::elf_end(elf);
  };

  if (elf_kind(elf) != ELF_K_ELF) {
    return std::nullopt;
  }

  if (!gelf_getehdr(elf, &ehdr)) {
    return std::nullopt;
  }

  return ehdr.e_type;
}

static bool has_exec_permission(const std::string &path)
{
  using std::filesystem::perms;

  auto perms = std::filesystem::status(path).permissions();
  return (perms & perms::owner_exec) != perms::none;
}

// Check whether 'path' refers to an executable ELF file.
bool is_exe(const std::string &path)
{
  if (auto e_type = is_elf(path)) {
    return e_type == ET_EXEC && has_exec_permission(path);
  }
  return false;
}

// Private interface to resolve_binary_path, used for the exposed variants
// above, allowing for a PID whose mount namespace should be optionally
// considered.
static std::vector<std::string> resolve_binary_path(const std::string &cmd,
                                                    const char *env_paths,
                                                    std::optional<int> pid)
{
  std::vector<std::string> candidate_paths = { cmd };

  if (env_paths != nullptr && cmd.find("/") == std::string::npos)
    for (const auto &path : split_string(env_paths, ':'))
      candidate_paths.push_back(path + "/" + cmd);

  if (cmd.find("*") != std::string::npos)
    candidate_paths = ::expand_wildcard_paths(candidate_paths);

  std::vector<std::string> valid_executable_paths;
  for (const auto &path : candidate_paths) {
    std::string rel_path;
    if (pid.has_value() && pid_in_different_mountns(*pid))
      rel_path = path_for_pid_mountns(*pid, path);
    else
      rel_path = path;

    // Both executables and shared objects are game.
    if (auto e_type = is_elf(rel_path)) {
      if ((e_type == ET_EXEC && has_exec_permission(rel_path)) ||
          e_type == ET_DYN) {
        valid_executable_paths.push_back(rel_path);
      }
    }
  }

  return valid_executable_paths;
}

std::string path_for_pid_mountns(int pid, const std::string &path)
{
  std::ostringstream pid_relative_path;
  char pid_root[64];

  snprintf(pid_root, sizeof(pid_root), "/proc/%d/root", pid);

  if (!path.starts_with(pid_root)) {
    std::string sep = (path.length() >= 1 && path.at(0) == '/') ? "" : "/";
    pid_relative_path << pid_root << sep << path;
  } else {
    // The path is already relative to the pid's root
    pid_relative_path << path;
  }
  return pid_relative_path.str();
}

// Determines if the target process is in a different mount namespace from
// bpftrace.
//
// If a process is in a different mount namespace (eg, container) it is very
// likely that any references to local paths will not be valid, and that paths
// need to be made relative to the PID.
//
// If an invalid PID is specified or doesn't exist, it returns false.
// True is only returned if the namespace of the target process could be read
// and it doesn't match that of bpftrace. If there was an error reading either
// mount namespace, it will throw an exception
static bool pid_in_different_mountns(int pid)
{
  if (pid <= 0)
    return false;

  std::error_code ec;
  std::filesystem::path self_path{ "/proc/self/ns/mnt" };
  std::filesystem::path target_path{ "/proc" };
  target_path /= std::to_string(pid);
  target_path /= "ns/mnt";

  if (!std::filesystem::exists(self_path, ec)) {
    throw MountNSException(
        "Failed to compare mount ns with PID " + std::to_string(pid) +
        ". The error was open (/proc/self/ns/mnt): " + ec.message());
  }

  if (!std::filesystem::exists(target_path, ec)) {
    throw MountNSException(
        "Failed to compare mount ns with PID " + std::to_string(pid) +
        ". The error was open (/proc/<pid>/ns/mnt): " + ec.message());
  }

  bool result = !std::filesystem::equivalent(self_path, target_path, ec);

  if (ec) {
    throw MountNSException("Failed to compare mount ns with PID " +
                           std::to_string(pid) +
                           ". The error was (fstat): " + ec.message());
  }

  return result;
}

void cat_file(const char *filename, size_t max_bytes, std::ostream &out)
{
  std::ifstream file(filename);
  const size_t BUFSIZE = 4096;

  if (file.fail()) {
    LOG(ERROR) << "failed to open file '" << filename
               << "': " << strerror(errno);
    return;
  }

  char buf[BUFSIZE];
  size_t bytes_read = 0;
  // Read the file batches to avoid allocating a potentially
  // massive buffer.
  while (bytes_read < max_bytes) {
    size_t size = std::min(BUFSIZE, max_bytes - bytes_read);
    file.read(buf, size);
    out.write(buf, file.gcount());
    if (file.eof()) {
      return;
    }
    if (file.fail()) {
      LOG(ERROR) << "failed to open file '" << filename
                 << "': " << strerror(errno);
      return;
    }
    bytes_read += file.gcount();
  }
}

std::string str_join(const std::vector<std::string> &list,
                     const std::string &delim)
{
  std::string str;
  bool first = true;
  for (const auto &elem : list) {
    if (first)
      first = false;
    else
      str += delim;

    str += elem;
  }
  return str;
}

std::optional<std::variant<int64_t, uint64_t>> get_int_from_str(
    const std::string &s)
{
  if (s.size() == 0) {
    return std::nullopt;
  }

  if (s.starts_with("0x") || s.starts_with("0X")) {
    // Treat all hex's as unsigned
    std::size_t idx;
    try {
      uint64_t ret = std::stoull(s, &idx, 0);
      if (idx == s.size()) {
        return ret;
      } else {
        return std::nullopt;
      }
    } catch (...) {
      return std::nullopt;
    }
  }

  char *endptr;
  const char *s_ptr = s.c_str();
  errno = 0;

  if (s.at(0) == '-') {
    int64_t ret = strtol(s_ptr, &endptr, 0);
    if (endptr == s_ptr || *endptr != '\0' || errno == ERANGE ||
        errno == EINVAL) {
      return std::nullopt;
    }
    return ret;
  }

  uint64_t ret = strtoul(s_ptr, &endptr, 0);
  if (endptr == s_ptr || *endptr != '\0' || errno == ERANGE ||
      errno == EINVAL) {
    return std::nullopt;
  }
  return ret;
}

bool symbol_has_cpp_mangled_signature(const std::string &sym_name)
{
  if (!sym_name.rfind("_Z", 0) || !sym_name.rfind("____Z", 0))
    return true;
  else
    return false;
}

static std::string get_invalid_pid_message(const std::string &pid,
                                           const std::string &msg)
{
  return "pid '" + pid + "' " + msg;
}

std::optional<pid_t> parse_pid(const std::string &str, std::string &err)
{
  std::size_t idx = 0;
  pid_t pid;
  constexpr ssize_t pid_max = 4 * 1024 * 1024;
  try {
    pid = std::stol(str, &idx, 10);
  } catch (const std::out_of_range &e) {
    err = get_invalid_pid_message(str, "outside of integer range");
    return std::nullopt;
  } catch (const std::invalid_argument &e) {
    err = get_invalid_pid_message(str, "is not a valid decimal number");
    return std::nullopt;
  }
  // Detect cases like `13ABC`
  if (idx < str.size()) {
    err = get_invalid_pid_message(str, "is not a valid decimal number");
    return std::nullopt;
  }

  if (pid < 1 || pid > pid_max) {
    err = get_invalid_pid_message(str,
                                  "out of valid pid range [1," +
                                      std::to_string(pid_max) + "]");
    return std::nullopt;
  }

  return pid;
}

std::string hex_format_buffer(const char *buf,
                              size_t size,
                              bool keep_ascii,
                              bool escape_hex)
{
  // Allow enough space for every byte to be sanitized in the form "\x00"
  std::string str(size * 4 + 1, '\0');
  char *s = str.data();

  size_t offset = 0;
  for (size_t i = 0; i < size; i++)
    if (keep_ascii && buf[i] >= 32 && buf[i] <= 126)
      offset += sprintf(s + offset,
                        "%c",
                        (reinterpret_cast<const uint8_t *>(buf))[i]);
    else if (escape_hex)
      offset += sprintf(s + offset,
                        "\\x%02x",
                        (reinterpret_cast<const uint8_t *>(buf))[i]);
    else
      offset += sprintf(s + offset,
                        i == size - 1 ? "%02x" : "%02x ",
                        (reinterpret_cast<const uint8_t *>(buf))[i]);

  // Fit return value to actual length
  str.resize(offset);
  return str;
}

// Attaching to these kernel functions with fentry/fexit (kfunc/kretfunc)
// could lead to a recursive loop and kernel crash so we need additional
// generated BPF code to protect against this if one of these are being
// attached to.
bool is_recursive_func(const std::string &func_name)
{
  return RECURSIVE_KERNEL_FUNCS.find(func_name) != RECURSIVE_KERNEL_FUNCS.end();
}

static bool is_bad_func(std::string &func)
{
  // Certain kernel functions are known to cause system stability issues if
  // traced (but not marked "notrace" in the kernel) so they should be filtered
  // out as the list is built. The list of functions have been taken from the
  // bpf kernel selftests (bpf/prog_tests/kprobe_multi_test.c).
  static const std::unordered_set<std::string> bad_funcs = {
    "arch_cpu_idle", "default_idle", "bpf_dispatcher_xdp_func"
  };

  static const std::vector<std::string> bad_funcs_partial = {
    "__ftrace_invalid_address__", "rcu_"
  };

  if (bad_funcs.find(func) != bad_funcs.end())
    return true;

  for (const auto &s : bad_funcs_partial) {
    if (!std::strncmp(func.c_str(), s.c_str(), s.length()))
      return true;
  }

  return false;
}

FuncsModulesMap parse_traceable_funcs()
{
  // Try to get the list of functions from BPFTRACE_AVAILABLE_FUNCTIONS_TEST env
  const char *path_env = std::getenv("BPFTRACE_AVAILABLE_FUNCTIONS_TEST");
  const std::string kprobe_path = path_env
                                      ? path_env
                                      : tracefs::available_filter_functions();

  std::ifstream available_funs(kprobe_path);
  if (available_funs.fail()) {
    LOG(V1) << "Error while reading traceable functions from " << kprobe_path
            << ": " << strerror(errno);
    return {};
  }

  FuncsModulesMap result;
  std::string line;
  while (std::getline(available_funs, line)) {
    auto func_mod = split_symbol_module(line);
    if (func_mod.second.empty())
      func_mod.second = "vmlinux";

    if (!is_bad_func(func_mod.first))
      result[func_mod.first].insert(func_mod.second);
  }

  // Filter out functions from the kprobe blacklist.
  const std::string kprobes_blacklist_path = debugfs::kprobes_blacklist();
  std::ifstream kprobes_blacklist_funs(kprobes_blacklist_path);
  while (std::getline(kprobes_blacklist_funs, line)) {
    auto addr_func_mod = split_addrrange_symbol_module(line);
    if (result.find(std::get<1>(addr_func_mod)) != result.end()) {
      result.erase(std::get<1>(addr_func_mod));
    }
  }

  return result;
}

// Search for LINUX_VERSION_CODE in the vDSO, returning 0 if it can't be found.
static uint32_t _find_version_note(unsigned long base)
{
  auto ehdr = reinterpret_cast<const ElfW(Ehdr) *>(base);

  for (int i = 0; i < ehdr->e_shnum; i++) {
    auto shdr = reinterpret_cast<const ElfW(Shdr) *>(base + ehdr->e_shoff +
                                                     (i * ehdr->e_shentsize));

    if (shdr->sh_type == SHT_NOTE) {
      auto ptr = reinterpret_cast<const char *>(base + shdr->sh_offset);
      auto end = ptr + shdr->sh_size;

      while (ptr < end) {
        auto nhdr = reinterpret_cast<const ElfW(Nhdr) *>(ptr);
        ptr += sizeof *nhdr;

        auto name = ptr;
        ptr += (nhdr->n_namesz + sizeof(ElfW(Word)) - 1) & -sizeof(ElfW(Word));

        auto desc = ptr;
        ptr += (nhdr->n_descsz + sizeof(ElfW(Word)) - 1) & -sizeof(ElfW(Word));

        if ((nhdr->n_namesz > 5 && !memcmp(name, "Linux", 5)) &&
            nhdr->n_descsz == 4 && !nhdr->n_type)
          return *reinterpret_cast<const uint32_t *>(desc);
      }
    }
  }

  return 0;
}

static uint32_t kernel_version_from_vdso()
{
  // Fetch LINUX_VERSION_CODE from the vDSO .note section, falling back on
  // the build-time constant if unavailable. This always matches the
  // running kernel, but is not supported on arm32.
  unsigned code = 0;
  unsigned long base = getauxval(AT_SYSINFO_EHDR);
  if (base && !memcmp(reinterpret_cast<void *>(base), ELFMAG, 4))
    code = _find_version_note(base);
  if (!code)
    code = LINUX_VERSION_CODE;
  return code;
}

static uint32_t kernel_version_from_uts()
{
  struct utsname utsname;
  if (uname(&utsname) < 0)
    return 0;
  unsigned x, y, z;
  if (sscanf(utsname.release, "%u.%u.%u", &x, &y, &z) != 3)
    return 0;
  return KERNEL_VERSION(x, y, z);
}

static uint32_t kernel_version_from_khdr()
{
  // Try to get the definition of LINUX_VERSION_CODE at runtime.
  std::ifstream linux_version_header{ "/usr/include/linux/version.h" };
  const std::string content{ std::istreambuf_iterator<char>(
                                 linux_version_header),
                             std::istreambuf_iterator<char>() };
  const std::regex regex{ R"(#define\s+LINUX_VERSION_CODE\s+(\d+))" };
  std::smatch match;

  if (std::regex_search(content.begin(), content.end(), match, regex))
    return static_cast<unsigned>(std::stoi(match[1]));

  return 0;
}

// Find a LINUX_VERSION_CODE matching the host kernel. The build-time constant
// may not match if bpftrace is compiled on a different Linux version than it's
// used on, e.g. if built with Docker.
uint32_t kernel_version(KernelVersionMethod method)
{
  static std::optional<uint32_t> a0, a1, a2;
  switch (method) {
    case vDSO: {
      if (!a0)
        a0 = kernel_version_from_vdso();
      return *a0;
    }
    case UTS: {
      if (!a1)
        a1 = kernel_version_from_uts();
      return *a1;
    }
    case File: {
      if (!a2)
        a2 = kernel_version_from_khdr();
      return *a2;
    }
    case None:
      return 0;
  }

  // Unreachable
  return 0;
}

std::optional<std::string> abs_path(const std::string &rel_path)
{
  // filesystem::canonical does not work very well with /proc/<pid>/root paths
  // of processes in a different mount namespace (than the one bpftrace is
  // running in), failing during canonicalization. See bpftrace:bpftrace#1595
  static auto re = std::regex("^/proc/\\d+/root/.*");
  if (!std::regex_match(rel_path, re)) {
    try {
      auto p = std::filesystem::path(rel_path);
      return std::filesystem::canonical(std::filesystem::absolute(p)).string();
    } catch (std::filesystem::filesystem_error &) {
      return {};
    }
  } else {
    return rel_path;
  }
}

bool symbol_has_module(const std::string &symbol)
{
  return !symbol.empty() && symbol[symbol.size() - 1] == ']';
}

std::pair<std::string, std::string> split_symbol_module(
    const std::string &symbol)
{
  if (!symbol_has_module(symbol))
    return { symbol, "" };

  size_t idx = symbol.rfind(" [");
  if (idx == std::string::npos)
    return { symbol, "" };

  return { symbol.substr(0, idx),
           symbol.substr(idx + strlen(" ["),
                         symbol.length() - idx - strlen(" []")) };
}

// Usually the /sys/kernel/debug/kprobes/blacklist file.
// Format example:
// 0xffffffff85201511-0xffffffff8520152f	first_nmi
// 0xffffffffc17e9373-0xffffffffc17e94ff	vmx_vmexit [kvm_intel]
// The outputs are:
// { "0xffffffff85201511-0xffffffff8520152f", "first_nmi", "" }
// { "0xffffffffc17e9373-0xffffffffc17e94ff", "vmx_vmexit", "kvm_intel" }
std::tuple<std::string, std::string, std::string> split_addrrange_symbol_module(
    const std::string &symbol)
{
  size_t idx1 = symbol.rfind("\t");
  size_t idx2 = symbol.rfind(" [");

  if (idx2 == std::string::npos)
    return { symbol.substr(0, idx1),
             symbol.substr(idx1 + strlen("\t"),
                           symbol.length() - idx1 - strlen("\t")),
             "" };

  return { symbol.substr(0, idx1),
           symbol.substr(idx1 + strlen("\t"), idx2 - idx1 - strlen("\t")),
           symbol.substr(idx2 + strlen(" ["),
                         symbol.length() - idx2 - strlen(" []")) };
}

std::map<uintptr_t, elf_symbol, std::greater<>> get_symbol_table_for_elf(
    const std::string &elf_file)
{
  std::map<uintptr_t, elf_symbol, std::greater<>> symbol_table;

  bcc_elf_symcb sym_resolve_callback = [](const char *name,
                                          uint64_t start,
                                          uint64_t length,
                                          void *payload) {
    auto *symbol_table =
        static_cast<std::map<uintptr_t, elf_symbol, std::greater<>> *>(payload);
    symbol_table->insert({ start,
                           { .name = std::string(name),
                             .start = start,
                             .end = start + length } });
    return 0;
  };
  struct bcc_symbol_option option;
  memset(&option, 0, sizeof(option));
  option.use_symbol_type = BCC_SYM_ALL_TYPES ^ (1 << STT_NOTYPE);
  bcc_elf_foreach_sym(
      elf_file.c_str(), sym_resolve_callback, &option, &symbol_table);

  return symbol_table;
}

std::vector<int> get_pids_for_program(const std::string &program)
{
  std::error_code ec;
  auto program_abs = std::filesystem::canonical(program, ec);
  if (ec) {
    // std::filesystem::canonical will fail if we are attaching to a uprobe that
    // lives in another filesystem namespace. For example,
    // uprobe:/proc/12345/root/my_program:function1
    // This shouldn't be a fatal condition as this function is only used to
    // attach to all running processes for a given binary, and the above uprobe
    // is targetting a specific process. So if this happens, just return no
    // pids. The probe will still attach directly to the targeted process.
    return {};
  }

  std::vector<int> pids;
  for (const auto &process : std::filesystem::directory_iterator("/proc")) {
    std::string filename = process.path().filename().string();
    if (!std::ranges::all_of(filename, ::isdigit))
      continue;
    std::error_code ec;
    std::filesystem::path pid_program = std::filesystem::read_symlink(
        process.path() / "exe", ec);
    if (!ec && program_abs == pid_program)
      pids.emplace_back(std::stoi(filename));
  }
  return pids;
}

std::vector<int> get_all_running_pids()
{
  std::vector<int> pids;
  for (const auto &process : std::filesystem::directory_iterator("/proc")) {
    std::string filename = process.path().filename().string();
    if (!std::ranges::all_of(filename, ::isdigit))
      continue;
    pids.emplace_back(std::stoi(filename));
  }
  return pids;
}

// BPF verifier rejects programs with names containing certain characters, use
// this function to replace every character not valid for C identifiers by '_'
std::string sanitise_bpf_program_name(const std::string &name)
{
  std::string sanitised_name = name;
  std::ranges::replace_if(
      sanitised_name,

      [](char c) { return !isalnum(c) && c != '_'; },
      '_');

  // Kernel KSYM_NAME_LEN is 128 until 6.1
  // If we'll exceed the limit, hash the string and cap at 127 (+ null byte).
  if (sanitised_name.size() > 127) {
    size_t hash = std::hash<std::string>{}(sanitised_name);

    // std::hash returns size_t, so we reserve 2*sizeof(size_t)+1 characters
    std::ostringstream os;
    os << sanitised_name.substr(0, 127 - (2 * sizeof(hash)) - 1) << '_'
       << std::setfill('0') << std::hex << hash;
    sanitised_name = os.str();
  }
  return sanitised_name;
}

uint32_t round_up_to_next_power_of_two(uint32_t n)
{
  // http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
  if (n == 0) {
    return 0;
  }
  // Note this function doesn't work if n > 2^31 since there are not enough
  // bits in unsigned 32 bit integers. This should be fine since its unlikely
  // anyone has > 2^31 CPUs.
  assert(n <= 2147483648);
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}

} // namespace bpftrace
