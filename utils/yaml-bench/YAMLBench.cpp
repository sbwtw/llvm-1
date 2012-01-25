//===- YAMLBench - Benchmark the YAMLParser implementation ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This program executes the YAMLParser on differntly sized YAML texts and
// outputs the run time.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/SmallVector.h"

#include "llvm/Support/Allocator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"

#include <algorithm>
#include <deque>
#include <utility>

static llvm::cl::opt<bool>
  DumpTokens( "tokens"
            , llvm::cl::desc("Print the tokenization of the file.")
            , llvm::cl::init(false)
            );

static llvm::cl::opt<bool>
  DumpCanonical( "canonical"
               , llvm::cl::desc("Print the canonical YAML for this file.")
               , llvm::cl::init(false)
               );

static llvm::cl::opt<std::string>
  Input(llvm::cl::Positional, llvm::cl::desc("<input>"));

static llvm::cl::opt<bool>
  Verify( "verify"
        , llvm::cl::desc(
            "Run a quick verification useful for regression testing")
        , llvm::cl::init(false)
        );

namespace llvm {
namespace yaml {

enum BOMType {
  BT_UTF32_LE,
  BT_UTF32_BE,
  BT_UTF16_LE,
  BT_UTF16_BE,
  BT_UTF8,
  BT_Unknown
};

typedef std::pair<BOMType, unsigned> BOMInfo;

// Returns the BOM and how many bytes to skip.
BOMInfo getBOM(StringRef input) {
  if (input.size() == 0)
    return std::make_pair(BT_Unknown, 0);

  switch (uint8_t(input[0])) {
  case 0x00:
    if (input.size() >= 4) {
      if (  input[1] == 0
         && uint8_t(input[2]) == 0xFE
         && uint8_t(input[3]) == 0xFF)
        return std::make_pair(BT_UTF32_BE, 4);
      if (input[1] == 0 && input[2] == 0 && input[3] != 0)
        return std::make_pair(BT_UTF32_BE, 0);
    }

    if (input.size() >= 2 && input[1] != 0)
      return std::make_pair(BT_UTF16_BE, 0);
    return std::make_pair(BT_Unknown, 0);
  case 0xFF:
    if (  input.size() >= 4
       && uint8_t(input[1]) == 0xFE
       && input[2] == 0
       && input[3] == 0)
      return std::make_pair(BT_UTF32_LE, 4);

    if (input.size() >= 2 && uint8_t(input[1]) == 0xFE)
      return std::make_pair(BT_UTF16_LE, 2);
    return std::make_pair(BT_Unknown, 0);
  case 0xFE:
    if (input.size() >= 2 && uint8_t(input[1]) == 0xFF)
      return std::make_pair(BT_UTF16_BE, 2);
    return std::make_pair(BT_Unknown, 0);
  case 0xEF:
    if (  input.size() >= 3
       && uint8_t(input[1]) == 0xBB
       && uint8_t(input[2]) == 0xBF)
      return std::make_pair(BT_UTF8, 3);
    return std::make_pair(BT_Unknown, 0);
  }

  // It could still be utf-32 or utf-16.
  if (input.size() >= 4 && input[1] == 0 && input[2] == 0 && input[3] == 0)
    return std::make_pair(BT_UTF32_LE, 0);

  if (input.size() >= 2 && input[1] == 0)
    return std::make_pair(BT_UTF16_LE, 0);

  return std::make_pair(BT_UTF8, 0);
}

struct StreamStartInfo {
  BOMType BOM;
};

struct VersionDirectiveInfo {
  StringRef Value;
};

struct ScalarInfo {
  StringRef Value;
};

struct Token {
  enum TokenKind {
    TK_Error, // Uninitialized token.
    TK_StreamStart,
    TK_StreamEnd,
    TK_VersionDirective,
    TK_TagDirective,
    TK_DocumentStart,
    TK_DocumentEnd,
    TK_BlockEntry,
    TK_BlockEnd,
    TK_BlockSequenceStart,
    TK_BlockMappingStart,
    TK_FlowEntry,
    TK_FlowSequenceStart,
    TK_FlowSequenceEnd,
    TK_FlowMappingStart,
    TK_FlowMappingEnd,
    TK_Key,
    TK_Value,
    TK_Scalar,
    TK_Alias,
    TK_Anchor,
    TK_Tag
  } Kind;

  StringRef Range;

  union {
    StreamStartInfo StreamStart;
  };
  VersionDirectiveInfo VersionDirective;
  ScalarInfo Scalar;

  Token() : Kind(TK_Error) {}
};

struct SimpleKey {
  const Token *Tok;
  unsigned Column;
  unsigned Line;
  unsigned FlowLevel;
  bool IsRequired;

  bool operator <(const SimpleKey &Other) {
    return Tok <  Other.Tok;
  }

  bool operator ==(const SimpleKey &Other) {
    return Tok == Other.Tok;
  }
};

// Forbidding inlining improves performance by roughly 20%.
// FIXME: Remove once llvm optimizes this to the faster version without hints.
LLVM_ATTRIBUTE_NOINLINE static bool
wasEscaped(StringRef::iterator First, StringRef::iterator Position);

// Returns whether a character at 'Position' was escaped with a leading '\'.
// 'First' specifies the position of the first character in the string.
static bool wasEscaped(StringRef::iterator First,
                       StringRef::iterator Position) {
  assert(Position - 1 >= First);
  StringRef::iterator I = Position - 1;
  // We calulate the number of consecutive '\'s before the current position
  // by iterating backwards through our string.
  while (I >= First && *I == '\\') --I;
  // (Position - 1 - I) now contains the number of '\'s before the current
  // position. If it is odd, the character at 'Positon' was escaped.
  return (Position - 1 - I) % 2 == 1;
}

static bool is_ns_hex_digit(const char C) {
  if (  (C >= '0' && C <= '9')
     || (C >= 'a' && C <= 'z')
     || (C >= 'A' && C <= 'Z'))
    return true;
  return false;
}

static bool is_ns_word_char(const char C) {
  if (  C == '-'
     || (C >= 'a' && C <= 'z')
     || (C >= 'A' && C <= 'Z'))
    return true;
  return false;
}

class Scanner {
  SourceMgr *SM;
  MemoryBuffer *InputBuffer;
  StringRef::iterator Cur;
  StringRef::iterator End;
  int Indent; //< Current YAML indentation level in spaces.
  unsigned Column; //< Current column number in unicode code points.
  unsigned Line; //< Current line number.
  unsigned FlowLevel;
  unsigned BlockLevel;
  bool IsStartOfStream;
  bool IsSimpleKeyAllowed;
  bool IsSimpleKeyRequired;
  bool Failed;
  std::deque<Token> TokenQueue;
  SmallVector<int, 4> Indents;
  SmallVector<SimpleKey, 4> SimpleKeys;

  StringRef currentInput() {
    return StringRef(Cur, End - Cur);
  }

  bool isAtEnd(StringRef::iterator i = 0) {
    if (i)
      return i == End;
    return Cur == End;
  }

  typedef std::pair<uint32_t, unsigned> UTF8Decoded;

