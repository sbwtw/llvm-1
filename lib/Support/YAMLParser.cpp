//===--- YAMLParser.cpp - Simple YAML parser ------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements a YAML parser.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/YAMLParser.h"

#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"

using namespace llvm;
using namespace yaml;

namespace llvm {
namespace yaml {

enum UnicodeEncodingForm {
  UEF_UTF32_LE, //< UTF-32 Little Endian
  UEF_UTF32_BE, //< UTF-32 Big Endian
  UEF_UTF16_LE, //< UTF-16 Little Endian
  UEF_UTF16_BE, //< UTF-16 Big Endian
  UEF_UTF8,     //< UTF-8 or ascii.
  UEF_Unknown   //< Not a valid Unicode encoding.
};

/// EncodingInfo - Holds the encoding type and length of the byte order mark if
///                it exists. Length is in {0, 2, 3, 4}.
typedef std::pair<UnicodeEncodingForm, unsigned> EncodingInfo;

/// getBOM - Reads up to the first 4 bytes to determine the Unicode encoding
///          form of \a Input.
///
/// @param Input A string of length 0 or more.
/// @returns An EncodingInfo indicating the Unicode encoding form of the input
///          and how long the byte order mark is if one exists.
EncodingInfo getUnicodeEncoding(StringRef Input) {
  if (Input.size() == 0)
    return std::make_pair(UEF_Unknown, 0);

  switch (uint8_t(Input[0])) {
  case 0x00:
    if (Input.size() >= 4) {
      if (  Input[1] == 0
         && uint8_t(Input[2]) == 0xFE
         && uint8_t(Input[3]) == 0xFF)
        return std::make_pair(UEF_UTF32_BE, 4);
      if (Input[1] == 0 && Input[2] == 0 && Input[3] != 0)
        return std::make_pair(UEF_UTF32_BE, 0);
    }

    if (Input.size() >= 2 && Input[1] != 0)
      return std::make_pair(UEF_UTF16_BE, 0);
    return std::make_pair(UEF_Unknown, 0);
  case 0xFF:
    if (  Input.size() >= 4
       && uint8_t(Input[1]) == 0xFE
       && Input[2] == 0
       && Input[3] == 0)
      return std::make_pair(UEF_UTF32_LE, 4);

    if (Input.size() >= 2 && uint8_t(Input[1]) == 0xFE)
      return std::make_pair(UEF_UTF16_LE, 2);
    return std::make_pair(UEF_Unknown, 0);
  case 0xFE:
    if (Input.size() >= 2 && uint8_t(Input[1]) == 0xFF)
      return std::make_pair(UEF_UTF16_BE, 2);
    return std::make_pair(UEF_Unknown, 0);
  case 0xEF:
    if (  Input.size() >= 3
       && uint8_t(Input[1]) == 0xBB
       && uint8_t(Input[2]) == 0xBF)
      return std::make_pair(UEF_UTF8, 3);
    return std::make_pair(UEF_Unknown, 0);
  }

  // It could still be utf-32 or utf-16.
  if (Input.size() >= 4 && Input[1] == 0 && Input[2] == 0 && Input[3] == 0)
    return std::make_pair(UEF_UTF32_LE, 0);

  if (Input.size() >= 2 && Input[1] == 0)
    return std::make_pair(UEF_UTF16_LE, 0);

  return std::make_pair(UEF_UTF8, 0);
}

/// Token - A single YAML token.
struct Token : ilist_node<Token> {
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

  /// A string of length 0 or more whose begin() points to the logical location
  /// of the token in the input.
  StringRef Range;

  Token() : Kind(TK_Error) {}
};

}

template<>
struct ilist_sentinel_traits<Token> {
  Token *createSentinel() const {
    return &Sentinel;
  }
  static void destroySentinel(Token*) {}

  Token *provideInitialHead() const { return createSentinel(); }
  Token *ensureHead(Token*) const { return createSentinel(); }
  static void noteHead(Token*, Token*) {}

private:
  mutable Token Sentinel;
};

template<>
struct ilist_node_traits<Token> {
  Token *createNode(const Token &V) {
    return new (Alloc.Allocate<Token>()) Token(V);
  }
  static void deleteNode(Token *V) {}

  void addNodeToList(Token *) {}
  void removeNodeFromList(Token *) {}
  void transferNodesFromList(ilist_node_traits &    /*SrcTraits*/,
                             ilist_iterator<Token> /*first*/,
                             ilist_iterator<Token> /*last*/) {}

  BumpPtrAllocator Alloc;
};

namespace yaml {

typedef ilist<Token> TokenQueueT;

/// @brief This struct is used to track simple keys.
///
/// Simple keys are handled by creating an entry in SimpleKeys for each Token
/// which could legally be the start of a simple key. When peekNext is called,
/// if the Token To be returned is referenced by a SimpleKey, we continue
/// tokenizing until that potential simple key has either been found to not be
/// a simple key (we moved on to the next line or went further than 1024 chars).
/// Or when we run into a Value, and then insert a Key token (and possibly
/// others) before the SimpleKey's Tok.
struct SimpleKey {
  TokenQueueT::iterator Tok;
  unsigned Column;
  unsigned Line;
  unsigned FlowLevel;
  bool IsRequired;

  bool operator ==(const SimpleKey &Other) {
    return Tok == Other.Tok;
  }
};

/// @brief The Unicode scalar value of a UTF-8 minimal well-formed code unit
///        subsequence and the subsequence's length in code units (uint8_t).
///        A length of 0 represents an error.
typedef std::pair<uint32_t, unsigned> UTF8Decoded;

UTF8Decoded decodeUTF8(StringRef Range) {
  StringRef::iterator Position = Range.begin();
  StringRef::iterator End = Range.end();
  if ((uint8_t(*Position) & 0x80) == 0)
    return std::make_pair(*Position, 1);

  if (   (uint8_t(*Position) & 0xE0) == 0xC0
      && Position + 1 != End
      && uint8_t(*Position) >= 0xC2
      && uint8_t(*Position) <= 0xDF
      && uint8_t(*(Position + 1)) >= 0x80
      && uint8_t(*(Position + 1)) <= 0xBF) {
    uint32_t codepoint = uint8_t(*(Position + 1)) & 0x3F;
    codepoint |= uint16_t(uint8_t(*Position) & 0x1F) << 6;
    return std::make_pair(codepoint, 2);
  }

  if (   (uint8_t(*Position) & 0xF0) == 0xE0
      && Position + 2 < End) {
    if (   uint8_t(*Position) == 0xE0
        && (  uint8_t(*(Position + 1)) < 0xA0
          || uint8_t(*(Position + 1)) > 0xBF));
    else if (  uint8_t(*Position) >= 0xE1
            && uint8_t(*Position) <= 0xEC
            && (  uint8_t(*(Position + 1)) < 0x80
                || uint8_t(*(Position + 1)) > 0xBF));
    else if (  uint8_t(*Position) == 0xED
            && (  uint8_t(*(Position + 1)) < 0x80
                || uint8_t(*(Position + 1)) > 0x9F));
    else if (  uint8_t(*Position) >= 0xEE
            && uint8_t(*Position) <= 0xEF
            && (  uint8_t(*(Position + 1)) < 0x80
                || uint8_t(*(Position + 1)) > 0xBF));
    else {
      if (   uint8_t(*(Position + 2)) >= 0x80
          && uint8_t(*(Position + 2)) <= 0xBF) {
        uint32_t codepoint = uint8_t(*(Position + 2)) & 0x3F;
        codepoint |= uint16_t(uint8_t(*(Position + 1)) & 0x3F) << 6;
        codepoint |= uint16_t(uint8_t(*Position) & 0x0F) << 12;
        return std::make_pair(codepoint, 3);
      }
    }
  }

  if (   (uint8_t(*Position) & 0xF8) == 0xF0
      && Position + 3 < End) {
    if (  uint8_t(*Position) == 0xF0
        && (  uint8_t(*(Position + 1)) < 0x90
          || uint8_t(*(Position + 1)) > 0xBF));
    else if (  uint8_t(*Position) >= 0xF1
            && uint8_t(*Position) <= 0xF3
            && (  uint8_t(*(Position + 1)) < 0x80
                || uint8_t(*(Position + 1)) > 0xBF));
    else if (  uint8_t(*Position) == 0xF4
            && (  uint8_t(*(Position + 1)) < 0x80
                || uint8_t(*(Position + 1)) > 0x8F));
    else {
      if (   uint8_t(*(Position + 2)) >= 0x80
          && uint8_t(*(Position + 2)) <= 0xBF
          && uint8_t(*(Position + 3)) >= 0x80
          && uint8_t(*(Position + 3)) <= 0xBF) {
        uint32_t codepoint = uint8_t(*(Position + 3)) & 0x3F;
        codepoint |= uint32_t(uint8_t(*(Position + 2)) & 0x3F) << 6;
        codepoint |= uint32_t(uint8_t(*(Position + 1)) & 0x3F) << 12;
        codepoint |= uint32_t(uint8_t(*Position) & 0x07) << 18;
        return std::make_pair(codepoint, 4);
      }
    }
  }

  return std::make_pair(0, 0);
}

/// @brief Scans YAML tokens from a MemoryBuffer.
class Scanner {
public:
  Scanner(const StringRef Input, SourceMgr &SM);

