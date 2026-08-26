//===- unittest/Format/CParseFormatTest.cpp -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Format/Format.h"
#include "gtest/gtest.h"

namespace clang {
namespace format {
namespace {

class CParseFormatTest : public testing::Test {
protected:
  static FormatStyle getStyle() {
    FormatStyle Style = getLLVMStyle();
    Style.ColumnLimit = 100;
    Style.IndentWidth = 4;
    Style.AllowShortFunctionsOnASingleLine = FormatStyle::SFS_None;
    FormatStyle::RawStringFormat Raw;
    Raw.Language = FormatStyle::LK_CParse;
    Raw.Delimiters = {"PS"};
    Style.RawStringFormats.push_back(Raw);
    return Style;
  }

  static std::string format(llvm::StringRef Code) {
    FormatStyle Style = getStyle();
    tooling::Replacements Replaces =
        reformat(Style, Code, {tooling::Range(0, Code.size())}, "<stdin>");
    auto Result = applyAllReplacements(Code, Replaces);
    EXPECT_TRUE(static_cast<bool>(Result));
    return Result ? *Result : std::string();
  }

  // The spec formats to Expected, and Expected is a fixed point (idempotent).
  static void expect(llvm::StringRef Expected, llvm::StringRef Input) {
    EXPECT_EQ(Expected.str(), format(Input));
    EXPECT_EQ(Expected.str(), format(Expected));
  }
  static void stable(llvm::StringRef Code) { expect(Code, Code); }
};

TEST_F(CParseFormatTest, InlineScalarsStayInline) {
  stable(R"x(void t(void) {
    f(v, R"PS({ .a = 1, .b = 2 })PS");
    f(v, R"PS([ 1, 2, 3 ])PS");
    f(v, R"PS({ 1: 2, 3: 4 })PS");
    f(v, R"PS({ })PS");
    f(v, R"PS([ ])PS");
    f(v, R"PS({ true, false })PS");
})x");
}

TEST_F(CParseFormatTest, MessyCompressesToInline) {
  expect(R"x(void t(void) {
    f(v, R"PS({ .a = 1, .b = 2 })PS");
})x",
         R"x(void t(void) {
    f(v, R"PS({.a=1,.b=2})PS");
})x");
}

TEST_F(CParseFormatTest, ArrayOfObjectsExpands) {
  stable(R"x(void t(void) {
    f(v, R"PS([
        { .a = 1 },
        { .b = 2 },
    ])PS");
})x");
}

TEST_F(CParseFormatTest, Flagship) {
  stable(R"x(void t(void) {
    f(plans, R"PS({
        .baddrs_to_read = [ 1, 2, 3 ],
        .read_plans = [
            {
                .delta_daddrs = [ 2.456 ],
                .byte_deltas = [
                    { .offset = 0, .delta = 0.1 },
                    { .offset = 2, .delta = 0.3 },
                ],
            },
            { .base_crc_daddr = 3.123, .delta_daddrs = [ 4.789 ] },
            { },
        ],
    })PS");
})x");
}

TEST_F(CParseFormatTest, ObjectHoldingObjectExpands) {
  stable(R"x(void t(void) {
    f(v, R"PS({
        .stripe_config = { 3, 2 },
        .disks = [ 3.1, 2.3, 1.4 ],
    })PS");
})x");
}

TEST_F(CParseFormatTest, UnionTags) {
  stable(R"x(void t(void) {
    f(v, R"PS(B: A: false)PS");
    f(v, R"PS(B: {
        .b = { .a = 3, .b = 9 },
    })PS");
})x");
}

TEST_F(CParseFormatTest, ContainerAsMapKey) {
  stable(R"x(void t(void) {
    f(v, R"PS({
        { HDD_SLOTTED, .size = 10 }: { .num = 10 },
        { NON_SLOTTED, .size = 14 }: { .num = 14 },
    })PS");
})x");
}

TEST_F(CParseFormatTest, OverflowObjectOnePerLine) {
  stable(R"x(void t(void) {
    f(v, R"PS({
        .alpha = 111,
        .beta = 222,
        .gamma = 333,
        .delta = 444,
        .epsilon = 555,
        .zeta = 666,
        .eta = 777,
        .theta = 888,
        .iota = 999,
    })PS");
})x");
}

TEST_F(CParseFormatTest, WrappedTrailingArgHangsFromStatementIndent) {
  // The spec is the last argument of a call whose arguments stay on the opening
  // line. Its wrapped body hangs from the statement's indent -- entries at
  // +IndentWidth, closer at the statement indent -- not from the call's
  // argument column. AlwaysBreak is the style that exposed the difference;
  // since LLVM 22 it is spelled as the BreakAfterOpenBracket* booleans.
  FormatStyle Style = getStyle();
  Style.AlignAfterOpenBracket = true;
  Style.BreakAfterOpenBracketBracedList = true;
  Style.BreakAfterOpenBracketFunction = true;
  Style.BreakAfterOpenBracketIf = true;
  StringRef Code = R"x(void t(void) {
    record_txn(wal_info, fake_wal_memory_token_calculator, txn_spec_parse(R"PS({
        .txn_id = 1.1.1,
        .wal_blocks = 1,
        .changes_in_last_block = 6,
    })PS"));
})x";
  auto Result = applyAllReplacements(
      Code, reformat(Style, Code, {tooling::Range(0, Code.size())}, "<stdin>"));
  EXPECT_TRUE(static_cast<bool>(Result));
  EXPECT_EQ(Code.str(), Result ? *Result : std::string());
}

TEST_F(CParseFormatTest, CallsAndOperatorsAreOpaqueInline) {
  stable(R"x(void t(void) {
    f(v, R"PS({ 10: overwrite(empty), 19: overwrite(empty) })PS");
    f(v, R"PS([ 1 ] | RSP(6.4, 2) | [ 2 ])PS");
})x");
}

TEST_F(CParseFormatTest, CommentForcesExpand) {
  expect(R"x(void t(void) {
    f(v, R"PS({
        .a = 1, // note
        .b = 2,
    })PS");
})x",
         "void t(void) {\n"
         "    f(v, R\"PS({ .a = 1, // note\n"
         ".b = 2 })PS\");\n"
         "}");
}

TEST_F(CParseFormatTest, LeadingComments) {
  stable(R"x(void t(void) {
    f(v, R"PS([
        // tree daddrs
        { .a = 100 },
        // data daddrs
        { .a = 103 },
    ])PS");
})x");
}

TEST_F(CParseFormatTest, StringLiteralsAreOpaque) {
  stable(R"x(void t(void) {
    f(v, R"PS({ .name = "hello, world", .ch = 'a' })PS");
})x");
}

TEST_F(CParseFormatTest, MalformedBailsToVerbatim) {
  // Unbalanced and unterminated specs are left exactly as written.
  stable("void t(void) {\n"
         "    f(v, R\"PS({ .a = [ 1, 2 })PS\");\n"
         "}");
  stable("void t(void) {\n"
         "    f(v, R\"PS({ .a = \"oops })PS\");\n"
         "}");
}

} // namespace
} // namespace format
} // namespace clang