  UTF8Decoded decodeUTF8(StringRef::iterator Pos) {
    if ((uint8_t(*Pos) & 0x80) == 0)
      return std::make_pair(*Pos, 1);

    if (   (uint8_t(*Pos) & 0xE0) == 0xC0
        && Pos + 1 != End
        && uint8_t(*Pos) >= 0xC2
        && uint8_t(*Pos) <= 0xDF
        && uint8_t(*(Pos + 1)) >= 0x80
        && uint8_t(*(Pos + 1)) <= 0xBF) {
      uint32_t codepoint = uint8_t(*(Pos + 1)) & 0x3F;
      codepoint |= uint16_t(uint8_t(*Pos) & 0x1F) << 6;
      return std::make_pair(codepoint, 2);
    }

    if (   (uint8_t(*Pos) & 0xF0) == 0xE0
        && Pos + 2 < End) {
      if (   uint8_t(*Pos) == 0xE0
         && (  uint8_t(*(Pos + 1)) < 0xA0
            || uint8_t(*(Pos + 1)) > 0xBF));
      else if (  uint8_t(*Pos) >= 0xE1
              && uint8_t(*Pos) <= 0xEC
              && (  uint8_t(*(Pos + 1)) < 0x80
                 || uint8_t(*(Pos + 1)) > 0xBF));
      else if (  uint8_t(*Pos) == 0xED
              && (  uint8_t(*(Pos + 1)) < 0x80
                 || uint8_t(*(Pos + 1)) > 0x9F));
      else if (  uint8_t(*Pos) >= 0xEE
              && uint8_t(*Pos) <= 0xEF
              && (  uint8_t(*(Pos + 1)) < 0x80
                 || uint8_t(*(Pos + 1)) > 0xBF));
      else {
        if (uint8_t(*(Pos + 2)) >= 0x80 && uint8_t(*(Pos + 2)) <= 0xBF) {
          uint32_t codepoint = uint8_t(*(Pos + 2)) & 0x3F;
          codepoint |= uint16_t(uint8_t(*(Pos + 1)) & 0x3F) << 6;
          codepoint |= uint16_t(uint8_t(*Pos) & 0x0F) << 12;
          return std::make_pair(codepoint, 3);
        }
      }
    }

    if (   (uint8_t(*Pos) & 0xF8) == 0xF0
        && Pos + 3 < End) {
      if (  uint8_t(*Pos) == 0xF0
         && (  uint8_t(*(Pos + 1)) < 0x90
            || uint8_t(*(Pos + 1)) > 0xBF));
      else if (  uint8_t(*Pos) >= 0xF1
              && uint8_t(*Pos) <= 0xF3
              && (  uint8_t(*(Pos + 1)) < 0x80
                 || uint8_t(*(Pos + 1)) > 0xBF));
      else if (  uint8_t(*Pos) == 0xF4
              && (  uint8_t(*(Pos + 1)) < 0x80
                 || uint8_t(*(Pos + 1)) > 0x8F));
      else {
        if (   uint8_t(*(Pos + 2)) >= 0x80 && uint8_t(*(Pos + 2)) <= 0xBF
            && uint8_t(*(Pos + 3)) >= 0x80 && uint8_t(*(Pos + 3)) <= 0xBF) {
          uint32_t codepoint = uint8_t(*(Pos + 3)) & 0x3F;
          codepoint |= uint32_t(uint8_t(*(Pos + 2)) & 0x3F) << 6;
          codepoint |= uint32_t(uint8_t(*(Pos + 1)) & 0x3F) << 12;
          codepoint |= uint32_t(uint8_t(*Pos) & 0x07) << 18;
          return std::make_pair(codepoint, 4);
        }
      }
    }

    // Not valid utf-8.
    setError("Invalid utf8 code unit", Pos);
    return std::make_pair(0, 0);
  }

  // Skip valid printable characters.
  StringRef::iterator skip_nb_char(StringRef::iterator Pos) {
    // Check 7 bit c-printable - b-char.
    if (   *Pos == 0x09
        || (*Pos >= 0x20 && *Pos <= 0x7E))
      return Pos + 1;

    // Check for valid utf-8.
    if (uint8_t(*Pos) & 0x80) {
      UTF8Decoded u8d = decodeUTF8(Pos);
      if (   u8d.second != 0
          && u8d.first != 0xFEFF
          && ( u8d.first == 0x85
            || ( u8d.first >= 0xA0
              && u8d.first <= 0xD7FF)
            || ( u8d.first >= 0xE000
              && u8d.first <= 0xFFFD)
            || ( u8d.first >= 0x10000
              && u8d.first <= 0x10FFFF)))
        return Pos + u8d.second;
    }
    return Pos;
  }

  StringRef::iterator skip_b_char(StringRef::iterator Pos) {
    if (*Pos == 0x0D) {
      if (Pos + 1 != End && *(Pos + 1) == 0x0A)
        return Pos + 2;
      return Pos + 1;
    }

    if (*Pos == 0x0A)
      return Pos + 1;
    return Pos;
  }

  StringRef::iterator skip_s_white(StringRef::iterator Pos) {
    if (Pos == End)
      return Pos;
    if (*Pos == ' ' || *Pos == '\t')
      return Pos + 1;
    return Pos;
  }

  StringRef::iterator skip_ns_char(StringRef::iterator Pos) {
    if (Pos == End)
      return Pos;
    if (*Pos == ' ' || *Pos == '\t')
      return Pos;
    return skip_nb_char(Pos);
  }

  StringRef::iterator skip_nb_json(StringRef::iterator Pos) {
    if (Pos == End)
      return Pos;
    if (*Pos == 0x09 || (*Pos >= 0x20 && uint8_t(*Pos) <= 0x7E))
      return Pos + 1;
    UTF8Decoded u8d = decodeUTF8(Pos);
    if (u8d.second != 0 && u8d.first >= 0x80 && u8d.first <= 0x10FFFF)
      return Pos + u8d.second;
    return Pos;
  }

  template<StringRef::iterator (Scanner::*Func)(StringRef::iterator)>
  StringRef::iterator skip_while(StringRef::iterator Pos) {
    while (true) {
      StringRef::iterator i = (this->*Func)(Pos);
      if (i == Pos)
        break;
      Pos = i;
    }
    return Pos;
  }

  StringRef scan_ns_uri_char() {
    StringRef::iterator Start = Cur;
    while (true) {
      if (Cur == End)
        break;
      if ((   *Cur == '%'
           && Cur + 2 < End
           && is_ns_hex_digit(*(Cur + 1))
           && is_ns_hex_digit(*(Cur + 2)))
         || is_ns_word_char(*Cur)
         || StringRef(Cur, 1).find_first_of("#;/?:@&=+$,_.!~*'()[]")
            != StringRef::npos) {
        ++Cur;
        ++Column;
      } else
        break;
    }
    return StringRef(Start, Cur - Start);
  }

  StringRef scan_ns_plain_one_line() {
    StringRef::iterator start = Cur;
    // The first character must already be verified.
    ++Cur;
    while (true) {
      if (Cur == End) {
        break;
      } else if (*Cur == ':') {
        // Check if the next character is a ns-char.
        if (Cur + 1 == End)
          break;
        StringRef::iterator i = skip_ns_char(Cur + 1);
        if (Cur + 1 != i) {
          Cur = i;
          Column += 2; // Consume both the ':' and ns-char.
        } else
          break;
      } else if (*Cur == '#') {
        // Check if the previous character was a ns-char.
        // The & 0x80 check is to check for the trailing byte of a utf-8
        if (*(Cur - 1) & 0x80 || skip_ns_char(Cur - 1) == Cur) {
          ++Cur;
          ++Column;
        } else
          break;
      } else {
        StringRef::iterator i = skip_nb_char(Cur);
        if (i == Cur)
          break;
        Cur = i;
        ++Column;
      }
    }
    return StringRef(start, Cur - start);
  }

  bool consume(uint32_t expected) {
    if (expected >= 0x80)
      report_fatal_error("Not dealing with this yet");
    if (Cur == End)
      return false;
    if (uint8_t(*Cur) >= 0x80)
      report_fatal_error("Not dealing with this yet");
    if (uint8_t(*Cur) == expected) {
      ++Cur;
      ++Column;
      return true;
    }
    return false;
  }

  void skip(uint32_t Distance) {
    Cur += Distance;
    Column += Distance;
  }

