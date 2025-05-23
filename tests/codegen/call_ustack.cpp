#include "common.h"

#include <iterator>

namespace bpftrace {
namespace test {
namespace codegen {

TEST(codegen, call_ustack)
{
  auto result = NAME;

  test("kprobe:f { @x = ustack(); @y = ustack(6); @z = ustack(perf) }", result);
}

TEST(codegen, call_ustack_mapids)
{
  ast::ASTContext ast("stdin", R"(
kprobe:f {
  @x = ustack(5);
  @y = ustack(6);
  @z = ustack(6)
})");
  auto bpftrace = get_mock_bpftrace();
  Driver driver(ast, *bpftrace);

  driver.parse();
  ASSERT_TRUE(ast.diagnostics().ok());

  ast::AttachPointParser ap_parser(ast, *bpftrace, false);
  ap_parser.parse();
  ASSERT_TRUE(ast.diagnostics().ok());

  ClangParser clang;
  clang.parse(ast.root, *bpftrace);

  driver.parse();
  ASSERT_TRUE(ast.diagnostics().ok());

  ap_parser.parse();
  ASSERT_TRUE(ast.diagnostics().ok());

  ast::SemanticAnalyser semantics(ast, *bpftrace);
  semantics.analyse();
  ASSERT_TRUE(ast.diagnostics().ok());

  ast::ResourceAnalyser resource_analyser(*bpftrace);
  resource_analyser.visit(ast.root);
  bpftrace->resources = resource_analyser.resources();
  ASSERT_TRUE(ast.diagnostics().ok());

  ast::CodegenLLVM codegen(ast, *bpftrace);
  bpftrace->bytecode_ = codegen.compile();

  ASSERT_EQ(bpftrace->bytecode_.maps().size(), 8);
  ASSERT_EQ(bpftrace->bytecode_.countStackMaps(), 3U);

  StackType stack_type;
  stack_type.limit = 5;
  ASSERT_TRUE(bpftrace->bytecode_.hasMap(stack_type));
  stack_type.limit = 6;
  ASSERT_TRUE(bpftrace->bytecode_.hasMap(stack_type));
}

TEST(codegen, call_ustack_modes_mapids)
{
  ast::ASTContext ast("stdin", R"(
kprobe:f {
  @w = ustack(raw);
  @x = ustack(perf);
  @y = ustack(bpftrace);
  @z = ustack()
})");
  auto bpftrace = get_mock_bpftrace();
  Driver driver(ast, *bpftrace);

  driver.parse();
  ASSERT_TRUE(ast.diagnostics().ok());

  ast::AttachPointParser ap_parser(ast, *bpftrace, false);
  ap_parser.parse();
  ASSERT_TRUE(ast.diagnostics().ok());

  ClangParser clang;
  clang.parse(ast.root, *bpftrace);

  driver.parse();
  ASSERT_TRUE(ast.diagnostics().ok());

  ap_parser.parse();
  ASSERT_TRUE(ast.diagnostics().ok());

  ast::SemanticAnalyser semantics(ast, *bpftrace);
  semantics.analyse();
  ASSERT_TRUE(ast.diagnostics().ok());

  ast::ResourceAnalyser resource_analyser(*bpftrace);
  resource_analyser.visit(ast.root);
  bpftrace->resources = resource_analyser.resources();
  ASSERT_TRUE(ast.diagnostics().ok());

  ast::CodegenLLVM codegen(ast, *bpftrace);
  bpftrace->bytecode_ = codegen.compile();

  ASSERT_EQ(bpftrace->bytecode_.maps().size(), 10);
  ASSERT_EQ(bpftrace->bytecode_.countStackMaps(), 4U);

  StackType stack_type;
  stack_type.mode = StackMode::perf;
  ASSERT_TRUE(bpftrace->bytecode_.hasMap(stack_type));
  stack_type.mode = StackMode::bpftrace;
  ASSERT_TRUE(bpftrace->bytecode_.hasMap(stack_type));
  stack_type.mode = StackMode::raw;
  ASSERT_TRUE(bpftrace->bytecode_.hasMap(stack_type));
}

} // namespace codegen
} // namespace test
} // namespace bpftrace
