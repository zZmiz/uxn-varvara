
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UXN_SCREEN_MAX_WIDTH 1024
#define UXN_SCREEN_MAX_HEIGHT 768
#define UXN_FILE_PATH_MAX 512

typedef struct UxnDateTime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday;
    uint16_t yearday;
    int8_t is_dst;
} UxnDateTime;

bool uxn_runtime_boot(const char *rom_path);
void uxn_runtime_shutdown(void);
bool uxn_runtime_frame(void);
void uxn_runtime_request_redraw(void);
bool uxn_runtime_reset(bool soft);
void uxn_runtime_controller(uint8_t buttons);
void uxn_runtime_key(uint8_t key);
void uxn_runtime_mouse(uint16_t x, uint16_t y, uint8_t buttons,
                       int16_t scroll_x, int16_t scroll_y);
void uxn_runtime_audio(int16_t *samples, unsigned int frames);
unsigned int uxn_runtime_width(void);
unsigned int uxn_runtime_height(void);
uint64_t uxn_runtime_instruction_count(void);
typedef struct UxnDrawTiming {
    uint64_t fill_us, sprite_us;
    uint32_t fill_calls, sprite_calls;
} UxnDrawTiming;
UxnDrawTiming uxn_runtime_take_draw_timing(void);

const uint16_t *uxn_runtime_screen_damage(void);

void *uxn_platform_alloc(size_t size);
void *uxn_platform_realloc(void *pointer, size_t size);
void uxn_platform_free(void *pointer);
void uxn_platform_screen_resized(int width, int height);
void uxn_platform_present(const uint8_t *pixels, int stride, int width, int height,
                          const uint32_t palette[16]);
bool uxn_platform_resolve_path(const char *source, char *dest, size_t capacity);
void uxn_platform_datetime(UxnDateTime *result);

#ifdef __cplusplus
}
#endif