  /// @brief Parse the next token and return it without popping it.
  Token &peekNext();

  /// @brief Parse the next token and pop it from the queue.
  Token getNext();

  void printError(SMLoc Loc, SourceMgr::DiagKind Kind, const Twine &Message,
                  ArrayRef<SMRange> Ranges = ArrayRef<SMRange>()) {
    SM.PrintMessage(Loc, Kind, Message, Ranges);
  }

  void setError(const Twine &Message, StringRef::iterator Position) {
    if (Current >= End)
      Current = End - 1;

    // Don't print out more errors after the first one we encounter. The rest
    // are just the result of the first, and have no meaning.
    if (!Failed)
      printError(SMLoc::getFromPointer(Current), SourceMgr::DK_Error, Message);
    Failed = true;
  }

  void setError(const Twine &Message) {
    setError(Message, Current);
  }

  /// @brief Returns true if an error occurred while parsing.
  bool failed() {
    return Failed;
  }

private:
  StringRef currentInput() {
    return StringRef(Current, End - Current);
  }

  /// @brief Decode a UTF-8 minimal well-formed code unit subsequence starting
  ///        at \a Position.
  ///
  /// If the UTF-8 code units starting at Position do not form a well-formed
  /// code unit subsequence, then the Unicode scalar value is 0, and the length
  /// is 0.
  UTF8Decoded decodeUTF8(StringRef::iterator Position) {
    return yaml::decodeUTF8(StringRef(Position, End - Position));
  }

  // The following functions are based on the gramar rules in the YAML spec. The
  // style of the function names it meant to closely match how they are written
  // in the spec. The number within the [] is the number of the grammar rule in
  // the spec.
  //
  // See 4.2 [Production Naming Conventions] for the meaning of the prefixes.
  //
  // c-
  //   A production starting and ending with a special character.
  // b-
  //   A production matching a single line break.
  // nb-
  //   A production starting and ending with a non-break character.
  // s-
  //   A production starting and ending with a white space character.
  // ns-
  //   A production starting and ending with a non-space character.
  // l-
  //   A production matching complete line(s).

  /// @brief Skip a single nb-char[27] starting at Position.
  ///
  /// A nb-char is 0x9 | [0x20-0x7E] | 0x85 | [0xA0-0xD7FF] | [0xE000-0xFEFE]
  ///                  | [0xFF00-0xFFFD] | [0x10000-0x10FFFF]
  ///
  /// @returns The code unit after the nb-char, or Position if it's not an
  ///          nb-char.
  StringRef::iterator skip_nb_char(StringRef::iterator Position);

  /// @brief Skip a single b-break[28] starting at Position.
  ///
  /// A b-break is 0xD 0xA | 0xD | 0xA
  ///
  /// @returns The code unit after the b-break, or Position if it's not a
  ///          b-break.
  StringRef::iterator skip_b_break(StringRef::iterator Position);

  /// @brief Skip a single s-white[33] starting at Position.
  ///
  /// A s-white is 0x20 | 0x9
  ///
  /// @returns The code unit after the s-white, or Position if it's not a
  ///          s-white.
  StringRef::iterator skip_s_white(StringRef::iterator Position);

  /// @brief Skip a single ns-char[34] starting at Position.
  ///
  /// A ns-char is nb-char - s-white
  ///
  /// @returns The code unit after the ns-char, or Position if it's not a
  ///          ns-char.
  StringRef::iterator skip_ns_char(StringRef::iterator Position);

  /// @brief Skip minimal well-formed code unit subsequences until Func
  ///        returns its input.
  ///
  /// @returns The code unit after the last minimal well-formed code unit
  ///          subsequence that Func accepted.
  template<StringRef::iterator (Scanner::*Func)(StringRef::iterator)>
  StringRef::iterator skip_while(StringRef::iterator Position) {
    while (true) {
      StringRef::iterator i = (this->*Func)(Position);
      if (i == Position)
        break;
      Position = i;
    }
    return Position;
  }

  /// @brief Scan ns-uri-char[39]s starting at Cur.
  ///
  /// This updates Cur and Column while scanning.
  ///
  /// @returns A StringRef starting at Cur which covers the longest contiguous
  ///          sequence of ns-uri-char.
  StringRef scan_ns_uri_char();

  /// @brief Scan ns-plain-one-line[133] starting at \a Cur.
  StringRef scan_ns_plain_one_line();

  /// @brief Consume a minimal well-formed code unit subsequence starting at
  ///        \a Cur. Return false if it is not the same Unicode scalar value as
  ///        \a Expected. This updates \a Column.
  bool consume(uint32_t Expected);

  /// @brief Skip \a Distance UTF-8 code units. Updates \a Cur and \a Column.
  void skip(uint32_t Distance);

  /// @brief Return true if the minimal well-formed code unit subsequence at
  ///        Pos is whitespace or a new line
  bool isBlankOrBreak(StringRef::iterator Position);

  /// @brief If IsSimpleKeyAllowed, create and push_back a new SimpleKey.
  void saveSimpleKeyCandidate( TokenQueueT::iterator Tok
                             , unsigned AtColumn
                             , bool IsRequired);

  /// @brief Remove simple keys that can no longer be valid simple keys.
  ///
  /// Invalid simple keys are not on the current line or are further than 1024
  /// columns back.
  void removeStaleSimpleKeyCandidates();

  /// @brief Remove all simple keys on FlowLevel \a Level.
  void removeSimpleKeyCandidatesOnFlowLevel(unsigned Level);

  /// @brief Unroll indentation in \a Indents back to \a Col. Creates BlockEnd
  ///        tokens if needed.
  bool unrollIndent(int ToColumn);

  /// @brief Increase indent to \a Col. Creates \a Kind token at \a InsertPoint
  ///        if needed.
  bool rollIndent( int ToColumn
                 , Token::TokenKind Kind
                 , TokenQueueT::iterator InsertPoint);

  /// @brief Skip whitespace and comments until the start of the next token.
  void scanToNextToken();

  /// @brief Must be the first token generated.
  bool scanStreamStart();

  /// @brief Generate tokens needed to close out the stream.
  bool scanStreamEnd();

  /// @brief Scan a %BLAH directive.
  bool scanDirective();

  /// @brief Scan a ... or ---.
  bool scanDocumentIndicator(bool IsStart);

  /// @brief Scan a [ or { and generate the proper flow collection start token.
  bool scanFlowCollectionStart(bool IsSequence);

