package com.guacamole.efsd;
@VintfStability
parcelable ProductionStatus {
    // Catalog indices Common=0, SKT=1, KT=2, LGU=3; -1 UNKNOWN, -2 NONE.
    int modemActive = -1;
    int modemPending = -1;
    // Common/SKT/KT/LGU: -1 UNKNOWN, 0 protocol absent, 1 EXACT, 2 metadata mismatch.
    int[] profilePresence;
    int slot0Mode = -1;
    int slot0Flex = -1;
    int slot0Refresh = -1;
    int slot1Mode = -1;
    int slot1Flex = -1;
    int slot1Refresh = -1;
    @utf8InCpp String selDb;
    @utf8InCpp String recovery;
    @utf8InCpp String lastAction;
    @utf8InCpp String lastResult;
    long generation = 0;
    long observedElapsedMs = 0;
    long ageMs = -1;
    boolean busy = false;
    // UI hints only; backend always recollects its own preflight.
    boolean installCandidate = false;
    boolean rollbackCandidate = false;
}
