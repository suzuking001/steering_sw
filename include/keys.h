#pragma once
#include <cstdint>
#include <cstddef>
namespace sw {
inline bool asciiKey(unsigned char ch, uint8_t& key, uint8_t& modifiers) {
  modifiers = 0;
  if (ch >= 'a' && ch <= 'z') { key = 4 + ch - 'a'; return true; }
  if (ch >= 'A' && ch <= 'Z') { key = 4 + ch - 'A'; modifiers = 2; return true; }
  if (ch >= '1' && ch <= '9') { key = 30 + ch - '1'; return true; }
  if (ch == '0') { key = 39; return true; }
  const char* plain = " -=[]\\;'`,./";
  const uint8_t keys[] = {44,45,46,47,48,49,51,52,53,54,55,56};
  for (size_t i = 0; plain[i]; ++i) if (ch == plain[i]) { key = keys[i]; return true; }
  const char* shifted = "!@#$%^&*()_+{}|:\"~<>?";
  const uint8_t shiftKeys[] = {30,31,32,33,34,35,36,37,38,39,45,46,47,48,49,51,52,53,54,55,56};
  for (size_t i = 0; shifted[i]; ++i) if (ch == shifted[i]) { key = shiftKeys[i]; modifiers = 2; return true; }
  return false;
}
}
