// guacamole_efsd - stable AIDL surface. See GuacamoleEfsdState.aidl's
// header comment.

package com.guacamole.efsd;

@VintfStability
@Backing(type="int")
enum Carrier {
    AUTO = 0,
    SKT = 1,
    KT = 2,
    LGU = 3,
}