  /// @brief Scan a ] or } and generate the proper flow collection end token.
  bool scanFlowCollectionEnd(bool IsSequence);

  /// @brief Scan the , that separates entries in a flow collection.
  bool scanFlowEntry();

  /// @brief Scan the - that starts block sequence entries.
  bool scanBlockEntry();

  /// @brief Scan an explicit ? indicating a key.
  bool scanKey();

  /// @brief Scan an explicit : indicating a value.
  bool scanValue();

  /// @brief Scan a quoted scalar.
  bool scanFlowScalar(bool IsDoubleQuoted);

  /// @brief Scan an unquoted scalar.
  bool scanPlainScalar();

  /// @brief Scan an Alias or Anchor starting with * or &.
  bool scanAliasOrAnchor(bool IsAlias);

  /// @brief Scan a block scalar starting with | or >.
  bool scanBlockScalar(bool IsLiteral);

  /// @brief Scan a tag of the form !stuff.
  bool scanTag();

  /// @brief Dispatch to the next scanning function based on \a *Cur.
  bool fetchMoreTokens();

  /// @brief The SourceMgr used for diagnostics and buffer management.
  SourceMgr &SM;

  /// @brief The original input.
  MemoryBuffer *InputBuffer;

  /// @brief The current position of the scanner.
  StringRef::iterator Current;

  /// @brief The end of the input (one past the last character).
  StringRef::iterator End;

  /// @brief Current YAML indentation level in spaces.
  int Indent;

  /// @brief Current column number in Unicode code points.
  unsigned Column;

  /// @brief Current line number.
  unsigned Line;

  /// @brief How deep we are in flow style containers. 0 Means at block level.
  unsigned FlowLevel;

  /// @brief Are we at the start of the stream?
  bool IsStartOfStream;

  /// @brief Can the next token be the start of a simple key?
  bool IsSimpleKeyAllowed;

  /// @brief Is the next token required to start a simple key?
  bool IsSimpleKeyRequired;

  /// @brief True if an error has occurred.
  bool Failed;

  /// @brief Queue of tokens. This is required to queue up tokens while looking
  ///        for the end of a simple key. And for cases where a single character
  ///        can produce multiple tokens (e.g. BlockEnd).
  TokenQueueT TokenQueue;

  /// @brief Indentation levels.
  SmallVector<int, 4> Indents;

  /// @brief Potential simple keys.
  SmallVector<SimpleKey, 4> SimpleKeys;
};

} // end namespace yaml
} // end namespace llvm

namespace {
SmallString<4> encodeUTF8(uint32_t UnicodeScalarValue) {
  SmallString<4> UTF8Subsequence;
  if (UnicodeScalarValue <= 0x7F) {
    UTF8Subsequence.push_back(UnicodeScalarValue & 0x7F);
  } else if (UnicodeScalarValue <= 0x7FF) {
    uint8_t FirstByte = 0xC0 | ((UnicodeScalarValue & 0x7C0) >> 6);
    uint8_t SecondByte = 0x80 | (UnicodeScalarValue & 0x3F);
    UTF8Subsequence.push_back(FirstByte);
    UTF8Subsequence.push_back(SecondByte);
  } else if (UnicodeScalarValue <= 0xFFFF) {
    uint8_t FirstByte = 0xE0 | ((UnicodeScalarValue & 0xF000) >> 12);
    uint8_t SecondByte = 0x80 | ((UnicodeScalarValue & 0xFC0) >> 6);
    uint8_t ThirdByte = 0x80 | (UnicodeScalarValue & 0x3F);
    UTF8Subsequence.push_back(FirstByte);
    UTF8Subsequence.push_back(SecondByte);
    UTF8Subsequence.push_back(ThirdByte);
  } else if (UnicodeScalarValue <= 0x10FFFF) {
    uint8_t FirstByte = 0xF0 | ((UnicodeScalarValue & 0x1F0000) >> 18);
    uint8_t SecondByte = 0x80 | ((UnicodeScalarValue & 0x3F000) >> 12);
    uint8_t ThirdByte = 0x80 | ((UnicodeScalarValue & 0xFC0) >> 6);
    uint8_t FourthByte = 0x80 | (UnicodeScalarValue & 0x3F);
    UTF8Subsequence.push_back(FirstByte);
    UTF8Subsequence.push_back(SecondByte);
    UTF8Subsequence.push_back(ThirdByte);
    UTF8Subsequence.push_back(FourthByte);
  }
  return UTF8Subsequence;
}
}

bool yaml::dumpTokens(StringRef Input, raw_ostream &OS) {
  SourceMgr SM;
  Scanner scanner(Input, SM);
  while (true) {
    Token T = scanner.getNext();
    switch (T.Kind) {
    case Token::TK_StreamStart:
      OS << "Stream-Start: ";
      break;
    case Token::TK_StreamEnd:
      OS << "Stream-End: ";
      break;
    case Token::TK_VersionDirective:
      OS << "Version-Directive: ";
      break;
    case Token::TK_TagDirective:
      OS << "Tag-Directive: ";
      break;
    case Token::TK_DocumentStart:
      OS << "Document-Start: ";
      break;
    case Token::TK_DocumentEnd:
      OS << "Document-End: ";
      break;
    case Token::TK_BlockEntry:
      OS << "Block-Entry: ";
      break;
    case Token::TK_BlockEnd:
      OS << "Block-End: ";
      break;
    case Token::TK_BlockSequenceStart:
      OS << "Block-Sequence-Start: ";
      break;
    case Token::TK_BlockMappingStart:
      OS << "Block-Mapping-Start: ";
      break;
    case Token::TK_FlowEntry:
      OS << "Flow-Entry: ";
      break;
    case Token::TK_FlowSequenceStart:
      OS << "Flow-Sequence-Start: ";
      break;
    case Token::TK_FlowSequenceEnd:
      OS << "Flow-Sequence-End: ";
      break;
    case Token::TK_FlowMappingStart:
      OS << "Flow-Mapping-Start: ";
      break;
    case Token::TK_FlowMappingEnd:
      OS << "Flow-Mapping-End: ";
      break;
    case Token::TK_Key:
      OS << "Key: ";
      break;
    case Token::TK_Value:
      OS << "Value: ";
      break;
    case Token::TK_Scalar:
      OS << "Scalar: ";
      break;
    case Token::TK_Alias:
      OS << "Alias: ";
      break;
    case Token::TK_Anchor:
      OS << "Anchor: ";
      break;
    case Token::TK_Tag:
      OS << "Tag: ";
      break;
    case Token::TK_Error:
      break;
    }
    OS << T.Range << "\n";
    if (T.Kind == Token::TK_StreamEnd)
      break;
    else if (T.Kind == Token::TK_Error)
      return false;
  }
  return true;
}

bool yaml::scanTokens(StringRef Input) {
  llvm::SourceMgr SM;
  llvm::yaml::Scanner scanner(Input, SM);
  for (;;) {
    llvm::yaml::Token T = scanner.getNext();
    if (T.Kind == Token::TK_StreamEnd)
      break;
    else if (T.Kind == Token::TK_Error)
      return false;
  }
  return true;
}

