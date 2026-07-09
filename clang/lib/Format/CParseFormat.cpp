//===--- CParseFormat.cpp - Format Qumulo parse specs -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Deterministic formatter for Qumulo parse specs. See CParseFormat.h.
///
/// The body is lexed into a flat token stream with matched brackets, then a
/// recursive descent emits it. Layout is fully determined by structure (the
/// complexity budget, opener-attachment, trailing commas), so there is no
/// search. Anything the lexer cannot confidently handle returns std::nullopt.
///
//===----------------------------------------------------------------------===//

#include "CParseFormat.h"
#include "llvm/ADT/SmallVector.h"

#include <cctype>
#include <string>

using namespace llvm;

namespace clang {
namespace format {
namespace {

enum TokKind {
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  LParen,
  RParen,
  Comma,
  Colon,
  Equal,
  Atom,
  Comment
};

struct Tok {
  TokKind Kind;
  size_t Begin, End; // offsets into the body
  bool NlBefore;     // a newline preceded this token (vs same-line)
};

static bool isOpen(TokKind K) {
  return K == LBrace || K == LBracket || K == LParen;
}
static bool isClose(TokKind K) {
  return K == RBrace || K == RBracket || K == RParen;
}
static bool opensWith(TokKind Open, TokKind Close) {
  return (Open == LBrace && Close == RBrace) ||
         (Open == LBracket && Close == RBracket) ||
         (Open == LParen && Close == RParen);
}

// Lex the body. Returns false (bail) on an unterminated literal.
static bool lex(StringRef S, SmallVectorImpl<Tok> &Toks) {
  size_t N = S.size(), I = 0;
  bool Nl = true; // the first token behaves as if a newline preceded it
  auto isAtomChar = [&](size_t J) {
    char C = S[J];
    if (isspace((unsigned char)C))
      return false;
    if (strchr("{}[](),:=\"'", C))
      return false;
    if (C == '/' && J + 1 < N && S[J + 1] == '/')
      return false;
    return true;
  };
  auto push = [&](TokKind K, size_t B, size_t E) {
    Toks.push_back({K, B, E, Nl});
    Nl = false;
  };
  while (I < N) {
    char C = S[I];
    if (isspace((unsigned char)C)) {
      if (C == '\n')
        Nl = true;
      ++I;
      continue;
    }
    if (C == '/' && I + 1 < N && S[I + 1] == '/') {
      size_t J = I;
      while (J < N && S[J] != '\n')
        ++J;
      push(Comment, I, J);
      I = J;
      continue;
    }
    if (C == '"' || C == '\'') {
      size_t J = I + 1;
      while (J < N && S[J] != C) {
        if (S[J] == '\\' && J + 1 < N)
          ++J;
        ++J;
      }
      if (J >= N)
        return false; // unterminated literal
      ++J;            // closing quote
      push(Atom, I, J);
      I = J;
      continue;
    }
    TokKind K;
    switch (C) {
    case '{': K = LBrace; break;
    case '}': K = RBrace; break;
    case '[': K = LBracket; break;
    case ']': K = RBracket; break;
    case '(': K = LParen; break;
    case ')': K = RParen; break;
    case ',': K = Comma; break;
    case ':': K = Colon; break;
    case '=': K = Equal; break;
    default: K = Atom; break;
    }
    if (K != Atom) {
      push(K, I, I + 1);
      ++I;
      continue;
    }
    size_t J = I;
    while (J < N && isAtomChar(J))
      ++J;
    push(Atom, I, J);
    I = J;
  }
  return true;
}

// Pair brackets. Returns false (bail) on any imbalance/mismatch.
static bool match(const SmallVectorImpl<Tok> &Toks, SmallVectorImpl<int> &Match) {
  Match.assign(Toks.size(), -1);
  SmallVector<int, 16> Stack;
  for (int I = 0, E = Toks.size(); I < E; ++I) {
    if (isOpen(Toks[I].Kind)) {
      Stack.push_back(I);
    } else if (isClose(Toks[I].Kind)) {
      if (Stack.empty())
        return false;
      int O = Stack.pop_back_val();
      if (!opensWith(Toks[O].Kind, Toks[I].Kind))
        return false;
      Match[O] = I;
      Match[I] = O;
    }
  }
  return Stack.empty();
}

class Emitter {
public:
  Emitter(StringRef S, const SmallVectorImpl<Tok> &Toks,
          const SmallVectorImpl<int> &Match, unsigned IndentWidth,
          unsigned ColumnLimit)
      : S(S), Toks(Toks), Match(Match), IndentWidth(IndentWidth),
        ColumnLimit(ColumnLimit) {}

