#pragma once

#include <string>

// Compute the MD5 digest of `input` and return it as a 32-char lowercase hex
// string. Self-contained (no external crypto dependency).
std::string md5Hex(const std::string& input);