  // Skip whitespace while keeping track of line number and column.
  void scanToNextToken() {
    while (true) {
      while (*Cur == ' ') {
        skip(1);
      }

      // Skip comment.
      if (*Cur == '#') {
        while (true) {
          // This may skip more than one byte, thus Column is only incremented
          // for code points.
          StringRef::iterator i = skip_nb_char(Cur);
          if (i == Cur)
            break;
          Cur = i;
          ++Column;
        }
      }

      // Skip EOL.
      StringRef::iterator i = skip_b_char(Cur);
      if (i == Cur)
        break;
      Cur = i;
      ++Line;
      Column = 0;
      // New lines may start a simple key.
      if (!FlowLevel)
        IsSimpleKeyAllowed = true;
    }
  }

  bool isBlankOrBreak(StringRef::iterator Pos) {
    if (Pos == End)
      return false;
    if (*Pos == ' ' || *Pos == '\t' || *Pos == '\r' || *Pos == '\n')
      return true;
    return false;
  }

  void removeStaleSimpleKeys() {
    for (SmallVectorImpl<SimpleKey>::iterator i = SimpleKeys.begin();
                                              i != SimpleKeys.end();) {
      if (i->Line != Line || i->Column + 1024 < Column) {
        if (i->IsRequired)
          setError( "Could not find expected : for simple key"
                  , i->Tok->Range.begin());
        i = SimpleKeys.erase(i);
      } else
        ++i;
    }
  }

  void removeSimpleKeyOnFlowLevel(unsigned Level) {
    if (!SimpleKeys.empty() && (SimpleKeys.end() - 1)->FlowLevel == Level)
      SimpleKeys.pop_back();
  }

  bool scanStreamStart() {
    IsStartOfStream = false;

    BOMInfo BI = getBOM(currentInput());
    Cur += BI.second;

    Token t;
    t.Kind = Token::TK_StreamStart;
    t.StreamStart.BOM = BI.first;
    TokenQueue.push_back(t);
    return true;
  }

  bool scanStreamEnd() {
    // Force an ending new line if one isn't present.
    if (Column != 0) {
      Column = 0;
      ++Line;
    }

    unrollIndent(-1);
    SimpleKeys.clear();
    IsSimpleKeyAllowed = false;

    Token t;
    t.Kind = Token::TK_StreamEnd;
    t.Range = StringRef(Cur, 0);
    TokenQueue.push_back(t);
    return true;
  }

  bool unrollIndent(int Col) {
    Token t;
    // Indentation is ignored in flow.
    if (FlowLevel != 0)
      return true;

    while (Indent > Col) {
      t.Kind = Token::TK_BlockEnd;
      t.Range = StringRef(Cur, 1);
      TokenQueue.push_back(t);
      Indent = Indents.pop_back_val();
    }

    return true;
  }

  bool rollIndent( int Col
                 , Token::TokenKind Kind
                 , std::deque<Token>::iterator InsertPoint) {
    if (FlowLevel)
      return true;
    if (Indent < Col) {
      Indents.push_back(Indent);
      Indent = Col;

      Token t;
      t.Kind = Kind;
      t.Range = StringRef(Cur, 0);
      TokenQueue.insert(InsertPoint, t);
    }
    return true;
  }

  bool scanDirective() {
    // Reset the indentation level.
    unrollIndent(-1);
    SimpleKeys.clear();
    IsSimpleKeyAllowed = false;

    StringRef::iterator Start = Cur;
    consume('%');
    StringRef::iterator NameStart = Cur;
    Cur = skip_while<&Scanner::skip_ns_char>(Cur);
    StringRef Name(NameStart, Cur - NameStart);
    Cur = skip_while<&Scanner::skip_s_white>(Cur);

    if (Name == "YAML") {
      StringRef::iterator VersionStart = Cur;
      Cur = skip_while<&Scanner::skip_ns_char>(Cur);
      StringRef Version(VersionStart, Cur - VersionStart);
      Token t;
      t.Kind = Token::TK_VersionDirective;
      t.Range = StringRef(Start, Cur - Start);
      t.VersionDirective.Value = Version;
      TokenQueue.push_back(t);
      return true;
    }
    return false;
  }

  bool scanDocumentIndicator(bool IsStart) {
    unrollIndent(-1);
    SimpleKeys.clear();
    IsSimpleKeyAllowed = false;

    Token t;
    t.Kind = IsStart ? Token::TK_DocumentStart : Token::TK_DocumentEnd;
    t.Range = StringRef(Cur, 3);
    skip(3);
    TokenQueue.push_back(t);
    return true;
  }

  bool scanFlowCollectionStart(bool IsSequence) {
    Token t;
    t.Kind = IsSequence ? Token::TK_FlowSequenceStart
                        : Token::TK_FlowMappingStart;
    t.Range = StringRef(Cur, 1);
    skip(1);
    TokenQueue.push_back(t);

    // [ and { may begin a simple key.
    if (IsSimpleKeyAllowed) {
      SimpleKey SK;
      SK.Tok = &TokenQueue.back();
      SK.Line = Line;
      SK.Column = Column - 1;
      SK.IsRequired = false;
      SK.FlowLevel = FlowLevel;
      SimpleKeys.push_back(SK);
    }

    // And may also be followed by a simple key.
    IsSimpleKeyAllowed = true;
    ++FlowLevel;
    return true;
  }

  bool scanFlowCollectionEnd(bool IsSequence) {
    removeSimpleKeyOnFlowLevel(FlowLevel);
    IsSimpleKeyAllowed = false;
    Token t;
    t.Kind = IsSequence ? Token::TK_FlowSequenceEnd
                        : Token::TK_FlowMappingEnd;
    t.Range = StringRef(Cur, 1);
    skip(1);
    TokenQueue.push_back(t);
    if (FlowLevel)
      --FlowLevel;
    return true;
  }

  bool scanFlowEntry() {
    removeSimpleKeyOnFlowLevel(FlowLevel);
    IsSimpleKeyAllowed = true;
    Token t;
    t.Kind = Token::TK_FlowEntry;
    t.Range = StringRef(Cur, 1);
    skip(1);
    TokenQueue.push_back(t);
    return true;
  }

  bool scanBlockEntry() {
    rollIndent(Column, Token::TK_BlockSequenceStart, TokenQueue.end());
    removeSimpleKeyOnFlowLevel(FlowLevel);
    IsSimpleKeyAllowed = true;
    Token t;
    t.Kind = Token::TK_BlockEntry;
    t.Range = StringRef(Cur, 1);
    skip(1);
    TokenQueue.push_back(t);
    return true;
  }

  bool scanKey() {
    if (!FlowLevel)
      rollIndent(Column, Token::TK_BlockMappingStart, TokenQueue.end());

    removeSimpleKeyOnFlowLevel(FlowLevel);
    IsSimpleKeyAllowed = !FlowLevel;

    Token t;
    t.Kind = Token::TK_Key;
    t.Range = StringRef(Cur, 1);
    skip(1);
    TokenQueue.push_back(t);
    return true;
  }

  bool scanValue() {
    // If the previous token could have been a simple key, insert the key token
    // into the token queue.
    if (!SimpleKeys.empty()) {
      SimpleKey SK = SimpleKeys.pop_back_val();
      Token t;
      t.Kind = Token::TK_Key;
      t.Range = SK.Tok->Range;
      std::deque<Token>::iterator i, e;
      for (i = TokenQueue.begin(), e = TokenQueue.end(); i != e; ++i) {
        if (&(*i) == SK.Tok)
          break;
      }
      assert(i != e && "SimpleKey not in token queue!");
      i = TokenQueue.insert(i, t);

      // We may also need to add a Block-Mapping-Start token.
      rollIndent(SK.Column, Token::TK_BlockMappingStart, i);

      // FIXME: This clear is here because the above invalidates all the
      //        deque<Token>::iterators.
      SimpleKeys.clear();
      IsSimpleKeyAllowed = false;
    } else {
      if (!FlowLevel)
        rollIndent(Column, Token::TK_BlockMappingStart, TokenQueue.end());
      IsSimpleKeyAllowed = !FlowLevel;
    }

    Token t;
    t.Kind = Token::TK_Value;
    t.Range = StringRef(Cur, 1);
    skip(1);
    TokenQueue.push_back(t);
    return true;
  }