  std::string emitValue(int Lo, int Hi, unsigned LineIndent, unsigned Col) {
    int C = findTopLevel(Lo, Hi, Colon);
    if (C != -1) { // tag chain: TAG: rest
      std::string Prefix = collapse(Lo, C) + ": ";
      return Prefix +
             emitValue(C + 1, Hi, LineIndent, Col + Prefix.size());
    }
    if (singleContainer(Lo, Hi))
      return emitContainer(Lo, Hi, LineIndent, Col);
    return collapse(Lo, Hi);
  }

private:
  StringRef S;
  const SmallVectorImpl<Tok> &Toks;
  const SmallVectorImpl<int> &Match;
  unsigned IndentWidth, ColumnLimit;

  static std::string pad(unsigned N) { return std::string(N, ' '); }

  bool singleContainer(int Lo, int Hi) {
    return Lo < Hi && (Toks[Lo].Kind == LBrace ||
                       Toks[Lo].Kind == LBracket) &&
           Match[Lo] == Hi - 1;
  }

  // First top-level (bracket-depth 0) token of kind K in [Lo, Hi), or -1.
  int findTopLevel(int Lo, int Hi, TokKind K) {
    int Depth = 0;
    for (int I = Lo; I < Hi; ++I) {
      TokKind TK = Toks[I].Kind;
      if (isOpen(TK))
        ++Depth;
      else if (isClose(TK))
        --Depth;
      else if (Depth == 0 && TK == K)
        return I;
    }
    return -1;
  }

  // Does this container [Lo, Hi) lay out one entry per line? It expands iff it
  // directly holds a non-empty object, or an array that itself expands.
  bool expands(int Lo, int Hi) {
    for (int I = Lo + 1; I < Hi - 1;) {
      TokKind K = Toks[I].Kind;
      if (K == LBrace) {
        if (Match[I] > I + 1) // non-empty object
          return true;
        I = Match[I] + 1;
      } else if (K == LBracket) {
        if (expands(I, Match[I] + 1))
          return true;
        I = Match[I] + 1;
      } else if (K == LParen) {
        I = Match[I] + 1;
      } else {
        ++I;
      }
    }
    return false;
  }

  // Split a container interior [Lo, Hi) into entry ranges on top-level commas.
  SmallVector<std::pair<int, int>, 8> splitEntries(int Lo, int Hi) {
    SmallVector<std::pair<int, int>, 8> Out;
    int Depth = 0, Start = Lo;
    for (int I = Lo; I < Hi; ++I) {
      TokKind K = Toks[I].Kind;
      if (isOpen(K))
        ++Depth;
      else if (isClose(K))
        --Depth;
      else if (Depth == 0 && K == Comma) {
        if (I > Start)
          Out.push_back({Start, I});
        Start = I + 1;
      }
    }
    if (Start < Hi)
      Out.push_back({Start, Hi});
    return Out;
  }

  // The body text of [Lo, Hi) with whitespace runs collapsed to single spaces,
  // string/char literals preserved verbatim. Used for inline value-content.
  std::string collapse(int Lo, int Hi) {
    StringRef Raw = S.substr(Toks[Lo].Begin, Toks[Hi - 1].End - Toks[Lo].Begin);
    std::string Out;
    for (size_t I = 0, N = Raw.size(); I < N;) {
      char C = Raw[I];
      if (C == '"' || C == '\'') {
        Out += C;
        ++I;
        while (I < N && Raw[I] != C) {
          if (Raw[I] == '\\' && I + 1 < N) {
            Out += Raw[I];
            ++I;
          }
          Out += Raw[I];
          ++I;
        }
        if (I < N) {
          Out += Raw[I];
          ++I;
        }
      } else if (isspace((unsigned char)C)) {
        Out += ' ';
        while (I < N && isspace((unsigned char)Raw[I]))
          ++I;
      } else {
        Out += C;
        ++I;
      }
    }
    return Out;
  }

