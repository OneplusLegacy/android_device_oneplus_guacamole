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
parcelable ProductionStatus {
  int modemActive = (-1) /* -1 */;
  int modemPending = (-1) /* -1 */;
  int[] profilePresence;
  int slot0Mode = (-1) /* -1 */;
  int slot0Flex = (-1) /* -1 */;
  int slot0Refresh = (-1) /* -1 */;
  int slot1Mode = (-1) /* -1 */;
  int slot1Flex = (-1) /* -1 */;
  int slot1Refresh = (-1) /* -1 */;
  @utf8InCpp String selDb;
  @utf8InCpp String recovery;
  @utf8InCpp String lastAction;
  @utf8InCpp String lastResult;
  long generation = 0;
  long observedElapsedMs = 0;
  long ageMs = (-1) /* -1 */;
  boolean busy = false;
  boolean installCandidate = false;
  boolean rollbackCandidate = false;
}