  bool scanFlowScalar(bool IsDoubleQuoted) {
    StringRef::iterator Start = Cur;
    unsigned ColStart = Column;
    if (IsDoubleQuoted) {
      do {
        ++Cur;
        while (Cur != End && *Cur != '"')
          ++Cur;
        // Repeat until the previous character was not a '\' or was an escaped
        // backslash.
      } while (*(Cur - 1) == '\\' && wasEscaped(Start + 1, Cur));
    } else {
      skip(1);
      while (true) {
        // Skip a ' followed by another '.
        if (Cur + 1 < End && *Cur == '\'' && *(Cur + 1) == '\'') {
          skip(2);
          continue;
        } else if (*Cur == '\'')
          break;
        StringRef::iterator i = skip_nb_char(Cur);
        if (i == Cur) {
          i = skip_b_char(Cur);
          if (i == Cur)
            break;
          Cur = i;
          Column = 0;
          ++Line;
        } else {
          if (i == End)
            break;
          Cur = i;
          ++Column;
        }
      }
    }
    StringRef Value(Start + 1, Cur - (Start + 1));
    skip(1); // Skip ending quote.
    Token t;
    t.Kind = Token::TK_Scalar;
    t.Range = StringRef(Start, Cur - Start);
    t.Scalar.Value = Value;
    TokenQueue.push_back(t);

    if (IsSimpleKeyAllowed) {
      SimpleKey SK;
      SK.Tok = &TokenQueue.back();
      SK.Line = Line;
      SK.Column = ColStart;
      SK.IsRequired = false;
      SK.FlowLevel = FlowLevel;
      SimpleKeys.push_back(SK);
    }

    IsSimpleKeyAllowed = false;

    return true;
  }

  bool scanPlainScalar() {
    StringRef::iterator Start = Cur;
    unsigned ColStart = Column;
    unsigned LeadingBlanks = 0;
    assert(Indent >= -1 && "Indent must be >= -1 !");
    unsigned indent = static_cast<unsigned>(Indent + 1);
    while (true) {
      if (*Cur == '#')
        break;

      while (!isBlankOrBreak(Cur)) {
        if (  FlowLevel && *Cur == ':'
           && !(isBlankOrBreak(Cur + 1) || *(Cur + 1) == ',')) {
          setError("Found unexpected ':' while scanning a plain scalar", Cur);
          return false;
        }

        // Check for the end of the plain scalar.
        if (  (*Cur == ':' && isBlankOrBreak(Cur + 1))
           || (  FlowLevel
           && (StringRef(Cur, 1).find_first_of(",:?[]{}") != StringRef::npos)))
          break;

        StringRef::iterator i = skip_nb_char(Cur);
        if (i == Cur)
          break;
        Cur = i;
        ++Column;
      }

      // Are we at the end?
      if (!isBlankOrBreak(Cur))
        break;

      // Eat blanks.
      StringRef::iterator Tmp = Cur;
      while (isBlankOrBreak(Tmp)) {
        StringRef::iterator i = skip_s_white(Tmp);
        if (i != Tmp) {
          if (LeadingBlanks && (Column < indent) && *Tmp == '\t') {
            setError("Found invalid tab character in indentation", Tmp);
            return false;
          }
          Tmp = i;
          ++Column;
        } else {
          i = skip_b_char(Tmp);
          if (!LeadingBlanks)
            LeadingBlanks = 1;
          Tmp = i;
          Column = 0;
          ++Line;
        }
      }

      if (!FlowLevel && Column < indent)
        break;

      Cur = Tmp;
    }
    if (Start == Cur) {
      setError("Got empty plain scalar", Start);
      return false;
    }
    Token t;
    t.Kind = Token::TK_Scalar;
    t.Range = StringRef(Start, Cur - Start);
    t.Scalar.Value = t.Range;
    TokenQueue.push_back(t);

    // Plain scalars can be simple keys.
    if (IsSimpleKeyAllowed) {
      SimpleKey SK;
      SK.Tok = &TokenQueue.back();
      SK.Line = Line;
      SK.Column = ColStart;
      SK.IsRequired = false;
      SK.FlowLevel = FlowLevel;
      SimpleKeys.push_back(SK);
    }

    IsSimpleKeyAllowed = false;

    return true;
  }

  bool scanAliasOrAnchor(bool IsAlias) {
    StringRef::iterator Start = Cur;
    unsigned ColStart = Column;
    skip(1);
    while(true) {
      if (   *Cur == '[' || *Cur == ']'
          || *Cur == '{' || *Cur == '}'
          || *Cur == ','
          || *Cur == ':')
        break;
      StringRef::iterator i = skip_ns_char(Cur);
      if (i == Cur)
        break;
      Cur = i;
      ++Column;
    }

    if (Start == Cur) {
      setError("Got empty alias or anchor", Start);
      return false;
    }

    Token t;
    t.Kind = IsAlias ? Token::TK_Alias : Token::TK_Anchor;
    t.Range = StringRef(Start, Cur - Start);
    t.Scalar.Value = t.Range.substr(1);
    TokenQueue.push_back(t);

    // Alias and anchors can be simple keys.
    if (IsSimpleKeyAllowed) {
      SimpleKey SK;
      SK.Tok = &TokenQueue.back();
      SK.Line = Line;
      SK.Column = ColStart;
      SK.IsRequired = false;
      SK.FlowLevel = FlowLevel;
      SimpleKeys.push_back(SK);
    }

    IsSimpleKeyAllowed = false;

    return true;
  }

  bool scanBlockScalar(bool IsLiteral) {
    StringRef::iterator Start = Cur;
    skip(1); // Eat | or >
    while(true) {
      StringRef::iterator i = skip_nb_char(Cur);
      if (i == Cur) {
        if (Column == 0)
          break;
        i = skip_b_char(Cur);
        if (i != Cur) {
          // We got a line break.
          Column = 0;
          ++Line;
          Cur = i;
          continue;
        } else {
          // There was an error, which should already have been printed out.
          return false;
        }
      }
      Cur = i;
      ++Column;
    }

    if (Start == Cur) {
      setError("Got empty block scalar", Start);
      return false;
    }

    Token t;
    t.Kind = Token::TK_Scalar;
    t.Range = StringRef(Start, Cur - Start);
    t.Scalar.Value = t.Range;
    TokenQueue.push_back(t);
    return true;
  }

  bool scanTag() {
    StringRef::iterator Start = Cur;
    unsigned ColStart = Column;
    skip(1); // Eat !.
    if (Cur == End || isBlankOrBreak(Cur)); // An empty tag.
    else if (*Cur == '<') {
      skip(1);
      StringRef VerbatiumTag = scan_ns_uri_char();
      if (!consume('>'))
        return false;
    } else {
      // FIXME: Actually parse the c-ns-shorthand-tag rule.
      Cur = skip_while<&Scanner::skip_ns_char>(Cur);
    }

    Token t;
    t.Kind = Token::TK_Tag;
    t.Range = StringRef(Start, Cur - Start);
    TokenQueue.push_back(t);

    // Tags can be simple keys.
    if (IsSimpleKeyAllowed) {
      SimpleKey SK;
      SK.Tok = &TokenQueue.back();
      SK.Line = Line;
      SK.Column = ColStart;
      SK.IsRequired = false;
      SK.FlowLevel = FlowLevel;
      SimpleKeys.push_back(SK);
    }

    IsSimpleKeyAllowed = false;

    return true;
  }

