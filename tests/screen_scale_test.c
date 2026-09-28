
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/screen_scale.h"
#include "../src/screen_fill.h"
static uint8_t src[256 * 256];
static uint16_t dst[256 * 256];
static uint8_t padded[272 * 272], snapshots[2][256 * 256];
static uint16_t outputs[2][320 * 240], reference[320 * 240];
int main(void)
{

    uint8_t fill[272], expected[272];
    for (int alignment = 0; alignment < 4; ++alignment)
        for (int length = 0; length <= 256; ++length)
            for (int layer = 0; layer < 2; ++layer)
                for (int color = 0; color < 4; ++color) {
                    for (int i = 0; i < 272; ++i) fill[i] = expected[i] = (i * 37) & 15;
                    uint8_t mask = layer ? 3 : 12, value = layer ? color << 2 : color;
                    for (int i = 0; i < length; ++i)
                        expected[alignment + i] = (expected[alignment + i] & mask) | value;
                    uxn_fill_layer_row(fill + alignment, length, mask, value);
                    assert(!memcmp(fill, expected, sizeof(fill)));
                    for (int i = 0; i < 272; ++i) fill[i] = (i * 37) & 15;
                    uint16_t damage = 0;
                    uxn_fill_layer_row_tracked(fill + alignment, length, mask, value, &damage, 0);
                    assert(!memcmp(fill, expected, sizeof(fill)));
                    for (int i = 0; i < length; ++i)
                        if (fill[alignment + i] != (((alignment + i) * 37) & 15))
                            assert(damage & (1u << (i / 16)));
                    damage = 0;
                    uxn_fill_layer_row_tracked(fill + alignment, length, mask, value, &damage, 0);
                    assert(!damage);
                }
    puts("PASS: word fills match byte reference for both layers, all colors, lengths and alignments");
    const uint16_t colors[16] = {0, 0x7fff};

    for (int axis = 0; axis < 2; ++axis) {
        for (int pos = 0; pos < 256; ++pos) {
            memset(src, 0, sizeof(src));
            for (int i = 0; i < 256; ++i)
                src[axis ? i * 256 + pos : pos * 256 + i] = 1;
            uxn_scale_screen(src, 256, 256, 256, dst, 240, 240, 240, colors);
            unsigned visible = 0;
            for (int i = 0; i < 240 * 240; ++i) visible += dst[i] != 0;
            assert(visible >= 240);
        }
    }
    uxn_scale_screen(src, 256, 256, 256, dst, 256, 256, 256, colors);
    for (int i = 0; i < 256 * 256; ++i) assert(dst[i] == colors[src[i]]);
    const uint16_t palette[16] = {0, 31, 31 << 5, 31 << 10, 0x7fff,
        0x1234, 0x2345, 0x4567, 0x7777, 0x1111, 0x2222, 0x3333,
        0x4444, 0x5555, 0x6666, 0x7654};
    for (int c = 1; c <= 4; ++c) {
        memset(src, c, sizeof(src));
        uxn_scale_screen(src, 256, 256, 256, dst, 240, 240, 240, palette);
        for (int p = 0; p < 240 * 240; ++p) assert(dst[p] == palette[c]);
    }
    for (int p = 0; p < 256 * 256; ++p) src[p] = (p * 37 + p / 256 * 11) & 15;
    uxn_scale_screen(src, 256, 256, 256, dst, 240, 240, 240, palette);

    for (int y = 0; y < 240; ++y) for (int x = 0; x < 240; ++x) {
        unsigned sums[3] = {0};
        for (int sy = y * 256 / 240; sy * 240 < (y + 1) * 256; ++sy)
            for (int sx = x * 256 / 240; sx * 240 < (x + 1) * 256; ++sx) {
                int weight = 0;
                for (int yy = y * 256; yy < (y + 1) * 256; ++yy)
                    if (yy / 240 == sy) ++weight;
                int horizontal = 0;
                for (int xx = x * 256; xx < (x + 1) * 256; ++xx)
                    if (xx / 240 == sx) ++horizontal;
                for (int channel = 0; channel < 3; ++channel)
                    sums[channel] += ((palette[src[sy * 256 + sx]] >> (5 * channel)) & 31) * weight * horizontal;
            }
        for (int c = 0; c < 3; ++c)
            assert(abs((int)((sums[c] + 32768) / 65536) - ((dst[y * 240 + x] >> (5 * c)) & 31)) <= 1);
    }
    puts("PASS: RGB primaries preserved; optimized scaler matches area reference within one channel step");
    puts("PASS: all 512 one-pixel edges survive reduction; native pixels exact");
    UxnScaleCache caches[2] = {{.pixels = snapshots[0]}, {.pixels = snapshots[1]}};
    uint16_t damage[2][16] = {{0}};
    uint16_t changing_palette[16];
    memcpy(changing_palette, palette, sizeof(palette));
    uint8_t *visible = padded + 8 * 272 + 8;
    for (int frame = 0; frame < 256; ++frame) {
        int slot = frame & 1;

        for (int i = 0; i < 7; ++i) {
            int x = (frame + i * 16) & 255, y = (frame * 17 + i) & 255;
            visible[y * 272 + x] = (frame + i) & 15;
            damage[0][y / 16] |= (uint16_t)(1u << (x / 16));
            damage[1][y / 16] |= (uint16_t)(1u << (x / 16));
        }
        if (frame == 80) changing_palette[0] ^= 0x7fff;
        if (frame == 160) {
            memset(outputs[slot], 0, sizeof(outputs[slot]));
            caches[slot].valid = 0;
        }
        unsigned tiles = uxn_scale_cached_256_dirty(visible, 272, outputs[slot] + 40,
            320, changing_palette, &caches[slot], damage[slot]);
        memset(damage[slot], 0, sizeof(damage[slot]));
        if (frame < 2 || frame == 80 || frame == 81 || frame == 160) assert(tiles == 256);
        else assert(tiles < 32);
        uxn_scale_screen(visible, 272, 256, 256, reference + 40, 320, 240, 240, changing_palette);
        assert(!memcmp(outputs[slot], reference, sizeof(reference)));
        assert(uxn_scale_cached_256_dirty(visible, 272, outputs[slot] + 40, 320,
            changing_palette, &caches[slot], damage[slot]) == 0);
    }
    puts("PASS: cached alternating buffers match full scaling exactly across edits, palette changes and invalidation");
    const uint16_t no_damage[16] = {0};
    for (int mode = 0; mode < 8; ++mode) {
        int height = (mode & 2) ? 256 : 240;
        int out_width = height == 240 ? 256 : 240;
        changing_palette[0] ^= 0x7fff;
        unsigned changed = uxn_present_cached_256(visible, 272, outputs[0], 320,
            changing_palette, &caches[0], no_damage, height);
        assert(changed == (unsigned)(height / 16 * 16));
        uxn_scale_screen(visible, 272, 256, height, reference, 320, out_width, 240, changing_palette);
        for (int y = 0; y < 240; ++y)
            assert(!memcmp(outputs[0] + y * 320, reference + y * 320, out_width * sizeof(uint16_t)));
        assert(uxn_present_cached_256(visible, 272, outputs[0], 320,
            changing_palette, &caches[0], no_damage, height) == 0);
    }
    puts("PASS: native/reduced geometry switches and palette changes invalidate correctly; native pixels remain exact");
}
