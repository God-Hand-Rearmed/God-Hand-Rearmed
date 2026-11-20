#pragma once

#include "ps2_runtime.h"

namespace kamite
{
    // The main loop's wait for the next frame without the spin (frame_wait.cpp). KAMITE_FRAME_WAIT=0 (GHR.ini
    // [Runtime], hidden): the game's own spin.
    void installFrameWait();
}
