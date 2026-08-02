#pragma once

#include <Arduino.h>

namespace ProjectIdentity {
const char *version();
const char *productId();
const char *buildId();
String primaryCredit();
String backupCredit();
uint32_t hashText(const String &text);
}