  bool fetchMoreTokens() {
    if (IsStartOfStream)
      return scanStreamStart();

    scanToNextToken();

    if (Cur == End)
      return scanStreamEnd();

    removeStaleSimpleKeys();

    unrollIndent(Column);

    if (Column == 0 && *Cur == '%')
      return scanDirective();

    if (Column == 0 && Cur + 4 <= End
        && *Cur == '-'
        && *(Cur + 1) == '-'
        && *(Cur + 2) == '-'
        && (Cur + 3 == End || isBlankOrBreak(Cur + 3)))
      return scanDocumentIndicator(true);

    if (Column == 0 && Cur + 4 <= End
        && *Cur == '.'
        && *(Cur + 1) == '.'
        && *(Cur + 2) == '.'
        && (Cur + 3 == End || isBlankOrBreak(Cur + 3)))
      return scanDocumentIndicator(false);

    if (*Cur == '[')
      return scanFlowCollectionStart(true);

    if (*Cur == '{')
      return scanFlowCollectionStart(false);

    if (*Cur == ']')
      return scanFlowCollectionEnd(true);

    if (*Cur == '}')
      return scanFlowCollectionEnd(false);

    if (*Cur == ',')
      return scanFlowEntry();

    if (*Cur == '-' && isBlankOrBreak(Cur + 1))
      return scanBlockEntry();

    if (*Cur == '?' && (FlowLevel || isBlankOrBreak(Cur + 1)))
      return scanKey();

    if (*Cur == ':' && (FlowLevel || isBlankOrBreak(Cur + 1)))
      return scanValue();

    if (*Cur == '*')
      return scanAliasOrAnchor(true);

    if (*Cur == '&')
      return scanAliasOrAnchor(false);

    if (*Cur == '!')
      return scanTag();

    if (*Cur == '|' && !FlowLevel)
      return scanBlockScalar(true);

    if (*Cur == '>' && !FlowLevel)
      return scanBlockScalar(false);

    if (*Cur == '\'')
      return scanFlowScalar(false);

    if (*Cur == '"')
      return scanFlowScalar(true);

    // Get a plain scalar.
    StringRef FirstChar(Cur, 1);
    if (!(isBlankOrBreak(Cur)
          || FirstChar.find_first_of("-?:,[]{}#&*!|>'\"%@`") != StringRef::npos)
        || (*Cur == '-' && !isBlankOrBreak(Cur + 1))
        || (!FlowLevel && (*Cur == '?' || *Cur == ':')
            && isBlankOrBreak(Cur + 1))
        || (!FlowLevel && *Cur == ':'
                       && Cur + 2 < End
                       && *(Cur + 1) == ':'
                       && !isBlankOrBreak(Cur + 2)))
      return scanPlainScalar();

    setError("Unrecognized character while tokenizing.");
    return false;
  }

public:
  Scanner(StringRef input, SourceMgr *sm)
    : SM(sm)
    , Indent(-1)
    , Column(0)
    , Line(0)
    , FlowLevel(0)
    , BlockLevel(0)
    , IsStartOfStream(true)
    , IsSimpleKeyAllowed(true)
    , IsSimpleKeyRequired(false)
    , Failed(false) {
    InputBuffer = MemoryBuffer::getMemBuffer(input, "YAML");
    SM->AddNewSourceBuffer(InputBuffer, SMLoc());
    Cur = InputBuffer->getBufferStart();
    End = InputBuffer->getBufferEnd();
  }

  Token &peekNext() {
    // If the current token is a possible simple key, keep parsing until we
    // can confirm.
    bool NeedMore = false;
    while (true) {
      if (TokenQueue.empty() || NeedMore) {
        if (!fetchMoreTokens()) {
          TokenQueue.clear();
          TokenQueue.push_back(Token());
          return TokenQueue.front();
        }
      }
      assert(!TokenQueue.empty() &&
             "fetchMoreTokens lied about getting tokens!");

      removeStaleSimpleKeys();
      SimpleKey SK;
      SK.Tok = &TokenQueue.front();
      if (std::find(SimpleKeys.begin(), SimpleKeys.end(), SK)
          == SimpleKeys.end())
        break;
      else
        NeedMore = true;
    }
    return TokenQueue.front();
  }

  Token getNext() {
    Token ret = peekNext();
    // TokenQueue can be empty if there was an error getting the next token.
    if (!TokenQueue.empty())
      TokenQueue.pop_front();
    return ret;
  }

  void printError(SMLoc Loc, SourceMgr::DiagKind Kind, const Twine &Msg,
                  ArrayRef<SMRange> Ranges = ArrayRef<SMRange>()) {
    SM->PrintMessage(Loc, Kind, Msg, Ranges);
  }

  void setError(const Twine &Msg, StringRef::iterator Pos) {
    // Don't print out more errors after the first one we encounter. The rest
    // are just the result of the first, and have no meaning.
    if (!Failed)
      printError(SMLoc::getFromPointer(Pos), SourceMgr::DK_Error, Msg);
    Failed = true;
  }

  void setError(const Twine &Msg) {
    setError(Msg, Cur);
  }

  bool failed() {
    return Failed;
  }
};

class document_iterator;
class Document;

class Stream {
  Scanner S;
  OwningPtr<Document> CurrentDoc;

  friend class Document;

  void handleYAMLDirective(const Token &t) {

  }

public:
  Stream(StringRef input, SourceMgr *sm) : S(input, sm) {}

  document_iterator begin();
  document_iterator end();
  void skip();
};

class Node {
  unsigned int TypeID;
  StringRef Anchor;

protected:
  Document *Doc;

public:
  enum NodeKind {
    NK_Null,
    NK_Scalar,
    NK_KeyValue,
    NK_Mapping,
    NK_Sequence,
    NK_Alias
  };

  Node(unsigned int Type, Document *D, StringRef A)
    : TypeID(Type)
    , Anchor(A)
    , Doc(D)
  {}

  StringRef getAnchor() const { return Anchor; }

  unsigned int getType() const { return TypeID; }
  static inline bool classof(const Node *) { return true; }

  Token &peekNext();
  Token getNext();
  Node *parseBlockNode();
  BumpPtrAllocator &getAllocator();
  void setError(const Twine &Msg, Token &Tok);
  bool failed() const;

  virtual void skip() {}
};

class NullNode : public Node {
public:
  NullNode(Document *D) : Node(NK_Null, D, StringRef()) {}

  static inline bool classof(const NullNode *) { return true; }
  static inline bool classof(const Node *n) {
    return n->getType() == NK_Null;
  }
};

class ScalarNode : public Node {
  StringRef Value;

public:
  ScalarNode(Document *D, StringRef Anchor, StringRef Val)
    : Node(NK_Scalar, D, Anchor)
    , Value(Val)
  {}

  // Return Value without any escaping or folding or other fun YAML stuff. This
  // is the exact bytes that are contained in the file (after converstion to
  // utf8).
  StringRef getRawValue() const { return Value; }

  static inline bool classof(const ScalarNode *) { return true; }
  static inline bool classof(const Node *n) {
    return n->getType() == NK_Scalar;
  }
};

class KeyValueNode : public Node {
  Node *Key;
  Node *Value;

public:
  KeyValueNode(Document *D)
    : Node(NK_KeyValue, D, StringRef())
    , Key(0)
    , Value(0)
  {}

  static inline bool classof(const KeyValueNode *) { return true; }
  static inline bool classof(const Node *n) {
    return n->getType() == NK_KeyValue;
  }

  Node *getKey() {
    if (Key)
      return Key;
    // Handle implicit null keys.
    {
      Token &t = peekNext();
      if (   t.Kind == Token::TK_BlockEnd
          || t.Kind == Token::TK_Value
          || t.Kind == Token::TK_Error) {
        return Key = new (getAllocator().Allocate<NullNode>()) NullNode(Doc);
      }
      if (t.Kind == Token::TK_Key)
        getNext(); // skip TK_Key.
    }

    // Handle explicit null keys.
    Token &t = peekNext();
    if (t.Kind == Token::TK_BlockEnd || t.Kind == Token::TK_Value) {
      return Key = new (getAllocator().Allocate<NullNode>()) NullNode(Doc);
    }

    // We've got a normal key.
    return Key = parseBlockNode();
  }

