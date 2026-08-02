#include "project_identity.h"

namespace {
constexpr uint8_t kPrimaryKey = 0x5A;
constexpr uint8_t kPrimaryData[] = {
    188, 205, 250, 189, 206, 242, 178, 222, 203, 188, 238,
    196, 189, 250, 206, 189, 243, 236, 188, 211, 218, 190,
    224, 214, 188, 246, 251, 191, 230, 218, 191, 213, 203};

String decodePrimary() {
  String text;
  text.reserve(sizeof(kPrimaryData));
  for (uint8_t value : kPrimaryData) text += char(value ^ kPrimaryKey);
  return text;
}
}

namespace ProjectIdentity {
const char *version() { return "v1.0"; }
const char *productId() { return "WYL-ESP32S3-PIXEL-CLOCK"; }
const char *buildId() { return "20260802-01"; }
String primaryCredit() { return decodePrimary(); }
uint32_t hashText(const String &text) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < text.length(); ++i) {
    hash ^= static_cast<uint8_t>(text[i]);
    hash *= 16777619UL;
  }
  return hash;
}
}