std::string yaml::escape(StringRef Input) {
  std::string EscapedInput;
  for (StringRef::iterator i = Input.begin(), e = Input.end(); i != e; ++i) {
    if (*i == '\\')
      EscapedInput += "\\\\";
    else if (*i == '"')
      EscapedInput += "\\\"";
    else if (*i == 0)
      EscapedInput += "\\0";
    else if (*i == 0x07)
      EscapedInput += "\\a";
    else if (*i == 0x08)
      EscapedInput += "\\b";
    else if (*i == 0x09)
      EscapedInput += "\\t";
    else if (*i == 0x0A)
      EscapedInput += "\\n";
    else if (*i == 0x0B)
      EscapedInput += "\\v";
    else if (*i == 0x0C)
      EscapedInput += "\\f";
    else if (*i == 0x0D)
      EscapedInput += "\\r";
    else if (*i == 0x1B)
      EscapedInput += "\\e";
    else if (*i >= 0 && *i < 0x20) { // Control characters not handled above.
      std::string HexStr = utohexstr(*i);
      EscapedInput += "\\x" + std::string(HexStr.size() - 2, '0') + HexStr;
    } else if (*i & 0x80) { // UTF-8 multiple code unit subsequence.
      UTF8Decoded UnicodeScalarValue
        = decodeUTF8(StringRef(i, Input.end() - i));
      if (UnicodeScalarValue.second == 0) {
        // Found invalid char.
        SmallString<4> Val = encodeUTF8(0xFFFD);
        EscapedInput.insert(EscapedInput.end(), Val.begin(), Val.end());
        // FIXME: Error reporting.
        return EscapedInput;
      }
      if (UnicodeScalarValue.first == 0x85)
        EscapedInput += "\\N";
      else if (UnicodeScalarValue.first == 0xA0)
        EscapedInput += "\\_";
      else if (UnicodeScalarValue.first == 0x2028)
        EscapedInput += "\\L";
      else if (UnicodeScalarValue.first == 0x2029)
        EscapedInput += "\\P";
      else {
        std::string HexStr = utohexstr(UnicodeScalarValue.first);
        if (HexStr.size() <= 2)
          EscapedInput += "\\x" + std::string(2 - HexStr.size(), '0') + HexStr;
        else if (HexStr.size() <= 4)
          EscapedInput += "\\u" + std::string(4 - HexStr.size(), '0') + HexStr;
        else if (HexStr.size() <= 8)
          EscapedInput += "\\U" + std::string(8 - HexStr.size(), '0') + HexStr;
      }
      i += UnicodeScalarValue.second - 1;
    } else
      EscapedInput.push_back(*i);
  }
  return EscapedInput;
}

Scanner::Scanner(StringRef Input, SourceMgr &sm)
  : SM(sm)
  , Indent(-1)
  , Column(0)
  , Line(0)
  , FlowLevel(0)
  , IsStartOfStream(true)
  , IsSimpleKeyAllowed(true)
  , IsSimpleKeyRequired(false)
  , Failed(false) {
  InputBuffer = MemoryBuffer::getMemBuffer(Input, "YAML");
  SM.AddNewSourceBuffer(InputBuffer, SMLoc());
  Current = InputBuffer->getBufferStart();
  End = InputBuffer->getBufferEnd();
}

Token &Scanner::peekNext() {
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

    removeStaleSimpleKeyCandidates();
    SimpleKey SK;
    SK.Tok = TokenQueue.front();
    if (std::find(SimpleKeys.begin(), SimpleKeys.end(), SK)
        == SimpleKeys.end())
      break;
    else
      NeedMore = true;
  }
  return TokenQueue.front();
}

Token Scanner::getNext() {
  Token Ret = peekNext();
  // TokenQueue can be empty if there was an error getting the next token.
  if (!TokenQueue.empty())
    TokenQueue.pop_front();

  // There cannot be any referenced Token's if the TokenQueue is empty. So do a
  // quick deallocation of them all.
  if (TokenQueue.empty()) {
    TokenQueue.Alloc.Reset();
  }

  return Ret;
}

StringRef::iterator Scanner::skip_nb_char(StringRef::iterator Position) {
  // Check 7 bit c-printable - b-char.
  if (   *Position == 0x09
      || (*Position >= 0x20 && *Position <= 0x7E))
    return Position + 1;

  // Check for valid UTF-8.
  if (uint8_t(*Position) & 0x80) {
    UTF8Decoded u8d = decodeUTF8(Position);
    if (   u8d.second != 0
        && u8d.first != 0xFEFF
        && ( u8d.first == 0x85
          || ( u8d.first >= 0xA0
            && u8d.first <= 0xD7FF)
          || ( u8d.first >= 0xE000
            && u8d.first <= 0xFFFD)
          || ( u8d.first >= 0x10000
            && u8d.first <= 0x10FFFF)))
      return Position + u8d.second;
  }
  return Position;
}

StringRef::iterator Scanner::skip_b_break(StringRef::iterator Position) {
  if (*Position == 0x0D) {
    if (Position + 1 != End && *(Position + 1) == 0x0A)
      return Position + 2;
    return Position + 1;
  }

  if (*Position == 0x0A)
    return Position + 1;
  return Position;
}


StringRef::iterator Scanner::skip_s_white(StringRef::iterator Position) {
  if (Position == End)
    return Position;
  if (*Position == ' ' || *Position == '\t')
    return Position + 1;
  return Position;
}

StringRef::iterator Scanner::skip_ns_char(StringRef::iterator Position) {
  if (Position == End)
    return Position;
  if (*Position == ' ' || *Position == '\t')
    return Position;
  return skip_nb_char(Position);
}

static bool is_ns_hex_digit(const char C) {
  return    (C >= '0' && C <= '9')
         || (C >= 'a' && C <= 'z')
         || (C >= 'A' && C <= 'Z');
}

static bool is_ns_word_char(const char C) {
  return    C == '-'
         || (C >= 'a' && C <= 'z')
         || (C >= 'A' && C <= 'Z');
}

StringRef Scanner::scan_ns_uri_char() {
  StringRef::iterator Start = Current;
  while (true) {
    if (Current == End)
      break;
    if ((   *Current == '%'
          && Current + 2 < End
          && is_ns_hex_digit(*(Current + 1))
          && is_ns_hex_digit(*(Current + 2)))
        || is_ns_word_char(*Current)
        || StringRef(Current, 1).find_first_of("#;/?:@&=+$,_.!~*'()[]")
          != StringRef::npos) {
      ++Current;
      ++Column;
    } else
      break;
  }
  return StringRef(Start, Current - Start);
}

StringRef Scanner::scan_ns_plain_one_line() {
  StringRef::iterator start = Current;
  // The first character must already be verified.
  ++Current;
  while (true) {
    if (Current == End) {
      break;
    } else if (*Current == ':') {
      // Check if the next character is a ns-char.
      if (Current + 1 == End)
        break;
      StringRef::iterator i = skip_ns_char(Current + 1);
      if (Current + 1 != i) {
        Current = i;
        Column += 2; // Consume both the ':' and ns-char.
      } else
        break;
    } else if (*Current == '#') {
      // Check if the previous character was a ns-char.
      // The & 0x80 check is to check for the trailing byte of a utf-8
      if (*(Current - 1) & 0x80 || skip_ns_char(Current - 1) == Current) {
        ++Current;
        ++Column;
      } else
        break;
    } else {
      StringRef::iterator i = skip_nb_char(Current);
      if (i == Current)
        break;
      Current = i;
      ++Column;
    }
  }
  return StringRef(start, Current - start);
}

bool Scanner::consume(uint32_t Expected) {
  if (Expected >= 0x80)
    report_fatal_error("Not dealing with this yet");
  if (Current == End)
    return false;
  if (uint8_t(*Current) >= 0x80)
    report_fatal_error("Not dealing with this yet");
  if (uint8_t(*Current) == Expected) {
    ++Current;
    ++Column;
    return true;
  }
  return false;
}

void Scanner::skip(uint32_t Distance) {
  Current += Distance;
  Column += Distance;
}

bool Scanner::isBlankOrBreak(StringRef::iterator Position) {
  if (Position == End)
    return false;
  if (   *Position == ' ' || *Position == '\t'
      || *Position == '\r' || *Position == '\n')
    return true;
  return false;
}

void Scanner::saveSimpleKeyCandidate( TokenQueueT::iterator Tok
                                    , unsigned AtColumn
                                    , bool IsRequired) {
  if (IsSimpleKeyAllowed) {
    SimpleKey SK;
    SK.Tok = Tok;
    SK.Line = Line;
    SK.Column = AtColumn;
    SK.IsRequired = IsRequired;
    SK.FlowLevel = FlowLevel;
    SimpleKeys.push_back(SK);
  }
}