  // Fully inline render of a value range (no breaks, no indentation).
  std::string inlineValue(int Lo, int Hi) {
    int C = findTopLevel(Lo, Hi, Colon);
    if (C != -1)
      return collapse(Lo, C) + ": " + inlineValue(C + 1, Hi);
    if (singleContainer(Lo, Hi)) {
      char Open = Toks[Lo].Kind == LBrace ? '{' : '[';
      char Close = Toks[Lo].Kind == LBrace ? '}' : ']';
      auto Entries = splitEntries(Lo + 1, Hi - 1);
      if (Entries.empty())
        return std::string{Open, ' ', Close};
      std::string Out{Open, ' '};
      for (size_t I = 0; I < Entries.size(); ++I) {
        if (I)
          Out += ", ";
        Out += inlineEntry(Entries[I].first, Entries[I].second);
      }
      Out += ' ';
      Out += Close;
      return Out;
    }
    return collapse(Lo, Hi);
  }

  std::string inlineEntry(int Lo, int Hi) {
    if (Toks[Lo].Kind == Atom && S[Toks[Lo].Begin] == '.') {
      int E = findTopLevel(Lo, Hi, Equal);
      if (E != -1)
        return collapse(Lo, E) + " = " + inlineValue(E + 1, Hi);
    }
    int C = findTopLevel(Lo, Hi, Colon);
    if (C != -1)
      return inlineValue(Lo, C) + ": " + inlineValue(C + 1, Hi);
    return inlineValue(Lo, Hi);
  }

  // An entry whose value may break (designated field / map entry / positional).
  std::string emitEntry(int Lo, int Hi, unsigned LineIndent, unsigned Col) {
    if (Toks[Lo].Kind == Atom && S[Toks[Lo].Begin] == '.') {
      int E = findTopLevel(Lo, Hi, Equal);
      if (E != -1) {
        std::string Prefix = collapse(Lo, E) + " = ";
        return Prefix +
               emitValue(E + 1, Hi, LineIndent, Col + Prefix.size());
      }
    }
    int C = findTopLevel(Lo, Hi, Colon);
    if (C != -1) {
      std::string Prefix = inlineValue(Lo, C) + ": ";
      return Prefix + emitValue(C + 1, Hi, LineIndent, Col + Prefix.size());
    }
    return emitValue(Lo, Hi, LineIndent, Col);
  }

  // A container entry plus any comments bound to it. Lo == Hi means a
  // comment-only item (standalone comments before the closer).
  struct Item {
    SmallVector<int, 2> Leading; // comments on their own lines, before content
    int Lo, Hi;
    int Trailing; // comment index on the content's line, or -1
  };

  bool hasCommentDirect(int Lo, int Hi) {
    int Depth = 0;
    for (int I = Lo; I < Hi; ++I) {
      TokKind K = Toks[I].Kind;
      if (isOpen(K))
        ++Depth;
      else if (isClose(K))
        --Depth;
      else if (Depth == 0 && K == Comment)
        return true;
    }
    return false;
  }

  std::string commentText(int Idx) {
    return S.substr(Toks[Idx].Begin, Toks[Idx].End - Toks[Idx].Begin)
        .rtrim()
        .str();
  }

