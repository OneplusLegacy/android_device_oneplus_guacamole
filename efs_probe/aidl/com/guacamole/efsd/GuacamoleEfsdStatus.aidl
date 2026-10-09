// guacamole_efsd - stable AIDL surface. See GuacamoleEfsdState.aidl's
// header comment.
//
// One field per efs_transaction.h TransactionRecord field the caller is
// allowed to see - never the raw preimage/proposed BYTES (those stay
// server-side, in the persistent transaction backend - see
// efs_transaction_backend.h), only their digests and outcome.

package com.guacamole.efsd;

import com.guacamole.efsd.Carrier;
import com.guacamole.efsd.GuacamoleEfsdResult;
import com.guacamole.efsd.GuacamoleEfsdState;

@VintfStability
parcelable GuacamoleEfsdStatus {
    GuacamoleEfsdState state;
    boolean pdcSktConfigPresent;   // GetConfigInfo-confirmed, not ListConfigs.
    boolean pdcKtConfigPresent;
    boolean pdcLguConfigPresent;
    boolean selDbPatched;          // korea_sel_db_patcher's own
                                    // idempotency check result, last
                                    // observed.
    @utf8InCpp String preimageSha256Hex;   // empty if unknown.
    @utf8InCpp String currentSha256Hex;    // empty if unknown.
    Carrier selectedCarrierSim1;    // AUTO if not determinable.
    Carrier selectedCarrierSim2;
    @utf8InCpp String lastTransactionId;
    GuacamoleEfsdResult lastResult;
    boolean rebootRequired;
}
