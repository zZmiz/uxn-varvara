#ifndef UXN_I2S_AUDIO_CLOCK_H
#define UXN_I2S_AUDIO_CLOCK_H

#include <stdint.h>

// One stereo frame takes 64 PIO cycles; the divider has 8 fractional bits.
// Preserve the division remainder across equal-sized DMA blocks rather than
// running permanently fast with the truncated divider.
typedef struct {
    uint32_t base, remainder, denominator, phase;
} audio_clock_t;

static inline void audio_clock_init(audio_clock_t *clock, uint32_t sys_hz,
                                    uint32_t sample_hz)
{
    const uint64_t numerator = (uint64_t)sys_hz * 4;
    clock->base = (uint32_t)(numerator / sample_hz);
    clock->remainder = (uint32_t)(numerator % sample_hz);
    clock->denominator = sample_hz;
    clock->phase = 0;
}

static inline uint32_t audio_clock_next(audio_clock_t *clock)
{
    uint32_t divider = clock->base;
    clock->phase += clock->remainder;
    if (clock->phase >= clock->denominator) {
        clock->phase -= clock->denominator;
        ++divider;
    }
    return divider;
}

#endif
