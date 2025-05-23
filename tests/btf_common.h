#pragma once

#include <array>
#include <cstdint>
#include <unistd.h>

#include "data/btf_data.h"
#include "gtest/gtest.h"

namespace {
constexpr std::array<uint8_t, 4> INVALID_BTF_DATA = { 0xDE, 0xAD, 0xBE, 0xEF };

bool create_tmp_with_data(char *path,
                          const unsigned char *data,
                          unsigned int data_len)
{
  if (!path)
    return false;

  int fd = mkstemp(path);
  if (fd < 0) {
    std::remove(path);
    return false;
  }

  if (write(fd, data, data_len) != data_len) {
    close(fd);
    std::remove(path);
    return false;
  }

  close(fd);
  return true;
}
} // namespace

class test_btf : public ::testing::Test {
protected:
  void SetUp() override
  {
    // BTF data file
    char *btf_path = strdup("/tmp/btf_dataXXXXXX");
    if (create_tmp_with_data(btf_path, btf_data, btf_data_len)) {
      setenv("BPFTRACE_BTF", btf_path, true);
      btf_path_ = btf_path;
    }

    // available functions file
    char *funcs_path = strdup("/tmp/available_filter_functionsXXXXXX");
    if (create_tmp_with_data(funcs_path, func_list, func_list_len)) {
      setenv("BPFTRACE_AVAILABLE_FUNCTIONS_TEST", funcs_path, true);
      funcs_path_ = funcs_path;
    }
  }

  void TearDown() override
  {
    // clear the environment and remove the temp files
    unsetenv("BPFTRACE_BTF");
    unsetenv("BPFTRACE_AVAILABLE_FUNCTIONS_TEST");
    if (btf_path_) {
      std::remove(btf_path_);
      ::free(btf_path_);
    }
    if (funcs_path_) {
      std::remove(funcs_path_);
      ::free(funcs_path_);
    }
  }

  char *btf_path_ = nullptr;
  char *funcs_path_ = nullptr;
};

class test_bad_btf : public ::testing::Test {
protected:
  void SetUp() override
  {
    // BTF data file
    char *btf_path = strdup("/tmp/btf_dataXXXXXX");
    if (create_tmp_with_data(btf_path,
                             INVALID_BTF_DATA.data(),
                             INVALID_BTF_DATA.size())) {
      setenv("BPFTRACE_BTF", btf_path, true);
      btf_path_ = btf_path;
    }
  }

  void TearDown() override
  {
    // clear the environment and remove the temp files
    unsetenv("BPFTRACE_BTF");
    if (btf_path_) {
      std::remove(btf_path_);
      ::free(btf_path_);
    }
  }

  char *btf_path_ = nullptr;
};