void Scanner::removeStaleSimpleKeyCandidates() {
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

void Scanner::removeSimpleKeyCandidatesOnFlowLevel(unsigned Level) {
  if (!SimpleKeys.empty() && (SimpleKeys.end() - 1)->FlowLevel == Level)
    SimpleKeys.pop_back();
}

bool Scanner::unrollIndent(int ToColumn) {
  Token T;
  // Indentation is ignored in flow.
  if (FlowLevel != 0)
    return true;

  while (Indent > ToColumn) {
    T.Kind = Token::TK_BlockEnd;
    T.Range = StringRef(Current, 1);
    TokenQueue.push_back(T);
    Indent = Indents.pop_back_val();
  }

  return true;
}

bool Scanner::rollIndent( int ToColumn
                        , Token::TokenKind Kind
                        , TokenQueueT::iterator InsertPoint) {
  if (FlowLevel)
    return true;
  if (Indent < ToColumn) {
    Indents.push_back(Indent);
    Indent = ToColumn;

    Token T;
    T.Kind = Kind;
    T.Range = StringRef(Current, 0);
    TokenQueue.insert(InsertPoint, T);
  }
  return true;
}

void Scanner::scanToNextToken() {
  while (true) {
    while (*Current == ' ' || *Current == '\t') {
      skip(1);
    }

    // Skip comment.
    if (*Current == '#') {
      while (true) {
        // This may skip more than one byte, thus Column is only incremented
        // for code points.
        StringRef::iterator i = skip_nb_char(Current);
        if (i == Current)
          break;
        Current = i;
        ++Column;
      }
    }

    // Skip EOL.
    StringRef::iterator i = skip_b_break(Current);
    if (i == Current)
      break;
    Current = i;
    ++Line;
    Column = 0;
    // New lines may start a simple key.
    if (!FlowLevel)
      IsSimpleKeyAllowed = true;
  }
}

bool Scanner::scanStreamStart() {
  IsStartOfStream = false;

  EncodingInfo EI = getUnicodeEncoding(currentInput());

  Token T;
  T.Kind = Token::TK_StreamStart;
  T.Range = StringRef(Current, EI.second);
  TokenQueue.push_back(T);
  Current += EI.second;
  return true;
}

bool Scanner::scanStreamEnd() {
  // Force an ending new line if one isn't present.
  if (Column != 0) {
    Column = 0;
    ++Line;
  }

  unrollIndent(-1);
  SimpleKeys.clear();
  IsSimpleKeyAllowed = false;

  Token T;
  T.Kind = Token::TK_StreamEnd;
  T.Range = StringRef(Current, 0);
  TokenQueue.push_back(T);
  return true;
}

bool Scanner::scanDirective() {
  // Reset the indentation level.
  unrollIndent(-1);
  SimpleKeys.clear();
  IsSimpleKeyAllowed = false;

  StringRef::iterator Start = Current;
  consume('%');
  StringRef::iterator NameStart = Current;
  Current = skip_while<&Scanner::skip_ns_char>(Current);
  StringRef Name(NameStart, Current - NameStart);
  Current = skip_while<&Scanner::skip_s_white>(Current);

  if (Name == "YAML") {
    Current = skip_while<&Scanner::skip_ns_char>(Current);
    Token T;
    T.Kind = Token::TK_VersionDirective;
    T.Range = StringRef(Start, Current - Start);
    TokenQueue.push_back(T);
    return true;
  }
  return false;
}

bool Scanner::scanDocumentIndicator(bool IsStart) {
  unrollIndent(-1);
  SimpleKeys.clear();
  IsSimpleKeyAllowed = false;

  Token T;
  T.Kind = IsStart ? Token::TK_DocumentStart : Token::TK_DocumentEnd;
  T.Range = StringRef(Current, 3);
  skip(3);
  TokenQueue.push_back(T);
  return true;
}

bool Scanner::scanFlowCollectionStart(bool IsSequence) {
  Token T;
  T.Kind = IsSequence ? Token::TK_FlowSequenceStart
                      : Token::TK_FlowMappingStart;
  T.Range = StringRef(Current, 1);
  skip(1);
  TokenQueue.push_back(T);

  // [ and { may begin a simple key.
  saveSimpleKeyCandidate(TokenQueue.back(), Column - 1, false);

  // And may also be followed by a simple key.
  IsSimpleKeyAllowed = true;
  ++FlowLevel;
  return true;
}

bool Scanner::scanFlowCollectionEnd(bool IsSequence) {
  removeSimpleKeyCandidatesOnFlowLevel(FlowLevel);
  IsSimpleKeyAllowed = false;
  Token T;
  T.Kind = IsSequence ? Token::TK_FlowSequenceEnd
                      : Token::TK_FlowMappingEnd;
  T.Range = StringRef(Current, 1);
  skip(1);
  TokenQueue.push_back(T);
  if (FlowLevel)
    --FlowLevel;
  return true;
}

bool Scanner::scanFlowEntry() {
  removeSimpleKeyCandidatesOnFlowLevel(FlowLevel);
  IsSimpleKeyAllowed = true;
  Token T;
  T.Kind = Token::TK_FlowEntry;
  T.Range = StringRef(Current, 1);
  skip(1);
  TokenQueue.push_back(T);
  return true;
}

bool Scanner::scanBlockEntry() {
  rollIndent(Column, Token::TK_BlockSequenceStart, TokenQueue.end());
  removeSimpleKeyCandidatesOnFlowLevel(FlowLevel);
  IsSimpleKeyAllowed = true;
  Token T;
  T.Kind = Token::TK_BlockEntry;
  T.Range = StringRef(Current, 1);
  skip(1);
  TokenQueue.push_back(T);
  return true;
}

bool Scanner::scanKey() {
  if (!FlowLevel)
    rollIndent(Column, Token::TK_BlockMappingStart, TokenQueue.end());

  removeSimpleKeyCandidatesOnFlowLevel(FlowLevel);
  IsSimpleKeyAllowed = !FlowLevel;

  Token T;
  T.Kind = Token::TK_Key;
  T.Range = StringRef(Current, 1);
  skip(1);
  TokenQueue.push_back(T);
  return true;
}

bool Scanner::scanValue() {
  // If the previous token could have been a simple key, insert the key token
  // into the token queue.
  if (!SimpleKeys.empty()) {
    SimpleKey SK = SimpleKeys.pop_back_val();
    Token T;
    T.Kind = Token::TK_Key;
    T.Range = SK.Tok->Range;
    TokenQueueT::iterator i, e;
    for (i = TokenQueue.begin(), e = TokenQueue.end(); i != e; ++i) {
      if (i == SK.Tok)
        break;
    }
    assert(i != e && "SimpleKey not in token queue!");
    i = TokenQueue.insert(i, T);

    // We may also need to add a Block-Mapping-Start token.
    rollIndent(SK.Column, Token::TK_BlockMappingStart, i);

    IsSimpleKeyAllowed = false;
  } else {
    if (!FlowLevel)
      rollIndent(Column, Token::TK_BlockMappingStart, TokenQueue.end());
    IsSimpleKeyAllowed = !FlowLevel;
  }

  Token T;
  T.Kind = Token::TK_Value;
  T.Range = StringRef(Current, 1);
  skip(1);
  TokenQueue.push_back(T);
  return true;
}

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
  // We calculate the number of consecutive '\'s before the current position
  // by iterating backwards through our string.
  while (I >= First && *I == '\\') --I;
  // (Position - 1 - I) now contains the number of '\'s before the current
  // position. If it is odd, the character at 'Position' was escaped.
  return (Position - 1 - I) % 2 == 1;
}

