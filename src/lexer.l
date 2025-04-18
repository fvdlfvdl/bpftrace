%option yylineno noyywrap noinput
%option never-interactive
%option reentrant
%option stack

%{
#include <climits>
#include "driver.h"
#include "utils.h"
#include "parser.tab.hh"
#include "ast/int_parser.h"

bpftrace::location loc;
static std::string struct_type;
static std::string buffer;

#define YY_USER_ACTION loc.columns(yyleng);
#define yyterminate() return bpftrace::Parser::make_END(loc)

using namespace bpftrace;
%}

/* https://en.cppreference.com/w/cpp/language/integer_literal#The_type_of_the_literal */
int_size (([uU])|([uU]?[lL]?[lL]))

/* Number with underscores in it, e.g. 1_000_000 */
int      [0-9]([0-9_]*[0-9])?{int_size}?
hex      0[xX][0-9a-fA-F]+
/* scientific notation, e.g. 2e4 or 1e6 */
exponent {int}[eE]{int}

ident    [_a-zA-Z][_a-zA-Z0-9]*
map      @{ident}|@
var      ${ident}
hspace   [ \t]
vspace   [\n\r]
space    {hspace}|{vspace}
path     :(\\.|[_\-\./a-zA-Z0-9#+\*])+
builtin  arg[0-9]|args|cgroup|comm|cpid|numaid|cpu|ctx|curtask|elapsed|func|gid|pid|probe|rand|retval|sarg[0-9]|tid|uid|username|jiffies
call     avg|buf|cat|cgroupid|clear|count|delete|exit|hist|join|kaddr|kptr|ksym|len|lhist|macaddr|max|min|ntop|override|print|printf|cgroup_path|reg|signal|stats|str|strerror|strftime|strncmp|strcontains|sum|system|time|uaddr|uptr|usym|zero|path|unwatch|bswap|skboutput|pton|debugf|has_key|percpu_kaddr

int_type        bool|(u)?int(8|16|32|64)
builtin_type    void|(u)?(min|max|sum|count|avg|stats)_t|probe_t|username|lhist_t|hist_t|usym_t|ksym_t|timestamp|macaddr_t|cgroup_path_t|strerror_t|kstack_t|ustack_t
sized_type      string|inet|buffer
subprog         fn

/* Don't add to this! Use builtin OR call not both */
call_and_builtin kstack|ustack|nsecs

/* escape sequences in strings */
hex_esc  (x|X)[0-9a-fA-F]{1,2}
oct_esc  [0-7]{1,3}

%x STR
%x STRUCT
%x ENUM
%x BRACE
%x COMMENT
%x AFTER_COLON
%x STRUCT_AFTER_COLON

%%

{hspace}+               { loc.step(); }
{vspace}+               { loc.lines(yyleng); loc.step(); }

^"#!".*$                // executable line
"//".*$                 // single-line comments
"/*"                    yy_push_state(COMMENT, yyscanner);
<COMMENT>{
  "*/"                  yy_pop_state(yyscanner);
  [^*\n]+|"*"           {}
  \n                    loc.lines(1); loc.step();
  <<EOF>>               yy_pop_state(yyscanner); driver.error(loc, "end of file during comment");
}

bpftrace|perf|raw       { return Parser::make_STACK_MODE(yytext, loc); }
{builtin}               { return Parser::make_BUILTIN(yytext, loc); }
{call}                  { return Parser::make_CALL(yytext, loc); }
{call_and_builtin}      { return Parser::make_CALL_BUILTIN(yytext, loc); }
{subprog}               { return Parser::make_SUBPROG(yytext, loc); }
{int}|{hex}|{exponent}  {
                          try
                          {
                            auto res = ast::int_parser::to_uint(yytext, 0);
                            return Parser::make_INT(res, loc);
                          }
                          catch (const std::exception &e)
                          {
                            driver.error(loc, e.what());
                          }
                        }
{path}                  { return Parser::make_PATH(yytext, loc); }
{map}                   { return Parser::make_MAP(yytext, loc); }
{var}                   { return Parser::make_VAR(yytext, loc); }
":"                     {
                          /* For handling "struct x" in "fn name(...): struct x {  }" as a type rather than
                            a beginning of a struct definition; see AFTER_COLON rules below */
                          yy_push_state(AFTER_COLON, yyscanner);
                          return Parser::make_COLON(loc);
                        }
";"                     { return Parser::make_SEMI(loc); }
"{"                     { return Parser::make_LBRACE(loc); }
"}"                     { return Parser::make_RBRACE(loc); }
"["                     { return Parser::make_LBRACKET(loc); }
"]"                     { return Parser::make_RBRACKET(loc); }
"("                     { return Parser::make_LPAREN(loc); }
")"                     { return Parser::make_RPAREN(loc); }
\//{space}*[\/\{]       { return Parser::make_ENDPRED(loc); } /* If "/" is followed by "/" or "{", choose ENDPRED, otherwise DIV */
","                     { return Parser::make_COMMA(loc); }
"="                     { return Parser::make_ASSIGN(loc); }
"<<="                   { return Parser::make_LEFTASSIGN(loc); }
">>="                   { return Parser::make_RIGHTASSIGN(loc); }
"+="                    { return Parser::make_PLUSASSIGN(loc); }
"-="                    { return Parser::make_MINUSASSIGN(loc); }
"*="                    { return Parser::make_MULASSIGN(loc); }
"/="                    { return Parser::make_DIVASSIGN(loc); }
"%="                    { return Parser::make_MODASSIGN(loc); }
"&="                    { return Parser::make_BANDASSIGN(loc); }
"|="                    { return Parser::make_BORASSIGN(loc); }
"^="                    { return Parser::make_BXORASSIGN(loc); }
"=="                    { return Parser::make_EQ(loc); }
"!="                    { return Parser::make_NE(loc); }
"<="                    { return Parser::make_LE(loc); }
">="                    { return Parser::make_GE(loc); }
"<<"                    { return Parser::make_LEFT(loc); }
">>"                    { return Parser::make_RIGHT(loc); }
"<"                     { return Parser::make_LT(loc); }
">"                     { return Parser::make_GT(loc); }
"&&"                    { return Parser::make_LAND(loc); }
"||"                    { return Parser::make_LOR(loc); }
"+"                     { return Parser::make_PLUS(loc); }
"-"                     { return Parser::make_MINUS(loc); }
"++"                    { return Parser::make_INCREMENT(loc); }
"--"                    { return Parser::make_DECREMENT(loc); }
"*"                     { return Parser::make_MUL(loc); }
"/"                     { return Parser::make_DIV(loc); }
"%"                     { return Parser::make_MOD(loc); }
"&"                     { return Parser::make_BAND(loc); }
"|"                     { return Parser::make_BOR(loc); }
"^"                     { return Parser::make_BXOR(loc); }
"!"                     { return Parser::make_LNOT(loc); }
"~"                     { return Parser::make_BNOT(loc); }
"."                     { return Parser::make_DOT(loc); }
"->"                    { return Parser::make_PTR(loc); }
"$"[0-9]+               { return Parser::make_PARAM(yytext, loc); }
"$"#                    { return Parser::make_PARAMCOUNT(loc); }
"#"[^!].*               { return Parser::make_CPREPROC(yytext, loc); }
"if"                    { return Parser::make_IF(yytext, loc); }
"else"                  { return Parser::make_ELSE(yytext, loc); }
"?"                     { return Parser::make_QUES(loc); }
"unroll"                { return Parser::make_UNROLL(yytext, loc); }
"while"                 { return Parser::make_WHILE(yytext, loc); }
"config"                { return Parser::make_CONFIG(yytext, loc); }
"for"                   { return Parser::make_FOR(yytext, loc); }
"return"                { return Parser::make_RETURN(yytext, loc); }
"continue"              { return Parser::make_CONTINUE(yytext, loc); }
"break"                 { return Parser::make_BREAK(yytext, loc); }
"sizeof"                { return Parser::make_SIZEOF(yytext, loc); }
"offsetof"              { return Parser::make_OFFSETOF(yytext, loc); }
"let"                   { return Parser::make_LET(yytext, loc); }

{int_type}              { return Parser::make_INT_TYPE(yytext, loc); }
{builtin_type}          { return Parser::make_BUILTIN_TYPE(yytext, loc); }
{sized_type}            { return Parser::make_SIZED_TYPE(yytext, loc); }


\"                      { yy_push_state(STR, yyscanner); buffer.clear(); }
<STR>{
  \"                    { yy_pop_state(yyscanner); return Parser::make_STRING(buffer, loc); }
  [^\\\n\"]+            buffer += yytext;
  \\n                   buffer += '\n';
  \\t                   buffer += '\t';
  \\r                   buffer += '\r';
  \\\"                  buffer += '\"';
  \\\\                  buffer += '\\';
  \\{oct_esc}           {
                            long value = strtol(yytext+1, NULL, 8);
                            if (value > UCHAR_MAX)
                              driver.error(loc, std::string("octal escape sequence out of range '") +
                                                yytext + "'");
                            buffer += value;
                        }
  \\{hex_esc}           buffer += strtol(yytext+2, NULL, 16);
  \n                    driver.error(loc, "unterminated string"); yy_pop_state(yyscanner); loc.lines(1); loc.step();
  <<EOF>>               driver.error(loc, "unterminated string"); yy_pop_state(yyscanner);
  \\.                   { driver.error(loc, std::string("invalid escape character '") +
                                            yytext + "'"); }
  .                     driver.error(loc, "invalid character"); yy_pop_state(yyscanner);
}

struct|union|enum       {
                            yy_push_state(STRUCT, yyscanner);
                            buffer.clear();
                            struct_type = yytext;
                            return Parser::make_STRUCT(loc);
                        }
<AFTER_COLON>{
  {hspace}+             { loc.step(); }
  {vspace}+             { loc.lines(yyleng); loc.step(); }
  struct|union|enum     {
                          yy_pop_state(yyscanner);
                          yy_push_state(STRUCT_AFTER_COLON, yyscanner);
                          buffer.clear();
                          struct_type = yytext;
                          return Parser::make_STRUCT(loc);
                        }
  .                     { unput(yytext[0]); yy_pop_state(yyscanner); }
}
<STRUCT_AFTER_COLON>{
  {hspace}+             { loc.step(); }
  {vspace}+             { loc.lines(yyleng); loc.step(); }
  {ident}               {
                          buffer = yytext;
                          yy_pop_state(yyscanner);
                          return Parser::make_IDENT(struct_type + " " + trim(buffer), loc);
                        }
}
<STRUCT,BRACE>{
  "*"|")"|","           {
                          if (YY_START == STRUCT)
                          {
                            // Finished parsing the typename of a cast or a call arg
                            // Put the cast type into a canonical form by trimming
                            // and then inserting a single space.
                            yy_pop_state(yyscanner);
                            for (int i = yyleng - 1; i >= 0; i--)
                              unput(yytext[i]);
                            return Parser::make_IDENT(struct_type + " " + trim(buffer), loc);
                          }
                          buffer += yytext[0];
                        }
  "{"                   yy_push_state(BRACE, yyscanner); buffer += '{';
  "}"|"};"              {
                          buffer += yytext;
                          yy_pop_state(yyscanner);
                          if (YY_START == STRUCT)
                          {
                            // Finished parsing a struct definition
                            // Trimming isn't needed here since the typenames
                            // will go through Clang before we get them back
                            // anyway.
                            yy_pop_state(yyscanner);
                            return Parser::make_STRUCT_DEFN(struct_type + buffer, loc);
                          }
                        }
  .                     buffer += yytext[0];
  \n                    buffer += '\n'; loc.lines(1); loc.step();
}

{ident}                 {
                          static int unput_count = 0;
                          if (driver.bpftrace.macros_.count(yytext) != 0)
                          {
                            const char *s = driver.bpftrace.macros_.find(yytext)->second.c_str();
                            int z;
                            // NOTE(mmarchini) workaround for simple recursive
                            // macros. More complex recursive macros (for
                            // example, with operators) will go into an
                            // infinite loop. Yes, we should fix that in the
                            // future.
                            if (strcmp(s, yytext) == 0)
                            {
                              unput_count = 0;
                              return Parser::make_IDENT(yytext, loc);
                            }
                            else
                            {
                              int leng = YY_BUF_SIZE;
                              std::string original_s(s);
                              std::string original_yytext(yytext);
                              for (z=strlen(s) - 1; z >= 0; z--){
                                if (unput_count >= 1000 || leng <= 0) {
                                  driver.error(loc, std::string("Macro recursion limit reached: ")
                                                    + original_yytext + ", " + original_s);
                                  yyterminate();
                                }
                                unput(s[z]);
                                unput_count++;
                                leng--;
                              }
                            }
                          } else {
                            unput_count = 0;
                            return Parser::make_IDENT(yytext, loc);
                          }
                        }

.                       { driver.error(loc, std::string("invalid character '") +
                                            std::string(yytext) + std::string("'")); }

%%
