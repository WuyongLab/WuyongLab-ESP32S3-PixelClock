#include "integrity_guard.h"
#include "project_identity.h"

namespace {
constexpr uint32_t kExpectedCreditHash = 0x5A92B6AFUL;
constexpr uint8_t kBackupData[] = {
    176, 111, 118, 191, 98, 126, 190, 18, 101, 176, 66,
    104, 191, 118, 98, 191, 125, 64, 176, 29, 22, 178,
    76, 26, 176, 122, 117, 177, 74, 22, 177, 103, 101};

String decodeBackup() {
  String text;
  text.reserve(sizeof(kBackupData));
  for (uint8_t value : kBackupData) {
    text += char(static_cast<uint8_t>((value ^ 0xA7U) - 0x31U));
  }
  return text;
}
}

namespace ProjectIdentity {
String backupCredit() { return decodeBackup(); }
}

namespace IntegrityGuard {
bool identityValid() {
  const String primary = ProjectIdentity::primaryCredit();
  const String backup = ProjectIdentity::backupCredit();
  return ProjectIdentity::hashText(primary) == kExpectedCreditHash &&
         ProjectIdentity::hashText(backup) == kExpectedCreditHash &&
         primary == backup;
}
String validatedCredit() {
  const String primary = ProjectIdentity::primaryCredit();
  if (ProjectIdentity::hashText(primary) == kExpectedCreditHash) return primary;
  const String backup = ProjectIdentity::backupCredit();
  if (ProjectIdentity::hashText(backup) == kExpectedCreditHash) return backup;
  return String("WYL Studio");
}
}
