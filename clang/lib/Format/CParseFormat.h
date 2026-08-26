//===--- CParseFormat.h - Format Qumulo parse specs -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A self-contained deterministic formatter for Qumulo "parse specs" — the
/// value literals carried in R"PS(...)PS" raw strings. It bypasses the optimizing
/// line formatter entirely: the layout is fully determined by structural rules,
/// so there is nothing to search and no combinatorial blow-up at depth.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_FORMAT_CPARSEFORMAT_H
#define LLVM_CLANG_LIB_FORMAT_CPARSEFORMAT_H

#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>

namespace clang {
namespace format {

/// Format a parse-spec raw-string body \p Content.
///
/// \p BaseIndent is the column of the enclosing statement: a wrapped container's
/// entries land at \p BaseIndent + \p IndentWidth and its closer at
/// \p BaseIndent. \p ColumnLimit drives overflow wrapping of otherwise-inline
/// containers.
///
/// \p FirstCol is the column at which the body's first character lands (right
/// after the `R"PS(` prefix); it drives overflow only for a top-level value that
/// stays on the opening line.
///
/// Returns the formatted body (first token unindented — it lands right after the
/// `R"PS(` prefix; subsequent lines carry absolute indentation), or std::nullopt
/// to signal "leave verbatim" (anything the lexer cannot confidently handle).
std::optional<std::string> formatCParseSpec(llvm::StringRef Content,
                                            unsigned FirstCol,
                                            unsigned BaseIndent,
                                            unsigned IndentWidth,
                                            unsigned ColumnLimit);

} // namespace format
} // namespace clang

#endif
