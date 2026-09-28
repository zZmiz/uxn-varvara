
#pragma once
#include <stdint.h>
#include <string.h>

static inline void uxn_fill_layer_row_tracked(uint8_t *dst, unsigned count,
    uint8_t mask, uint8_t color, uint16_t *dirty, unsigned x)
{
    while (count && ((uintptr_t)dst & 3)) {
        uint8_t old = *dst, value = (old & mask) | color;
        if (old != value) {
            *dst = value;
            if (dirty) *dirty |= (uint16_t)(1u << (x / 16));
        }
        ++dst;
        ++x;
        --count;
    }
    const uint32_t masks = (uint32_t)mask * 0x01010101u;
    const uint32_t colors = (uint32_t)color * 0x01010101u;
    while (count >= 4) {
        uint32_t word;
        memcpy(&word, dst, sizeof(word));
        uint32_t value = (word & masks) | colors;
        if (word != value) {
            memcpy(dst, &value, sizeof(value));
            if (dirty) *dirty |= (uint16_t)((1u << (x / 16)) | (1u << ((x + 3) / 16)));
        }
        dst += 4;
        x += 4;
        count -= 4;
    }
    while (count--) {
        uint8_t old = *dst, value = (old & mask) | color;
        if (old != value) {
            *dst = value;
            if (dirty) *dirty |= (uint16_t)(1u << (x / 16));
        }
        ++dst;
        ++x;
    }
}
static inline void uxn_fill_layer_row(uint8_t *dst, unsigned count,
    uint8_t mask, uint8_t color)
{
    uxn_fill_layer_row_tracked(dst, count, mask, color, 0, 0);
}
