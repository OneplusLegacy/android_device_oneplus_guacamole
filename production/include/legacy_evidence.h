#pragma once
#include <vector>
#include <cstdint>
namespace guacamole_production {
struct LegacyEvidence {
 bool available=false,auto_exact=false,auto_valid=false,valid=false,pending=false,worker=false,verified=false,backup=false,rollback=false;
 std::vector<uint8_t> before;
 bool InstalledHistory() const {return available&&valid&&pending&&worker&&verified;}
 bool RecoveryBackup() const {return available&&auto_exact&&valid&&pending&&backup&&!rollback&&!before.empty();}
};
// Fixed protocol to root read-only authority. Backup returned ONLY for explicit recovery.
LegacyEvidence RequestLegacyEvidence(bool recovery);
}
