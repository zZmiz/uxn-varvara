
#include <assert.h>
#include <stdlib.h>
#include "../src/uxn_runtime.c"
#include "../src/file_path.h"
#include "../src/screen_scale.h"
#include "diskio.h"

#define SECTORS 65536u
static unsigned char *disk;
DSTATUS disk_initialize(BYTE d) { return d ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE d) { return d ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE d, BYTE *b, LBA_t s, UINT n) {
    if (d || s + n > SECTORS) return RES_PARERR;
    memcpy(b, disk + s * 512, n * 512); return RES_OK;
}
DRESULT disk_write(BYTE d, const BYTE *b, LBA_t s, UINT n) {
    if (d || s + n > SECTORS) return RES_PARERR;
    memcpy(disk + s * 512, b, n * 512); return RES_OK;
}
DRESULT disk_ioctl(BYTE d, BYTE cmd, void *b) {
    if (d) return RES_PARERR;
    if (cmd == GET_SECTOR_COUNT) *(LBA_t *)b = SECTORS;
    else if (cmd == GET_SECTOR_SIZE) *(WORD *)b = 512;
    else if (cmd == GET_BLOCK_SIZE) *(DWORD *)b = 1;
    else if (cmd != CTRL_SYNC) return RES_PARERR;
    return RES_OK;
}
void *uxn_platform_alloc(size_t n) { return calloc(1, n); }
void *uxn_platform_realloc(void *p, size_t n) { return realloc(p, n); }
void uxn_platform_free(void *p) { free(p); }
static uint8_t game_snapshots[2][65536];
static uint16_t game_outputs[2][320 * 240], game_reference[320 * 240];
static UxnScaleCache game_caches[2] = {{.pixels = game_snapshots[0]}, {.pixels = game_snapshots[1]}};
static unsigned game_presents, game_tiles;
static unsigned game_candidates;
static uint16_t game_damage[2][16];
static bool check_game_scaling;
void uxn_platform_screen_resized(int w, int h) {
    (void)w; (void)h;
    game_caches[0].valid = game_caches[1].valid = 0;
}
void uxn_platform_present(const uint8_t *p, int s, int w, int h, const uint32_t c[16]) {
    if (!check_game_scaling || w != 256 || (h != 256 && h != 240)) return;
    uint16_t colors[16];
    for (int i = 0; i < 16; ++i)
        colors[i] = (((c[i] >> 19) & 31) << 10) | (((c[i] >> 11) & 31) << 5) | ((c[i] >> 3) & 31);
    if (h == 240) {
        unsigned slot = game_presents++ & 1;
        const uint16_t *damage = uxn_runtime_screen_damage();
        for (int y = 0; y < 16; ++y) for (int target = 0; target < 2; ++target)
            game_damage[target][y] |= damage ? damage[y] : 0xffffu;
        game_tiles += uxn_present_cached_256(p, s, game_outputs[slot] + 32, 320,
            colors, &game_caches[slot], game_damage[slot], h);
        memset(game_damage[slot], 0, sizeof(game_damage[slot]));
        uxn_scale_screen(p, s, w, h, game_reference + 32, 320, w, h, colors);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            assert(game_outputs[slot][y * 320 + x + 32] == colors[p[y * s + x] & 15]);
        const char *directory = getenv("UXN_SCREENSHOT_DIR");
        if (directory && (game_presents == 1 || game_presents == 60 || game_presents == 180)) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame-%u.ppm", directory, game_presents);
            FILE *picture = fopen(path, "wb"); assert(picture);
            fprintf(picture, "P6\n256 240\n255\n");
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                uint32_t color = c[p[y * s + x] & 15];
                fputc((color >> 16) & 255, picture);
                fputc((color >> 8) & 255, picture);
                fputc(color & 255, picture);
            }
            fclose(picture);
        }
        return;
    }
    unsigned slot = game_presents++ & 1;
    const uint16_t *damage = uxn_runtime_screen_damage();
    for (int y = 0; y < 16; ++y) for (int target = 0; target < 2; ++target)
        game_damage[target][y] |= damage ? damage[y] : 0xffffu;
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x)
        game_candidates += !!(game_damage[slot][y] & (1u << x));
    game_tiles += uxn_scale_cached_256_dirty(p, s, game_outputs[slot] + 40, 320,
        colors, &game_caches[slot], game_damage[slot]);
    memset(game_damage[slot], 0, sizeof(game_damage[slot]));
    uxn_scale_screen(p, s, w, h, game_reference + 40, 320, 240, 240, colors);
    assert(!memcmp(game_outputs[slot], game_reference, sizeof(game_reference)));
}
bool uxn_platform_resolve_path(const char *p, char *out, size_t cap) {
    return uxn_resolve_file_path(p, "/diag", out, cap);
}
void uxn_platform_datetime(UxnDateTime *d) { memset(d, 0, sizeof(*d)); }