bool Scanner::scanFlowScalar(bool IsDoubleQuoted) {
  StringRef::iterator Start = Current;
  unsigned ColStart = Column;
  if (IsDoubleQuoted) {
    do {
      ++Current;
      while (Current != End && *Current != '"')
        ++Current;
      // Repeat until the previous character was not a '\' or was an escaped
      // backslash.
    } while (*(Current - 1) == '\\' && wasEscaped(Start + 1, Current));
  } else {
    skip(1);
    while (true) {
      // Skip a ' followed by another '.
      if (Current + 1 < End && *Current == '\'' && *(Current + 1) == '\'') {
        skip(2);
        continue;
      } else if (*Current == '\'')
        break;
      StringRef::iterator i = skip_nb_char(Current);
      if (i == Current) {
        i = skip_b_break(Current);
        if (i == Current)
          break;
        Current = i;
        Column = 0;
        ++Line;
      } else {
        if (i == End)
          break;
        Current = i;
        ++Column;
      }
    }
  }
  skip(1); // Skip ending quote.
  Token T;
  T.Kind = Token::TK_Scalar;
  T.Range = StringRef(Start, Current - Start);
  TokenQueue.push_back(T);

  saveSimpleKeyCandidate(TokenQueue.back(), ColStart, false);

  IsSimpleKeyAllowed = false;

  return true;
}

bool Scanner::scanPlainScalar() {
  StringRef::iterator Start = Current;
  unsigned ColStart = Column;
  unsigned LeadingBlanks = 0;
  assert(Indent >= -1 && "Indent must be >= -1 !");
  unsigned indent = static_cast<unsigned>(Indent + 1);
  while (true) {
    if (*Current == '#')
      break;

    while (!isBlankOrBreak(Current)) {
      if (  FlowLevel && *Current == ':'
          && !(isBlankOrBreak(Current + 1) || *(Current + 1) == ',')) {
        setError("Found unexpected ':' while scanning a plain scalar", Current);
        return false;
      }

      // Check for the end of the plain scalar.
      if (  (*Current == ':' && isBlankOrBreak(Current + 1))
          || (  FlowLevel
          && (StringRef(Current, 1).find_first_of(",:?[]{}") != StringRef::npos)))
        break;

      StringRef::iterator i = skip_nb_char(Current);
      if (i == Current)
        break;
      Current = i;
      ++Column;
    }

    // Are we at the end?
    if (!isBlankOrBreak(Current))
      break;

    // Eat blanks.
    StringRef::iterator Tmp = Current;
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
        i = skip_b_break(Tmp);
        if (!LeadingBlanks)
          LeadingBlanks = 1;
        Tmp = i;
        Column = 0;
        ++Line;
      }
    }

    if (!FlowLevel && Column < indent)
      break;

    Current = Tmp;
  }
  if (Start == Current) {
    setError("Got empty plain scalar", Start);
    return false;
  }
  Token T;
  T.Kind = Token::TK_Scalar;
  T.Range = StringRef(Start, Current - Start);
  TokenQueue.push_back(T);

  // Plain scalars can be simple keys.
  saveSimpleKeyCandidate(TokenQueue.back(), ColStart, false);

  IsSimpleKeyAllowed = false;

  return true;
}

bool Scanner::scanAliasOrAnchor(bool IsAlias) {
  StringRef::iterator Start = Current;
  unsigned ColStart = Column;
  skip(1);
  while(true) {
    if (   *Current == '[' || *Current == ']'
        || *Current == '{' || *Current == '}'
        || *Current == ','
        || *Current == ':')
      break;
    StringRef::iterator i = skip_ns_char(Current);
    if (i == Current)
      break;
    Current = i;
    ++Column;
  }

  if (Start == Current) {
    setError("Got empty alias or anchor", Start);
    return false;
  }

  Token T;
  T.Kind = IsAlias ? Token::TK_Alias : Token::TK_Anchor;
  T.Range = StringRef(Start, Current - Start);
  TokenQueue.push_back(T);

  // Alias and anchors can be simple keys.
  saveSimpleKeyCandidate(TokenQueue.back(), ColStart, false);

  IsSimpleKeyAllowed = false;

  return true;
}

bool Scanner::scanBlockScalar(bool IsLiteral) {
  StringRef::iterator Start = Current;
  skip(1); // Eat | or >
  while(true) {
    StringRef::iterator i = skip_nb_char(Current);
    if (i == Current) {
      if (Column == 0)
        break;
      i = skip_b_break(Current);
      if (i != Current) {
        // We got a line break.
        Column = 0;
        ++Line;
        Current = i;
        continue;
      } else {
        // There was an error, which should already have been printed out.
        return false;
      }
    }
    Current = i;
    ++Column;
  }

  if (Start == Current) {
    setError("Got empty block scalar", Start);
    return false;
  }

  Token T;
  T.Kind = Token::TK_Scalar;
  T.Range = StringRef(Start, Current - Start);
  TokenQueue.push_back(T);
  return true;
}

bool Scanner::scanTag() {
  StringRef::iterator Start = Current;
  unsigned ColStart = Column;
  skip(1); // Eat !.
  if (Current == End || isBlankOrBreak(Current)); // An empty tag.
  else if (*Current == '<') {
    skip(1);
    scan_ns_uri_char();
    if (!consume('>'))
      return false;
  } else {
    // FIXME: Actually parse the c-ns-shorthand-tag rule.
    Current = skip_while<&Scanner::skip_ns_char>(Current);
  }

  Token T;
  T.Kind = Token::TK_Tag;
  T.Range = StringRef(Start, Current - Start);
  TokenQueue.push_back(T);

  // Tags can be simple keys.
  saveSimpleKeyCandidate(TokenQueue.back(), ColStart, false);

  IsSimpleKeyAllowed = false;

  return true;
}

bool Scanner::fetchMoreTokens() {
  if (IsStartOfStream)
    return scanStreamStart();

  scanToNextToken();

  if (Current == End)
    return scanStreamEnd();

  removeStaleSimpleKeyCandidates();

  unrollIndent(Column);

  if (Column == 0 && *Current == '%')
    return scanDirective();

  if (Column == 0 && Current + 4 <= End
      && *Current == '-'
      && *(Current + 1) == '-'
      && *(Current + 2) == '-'
      && (Current + 3 == End || isBlankOrBreak(Current + 3)))
    return scanDocumentIndicator(true);

  if (Column == 0 && Current + 4 <= End
      && *Current == '.'
      && *(Current + 1) == '.'
      && *(Current + 2) == '.'
      && (Current + 3 == End || isBlankOrBreak(Current + 3)))
    return scanDocumentIndicator(false);

  if (*Current == '[')
    return scanFlowCollectionStart(true);

  if (*Current == '{')
    return scanFlowCollectionStart(false);

  if (*Current == ']')
    return scanFlowCollectionEnd(true);

  if (*Current == '}')
    return scanFlowCollectionEnd(false);

  if (*Current == ',')
    return scanFlowEntry();

  if (*Current == '-' && isBlankOrBreak(Current + 1))
    return scanBlockEntry();

  if (*Current == '?' && (FlowLevel || isBlankOrBreak(Current + 1)))
    return scanKey();

  if (*Current == ':' && (FlowLevel || isBlankOrBreak(Current + 1)))
    return scanValue();

  if (*Current == '*')
    return scanAliasOrAnchor(true);

  if (*Current == '&')
    return scanAliasOrAnchor(false);

  if (*Current == '!')
    return scanTag();

  if (*Current == '|' && !FlowLevel)
    return scanBlockScalar(true);

  if (*Current == '>' && !FlowLevel)
    return scanBlockScalar(false);

  if (*Current == '\'')
    return scanFlowScalar(false);

  if (*Current == '"')
    return scanFlowScalar(true);

  // Get a plain scalar.
  StringRef FirstChar(Current, 1);
  if (!(isBlankOrBreak(Current)
        || FirstChar.find_first_of("-?:,[]{}#&*!|>'\"%@`") != StringRef::npos)
      || (*Current == '-' && !isBlankOrBreak(Current + 1))
      || (!FlowLevel && (*Current == '?' || *Current == ':')
          && isBlankOrBreak(Current + 1))
      || (!FlowLevel && *Current == ':'
                      && Current + 2 < End
                      && *(Current + 1) == ':'
                      && !isBlankOrBreak(Current + 2)))
    return scanPlainScalar();

  setError("Unrecognized character while tokenizing.");
  return false;
}