  Node *getValue() {
    if (Value)
      return Value;
    getKey()->skip();
    if (failed())
      return Value = new (getAllocator().Allocate<NullNode>()) NullNode(Doc);

    // Handle implicit null values.
    {
      Token &t = peekNext();
      if (   t.Kind == Token::TK_BlockEnd
          || t.Kind == Token::TK_FlowMappingEnd
          || t.Kind == Token::TK_Key
          || t.Kind == Token::TK_FlowEntry
          || t.Kind == Token::TK_Error) {
        return Value = new (getAllocator().Allocate<NullNode>()) NullNode(Doc);
      }

      if (t.Kind != Token::TK_Value) {
        setError("Unexpected token in Key Value.", t);
        return Value = new (getAllocator().Allocate<NullNode>()) NullNode(Doc);
      }
      getNext(); // skip TK_Value.
    }

    // Handle explicit null values.
    Token &t = peekNext();
    if (t.Kind == Token::TK_BlockEnd || t.Kind == Token::TK_Key) {
      return Value = new (getAllocator().Allocate<NullNode>()) NullNode(Doc);
    }

    // We got a normal value.
    return Value = parseBlockNode();
  }

  virtual void skip() {
    getKey()->skip();
    getValue()->skip();
  }
};

class MappingNode : public Node {
public:
  enum Type {
    MT_Block,
    MT_Flow,
    MT_Inline //< An inline mapping node is used for "[key: value]".
  };

private:
  Type MType;
  bool IsAtBeginning;
  bool IsAtEnd;

public:
  MappingNode(Document *D, StringRef Anchor, Type T)
    : Node(NK_Mapping, D, Anchor)
    , MType(T)
    , IsAtBeginning(true)
    , IsAtEnd(false)
  {}

  static inline bool classof(const MappingNode *) { return true; }
  static inline bool classof(const Node *n) {
    return n->getType() == NK_Mapping;
  }

  class iterator {
    MappingNode *MN;
    KeyValueNode *CurrentEntry;

  public:
    iterator() : MN(0), CurrentEntry(0) {}
    iterator(MappingNode *mn) : MN(mn), CurrentEntry(0) {}

    KeyValueNode *operator ->() const {
      assert(CurrentEntry && "Attempted to access end iterator!");
      return CurrentEntry;
    }

    KeyValueNode &operator *() const {
      assert(CurrentEntry && "Attempted to dereference end iterator!");
      return *CurrentEntry;
    }

    operator KeyValueNode*() const {
      assert(CurrentEntry && "Attempted to access end iterator!");
      return CurrentEntry;
    }

    bool operator !=(const iterator &Other) const {
      return MN != Other.MN;
    }

    iterator &operator++() {
      assert(MN && "Attempted to advance iterator past end!");
      if (MN->failed()) {
        MN->IsAtEnd = true;
        MN = 0;
        CurrentEntry = 0;
        return *this;
      }
      if (CurrentEntry) {
        CurrentEntry->skip();
        if (MN->MType == MT_Inline) {
          MN->IsAtEnd = true;
          MN = 0;
          CurrentEntry = 0;
          return *this;
        }
      }
      Token t = MN->peekNext();
      if (t.Kind == Token::TK_Key || t.Kind == Token::TK_Scalar) {
        // KeyValueNode eats the TK_Key. That way it can detect null keys.
        CurrentEntry = new (MN->getAllocator().Allocate<KeyValueNode>())
          KeyValueNode(MN->Doc);
      } else if (MN->MType == MT_Block) {
        switch (t.Kind) {
        case Token::TK_BlockEnd:
          MN->getNext();
          MN->IsAtEnd = true;
          MN = 0;
          CurrentEntry = 0;
          break;
        default:
          MN->setError("Unexpected token. Expected Key or Block End", t);
        case Token::TK_Error:
          MN->IsAtEnd = true;
          MN = 0;
          CurrentEntry = 0;
        }
      } else {
        switch (t.Kind) {
        case Token::TK_FlowEntry:
          // Eat the flow entry and recurse.
          MN->getNext();
          return ++(*this);
        case Token::TK_FlowMappingEnd:
          MN->getNext();
        case Token::TK_Error:
          // Set this to end iterator.
          MN->IsAtEnd = true;
          MN = 0;
          CurrentEntry = 0;
          break;
        default:
          MN->setError( "Unexpected token. Expected Key, Flow Entry, or Flow "
                        "Mapping End."
                      , t);
          MN->IsAtEnd = true;
          MN = 0;
          CurrentEntry = 0;
        }
      }
      return *this;
    }
  };

  iterator begin() {
    assert(IsAtBeginning && "You may only iterate over a collection once!");
    IsAtBeginning = false;
    iterator ret(this);
    ++ret;
    return ret;
  }

  iterator end() { return iterator(); }

  virtual void skip() {
    // TODO: support skipping from the middle of a parsed map ;/
    assert((IsAtBeginning || IsAtEnd) && "Cannot skip mid parse!");
    if (IsAtBeginning)
      for (iterator i = begin(), e = end(); i != e; ++i)
        i->skip();
  }
};

class SequenceNode : public Node {
public:
  enum Type {
    ST_Block,
    ST_Flow,
    ST_Indentless
  };

private:
  Type SeqType;
  bool IsAtBeginning;
  bool IsAtEnd;

public:
  class iterator {
    SequenceNode *SN;
    Node *CurrentEntry;

  public:
    iterator() : SN(0), CurrentEntry(0) {}
    iterator(SequenceNode *sn) : SN(sn), CurrentEntry(0) {}

    Node *operator ->() const {
      assert(CurrentEntry && "Attempted to access end iterator!");
      return CurrentEntry;
    }

    Node &operator *() const {
      assert(CurrentEntry && "Attempted to dereference end iterator!");
      return *CurrentEntry;
    }

    operator Node*() const {
      assert(CurrentEntry && "Attempted to access end iterator!");
      return CurrentEntry;
    }

    bool operator !=(const iterator &Other) const {
      return SN != Other.SN;
    }

    iterator &operator++() {
      assert(SN && "Attempted to advance iterator past end!");
      if (SN->failed()) {
        SN->IsAtEnd = true;
        SN = 0;
        CurrentEntry = 0;
        return *this;
      }
      if (CurrentEntry)
        CurrentEntry->skip();
      Token t = SN->peekNext();
      if (SN->SeqType == ST_Block) {
        switch (t.Kind) {
        case Token::TK_BlockEntry:
          SN->getNext();
          CurrentEntry = SN->parseBlockNode();
          if (CurrentEntry == 0) { // An error occured.
            SN->IsAtEnd = true;
            SN = 0;
            CurrentEntry = 0;
          }
          break;
        case Token::TK_BlockEnd:
          SN->getNext();
          SN->IsAtEnd = true;
          SN = 0;
          CurrentEntry = 0;
          break;
        default:
          SN->setError( "Unexpected token. Expected Block Entry or Block End."
                      , t);
        case Token::TK_Error:
          SN->IsAtEnd = true;
          SN = 0;
          CurrentEntry = 0;
        }
      } else if (SN->SeqType == ST_Indentless) {
        switch (t.Kind) {
        case Token::TK_BlockEntry:
          SN->getNext();
          CurrentEntry = SN->parseBlockNode();
          if (CurrentEntry == 0) { // An error occured.
            SN->IsAtEnd = true;
            SN = 0;
            CurrentEntry = 0;
          }
          break;
        default:
        case Token::TK_Error:
          SN->IsAtEnd = true;
          SN = 0;
          CurrentEntry = 0;
        }
      } else if (SN->SeqType == ST_Flow) {
        switch (t.Kind) {
        case Token::TK_FlowEntry:
          // Eat the flow entry and recurse.
          SN->getNext();
          return ++(*this);
        case Token::TK_FlowSequenceEnd:
          SN->getNext();
        case Token::TK_Error:
          // Set this to end iterator.
          SN->IsAtEnd = true;
          SN = 0;
          CurrentEntry = 0;
          break;
        default:
          // Otherwise it must be a flow entry.
          CurrentEntry = SN->parseBlockNode();
          if (!CurrentEntry) {
            SN->IsAtEnd = true;
            SN = 0;
          }
          break;
        }
      }
      return *this;
    }
  };

