#include "core/tokenizer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

std::vector<std::string> Tokens(std::string_view text) {
  return search::Tokenize(text);
}

// Builds a byte string from explicit values. Avoids "\xNN" escapes, which
// would swallow the following hex digit into the same escape.
std::string Bytes(const std::vector<int>& bytes) {
  std::string result;
  result.reserve(bytes.size());
  for (int byte : bytes) {
    result.push_back(static_cast<char>(static_cast<unsigned char>(byte)));
  }
  return result;
}

struct InvalidUtf8 {
  const char* description;
  std::vector<int> bytes;
};

}  // namespace

TEST(Tokenizer, LowercasesAscii) {
  EXPECT_EQ((std::vector<std::string>{"hello", "world"}), Tokens("Hello, WORLD!"));
}

TEST(Tokenizer, SplitsOnPunctuationAndSymbols) {
  EXPECT_EQ((std::vector<std::string>{"foo", "bar", "baz"}),
            Tokens("foo-bar_baz."));
  EXPECT_EQ((std::vector<std::string>{"don", "t"}),
            Tokens("don't"));
  EXPECT_EQ((std::vector<std::string>{"c", "20"}), Tokens("C++20"));
}

TEST(Tokenizer, KeepsDigitsInsideTokens) {
  EXPECT_EQ((std::vector<std::string>{"utf8", "b2b", "42"}), Tokens("utf8 b2b 42"));
}

TEST(Tokenizer, CollapsesRepeatedSeparators) {
  EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), Tokens("  a \t\n b,,,c  "));
}

TEST(Tokenizer, ReturnsNothingForEmptyOrPunctuationOnlyText) {
  EXPECT_TRUE(Tokens("").empty());
  EXPECT_TRUE(Tokens("   \n\t").empty());
  EXPECT_TRUE(Tokens("?!.,;:-—").empty());
}

TEST(Tokenizer, KeepsMultiByteWordsIntact) {
  EXPECT_EQ((std::vector<std::string>{"дніпро", "київ", "café"}),
            Tokens("дніпро київ café"));
}

TEST(Tokenizer, LowercasesCyrillic) {
  EXPECT_EQ((std::vector<std::string>{"дніпро", "ґава", "їжак", "єнот"}),
            Tokens("ДНІПРО ҐАВА ЇЖАК ЄНОТ"));
}

TEST(Tokenizer, TreatsNonAsciiPunctuationAsSeparator) {
  EXPECT_EQ((std::vector<std::string>{"word", "word"}),
            Tokens("word—word"));
  EXPECT_EQ((std::vector<std::string>{"слово", "слово"}),
            Tokens("«слово» — слово"));
  EXPECT_TRUE(Tokens("—–…“”").empty());
}

TEST(Tokenizer, LeavesUnknownMultiByteLettersUnchanged) {
  // Greek is outside the folded ranges: the code points must survive untouched.
  EXPECT_EQ((std::vector<std::string>{"ΑΒΓ"}), Tokens("ΑΒΓ"));
}

TEST(Tokenizer, TreatsInvalidUtf8AsSeparator) {
  const std::string text = std::string("ab") + '\xFF' + "cd";
  EXPECT_EQ((std::vector<std::string>{"ab", "cd"}), Tokens(text));
  EXPECT_TRUE(Tokens(std::string(1, '\xFF')).empty());
}

TEST(Tokenizer, RejectsInvalidUtf8Sequences) {
  const std::vector<InvalidUtf8> cases = {
      {"overlong two-byte encoding of 'A' (C1 81)", {0xC1, 0x81}},
      {"overlong three-byte encoding (E0 81 81)", {0xE0, 0x81, 0x81}},
      {"surrogate U+D800 (ED A0 80)", {0xED, 0xA0, 0x80}},
      {"value above U+10FFFF (F4 90 80 80)", {0xF4, 0x90, 0x80, 0x80}},
      {"forbidden lead byte F5", {0xF5}},
      {"forbidden lead byte FF", {0xFF}},
      {"lone continuation byte 80", {0x80}},
      {"wrong continuation byte (E2 28 A1)", {0xE2, 0x28, 0xA1}},
      {"truncated two-byte sequence (E2)", {0xE2}},
      {"truncated three-byte sequence (F0 9F 98)", {0xF0, 0x9F, 0x98}},
  };

  for (const InvalidUtf8& invalid : cases) {
    SCOPED_TRACE(invalid.description);
    const std::string text = "ab" + Bytes(invalid.bytes) + "cd";
    EXPECT_EQ((std::vector<std::string>{"ab", "cd"}), Tokens(text));
  }
}

TEST(Tokenizer, TreatsTruncatedUtf8AtEndOfTextAsSeparator) {
  const std::vector<InvalidUtf8> cases = {
      {"truncated two-byte sequence (E2 82)", {0xE2, 0x82}},
      {"truncated four-byte sequence (F0 9F 98)", {0xF0, 0x9F, 0x98}},
  };

  for (const InvalidUtf8& invalid : cases) {
    SCOPED_TRACE(invalid.description);
    const std::string text = "ab" + Bytes(invalid.bytes);
    EXPECT_EQ((std::vector<std::string>{"ab"}), Tokens(text));
  }
}

TEST(Tokenizer, AcceptsValidUtf8BoundarySequences) {
  const std::vector<InvalidUtf8> cases = {
      {"U+0080 (C2 80)", {0xC2, 0x80}},
      {"U+07FF (DF BF)", {0xDF, 0xBF}},
      {"U+D7FF (ED 9F BF)", {0xED, 0x9F, 0xBF}},
      {"U+E000 (EE 80 80)", {0xEE, 0x80, 0x80}},
      {"U+FFFD (EF BF BD)", {0xEF, 0xBF, 0xBD}},
      {"U+10000 (F0 90 80 80)", {0xF0, 0x90, 0x80, 0x80}},
      {"U+10FFFF (F4 8F BF BF)", {0xF4, 0x8F, 0xBF, 0xBF}},
  };

  for (const InvalidUtf8& valid : cases) {
    SCOPED_TRACE(valid.description);
    const std::string sequence = Bytes(valid.bytes);
    const std::string text = "ab" + sequence + "cd";
    // A valid non-punctuation code point is part of a word, and nothing may be
    // re-encoded or dropped.
    EXPECT_EQ((std::vector<std::string>{text}), Tokens(text));
  }
}
