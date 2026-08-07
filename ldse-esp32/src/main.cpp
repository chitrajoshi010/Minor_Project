/*
 * LDSE role dispatcher.
 *
 * PlatformIO 6.x no longer supports per-env `src_dir`, so all three role
 * implementations live in firmware/{gateway,relay,node}/main.cpp and are
 * selected here at build time by the -DLDSE_ROLE build flag set in
 * platformio.ini (0 = gateway, 1 = relay, 2 = node).
 */

#include "LdseConfig.h"

#if !defined(LDSE_ROLE)
#error "LDSE_ROLE must be defined: 0=gateway, 1=relay, 2=node"
#elif LDSE_ROLE == 0
#include "../firmware/gateway/main.cpp"
#elif LDSE_ROLE == 1
#include "../firmware/relay/main.cpp"
#elif LDSE_ROLE == 2
#include "../firmware/node/main.cpp"
#else
#error "Unknown LDSE_ROLE"
#endif
