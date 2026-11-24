#pragma once
#include <string>

namespace lumen {

// Implementation of the classic Porter (1980) stemming algorithm.
// Input must be lowercase ASCII letters; anything else is returned unchanged.
std::string porterStem(const std::string& word);

}  // namespace lumen
