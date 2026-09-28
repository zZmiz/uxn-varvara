
#pragma once
#include <stdint.h>
#include <string.h>

#ifndef UXN_SCALE_HOT
#define UXN_SCALE_HOT
#endif
#ifndef UXN_SCALE_CLOCK
#define UXN_SCALE_CLOCK() 0u
#endif

typedef struct UxnScaleCache {
    uint8_t *pixels;
    uint16_t colors[16];
    int valid;
    uint32_t scan_us, render_us;
    int height;
} UxnScaleCache;

static UXN_SCALE_HOT unsigned uxn_present_cached_256(const uint8_t *src, int stride,
    uint16_t *dst, int dst_stride, const uint16_t colors[16], UxnScaleCache *cache,
    const uint16_t *damage, int height)
{
    uint32_t started = UXN_SCALE_CLOCK();
    uint16_t dirty[16] = {0};
    uint32_t current[64], previous[64];
    const int all = !cache->valid || cache->height != height ||
        memcmp(colors, cache->colors, sizeof(cache->colors));
    for (int y = 0; y < height; ++y) {
        unsigned candidates = all || !damage ? 0xffffu : damage[y / 16];
        if (!candidates) continue;

        for (int x = 0; x < 16;) {
            if (!(candidates & (1u << x))) { ++x; continue; }
            int first = x++;
            while (x < 16 && (candidates & (1u << x))) ++x;
            memcpy(current + first * 4, src + y * stride + first * 16, (x - first) * 16);
            if (!all) memcpy(previous + first * 4, cache->pixels + y * 256 + first * 16, (x - first) * 16);
        }
        for (int x = 0; x < 16; ++x) {
            if (!(candidates & (1u << x))) continue;
            int p = x * 4;
            if (all || current[p] != previous[p] || current[p + 1] != previous[p + 1] ||
                current[p + 2] != previous[p + 2] || current[p + 3] != previous[p + 3]) {
                dirty[y / 16] |= (uint16_t)(1u << x);
                memcpy(cache->pixels + y * 256 + x * 16, current + p, 16);
            }
        }
    }
    cache->scan_us = UXN_SCALE_CLOCK() - started;
    started = UXN_SCALE_CLOCK();
    uint32_t packed[16];
    for (int i = 0; i < 16; ++i)
        packed[i] = (colors[i] & 31u) | (((colors[i] >> 5) & 31u) << 10) |
            (((colors[i] >> 10) & 31u) << 20);
    unsigned tiles = 0;
    for (int by = 0; by < height / 16; ++by) for (int bx = 0; bx < 16; ++bx) {
        if (!(dirty[by] & (1u << bx))) continue;
        ++tiles;
        uint32_t horizontal[16][15];
        uint8_t row[16];
        if (height == 240) {

            for (int y = 0; y < 16; ++y) {
                memcpy(row, cache->pixels + (by * 16 + y) * 256 + bx * 16, 16);
                for (int x = 0; x < 16; ++x)
                    dst[(by * 16 + y) * dst_stride + bx * 16 + x] = colors[row[x] & 15];
            }
            continue;
        }
        const uint32_t rounding = 8u | (8u << 10) | (8u << 20);
        for (int y = 0; y < 16; ++y) {
            memcpy(row, cache->pixels + (by * 16 + y) * 256 + bx * 16, 16);
            for (int x = 0; x < 15; ++x)
                horizontal[y][x] = ((packed[row[x] & 15] * (15 - x) +
                    packed[row[x + 1] & 15] * (x + 1) + rounding) >> 4) &
                    (31u | (31u << 10) | (31u << 20));
        }
        for (int y = 0; y < 15; ++y) for (int x = 0; x < 15; ++x) {
            uint32_t c = (horizontal[y][x] * (15 - y) +
                horizontal[y + 1][x] * (y + 1) + rounding) >> 4;
            dst[(by * 15 + y) * dst_stride + bx * 15 + x] = (uint16_t)((c & 31u) |
                ((c >> 5) & (31u << 5)) | ((c >> 10) & (31u << 10)));
        }
    }
    memcpy(cache->colors, colors, sizeof(cache->colors));
    cache->valid = 1;
    cache->height = height;
    cache->render_us = UXN_SCALE_CLOCK() - started;
    return tiles;
}
static inline unsigned uxn_scale_cached_256_dirty(const uint8_t *src, int stride,
    uint16_t *dst, int dst_stride, const uint16_t colors[16], UxnScaleCache *cache,
    const uint16_t *damage)
{
    return uxn_present_cached_256(src, stride, dst, dst_stride, colors, cache, damage, 256);
}
static inline unsigned uxn_scale_cached_256(const uint8_t *src, int stride,
    uint16_t *dst, int dst_stride, const uint16_t colors[16], UxnScaleCache *cache)
{
    return uxn_scale_cached_256_dirty(src, stride, dst, dst_stride, colors, cache, NULL);
}