Stream::Stream(StringRef Input, SourceMgr &SM)
  : scanner(new Scanner(Input, SM))
  , CurrentDoc(0) {}

Stream::~Stream() { delete CurrentDoc; }

bool Stream::failed() { return scanner->failed(); }

void Stream::handleYAMLDirective(const Token &t) {
  // TODO: Ensure version is 1.x.
}

document_iterator Stream::begin() {
  if (CurrentDoc)
    report_fatal_error("Can only iterate over the stream once");

  // Skip Stream-Start.
  scanner->getNext();

  CurrentDoc = new Document(*this);
  return document_iterator(CurrentDoc);
}

document_iterator Stream::end() {
  return document_iterator();
}

void Stream::skip() {
  for (document_iterator i = begin(), e = end(); i != e; ++i)
    i->skip();
}

Node::Node(unsigned int Type, Document *D, StringRef A)
  : Doc(D)
  , TypeID(Type)
  , Anchor(A)
{}

Node::~Node() {}

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

void Node::setError(const Twine &Msg, Token &Tok) const {
  Doc->setError(Msg, Tok);
}

bool Node::failed() const {
  return Doc->failed();
}

StringRef ScalarNode::getValue(SmallVectorImpl<char> &Storage) const {
  // TODO: Handle newlines properly. We need to remove leading whitespace.
  if (Value[0] == '"') { // Double quoted.
    // Search for characters that would require rebuilding the value.
    StringRef UnquotedValue = Value.substr(1, Value.size() - 2);
    StringRef::size_type i = UnquotedValue.find_first_of("\\\r\n");
    if (i != StringRef::npos) {
      // Use Storage to build proper value.
      Storage.clear();
      Storage.reserve(UnquotedValue.size());
      for (; i != StringRef::npos; i = UnquotedValue.find_first_of("\\\r\n")) {
        // Insert all previous chars into Storage.
        StringRef Valid(UnquotedValue.begin(), i);
        Storage.insert(Storage.end(), Valid.begin(), Valid.end());
        // Chop off inserted chars.
        UnquotedValue = UnquotedValue.substr(i);

        assert(!UnquotedValue.empty() && "Can't be empty!");

        // Parse escape or line break.
        switch (UnquotedValue[0]) {
        case '\r':
        case '\n':
          Storage.push_back('\n');
          if (   UnquotedValue.size() > 1
              && (UnquotedValue[1] == '\r' || UnquotedValue[1] == '\n'))
            UnquotedValue = UnquotedValue.substr(1);
          UnquotedValue = UnquotedValue.substr(1);
          break;
        default:
          if (UnquotedValue.size() == 1)
            // TODO: Report error.
            break;
          UnquotedValue = UnquotedValue.substr(1);
          switch (UnquotedValue[0]) {
          default: {
              Token T;
              T.Range = StringRef(UnquotedValue.begin(), 1);
              setError("Unrecognized escape code!", T);
              return "";
            }
          case '\r':
          case '\n':
            // Remove the new line.
            if (   UnquotedValue.size() > 1
                && (UnquotedValue[1] == '\r' || UnquotedValue[1] == '\n'))
              UnquotedValue = UnquotedValue.substr(1);
            // If this was just a single byte newline, it will get skipped
            // below.
            break;
          case '0':
            Storage.push_back(0x00);
            break;
          case 'a':
            Storage.push_back(0x07);
            break;
          case 'b':
            Storage.push_back(0x08);
            break;
          case 't':
          case 0x09:
            Storage.push_back(0x09);
            break;
          case 'n':
            Storage.push_back(0x0A);
            break;
          case 'v':
            Storage.push_back(0x0B);
            break;
          case 'f':
            Storage.push_back(0x0C);
            break;
          case 'r':
            Storage.push_back(0x0D);
            break;
          case 'e':
            Storage.push_back(0x1B);
            break;
          case ' ':
            Storage.push_back(0x20);
            break;
          case '"':
            Storage.push_back(0x22);
            break;
          case '/':
            Storage.push_back(0x2F);
            break;
          case '\\':
            Storage.push_back(0x5C);
            break;
          case 'N': {
              SmallString<4> Val = encodeUTF8(0x85);
              Storage.insert(Storage.end(), Val.begin(), Val.end());
              break;
            }
          case '_': {
              SmallString<4> Val = encodeUTF8(0xA0);
              Storage.insert(Storage.end(), Val.begin(), Val.end());
              break;
            }
          case 'L': {
              SmallString<4> Val = encodeUTF8(0x2028);
              Storage.insert(Storage.end(), Val.begin(), Val.end());
              break;
            }
          case 'P': {
              SmallString<4> Val = encodeUTF8(0x2029);
              Storage.insert(Storage.end(), Val.begin(), Val.end());
              break;
            }
          case 'x': {
              if (UnquotedValue.size() < 3)
                // TODO: Report error.
                break;
              unsigned int UnicodeScalarValue;
              UnquotedValue.substr(1, 2).getAsInteger(16, UnicodeScalarValue);
              SmallString<4> Val = encodeUTF8(UnicodeScalarValue);
              Storage.insert(Storage.end(), Val.begin(), Val.end());
              UnquotedValue = UnquotedValue.substr(2);
              break;
            }
          case 'u': {
              if (UnquotedValue.size() < 5)
                // TODO: Report error.
                break;
              unsigned int UnicodeScalarValue;
              UnquotedValue.substr(1, 4).getAsInteger(16, UnicodeScalarValue);
              SmallString<4> Val = encodeUTF8(UnicodeScalarValue);
              Storage.insert(Storage.end(), Val.begin(), Val.end());
              UnquotedValue = UnquotedValue.substr(4);
              break;
            }
          case 'U': {
              if (UnquotedValue.size() < 9)
                // TODO: Report error.
                break;
              unsigned int UnicodeScalarValue;
              UnquotedValue.substr(1, 8).getAsInteger(16, UnicodeScalarValue);
              SmallString<4> Val = encodeUTF8(UnicodeScalarValue);
              Storage.insert(Storage.end(), Val.begin(), Val.end());
              UnquotedValue = UnquotedValue.substr(8);
              break;
            }
          }
          UnquotedValue = UnquotedValue.substr(1);
        }
      }
      Storage.insert(Storage.end(), UnquotedValue.begin(), UnquotedValue.end());
      return StringRef(Storage.begin(), Storage.size());
    }
    return UnquotedValue;
  } else if (Value[0] == '\'') { // Single quoted.
    StringRef UnquotedValue = Value.substr(1, Value.size() - 2);
    StringRef::size_type i = UnquotedValue.find('\'');
    if (i != StringRef::npos) {
      // We're going to need Storage.
      Storage.clear();
      Storage.reserve(UnquotedValue.size());
      for (; i != StringRef::npos; i = UnquotedValue.find('\'')) {
        StringRef Valid(UnquotedValue.begin(), i);
        Storage.insert(Storage.end(), Valid.begin(), Valid.end());
        Storage.push_back('\'');
        UnquotedValue = UnquotedValue.substr(i + 2);
      }
      Storage.insert(Storage.end(), UnquotedValue.begin(), UnquotedValue.end());
      return StringRef(Storage.begin(), Storage.size());
    }
    return UnquotedValue;
  }
  // Plain or block.
  return Value;
}

