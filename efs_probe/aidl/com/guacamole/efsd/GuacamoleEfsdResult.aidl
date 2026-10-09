// guacamole_efsd - stable AIDL surface. See GuacamoleEfsdState.aidl's
// header comment.

package com.guacamole.efsd;

@VintfStability
@Backing(type="int")
enum GuacamoleEfsdResult {
    OK = 0,
    BLOCKED_MAPPING = 1,
    ALREADY_IN_PROGRESS = 2,
    INVALID_INPUT = 3,
    PDC_LOAD_FAILED = 4,
    EFS_TRANSACTION_FAILED = 5,
    MUTATION_UNKNOWN = 6,
    HARD_STOP = 7,
    INTERNAL_ERROR = 8,
}
