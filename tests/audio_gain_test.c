
#include <assert.h>
#include <stdio.h>
#include "../src/audio_gain.h"

int main(void)
{
    int previous = -32768;
    for (int sample = -32768; sample <= 32767; ++sample) {
        unsigned clipped = 0;
        const int result = uxn_hdmi_gain((int16_t)sample, &clipped);
        assert(result >= previous);
        if (sample >= -8192 && sample <= 8191) {
            assert(result == sample * 4);
            assert(!clipped);
        } else {
            assert(result == (sample < 0 ? -32768 : 32767));
            assert(clipped == 1);
        }
        previous = result;
    }
    puts("PASS: all 65536 PCM inputs have exact fourfold gain or correct saturation, no wraparound");
}