  SequenceNode(Document *D, StringRef Anchor, Type T)
    : Node(NK_Sequence, D, Anchor)
    , SeqType(T)
    , IsAtBeginning(true)
    , IsAtEnd(false)
  {}

  static inline bool classof(const SequenceNode *) { return true; }
  static inline bool classof(const Node *n) {
    return n->getType() == NK_Sequence;
  }

  iterator begin() {
    assert(IsAtBeginning && "You may only iterate over a collection once!");
    IsAtBeginning = false;
    iterator ret(this);
    ++ret;
    return ret;
  }

  iterator end() { return iterator(); }

  virtual void skip() {
    // TODO: support skipping from the middle of a parsed sequence ;/
    assert((IsAtBeginning || IsAtEnd) && "Cannot skip mid parse!");
    if (IsAtBeginning)
      for (iterator i = begin(), e = end(); i != e; ++i)
        i->skip();
  }
};

class AliasNode : public Node {
  StringRef Name;

public:
  AliasNode(Document *D, StringRef Val)
    : Node(NK_Alias, D, StringRef()), Name(Val) {}

  StringRef getName() const { return Name; }
  Node *getTarget();

  static inline bool classof(const ScalarNode *) { return true; }
  static inline bool classof(const Node *n) {
    return n->getType() == NK_Alias;
  }
};

class Document {
  friend class Node;
  friend class document_iterator;

  Stream &S;
  BumpPtrAllocator NodeAllocator;
  Node *Root;

  Token &peekNext() {
    return S.S.peekNext();
  }

  Token getNext() {
    return S.S.getNext();
  }

  void setError(const Twine &Msg, Token &Tok) {
    S.S.setError(Msg, Tok.Range.begin());
  }

  bool failed() const {
    return S.S.failed();
  }

  void handleTagDirective(const Token &t) {

  }

  bool parseDirectives() {
    bool dir = false;
    while (true) {
      Token t = peekNext();
      if (t.Kind == Token::TK_TagDirective) {
        handleTagDirective(getNext());
        dir = true;
      } else if (t.Kind == Token::TK_VersionDirective) {
        S.handleYAMLDirective(getNext());
        dir = true;
      } else
        break;
    }
    return dir;
  }

  bool expectToken(Token::TokenKind TK) {
    Token t = getNext();
    if (t.Kind != TK) {
      setError("Unexpected token", t);
      return false;
    }
    return true;
  }

public:
  Node *parseBlockNode() {
    Token t = peekNext();
    // Handle properties.
    Token AnchorInfo;
  parse_property:
    switch (t.Kind) {
    case Token::TK_Alias:
      getNext();
      return new (NodeAllocator.Allocate<AliasNode>())
        AliasNode(this, t.Scalar.Value);
    case Token::TK_Anchor:
      if (AnchorInfo.Kind == Token::TK_Anchor) {
        setError("Already encountered an anchor for this node!", t);
        return 0;
      }
      AnchorInfo = getNext(); // Consume TK_Anchor.
      t = peekNext();
      goto parse_property;
    case Token::TK_Tag:
      getNext(); // Skip TK_Tag.
      t = peekNext();
      goto parse_property;
    default:
      break;
    }

    switch (t.Kind) {
    case Token::TK_BlockEntry:
      // We got an unindented BlockEntry sequence. This is not terminated with
      // a BlockEnd.
      // Don't eat the TK_BlockEntry, SequenceNode needs it.
      return new (NodeAllocator.Allocate<SequenceNode>())
        SequenceNode( this
                    , AnchorInfo.Scalar.Value
                    , SequenceNode::ST_Indentless);
    case Token::TK_BlockSequenceStart:
      getNext();
      return new (NodeAllocator.Allocate<SequenceNode>())
        SequenceNode(this, AnchorInfo.Scalar.Value, SequenceNode::ST_Block);
    case Token::TK_BlockMappingStart:
      getNext();
      return new (NodeAllocator.Allocate<MappingNode>())
        MappingNode(this, AnchorInfo.Scalar.Value, MappingNode::MT_Block);
    case Token::TK_FlowSequenceStart:
      getNext();
      return new (NodeAllocator.Allocate<SequenceNode>())
        SequenceNode(this, AnchorInfo.Scalar.Value, SequenceNode::ST_Flow);
    case Token::TK_FlowMappingStart:
      getNext();
      return new (NodeAllocator.Allocate<MappingNode>())
        MappingNode(this, AnchorInfo.Scalar.Value, MappingNode::MT_Flow);
    case Token::TK_Scalar:
      getNext();
      return new (NodeAllocator.Allocate<ScalarNode>())
        ScalarNode(this, AnchorInfo.Scalar.Value, t.Scalar.Value);
    case Token::TK_Key:
      // Don't eat the TK_Key, KeyValueNode expects it.
      return new (NodeAllocator.Allocate<MappingNode>())
        MappingNode(this, AnchorInfo.Scalar.Value, MappingNode::MT_Inline);
    case Token::TK_DocumentStart:
    case Token::TK_DocumentEnd:
    case Token::TK_StreamEnd:
    default:
      // TODO: Properly handle tags. "[!!str ]" should resolve to !!str "", not
      //       !!null null.
      return new (NodeAllocator.Allocate<NullNode>()) NullNode(this);
    case Token::TK_Error:
      return 0;
    }
    llvm_unreachable("Control flow shouldn't reach here.");
    return 0;
  }

  Document(Stream &s) : S(s), Root(0) {
    if (parseDirectives())
      expectToken(Token::TK_DocumentStart);
    Token &t = peekNext();
    if (t.Kind == Token::TK_DocumentStart)
      getNext();
  }

  /// Finish parsing the current document and return true if there are more.
  /// Return false otherwise.
  bool skip() {
    if (S.S.failed())
      return false;
    if (!Root)
      getRoot();
    Root->skip();
    Token &t = peekNext();
    if (t.Kind == Token::TK_StreamEnd)
      return false;
    if (t.Kind == Token::TK_DocumentEnd) {
      getNext();
      return skip();
    }
    return true;
  }

  Node *getRoot() {
    assert(!Root && "getRoot may only be called once per document!");
    return Root = parseBlockNode();
  }
};

class document_iterator {
  Document *Doc;

public:
  document_iterator() : Doc(0) {}
  document_iterator(Document *d) : Doc(d) {}

  bool operator !=(const document_iterator &other) {
    return Doc != other.Doc;
  }

  document_iterator operator ++() {
    if (!Doc->skip())
      Doc = 0;
    else {
      Doc->~Document();
      new (Doc) Document(Doc->S);
    }
    return *this;
  }

  Document *operator ->() {
    return Doc;
  }
};

document_iterator Stream::begin() {
  if (CurrentDoc)
    report_fatal_error("Can only iterate over the stream once");

  // Skip Stream-Start.
  S.getNext();

  CurrentDoc.reset(new Document(*this));
  return document_iterator(CurrentDoc.get());
}

document_iterator Stream::end() {
  return document_iterator();
}

void Stream::skip() {
  for (document_iterator i = begin(), e = end(); i != e; ++i)
    i->skip();
}

Token &Node::peekNext() {
  return Doc->peekNext();
}

Token Node::getNext() {
  return Doc->getNext();
}

Node *Node::parseBlockNode() {
  return Doc->parseBlockNode();
}

BumpPtrAllocator &Node::getAllocator() {
  return Doc->NodeAllocator;
}

void Node::setError(const Twine &Msg, Token &Tok) {
  Doc->setError(Msg, Tok);
}

bool Node::failed() const {
  return Doc->failed();
}

} // end namespace yaml.
} // end namespace llvm.

using namespace llvm;

