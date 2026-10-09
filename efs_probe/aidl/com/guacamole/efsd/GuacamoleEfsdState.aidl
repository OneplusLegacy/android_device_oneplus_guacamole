// guacamole_efsd - stable AIDL surface. Cycle 2 Section K (2026-09-24;
// restructured into one-type-per-file 2026-09-24 W1 Finalization Round,
// item G - see ../../Android.bp's header comment for why).

package com.guacamole.efsd;

@VintfStability
@Backing(type="int")
enum GuacamoleEfsdState {
    IDLE = 0,
    MAPPING_BLOCKED = 1,        // Korea carrier-index mapping unresolved -
                                 // see korea_sel_db_patcher.h.
    READY_FOR_PREPARE = 2,
    TRANSACTION_IN_PROGRESS = 3,
    REBOOT_REQUIRED = 4,
    MUTATION_UNKNOWN = 5,       // ambiguous mutation outcome - see
                                 // efs_transaction.h's
                                 // TransactionState::kMutationOutcomeUnknown.
    ROLLBACK_REQUIRED = 6,
    HARD_STOP = 7,              // TransactionState::kRollbackFailedHardStop -
                                 // no further mutation permitted without
                                 // out-of-band operator intervention.
}
