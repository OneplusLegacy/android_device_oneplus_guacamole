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
///////////////////////////////////////////////////////////////////////////////
// THIS FILE IS IMMUTABLE. DO NOT EDIT IN ANY CASE.                          //
///////////////////////////////////////////////////////////////////////////////

// This file is a snapshot of an AIDL file. Do not edit it manually. There are
// two cases:
// 1). this is a frozen version file - do not edit this in any case.
// 2). this is a 'current' file. If you make a backwards compatible change to
//     the interface (from the latest frozen version), the build system will
//     prompt you to update this file with `m <name>-update-api`.
//
// You must not make a backward incompatible change to any AIDL file built
// with the aidl_interface module type with versions property set. The module
// type is used to build AIDL files in a way that they can be used across
// independently updatable components of the system. If a device is shipped
// with such a backward incompatible change, it has a high risk of breaking
// later when a module using the interface is updated, e.g., Mainline modules.

package com.guacamole.efsd;
@VintfStability
interface IGuacamoleEfsd {
  com.guacamole.efsd.GuacamoleEfsdStatus getStatus();
  com.guacamole.efsd.GuacamoleEfsdResult prepareKoreanSupport();
  com.guacamole.efsd.GuacamoleEfsdResult applyCarrier(com.guacamole.efsd.Slot slot, com.guacamole.efsd.Carrier carrier);
  com.guacamole.efsd.GuacamoleEfsdResult restoreOriginal();
  @utf8InCpp String getLastError();
}