struct indent {
  unsigned distance;
  indent(unsigned d) : distance(d) {}
};

raw_ostream &operator <<(raw_ostream &os, const indent &in) {
  for (unsigned i = 0; i < in.distance; ++i)
    os << "  ";
  return os;
}

void dumpNode( yaml::Node *n
             , unsigned Indent = 0
             , bool SuppressFirstIndent = false) {
  if (!n)
    return;
  if (!SuppressFirstIndent)
    outs() << indent(Indent);
  StringRef Anchor = n->getAnchor();
  if (!Anchor.empty())
    outs() << "&" << Anchor << " ";
  if (yaml::ScalarNode *sn = dyn_cast<yaml::ScalarNode>(n)) {
    outs() << "!!str \"" << sn->getRawValue() << "\"";
  } else if (yaml::SequenceNode *sn = dyn_cast<yaml::SequenceNode>(n)) {
    outs() << "!!seq [\n";
    ++Indent;
    for (yaml::SequenceNode::iterator i = sn->begin(), e = sn->end();
                                      i != e; ++i) {
      dumpNode(i, Indent);
      outs() << ",\n";
    }
    --Indent;
    outs() << indent(Indent) << "]";
  } else if (yaml::MappingNode *mn = dyn_cast<yaml::MappingNode>(n)) {
    outs() << "!!map {\n";
    ++Indent;
    for (yaml::MappingNode::iterator i = mn->begin(), e = mn->end();
                                     i != e; ++i) {
      outs() << indent(Indent) << "? ";
      dumpNode(i->getKey(), Indent, true);
      outs() << "\n";
      outs() << indent(Indent) << ": ";
      dumpNode(i->getValue(), Indent, true);
      outs() << ",\n";
    }
    --Indent;
    outs() << indent(Indent) << "}";
  } else if (yaml::AliasNode *an = dyn_cast<yaml::AliasNode>(n)){
    outs() << "*" << an->getName();
  } else if (dyn_cast<yaml::NullNode>(n)) {
    outs() << "!!null null";
  }
}

void dumpTokens(yaml::Scanner &s) {
  while (true) {
    yaml::Token t = s.getNext();
    switch (t.Kind) {
    case yaml::Token::TK_StreamStart:
      outs() << "Stream-Start(" << t.StreamStart.BOM << "): ";
      break;
    case yaml::Token::TK_StreamEnd:
      outs() << "Stream-End: ";
      break;
    case yaml::Token::TK_VersionDirective:
      outs() << "Version-Directive(" << t.VersionDirective.Value << "): ";
      break;
    case yaml::Token::TK_TagDirective:
      outs() << "Tag-Directive: ";
      break;
    case yaml::Token::TK_DocumentStart:
      outs() << "Document-Start: ";
      break;
    case yaml::Token::TK_DocumentEnd:
      outs() << "Document-End: ";
      break;
    case yaml::Token::TK_BlockEntry:
      outs() << "Block-Entry: ";
      break;
    case yaml::Token::TK_BlockEnd:
      outs() << "Block-End: ";
      break;
    case yaml::Token::TK_BlockSequenceStart:
      outs() << "Block-Sequence-Start: ";
      break;
    case yaml::Token::TK_BlockMappingStart:
      outs() << "Block-Mapping-Start: ";
      break;
    case yaml::Token::TK_FlowEntry:
      outs() << "Flow-Entry: ";
      break;
    case yaml::Token::TK_FlowSequenceStart:
      outs() << "Flow-Sequence-Start: ";
      break;
    case yaml::Token::TK_FlowSequenceEnd:
      outs() << "Flow-Sequence-End: ";
      break;
    case yaml::Token::TK_FlowMappingStart:
      outs() << "Flow-Mapping-Start: ";
      break;
    case yaml::Token::TK_FlowMappingEnd:
      outs() << "Flow-Mapping-End: ";
      break;
    case yaml::Token::TK_Key:
      outs() << "Key: ";
      break;
    case yaml::Token::TK_Value:
      outs() << "Value: ";
      break;
    case yaml::Token::TK_Scalar:
      outs() << "Scalar(" << t.Scalar.Value << "): ";
      break;
    case yaml::Token::TK_Alias:
      outs() << "Alias(" << t.Scalar.Value << "): ";
      break;
    case yaml::Token::TK_Anchor:
      outs() << "Anchor(" << t.Scalar.Value << "): ";
      break;
    case yaml::Token::TK_Tag:
      outs() << "Tag: ";
      break;
    case yaml::Token::TK_Error:
      break;
    }
    outs() << t.Range << "\n";
    if (t.Kind == yaml::Token::TK_StreamEnd || t.Kind == yaml::Token::TK_Error)
      break;
    outs().flush();
  }
}

void dumpStream(yaml::Stream &stream) {
  for (yaml::document_iterator di = stream.begin(), de = stream.end(); di != de;
       ++di) {
    outs() << "%YAML 1.2\n"
           << "---\n";
    yaml::Node *n = di->getRoot();
    if (n)
      dumpNode(n);
    else
      break;
    outs() << "\n...\n";
  }
}

void benchmark(llvm::TimerGroup &Group, llvm::StringRef Name,
               llvm::StringRef JSONText) {
  llvm::Timer BaseLine((Name + ": Loop").str(), Group);
  BaseLine.startTimer();
  char C = 0;
  for (llvm::StringRef::iterator I = JSONText.begin(),
                                 E = JSONText.end();
       I != E; ++I) { C += *I; }
  BaseLine.stopTimer();
  volatile char DontOptimizeOut = C; (void)DontOptimizeOut;

  llvm::Timer Tokenizing((Name + ": Tokenizing").str(), Group);
  Tokenizing.startTimer();
  {
    llvm::SourceMgr SM;
    llvm::yaml::Scanner scanner(JSONText, &SM);
    for (;;) {
      llvm::yaml::Token t = scanner.getNext();
      if (  t.Kind == llvm::yaml::Token::TK_StreamEnd
         || t.Kind == llvm::yaml::Token::TK_Error)
        break;
    }
  }
  Tokenizing.stopTimer();

  llvm::Timer Parsing((Name + ": Parsing").str(), Group);
  Parsing.startTimer();
  {
    llvm::SourceMgr SM;
    llvm::yaml::Stream stream(JSONText, &SM);
    stream.skip();
  }
  Parsing.stopTimer();
}

std::string createJSONText(size_t MemoryMB, unsigned ValueSize) {
  std::string JSONText;
  llvm::raw_string_ostream Stream(JSONText);
  Stream << "[\n";
  size_t MemoryBytes = MemoryMB * 1024 * 1024;
  while (JSONText.size() < MemoryBytes) {
    Stream << " {\n"
           << "  \"key1\": \"" << std::string(ValueSize, '*') << "\",\n"
           << "  \"key2\": \"" << std::string(ValueSize, '*') << "\",\n"
           << "  \"key3\": \"" << std::string(ValueSize, '*') << "\"\n"
           << " }";
    Stream.flush();
    if (JSONText.size() < MemoryBytes) Stream << ",";
    Stream << "\n";
  }
  Stream << "]\n";
  Stream.flush();
  return JSONText;
}

int main(int argc, char **argv) {
  llvm::cl::ParseCommandLineOptions(argc, argv);
  if (Input.getNumOccurrences()) {
    OwningPtr<MemoryBuffer> Buf;
    if (MemoryBuffer::getFileOrSTDIN(Input, Buf))
      return 1;

    llvm::SourceMgr sm;
    if (DumpTokens) {
      yaml::Scanner s(Buf->getBuffer(), &sm);
      dumpTokens(s);
    }

    if (DumpCanonical) {
      yaml::Stream stream(Buf->getBuffer(), &sm);
      dumpStream(stream);
    }
  }

  if (Verify) {
    llvm::TimerGroup Group("YAML parser benchmark");
    benchmark(Group, "Fast", createJSONText(10, 500));
  }

  return 0;
}
