
#pragma once
#include <stdint.h>

static inline int16_t uxn_hdmi_gain(int16_t sample, unsigned *clipped)
{
    const int32_t amplified = (int32_t)sample * 4;
    if (amplified > 32767) {
        ++*clipped;
        return 32767;
    }
    if (amplified < -32768) {
        ++*clipped;
        return -32768;
    }
    return (int16_t)amplified;
}
