// guacamole_efsd - stable AIDL surface. Cycle 2 Section K (2026-09-24;
// restructured into one-type-per-file 2026-09-24 W1 Finalization Round,
// item G - see ../../Android.bp's header comment for why).
//
// DESIGN RULES (mirrored from every other layer in this project):
//   - No raw filesystem path, no raw MBN path, no raw SHA/config ID
//     ever crosses this boundary - only fixed enums (Slot, Carrier)
//     and opaque status/result types.
//   - Every mutating call can fail with a specific, actionable status
//     rather than a raw errno - including BLOCKED_MAPPING for the
//     still-unresolved LGU/SKT/KT carrier-index question (see
//     korea_sel_db_patcher.h) and the ambiguous-mutation states from
//     efs_transaction.h's TransactionState.
//   - No implicit retry anywhere - a caller that wants to try again
//     issues a NEW call.

package com.guacamole.efsd;

import com.guacamole.efsd.Carrier;
import com.guacamole.efsd.GuacamoleEfsdResult;
import com.guacamole.efsd.GuacamoleEfsdStatus;
import com.guacamole.efsd.Slot;
import com.guacamole.efsd.ProductionStatus;
import com.guacamole.efsd.ProductionActionResult;

@VintfStability
interface IGuacamoleEfsd {
    GuacamoleEfsdStatus getStatus();

    // Loads the SKT/KT/LGU PDC profiles that are not already present
    // (load-if-absent, matching Phase C0's proven semantics - see
    // skt_load.h) and prepares (but does NOT yet apply) the Korea
    // mcfg_sel_db.xml patch. Returns BLOCKED_MAPPING immediately,
    // unconditionally, while the carrier-index mapping remains
    // unresolved - see korea_sel_db_patcher.h.
    GuacamoleEfsdResult prepareKoreanSupport();

    // Applies carrier `carrier` to `slot`. Internally resolves to
    // exactly one ApplyPatchPlan() transaction (see efs_transaction.h) -
    // never accepts a raw path/config id. Returns BLOCKED_MAPPING,
    // MUTATION_UNKNOWN, or HARD_STOP verbatim from that layer's own
    // TransactionState when applicable - the caller/UI is expected to
    // surface these as distinct, meaningful states, never collapse them
    // to a generic failure.
    GuacamoleEfsdResult applyCarrier(Slot slot, Carrier carrier);

    // Restores the ORIGINAL preimage (the exact bytes captured before
    // the first Cycle-2 mutation this daemon instance is aware of) via
    // the same verify-write-readback-verify flow as any other
    // transaction - never a bare "undo" flag.
    GuacamoleEfsdResult restoreOriginal();

    @utf8InCpp String getLastError();
    // V2 additions. V1 methods retain their existing meaning and remain blocked.
    ProductionStatus observe();
    ProductionActionResult installKoreaConfiguration();
    // sel_db restoration only. No AUTO restore, PDC selection or reboot.
    ProductionActionResult rollbackKoreaConfiguration();
}