int main(int argc, char **argv) {
    assert(argc == 2);
    check_game_scaling = strstr(argv[1], "/games/") != NULL;
    disk = calloc(SECTORS, 512);
    FATFS fs; BYTE work[4096]; MKFS_PARM opt = {.fmt = FM_FAT | FM_SFD};
    assert(f_mkfs("0:", &opt, work, sizeof(work)) == FR_OK);
    assert(f_mount(&fs, "0:", 1) == FR_OK);
    assert(f_mkdir("0:/diag") == FR_OK);
    FILE *host = fopen(argv[1], "rb"); assert(host);
    FIL rom; UINT written; size_t n;
    assert(f_open(&rom, "0:/diag/test.rom", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    while ((n = fread(work, 1, sizeof(work), host))) {
        assert(f_write(&rom, work, n, &written) == FR_OK && written == n);
    }
    fclose(host); assert(f_close(&rom) == FR_OK);
    bool running = uxn_runtime_boot("0:/diag/test.rom");
    const bool game = strstr(argv[1], "/games/") != NULL;
    if (game) assert(running);
    if (running) {
        if (strstr(argv[1], "hexpad")) {
            static const unsigned char keys[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
            for (unsigned i = 0; i < sizeof(keys) - 1; ++i) {
                const int x0 = (i & 0x0f) * 8;
                const int y0 = (i >> 4) * 8;
                int pixels = 0;
                uxn_runtime_key(keys[i]);
                assert(uxn_runtime_frame());
                for (int y = y0; y < y0 + 8; ++y)
                    for (int x = x0; x < x0 + 8; ++x)
                        pixels += screen_layers[(y + 8) * screen_wmar2 + x + 8] != 0;
                assert(pixels > 0);
            }
            for (unsigned i = 0; i < sizeof(keys) - 1; ++i)
                uxn_runtime_key(8);
            for (int y = 0; y < 24; ++y)
                for (int x = 0; x < 128; ++x)
                    assert(screen_layers[(y + 8) * screen_wmar2 + x + 8] == 0);
        }
        int16_t samples[735 * 2];
        unsigned voices_seen = 0;
        for (int frame = 0; frame < 1800; ++frame) {
            uxn_runtime_controller(game
                ? ((frame == 30 || frame == 90) ? 0x08 : (frame % 30 == 0 ? 0x11 : 0))
                : ((frame / 60) & 1 ? 0x40 : 0x80));
            if (frame % 30 == 0) uxn_runtime_key('A' + (frame / 30) % 26);
            if (frame % 60 == 0) uxn_runtime_key(8);
            assert(uxn_runtime_frame());
            uxn_runtime_audio(samples, 735);
            for (unsigned v = 0; v < 4; ++v)
                if (uxn_audio[v].advance && uxn_audio[v].period) voices_seen |= 1u << v;

            if (!game) assert(ptr[0] == 0 && ptr[1] == 0);
        }
        if (strstr(argv[1], "audio4")) assert(voices_seen == 15);
        if (game) {
            unsigned pixels = 0;
            for (int y = 0; y < screen_height; ++y)
                for (int x = 0; x < screen_width; ++x)
                    pixels += screen_layers[(y + 8) * screen_wmar2 + x + 8] != 0;
            assert(pixels > 0);
            printf("Game smoke PASS: %s, %dx%d, 1800 frames, %u nonzero pixels, voices=%x\n",
                argv[1], screen_width, screen_height, pixels, voices_seen);
            if (game_presents && screen_height == 256) printf("Game scaling PASS: %u presents, mean %u/256 tiles, exact framebuffer match\n",
                game_presents, game_tiles / game_presents);
            if (game_presents && screen_height == 256) printf("Game scan candidates: mean %u/256 tiles\n", game_candidates / game_presents);
            if (game_presents && screen_height == 240) printf("Native scaling PASS: %u presents, every output pixel matches source palette exactly\n", game_presents);
            if (screen_width == 256 && (screen_height == 256 || screen_height == 240)) {

                const int positions[] = {-7, -1, 0, 15, 16, 127, screen_height - 8, screen_height - 1};
                rML = rMX = rMY = rMA = rDX = rDY = 0;
                for (int i = 0; i < 16; ++i) ram[0x200 + i] = (Uint8)(i * 37 + 19);
                for (int ctrl = 0; ctrl < 256; ++ctrl) {
                    rX = positions[ctrl & 7];
                    rY = positions[(ctrl >> 3) & 7];
                    rA = 0x200;
                    dev[0x2f] = (Uint8)ctrl;
                    screen_deo_sprite();
                    screen_redraw();
                }
                for (int ctrl = 0; ctrl < 256; ++ctrl) {
                    rX = (ctrl * 17) & 255;
                    rY = (ctrl * 31) & 255;
                    dev[0x2e] = (Uint8)ctrl;
                    screen_deo_pixel();
                    screen_redraw();
                }
                puts("PASS: damage-driven presentation matches full output for all sprite/pixel controls and clipped edges");
            }
        }
    }
    assert((dev[0x0f] & 0x7f) == 0);
    uxn_runtime_shutdown();

    ram = calloc(1, BANKS_CAP);
    for (unsigned id = 0; id < 2; ++id) {
        strcpy((char *)&ram[0x200], "."); file_init(id, 0x200);
        assert(strcmp(ufs[id].filepath, "0:/diag") == 0);
        for (unsigned cap = 0; cap <= 32; ++cap) {
            file_reset(id); memset(ram + 0x400, 0xa5, 64);
            unsigned got = file_read(id, 0x400, cap);
            assert(got <= cap);
            for (unsigned j = cap; j < 64; ++j) assert(ram[0x400 + j] == 0xa5);
        }
        file_reset(id);
        ram[0xffff] = 0xa5;
        assert(file_read(id, 0xffff, 32) == 0);
        assert(ram[0x10000] == 0);
        file_reset(id);
    }
    char path[64];
    assert(uxn_resolve_file_path("/./sub//./a", "/diag", path, sizeof(path)));
    assert(!strcmp(path, "0:/diag/sub/a"));
    assert(!uxn_resolve_file_path("sub/../escape", "/diag", path, sizeof(path)));
    assert(!uxn_resolve_file_path("0:/escape", "/diag", path, sizeof(path)));
    assert(!uxn_resolve_file_path("abc", "/diag", path, 5));
    uxn_runtime_shutdown(); f_mount(NULL, "0:", 0); free(disk);
    puts("PASS: actual FatFs, File A/B directory bounds, dot paths, sandbox paths");
}
