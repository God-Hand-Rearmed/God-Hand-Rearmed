#pragma once

// Native EE functions (claude/native-next.md): the registry, their installation and, in dev builds, the shadow harness
// that checks them against the recompiled code during the test-case replays (rung 2; shadow.cpp).

#include "kamite/native/native_function.h"

namespace kamite::native
{
    // After every hook and observer is in place (it wraps whatever each slot holds now), before callprofile. Per native
    // function, in this order:
    //   - shadow-checked (dev builds): KAMITE_NATIVE_SHADOW lists it (names, 0x addresses, subsystems, "all"), or is
    //     unset during a test-case replay (KAMITE_REPLAY_CASE) and the function is not on (below). The slot's previous
    //     function keeps running for real; the native runs beside it on a scratch copy and is compared.
    //     KAMITE_NATIVE_SHADOW=none: no shadow checks.
    //   - native: its subsystem's switch KAMITE_NATIVE_<SUBSYSTEM> is 1, or unset and the function is checked.
    //   - else recompiled (untouched).
    void installFromEnvironment(PS2Runtime &runtime);

    // At exit: writes the shadow harness's summary (nothing to do without it).
    void flush();

    namespace libvu0
    {
        void registerFunctions();
    }
}
