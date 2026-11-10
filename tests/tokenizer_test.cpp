#include "core/tokenizer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

std::vector<std::string> Tokens(std::string_view text) {
  return search::Tokenize(text);
}

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
