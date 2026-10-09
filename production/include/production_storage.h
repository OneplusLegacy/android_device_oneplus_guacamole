#pragma once
#include <string>
namespace guacamole_production {
inline constexpr unsigned kDaemonUid=2909;
inline constexpr const char* kDir="/data/vendor/guacamole_efsd/production";
inline constexpr const char* kDataLabel="u:object_r:guacamole_efsd_journal_file:s0";
inline constexpr const char* kWorkerContext="u:r:guacamole_efs_worker:s0";
inline constexpr const char* kDaemonContext="u:r:guacamole_efsd:s0";
inline constexpr const char* kLegacyDir="/data/vendor/guacamole_efs_experiment";
inline constexpr const char* kLegacyLabel="u:object_r:guacamole_efs_experiment_data_file:s0";
inline constexpr const char* kBrokerContext="u:r:guacamole_efs_evidence:s0";
inline constexpr const char* kSocket="/dev/socket/guacamole_efs_evidence";
std::string Context();
bool Label(int fd,const char* expected);
int OpenDirectory(const char* path,unsigned owner,const char* label);
bool ProductionFileLabel(int fd);
}