Node *KeyValueNode::getKey() {
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

Node *KeyValueNode::getValue() {
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

void MappingNode::increment() {
  if (failed()) {
    IsAtEnd = true;
    CurrentEntry = 0;
    return;
  }
  if (CurrentEntry) {
    CurrentEntry->skip();
    if (Type == MT_Inline) {
      IsAtEnd = true;
      CurrentEntry = 0;
      return;
    }
  }
  Token T = peekNext();
  if (T.Kind == Token::TK_Key || T.Kind == Token::TK_Scalar) {
    // KeyValueNode eats the TK_Key. That way it can detect null keys.
    CurrentEntry = new (getAllocator().Allocate<KeyValueNode>())
      KeyValueNode(Doc);
  } else if (Type == MT_Block) {
    switch (T.Kind) {
    case Token::TK_BlockEnd:
      getNext();
      IsAtEnd = true;
      CurrentEntry = 0;
      break;
    default:
      setError("Unexpected token. Expected Key or Block End", T);
    case Token::TK_Error:
      IsAtEnd = true;
      CurrentEntry = 0;
    }
  } else {
    switch (T.Kind) {
    case Token::TK_FlowEntry:
      // Eat the flow entry and recurse.
      getNext();
      return increment();
    case Token::TK_FlowMappingEnd:
      getNext();
    case Token::TK_Error:
      // Set this to end iterator.
      IsAtEnd = true;
      CurrentEntry = 0;
      break;
    default:
      setError( "Unexpected token. Expected Key, Flow Entry, or Flow "
                "Mapping End."
              , T);
      IsAtEnd = true;
      CurrentEntry = 0;
    }
  }
}

void SequenceNode::increment() {
  if (failed()) {
    IsAtEnd = true;
    CurrentEntry = 0;
    return;
  }
  if (CurrentEntry)
    CurrentEntry->skip();
  Token T = peekNext();
  if (SeqType == ST_Block) {
    switch (T.Kind) {
    case Token::TK_BlockEntry:
      getNext();
      CurrentEntry = parseBlockNode();
      if (CurrentEntry == 0) { // An error occurred.
        IsAtEnd = true;
        CurrentEntry = 0;
      }
      break;
    case Token::TK_BlockEnd:
      getNext();
      IsAtEnd = true;
      CurrentEntry = 0;
      break;
    default:
      setError( "Unexpected token. Expected Block Entry or Block End."
              , T);
    case Token::TK_Error:
      IsAtEnd = true;
      CurrentEntry = 0;
    }
  } else if (SeqType == ST_Indentless) {
    switch (T.Kind) {
    case Token::TK_BlockEntry:
      getNext();
      CurrentEntry = parseBlockNode();
      if (CurrentEntry == 0) { // An error occurred.
        IsAtEnd = true;
        CurrentEntry = 0;
      }
      break;
    default:
    case Token::TK_Error:
      IsAtEnd = true;
      CurrentEntry = 0;
    }
  } else if (SeqType == ST_Flow) {
    switch (T.Kind) {
    case Token::TK_FlowEntry:
      // Eat the flow entry and recurse.
      getNext();
      WasPreviousTokenFlowEntry = true;
      return increment();
    case Token::TK_FlowSequenceEnd:
      getNext();
    case Token::TK_Error:
      // Set this to end iterator.
      IsAtEnd = true;
      CurrentEntry = 0;
      break;
    case Token::TK_StreamEnd:
    case Token::TK_DocumentEnd:
    case Token::TK_DocumentStart:
      setError("Could not find closing ]!", T);
      // Set this to end iterator.
      IsAtEnd = true;
      CurrentEntry = 0;
      break;
    default:
      if (!WasPreviousTokenFlowEntry) {
        setError("Expected , between entries!", T);
        IsAtEnd = true;
        CurrentEntry = 0;
        break;
      }
      // Otherwise it must be a flow entry.
      CurrentEntry = parseBlockNode();
      if (!CurrentEntry) {
        IsAtEnd = true;
      }
      WasPreviousTokenFlowEntry = false;
      break;
    }
  }
}

Document::Document(Stream &S) : stream(S), Root(0) {
  if (parseDirectives())
    expectToken(Token::TK_DocumentStart);
  Token &T = peekNext();
  if (T.Kind == Token::TK_DocumentStart)
    getNext();
}

bool Document::skip()  {
  if (stream.scanner->failed())
    return false;
  if (!Root)
    getRoot();
  Root->skip();
  Token &T = peekNext();
  if (T.Kind == Token::TK_StreamEnd)
    return false;
  if (T.Kind == Token::TK_DocumentEnd) {
    getNext();
    return skip();
  }
  return true;
}

Token &Document::peekNext() {
  return stream.scanner->peekNext();
}

Token Document::getNext() {
  return stream.scanner->getNext();
}

void Document::setError(const Twine &Message, Token &Location) const {
  stream.scanner->setError(Message, Location.Range.begin());
}

bool Document::failed() const {
  return stream.scanner->failed();
}

Node *Document::parseBlockNode() {
  Token T = peekNext();
  // Handle properties.
  Token AnchorInfo;
parse_property:
  switch (T.Kind) {
  case Token::TK_Alias:
    getNext();
    return new (NodeAllocator.Allocate<AliasNode>())
      AliasNode(this, T.Range.substr(1));
  case Token::TK_Anchor:
    if (AnchorInfo.Kind == Token::TK_Anchor) {
      setError("Already encountered an anchor for this node!", T);
      return 0;
    }
    AnchorInfo = getNext(); // Consume TK_Anchor.
    T = peekNext();
    goto parse_property;
  case Token::TK_Tag:
    getNext(); // Skip TK_Tag.
    T = peekNext();
    goto parse_property;
  default:
    break;
  }

  switch (T.Kind) {
  case Token::TK_BlockEntry:
    // We got an unindented BlockEntry sequence. This is not terminated with
    // a BlockEnd.
    // Don't eat the TK_BlockEntry, SequenceNode needs it.
    return new (NodeAllocator.Allocate<SequenceNode>())
      SequenceNode( this
                  , AnchorInfo.Range.substr(1)
                  , SequenceNode::ST_Indentless);
  case Token::TK_BlockSequenceStart:
    getNext();
    return new (NodeAllocator.Allocate<SequenceNode>())
      SequenceNode(this, AnchorInfo.Range.substr(1), SequenceNode::ST_Block);
  case Token::TK_BlockMappingStart:
    getNext();
    return new (NodeAllocator.Allocate<MappingNode>())
      MappingNode(this, AnchorInfo.Range.substr(1), MappingNode::MT_Block);
  case Token::TK_FlowSequenceStart:
    getNext();
    return new (NodeAllocator.Allocate<SequenceNode>())
      SequenceNode(this, AnchorInfo.Range.substr(1), SequenceNode::ST_Flow);
  case Token::TK_FlowMappingStart:
    getNext();
    return new (NodeAllocator.Allocate<MappingNode>())
      MappingNode(this, AnchorInfo.Range.substr(1), MappingNode::MT_Flow);
  case Token::TK_Scalar:
    getNext();
    return new (NodeAllocator.Allocate<ScalarNode>())
      ScalarNode(this, AnchorInfo.Range.substr(1), T.Range);
  case Token::TK_Key:
    // Don't eat the TK_Key, KeyValueNode expects it.
    return new (NodeAllocator.Allocate<MappingNode>())
      MappingNode(this, AnchorInfo.Range.substr(1), MappingNode::MT_Inline);
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

bool Document::parseDirectives() {
  bool isDirective = false;
  while (true) {
    Token T = peekNext();
    if (T.Kind == Token::TK_TagDirective) {
      handleTagDirective(getNext());
      isDirective = true;
    } else if (T.Kind == Token::TK_VersionDirective) {
      stream.handleYAMLDirective(getNext());
      isDirective = true;
    } else
      break;
  }
  return isDirective;
}

bool Document::expectToken(int TK) {
  Token T = getNext();
  if (T.Kind != TK) {
    setError("Unexpected token", T);
    return false;
  }
  return true;
}

Document *document_iterator::NullDoc = 0;