static inline void uxn_scale_row_256(const uint8_t *src, uint32_t *row,
    const uint32_t colors[16])
{

    uint8_t source_row[256];
    memcpy(source_row, src, sizeof(source_row));
    const uint32_t mask = 31u | (31u << 10) | (31u << 20);
    const uint32_t rounding = 8u | (8u << 10) | (8u << 20);
    for (int block = 0; block < 16; ++block)
        for (int i = 0; i < 15; ++i) {
            uint32_t a = colors[source_row[block * 16 + i] & 15];
            uint32_t b = colors[source_row[block * 16 + i + 1] & 15];
            row[block * 15 + i] = ((a * (15 - i) + b * (i + 1) + rounding) >> 4) & mask;
        }
}

static UXN_SCALE_HOT void uxn_scale_screen(const uint8_t *src, int stride, int width,
    int height, uint16_t *dst, int dst_stride, int out_width, int out_height,
    const uint16_t colors[16])
{
    if (width == out_width && height == out_height) {
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                dst[y * dst_stride + x] = colors[src[y * stride + x] & 15];
        return;
    }
    if (width == 256 && height == 256 && out_width == 240 && out_height == 240) {
        uint32_t packed[16], rows[2][240];
        for (int i = 0; i < 16; ++i)
            packed[i] = (colors[i] & 31u) | (((colors[i] >> 5) & 31u) << 10) |
                (((colors[i] >> 10) & 31u) << 20);
        int previous = -2;
        unsigned slot = 0;
        for (int block = 0; block < 16; ++block)
            for (int i = 0; i < 15; ++i) {
                int sy = block * 16 + i, y = block * 15 + i;
                if (sy == previous + 1) slot ^= 1u;
                else uxn_scale_row_256(src + sy * stride, rows[slot], packed);
                uxn_scale_row_256(src + (sy + 1) * stride, rows[slot ^ 1u], packed);
                previous = sy;
                for (int x = 0; x < 240; ++x) {
                    uint32_t c = (rows[slot][x] * (15 - i) + rows[slot ^ 1u][x] * (i + 1)
                        + (8u | (8u << 10) | (8u << 20))) >> 4;
                    dst[y * dst_stride + x] = (uint16_t)((c & 31u) |
                        ((c >> 5) & (31u << 5)) | ((c >> 10) & (31u << 10)));
                }
            }
        return;
    }
    const uint32_t area = (uint32_t)width * height;
    for (int y = 0; y < out_height; ++y) {
        const int top = y * height, bottom = top + height;
        for (int x = 0; x < out_width; ++x) {
            const int left = x * width, right = left + width;
            uint32_t r = 0, g = 0, b = 0;
            for (int sy = top / out_height; sy * out_height < bottom; ++sy) {
                const int a = top > sy * out_height ? top : sy * out_height;
                const int z = bottom < (sy + 1) * out_height ? bottom : (sy + 1) * out_height;
                for (int sx = left / out_width; sx * out_width < right; ++sx) {
                    const int l = left > sx * out_width ? left : sx * out_width;
                    const int h = right < (sx + 1) * out_width ? right : (sx + 1) * out_width;
                    const uint32_t weight = (uint32_t)(z - a) * (h - l);
                    const uint16_t c = colors[src[sy * stride + sx] & 15];
                    r += ((c >> 10) & 31) * weight;
                    g += ((c >> 5) & 31) * weight;
                    b += (c & 31) * weight;
                }
            }
            dst[y * dst_stride + x] = (uint16_t)(
                ((r + area / 2) / area << 10) |
                ((g + area / 2) / area << 5) | ((b + area / 2) / area));
        }
    }
}
