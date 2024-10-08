#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <ctime>

#include "child.h"
#include "procmon.h"

#include "childhelper.h"
#include "utils.h"

namespace bpftrace::test::procmon {

using ::testing::HasSubstr;

TEST(procmon, no_such_proc)
{
  try {
    // NOLINTNEXTLINE(bugprone-unused-raii)
    ProcMon(1 << 21);
    FAIL();
  } catch (const std::runtime_error &e) {
    EXPECT_THAT(e.what(), HasSubstr("No such process"));
  }
}

TEST(procmon, child_terminates)
{
  auto child = getChild("/bin/ls");
  auto procmon = std::make_unique<ProcMon>(child->pid());
  EXPECT_TRUE(procmon->is_alive());
  child->run();
  wait_for(child.get(), 1000);
  EXPECT_FALSE(child->is_alive());
  EXPECT_FALSE(procmon->is_alive());
  EXPECT_FALSE(procmon->is_alive());
}

} // namespace bpftrace::test::procmon
