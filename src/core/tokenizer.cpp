#include "core/tokenizer.h"

namespace search {

std::vector<std::string> Tokenize(std::string_view text) {
  std::vector<std::string> tokens;
  detail::ForEachToken(
      text, [&tokens](const std::string& token) { tokens.push_back(token); });
  return tokens;
}

}  // namespace search
