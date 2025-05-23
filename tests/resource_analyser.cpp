#include "ast/passes/resource_analyser.h"
#include "ast/attachpoint_parser.h"
#include "ast/passes/field_analyser.h"
#include "ast/passes/semantic_analyser.h"
#include "clang_parser.h"
#include "driver.h"
#include "mocks.h"
#include "gtest/gtest.h"

namespace bpftrace::test::resource_analyser {

using ::testing::_;

void test(BPFtrace &bpftrace,
          const std::string &input,
          bool expected_result = true,
          RequiredResources *out_p = nullptr)
{
  ast::ASTContext ast("stdin", input);
  Driver driver(ast, bpftrace);
  std::stringstream out;
  std::stringstream msg;
  msg << "\nInput:\n" << input << "\n\nOutput:\n";

  driver.parse();
  ASSERT_TRUE(ast.diagnostics().ok()) << msg.str();

  ast::AttachPointParser ap_parser(ast, bpftrace, false);
  ap_parser.parse();
  ASSERT_TRUE(ast.diagnostics().ok());

  ast::FieldAnalyser fields(bpftrace);
  fields.visit(ast.root);
  ASSERT_TRUE(ast.diagnostics().ok()) << msg.str();

  ClangParser clang;
  ASSERT_TRUE(clang.parse(ast.root, bpftrace));

  driver.parse();
  ASSERT_TRUE(ast.diagnostics().ok()) << msg.str();

  ap_parser.parse();
  ASSERT_TRUE(ast.diagnostics().ok());

  ast::SemanticAnalyser semantics(ast, bpftrace, false);
  semantics.analyse();
  ASSERT_TRUE(ast.diagnostics().ok()) << msg.str();

  ast::ResourceAnalyser resource_analyser(bpftrace);
  resource_analyser.visit(ast.root);
  auto r = resource_analyser.resources();
  ASSERT_EQ(ast.diagnostics().ok(), expected_result) << msg.str();

  if (out_p)
    *out_p = std::move(r);
}

void test(const std::string &input,
          bool expected_result = true,
          RequiredResources *out = nullptr,
          std::optional<uint64_t> on_stack_limit = std::nullopt)
{
  auto bpftrace = get_mock_bpftrace();
  auto configs = ConfigSetter(*bpftrace->config_, ConfigSource::script);
  configs.set(ConfigKeyInt::on_stack_limit, on_stack_limit.value_or(0));
  return test(*bpftrace, input, expected_result, out);
}

TEST(resource_analyser, multiple_hist_bits_in_single_map)
{
  test("BEGIN { @ = hist(1, 1); @ = hist(1, 2); exit()}", false);
}

TEST(resource_analyser, multiple_lhist_bounds_in_single_map)
{
  test("BEGIN { @[0] = lhist(0, 0, 100000, 1000); @[1] = lhist(0, 0, 100000, "
       "100); exit() }",
       false);
}

TEST(resource_analyser, printf_in_subprog)
{
  test(R"(fn greet(): void { printf("Hello, world\n"); })", true);
}

TEST(resource_analyser, fmt_string_args_size_ints)
{
  RequiredResources resources;
  test(R"(BEGIN { printf("%d %d", 3, 4) })", true, &resources);
  EXPECT_EQ(resources.max_fmtstring_args_size, 24);
}

TEST(resource_analyser, fmt_string_args_below_on_stack_limit)
{
  RequiredResources resources;
  test(R"(BEGIN { printf("%d %d", 3, 4) })", true, &resources, 32);
  EXPECT_EQ(resources.max_fmtstring_args_size, 0);
}

TEST(resource_analyser, fmt_string_args_size_arrays)
{
  RequiredResources resources;
  test(
      R"(struct Foo { int a; char b[10]; } BEGIN { $foo = (struct Foo *)0; $foo2 = (struct Foo *)1; printf("%d %s %d %s\n", $foo->a, $foo->b, $foo2->a, $foo2->b) })",
      true,
      &resources);
  EXPECT_EQ(resources.max_fmtstring_args_size, 56);
}

TEST(resource_analyser, fmt_string_args_size_strings)
{
  RequiredResources resources;
  test(
      R"(BEGIN { printf("%dst: %sa; %dnd: %sb;; %drd: %sc;;; %dth: %sd;;;;\n", 1, "a", 2, "ab", 3, "abc", 4, "abcd") })",
      true,
      &resources);
  EXPECT_EQ(resources.max_fmtstring_args_size, 72);
}

TEST(resource_analyser, fmt_string_args_non_map_print_int)
{
  RequiredResources resources;
  test(R"(BEGIN { print(5) })", true, &resources);
  EXPECT_EQ(resources.max_fmtstring_args_size, 24);
}

TEST(resource_analyser, fmt_string_args_non_map_print_arr)
{
  RequiredResources resources;
  test(
      R"(struct Foo { char a[24]; } BEGIN { print(5); $foo = (struct Foo *)0; print($foo->a) })",
      true,
      &resources);
  EXPECT_EQ(resources.max_fmtstring_args_size, 40);
}

} // namespace bpftrace::test::resource_analyser
