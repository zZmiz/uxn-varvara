
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../pico_shared/drivers/pico_audio_i2s/audio_clock.h"

static void check_two_hours(uint32_t sys_hz, uint32_t sample_hz)
{
    audio_clock_t clock;
    audio_clock_init(&clock, sys_hz, sample_hz);
    const uint64_t blocks = (uint64_t)sample_hz * 7200 / 128;
    uint64_t total = 0;
    for (uint64_t n = 1; n <= blocks; ++n) {
        uint32_t divider = audio_clock_next(&clock);
        assert(divider >= 256 && divider <= 0xffffff);
        total += divider;

        uint64_t ideal = n * sys_hz * 4;
        uint64_t actual = total * sample_hz;
        assert(actual <= ideal);
        assert(ideal - actual < sample_hz);
    }
}

int main(void)
{
    check_two_hours(252000000, 44100);
    check_two_hours(252000000, 48000);
    check_two_hours(252000000, 32000);
    check_two_hours(150000000, 44100);

    audio_clock_t clock;
    audio_clock_init(&clock, 252000000, 44100);
    for (int i = 0; i < 3; ++i) audio_clock_next(&clock);
    audio_clock_init(&clock, 252000000, 48000);
    for (int i = 0; i < 100; ++i) assert(audio_clock_next(&clock) == 21000);
    puts("PASS: two-hour clock schedules at four clock/rate pairs; frequency reset");
}