  // Split a container interior into items, binding comments. A same-line comment
  // trails the preceding entry; an own-line comment leads the next entry.
  SmallVector<Item, 8> buildItems(int Lo, int Hi) {
    SmallVector<Item, 8> Items;
    SmallVector<int, 2> Pending;
    int I = Lo;
    while (I < Hi) {
      TokKind K = Toks[I].Kind;
      if (K == Comment) {
        if (!Toks[I].NlBefore && !Items.empty() && Items.back().Trailing == -1 &&
            Items.back().Lo < Items.back().Hi) {
          Items.back().Trailing = I;
        } else {
          Pending.push_back(I);
        }
        ++I;
        continue;
      }
      if (K == Comma) {
        ++I;
        continue;
      }
      int Start = I, Depth = 0;
      while (I < Hi) {
        TokKind KK = Toks[I].Kind;
        if (isOpen(KK))
          ++Depth;
        else if (isClose(KK))
          --Depth;
        else if (Depth == 0 && (KK == Comma || KK == Comment))
          break;
        ++I;
      }
      Item It;
      It.Leading = Pending;
      It.Lo = Start;
      It.Hi = I;
      It.Trailing = -1;
      Pending.clear();
      if (I < Hi && Toks[I].Kind == Comment && !Toks[I].NlBefore) {
        It.Trailing = I;
        ++I;
      }
      Items.push_back(It);
    }
    if (!Pending.empty())
      Items.push_back({Pending, Hi, Hi, -1});
    return Items;
  }

  std::string emitContainer(int Lo, int Hi, unsigned LineIndent, unsigned Col) {
    char Open = Toks[Lo].Kind == LBrace ? '{' : '[';
    char Close = Toks[Lo].Kind == LBrace ? '}' : ']';
    bool IsArray = Toks[Lo].Kind == LBracket;
    bool HasComment = hasCommentDirect(Lo + 1, Hi - 1);
    auto Entries = splitEntries(Lo + 1, Hi - 1);
    if (Entries.empty() && !HasComment)
      return std::string{Open, ' ', Close};

    bool Expand = HasComment || expands(Lo, Hi);
    unsigned EntryIndent = LineIndent + IndentWidth;
    std::string Out(1, Open);

    if (!Expand) {
      std::string Inline = inlineValue(Lo, Hi);
      if (Col + Inline.size() <= ColumnLimit)
        return Inline;
      if (IsArray) {
        // Overflow fill-pack: scalars, multiple per line up to the limit.
        unsigned LineLen = 0;
        bool Started = false;
        for (auto &E : Entries) {
          std::string Es = inlineEntry(E.first, E.second);
          if (!Started) {
            Out += "\n" + pad(EntryIndent) + Es;
            LineLen = EntryIndent + Es.size();
            Started = true;
          } else if (LineLen + 2 + Es.size() <= ColumnLimit) {
            Out += ", " + Es;
            LineLen += 2 + Es.size();
          } else {
            Out += ",\n" + pad(EntryIndent) + Es;
            LineLen = EntryIndent + Es.size();
          }
        }
        Out += ",\n" + pad(LineIndent) + Close;
        return Out;
      }
      // Overflow object: fall through to one item per line.
    }

    for (const Item &It : buildItems(Lo + 1, Hi - 1)) {
      for (int C : It.Leading)
        Out += "\n" + pad(EntryIndent) + commentText(C);
      if (It.Lo < It.Hi) {
        Out += "\n" + pad(EntryIndent) +
               emitEntry(It.Lo, It.Hi, EntryIndent, EntryIndent) + ",";
        if (It.Trailing != -1)
          Out += " " + commentText(It.Trailing);
      }
    }
    Out += "\n" + pad(LineIndent) + Close;
    return Out;
  }
};

} // namespace

std::optional<std::string> formatCParseSpec(StringRef Content, unsigned FirstCol,
                                            unsigned BaseIndent,
                                            unsigned IndentWidth,
                                            unsigned ColumnLimit) {
  SmallVector<Tok, 64> Toks;
  if (!lex(Content, Toks))
    return std::nullopt;
  if (Toks.empty())
    return std::nullopt; // nothing to format; leave verbatim

  SmallVector<int, 64> M;
  if (!match(Toks, M))
    return std::nullopt;

  Emitter E(Content, Toks, M, IndentWidth, ColumnLimit);
  return E.emitValue(0, Toks.size(), BaseIndent, FirstCol);
}

} // namespace format
} // namespace clang
