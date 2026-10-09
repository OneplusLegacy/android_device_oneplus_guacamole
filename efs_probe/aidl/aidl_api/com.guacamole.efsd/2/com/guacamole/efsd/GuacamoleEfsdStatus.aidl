// guacamole_efsd - stable AIDL surface. See GuacamoleEfsdState.aidl's
// header comment.
//
// One field per efs_transaction.h TransactionRecord field the caller is
// allowed to see - never the raw preimage/proposed BYTES (those stay
// server-side, in the persistent transaction backend - see
// efs_transaction_backend.h), only their digests and outcome.
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
parcelable GuacamoleEfsdStatus {
  com.guacamole.efsd.GuacamoleEfsdState state;
  boolean pdcSktConfigPresent;
  boolean pdcKtConfigPresent;
  boolean pdcLguConfigPresent;
  boolean selDbPatched;
  @utf8InCpp String preimageSha256Hex;
  @utf8InCpp String currentSha256Hex;
  com.guacamole.efsd.Carrier selectedCarrierSim1;
  com.guacamole.efsd.Carrier selectedCarrierSim2;
  @utf8InCpp String lastTransactionId;
  com.guacamole.efsd.GuacamoleEfsdResult lastResult;
  boolean rebootRequired;
}
