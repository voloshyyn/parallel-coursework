#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace search {
namespace detail {

// Whether a complete sequence of `length` bytes encodes a legal code point.
// Rejects the three classic holes of form-only validation: overlong encodings,
// UTF-16 surrogates and code points above U+10FFFF.
inline bool IsValidCodePoint(char32_t code_point, std::size_t length) noexcept {
  switch (length) {
    case 1:
      return true;
    case 2:
      return code_point >= 0x80U;
    case 3:
      return code_point >= 0x800U &&
             (code_point < 0xD800U || code_point > 0xDFFFU);
    case 4:
      return code_point >= 0x10000U && code_point <= 0x10FFFFU;
    default:
      return false;
  }
}

// Decodes the UTF-8 sequence starting at `pos` and advances past it. Invalid
// input - a forbidden lead byte, a missing or wrong continuation byte, an
// overlong encoding, a surrogate or a value above U+10FFFF - yields nothing and
// consumes exactly one byte, so binary junk acts as a separator instead of
// gluing words together.
inline std::optional<char32_t> DecodeUtf8(std::string_view text,
                                          std::size_t& pos) {
  const unsigned char first = static_cast<unsigned char>(text[pos]);
  char32_t code_point = 0;
  std::size_t length = 0;
  if (first < 0x80U) {
    code_point = first;
    length = 1;
  } else if (first >= 0xC2U && first <= 0xDFU) {
    // C0 and C1 are always overlong; the shortest form starts at C2.
    code_point = first & 0x1FU;
    length = 2;
  } else if (first >= 0xE0U && first <= 0xEFU) {
    code_point = first & 0x0FU;
    length = 3;
  } else if (first >= 0xF0U && first <= 0xF4U) {
    // F5..FF would encode values above U+10FFFF.
    code_point = first & 0x07U;
    length = 4;
  } else {
    ++pos;
    return std::nullopt;
  }

  if (pos + length > text.size()) {
    ++pos;
    return std::nullopt;
  }
  for (std::size_t i = 1; i < length; ++i) {
    const unsigned char continuation =
        static_cast<unsigned char>(text[pos + i]);
    if ((continuation & 0xC0U) != 0x80U) {
      ++pos;
      return std::nullopt;
    }
    code_point = (code_point << 6) | (continuation & 0x3FU);
  }
  if (!IsValidCodePoint(code_point, length)) {
    ++pos;
    return std::nullopt;
  }
  pos += length;
  return code_point;
}

// UTF-8 encoding of a code point.
inline void AppendUtf8(char32_t code_point, std::string& out) {
  if (code_point < 0x80U) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else if (code_point < 0x10000U) {
    out.push_back(static_cast<char>(0xE0U | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 12) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  }
}

// Whether a code point may appear inside a token.
inline bool IsWordCodePoint(char32_t code_point) noexcept {
  if (code_point < 0x80U) {
    return (code_point >= '0' && code_point <= '9') ||
           (code_point >= 'a' && code_point <= 'z') ||
           (code_point >= 'A' && code_point <= 'Z');
  }
  // Punctuation and symbols that sit between words must stay separators:
  // dashes and curly quotes from Gutenberg texts (U+2000..U+206F), the Latin-1
  // punctuation block, the multiplication and division signs, and CJK
  // punctuation. Everything else counts as a letter, which keeps non-English
  // words such as "café" in one piece without pulling in ICU.
  if (code_point >= 0x2000U && code_point <= 0x206FU) return false;
  if (code_point >= 0x2E00U && code_point <= 0x2E7FU) return false;
  if (code_point >= 0x3000U && code_point <= 0x303FU) return false;
  if (code_point >= 0x00A0U && code_point <= 0x00BFU) return false;
  if (code_point == 0x00D7U || code_point == 0x00F7U) return false;
  return true;
}

// Folds the case of ASCII and Cyrillic (А-Я, Ѐ-Џ with Є, І, Ї, Ё, and Ґ).
// The dataset is English, but a search query may be Ukrainian: "Дніпро" must
// match "дніпро".
inline char32_t LowerCodePoint(char32_t code_point) noexcept {
  if (code_point >= 'A' && code_point <= 'Z') return code_point + 0x20U;
  if (code_point >= 0x0410U && code_point <= 0x042FU) return code_point + 0x20U;
  if (code_point >= 0x0400U && code_point <= 0x040FU) return code_point + 0x50U;
  if (code_point == 0x0490U) return 0x0491U;
  return code_point;
}

// Calls `fn(const std::string&, std::size_t)` for every token of `text`, where
// the second argument is the byte offset of the token in `text`. Returning
// false from `fn` stops the iteration, so a caller that only needs the first
// match does not scan the rest of the document. Reuses one buffer, so indexing
// a large document does not allocate per token. The string passed to `fn` is
// only valid during the call; copy it to keep it.
template <typename Fn>
void ForEachTokenAt(std::string_view text, Fn&& fn) {
  std::string buffer;
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t token_start = pos;
    const std::optional<char32_t> code_point = DecodeUtf8(text, pos);
    if (!code_point || !IsWordCodePoint(*code_point)) {
      continue;
    }

    buffer.clear();
    AppendUtf8(LowerCodePoint(*code_point), buffer);
    while (pos < text.size()) {
      std::size_t next = pos;
      const std::optional<char32_t> following = DecodeUtf8(text, next);
      if (!following || !IsWordCodePoint(*following)) {
        break;
      }
      AppendUtf8(LowerCodePoint(*following), buffer);
      pos = next;
    }
    if (!fn(buffer, token_start)) {
      return;
    }
  }
}

// Calls `fn(const std::string&)` for every token of `text`.
template <typename Fn>
void ForEachToken(std::string_view text, Fn&& fn) {
  ForEachTokenAt(text, [&fn](const std::string& token, std::size_t) {
    fn(token);
    return true;
  });
}

}  // namespace detail

// Splits text into lowercase tokens: maximal runs of letters and digits, where
// "letter" covers non-ASCII code points that are not punctuation. Everything
// else (spaces, punctuation, symbols) separates tokens. No stemming and no
// stop-word removal.
std::vector<std::string> Tokenize(std::string_view text);

}  // namespace search
