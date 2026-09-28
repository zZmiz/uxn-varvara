
#include "uxn_runtime.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"

#define USED(x) (void)(x)

typedef uint8_t Uint8;
typedef int8_t Sint8;
typedef uint16_t Uint16;
typedef int16_t Sint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;

typedef void (*deo_handler)(void);
typedef Uint8 (*dei_handler)(void);

static Uint8 *ram, dev[0x100], ptr[2], stk[2][0x100];
static uint64_t instruction_count;
static unsigned int uxn_eval(Uint16 pc);
static int console_vector, screen_vector, controller_vector, mouse_vector;
static int rX, rY, rA, rMX, rMY, rMA, rML, rDX, rDY, rL1, rL2;

#define BANKS 0x10
#define BANKS_CAP BANKS * 0x10000
#define WIDTH (64 * 8)
#define HEIGHT (40 * 8)

#define CLAMP(v, a, b) { if(v < a) v = a; else if(v >= b) v = b; }

static inline Uint16 peek2(const Uint8 *d) { return ((Uint16)d[0] << 8) | d[1]; }
static inline void poke2(Uint8 *d, Uint16 v) { d[0] = v >> 8, d[1] = v; }

static char system_boot_path[FF_MAX_LFN];

static void
system_print(char *name, int r)
{
	Uint8 i;
	fprintf(stderr, "%s%c", name, ptr[r] - 8 ? ' ' : '|');
	for(i = ptr[r] - 8; i != ptr[r]; i++)
		fprintf(stderr, "%02x%c", stk[r][i], i == 0xff ? '|' : ' ');
	fprintf(stderr, "<%02x\n", ptr[r]);
}

static unsigned int
system_load(const char *rom_path)
{
	FIL f;
	UINT read = 0;
	unsigned int total = 0;
	if(f_open(&f, rom_path, FA_READ) != FR_OK)
		return 0;
	while(total < BANKS_CAP - 0x100) {
		unsigned int request = BANKS_CAP - 0x100 - total;
		if(request > 0x8000) request = 0x8000;
		if(f_read(&f, ram + 0x100 + total, request, &read) != FR_OK)
			break;
		total += read;
		if(read != request) break;
	}
	f_close(&f);
	return total > 0;
}

static unsigned int
system_boot(const char *rom_path, const unsigned int has_args)
{
	ram = (Uint8 *)uxn_platform_alloc(BANKS_CAP);
	if(!ram) return 0;
	memset(ram, 0, BANKS_CAP);
	strncpy(system_boot_path, rom_path, sizeof(system_boot_path) - 1);
	system_boot_path[sizeof(system_boot_path) - 1] = 0;
	dev[0x17] = has_args;
	return ram && system_load(rom_path);
}

static unsigned int
system_reboot(const unsigned int soft)
{
	memset(dev, 0, 0x100);
	memset(stk[0], 0, 0x100);
	memset(stk[1], 0, 0x100);
	if(soft)
		memset(ram + 0x100, 0, 0xff00);
	else
		memset(ram, 0, 0x10000);
	ptr[0] = ptr[1] = 0;
	console_vector = screen_vector = controller_vector = mouse_vector = 0;
	rX = rY = rA = rMX = rMY = rMA = rML = rDX = rDY = rL1 = rL2 = 0;
	return system_load(system_boot_path);
}

static void
system_deo_expansion(void)
{
	const Uint16 exp = peek2(dev + 2);
	Uint8 *aptr = ram + exp;
	unsigned int cmd = ram[exp];
	unsigned int length = peek2(aptr + 1);
	unsigned int bank = peek2(aptr + 3) * 0x10000;
	unsigned int addr = peek2(aptr + 5);
	if(length > 0x10000 - addr)
		length = 0x10000 - addr;
	if(cmd == 0x0) {
		if(bank < BANKS_CAP)
			memset(ram + bank + addr, ram[exp + 7], length);
	} else if(cmd == 1 || cmd == 2) {
		unsigned int dst_bank = peek2(aptr + 7) * 0x10000;
		unsigned int dst_addr = peek2(aptr + 9);
		if(length > 0x10000 - dst_addr)
			length = 0x10000 - dst_addr;
		if(bank < BANKS_CAP && dst_bank < BANKS_CAP)
			memmove(ram + dst_bank + dst_addr, ram + bank + addr, length);
	} else
		fprintf(stderr, "Unknown command: %02x\n", ram[exp]);
}

static Uint8 system_dei_wst(void) { return ptr[0]; }
static Uint8 system_dei_rst(void) { return ptr[1]; }
static void system_deo_wst(void) { ptr[0] = dev[4]; }
static void system_deo_rst(void) { ptr[1] = dev[5]; }
static void system_deo_print(void) { system_print("WST", 0), system_print("RST", 1); }

#define CONSOLE_STD 0x1
#define CONSOLE_ARG 0x2
#define CONSOLE_EOA 0x3
#define CONSOLE_END 0x4

static unsigned int
console_input(int c, unsigned int type)
{
	dev[0x12] = c, dev[0x17] = type;
	if(console_vector)
		uxn_eval(console_vector);
	return type != CONSOLE_END;
}

static void console_deo_vector(void) { console_vector = peek2(&dev[0x10]); }
static void console_deo_stdout(void) { fputc(dev[0x18], stdout), fflush(stdout); }
static void console_deo_stderr(void) { fputc(dev[0x19], stderr), fflush(stderr); }
static void console_deo_hb(void) { fprintf(stderr, "%02x", dev[0x1a]); }
static void console_deo_lb(void) { fprintf(stderr, "%02x", dev[0x1b]); }

#include "screen_fill.h"
#ifdef PICO_ON_DEVICE
#include "pico/time.h"
#define DRAW_CLOCK() time_us_32()
#else
#define DRAW_CLOCK() 0u
#endif
static UxnDrawTiming draw_timing;
UxnDrawTiming uxn_runtime_take_draw_timing(void)
{
	UxnDrawTiming result = draw_timing;
	memset(&draw_timing, 0, sizeof(draw_timing));
	return result;
}

static int screen_reqsize, screen_reqdraw;
static int screen_width, screen_height, screen_wmar2, screen_hmar2;
static int screen_palette[16];
static Uint8 *screen_layers;
static uint16_t screen_damage[16];
const uint16_t *uxn_runtime_screen_damage(void)
{
	return screen_width == 256 && (screen_height == 256 || screen_height == 240) ? screen_damage : NULL;
}
static void screen_mark_damage(int x, int y, int w, int h)
{
	if(!uxn_runtime_screen_damage()) return;
	int right = x + w - 1, bottom = y + h - 1;
	if(x < 0) x = 0;
	if(y < 0) y = 0;
	if(right > 255) right = 255;
	if(bottom >= screen_height) bottom = screen_height - 1;
	if(x > right || y > bottom) return;
	unsigned bits = ((1u << (right / 16 + 1)) - 1) & ~((1u << (x / 16)) - 1);
	for(int row = y / 16; row <= bottom / 16; ++row) screen_damage[row] |= (uint16_t)bits;
}

static const Uint8 blend_lut[16][2][4] = {
	{{0, 0, 1, 2}, {0, 0, 4, 8}},
	{{0, 1, 2, 3}, {0, 4, 8, 12}},
	{{0, 2, 3, 1}, {0, 8, 12, 4}},
	{{0, 3, 1, 2}, {0, 12, 4, 8}},
	{{1, 0, 1, 2}, {4, 0, 4, 8}},
	{{1, 1, 2, 3}, {4, 4, 8, 12}},
	{{1, 2, 3, 1}, {4, 8, 12, 4}},
	{{1, 3, 1, 2}, {4, 12, 4, 8}},
	{{2, 0, 1, 2}, {8, 0, 4, 8}},
	{{2, 1, 2, 3}, {8, 4, 8, 12}},
	{{2, 2, 3, 1}, {8, 8, 12, 4}},
	{{2, 3, 1, 2}, {8, 12, 4, 8}},
	{{3, 0, 1, 2}, {12, 0, 4, 8}},
	{{3, 1, 2, 3}, {12, 4, 8, 12}},
	{{3, 2, 3, 1}, {12, 8, 12, 4}},
	{{3, 3, 1, 2}, {12, 12, 4, 8}}};

static void
system_deo_colorize(void)
{
	unsigned int i, shift, colors[4];
	for(i = 0, shift = 4; i < 4; ++i, shift ^= 4) {
		Uint8
			r = dev[0x8 + i / 2] >> shift & 0xf,
			g = dev[0xa + i / 2] >> shift & 0xf,
			b = dev[0xc + i / 2] >> shift & 0xf;
		colors[i] = 0x0f000000 | r << 16 | g << 8 | b;
		colors[i] |= colors[i] << 4;
	}
	for(i = 0; i < 16; i++)
		screen_palette[i] = colors[i < 4 ? i : i >> 2];
	memset(screen_damage, 0xff, sizeof(screen_damage));
	screen_reqdraw = 1;
}

static void
screen_resize(int width, int height)
{
	if(width != screen_width || height != screen_height) {
		int new_wmar2, new_hmar2, length;
		Uint8 *new_layers;
		if(width < 8 || height < 8 || width > UXN_SCREEN_MAX_WIDTH || height > UXN_SCREEN_MAX_HEIGHT)
			return;
		new_wmar2 = width + 0x10;
		new_hmar2 = height + 0x10;
		length = new_wmar2 * new_hmar2;
		new_layers = (Uint8 *)uxn_platform_realloc(screen_layers, length);
		if(!new_layers) return;
		screen_layers = new_layers;
		screen_width = width, screen_wmar2 = new_wmar2;
		poke2(&dev[0x22], screen_width);
		screen_height = height, screen_hmar2 = new_hmar2;
		poke2(&dev[0x24], screen_height);
		memset(screen_layers, 0, length);
		memset(screen_damage, 0xff, sizeof(screen_damage));
		screen_reqsize = screen_reqdraw = 1;
	}
}

static void
screen_redraw(void)
{
	uxn_platform_present(&screen_layers[8 * screen_wmar2 + 8], screen_wmar2,
		screen_width, screen_height, (const Uint32 *)screen_palette);
	memset(screen_damage, 0, sizeof(screen_damage));
	screen_reqdraw = 0;
}

static void
screen_update(void)
{
	if(screen_vector)
		uxn_eval(screen_vector);
	if(screen_reqsize) {
		screen_reqsize = 0;
		uxn_platform_screen_resized(screen_width, screen_height);
	}
	if(screen_reqdraw)
		screen_redraw();
}

static void
screen_deo_pixel(void)
{
	const int ctrl = dev[0x2e];
	const int hi = ctrl & 0x40;
	const Uint8 mask = hi ? 0x3 : 0xc;
	const Uint8 color = hi ? ((ctrl & 0x3) << 2) : (ctrl & 0x3);
	const Uint16 x = rX, y = rY;
	if(x < screen_width && y < screen_height) {

		if(ctrl & 0x80) {
			const uint32_t started = DRAW_CLOCK();
			const int x1 = (ctrl & 0x10) ? 8 : x + 8;
			const int x2 = (ctrl & 0x10) ? x : screen_width;
			const int y1 = (ctrl & 0x20) ? 8 : y + 8;
			const int y2 = (ctrl & 0x20) ? y : screen_height;
			const int hor = (x2 + 8) - x1;
			const int ver = (y2 + 8) - y1;
			Uint8 *row = &screen_layers[y1 * screen_wmar2 + x1];
			Uint8 *end = row + ver * screen_wmar2;
			int py = y1 - 8;
			for(; row < end; row += screen_wmar2, ++py)
				uxn_fill_layer_row_tracked(row, (unsigned)hor, mask, color,
					uxn_runtime_screen_damage() ? &screen_damage[py / 16] : NULL,
					(unsigned)(x1 - 8));
			draw_timing.fill_us += DRAW_CLOCK() - started;
			++draw_timing.fill_calls;
		}

		else {
			Uint8 *dst = &screen_layers[(y + 8) * screen_wmar2 + (x + 8)];
			Uint8 value = (*dst & mask) | color;
			if(*dst != value) {
				*dst = value;
				screen_mark_damage(x, y, 1, 1);
			}
			if(rMX) rX += ctrl & 0x10 ? -1 : 1;
			if(rMY) rY += ctrl & 0x20 ? -1 : 1;
		}
		screen_reqdraw = 1;
	}
}

static void
screen_deo_sprite(void)
{
	const uint32_t started = DRAW_CLOCK();
	const int ctrl = dev[0x2f];
	const int flipx = ctrl & 0x10, flipy = ctrl & 0x20;
	const int dx = flipx ? -rDY : rDY, dy = flipy ? -rDX : rDX;
	const int row_start = flipx ? 0 : 7, row_delta = flipx ? 1 : -1;
	const int col_start = flipy ? 7 : 0, col_delta = flipy ? -1 : 1;
	const int layer = ctrl & 0x40, layer_mask = layer ? 0x3 : 0xc;
	const int is_2bpp = ctrl >> 7;
	const int addr_incr = rMA << is_2bpp;
	const int stride = screen_wmar2;
	const int blend = ctrl & 0xf;
	const int row_step = dy * stride;
	const Uint8 opaque_mask = blend % 5;
	const Uint8 *table = blend_lut[blend][layer >> 6];
	int i, j, x = rX, y = rY;
	int row_base = (y + 8) * stride;
	for(i = 0; i <= rML; i++, x += dx, y += dy, rA += addr_incr, row_base += row_step) {
		const Uint16 x0 = x + 8, y0 = y + 8;
		if(x0 + 8 >= stride || y0 + 8 >= screen_hmar2) continue;
		Uint8 *dst = screen_layers + row_base + x0;
		int changed = 0;
		const Uint8 *col = &ram[rA + col_start];
		for(j = 0; j < 8; j++, dst += stride, col += col_delta) {
			Uint8 *d = dst;
			const int ch1 = *col;
			const int ch2 = is_2bpp ? (col[8] << 1) : 0;
			int row = row_start;
			for(int k = 0; k < 8; k++, d++, row += row_delta) {
				const int color = ((ch1 >> row) & 1) | ((ch2 >> row) & 2);
				if(opaque_mask || color) {
					Uint8 old = *d, value = (old & layer_mask) | table[color];
					if(old != value) { *d = value; changed = 1; }
				}
			}
		}
		if(changed) screen_mark_damage((int)x0 - 8, (int)y0 - 8, 8, 8);
	}
	screen_reqdraw = 1;
	if(rMX) rX += flipx ? -rDX : rDX;
	if(rMY) rY += flipy ? -rDY : rDY;
	draw_timing.sprite_us += DRAW_CLOCK() - started;
	++draw_timing.sprite_calls;
}

static Uint8 screen_dei_rx(void) { dev[0x29] = rX; return rX >> 8; }
static Uint8 screen_dei_ry(void) { dev[0x2b] = rY; return rY >> 8; }
static Uint8 screen_dei_ra(void) { dev[0x2d] = rA; return rA >> 8; }
static void screen_deo_vector(void) { screen_vector = peek2(&dev[0x20]); }
static void screen_deo_width(void) { screen_resize(peek2(&dev[0x22]) & 0xfff, screen_height & 0xfff); }
static void screen_deo_height(void) { screen_resize(screen_width & 0xfff, peek2(&dev[0x24]) & 0xfff); }
static void screen_deo_auto(void) { rMX = dev[0x26] & 0x1, rMY = dev[0x26] & 0x2, rMA = (dev[0x26] & 0x4) << 1, rML = dev[0x26] >> 4, rDX = rMX << 3, rDY = rMY << 2; }
static void screen_deo_x(void) { rX = (Sint16)peek2(&dev[0x28]); }
static void screen_deo_y(void) { rY = (Sint16)peek2(&dev[0x2a]); }
static void screen_deo_addr(void) { rA = peek2(&dev[0x2c]); }

#define SAMPLE_FREQUENCY 44100
#define POLYPHONY 4
#define NOTE_PERIOD (SAMPLE_FREQUENCY * 0x4000 / 11025)
#define ADSR_STEP (SAMPLE_FREQUENCY / 0xf)

Uint8 audio_get_vu(int instance);
Uint16 audio_get_position(int instance);

typedef struct {
	Uint8 *addr;
	Uint32 count, advance, period, age, a, d, s, r;
	Uint16 i, len;
	Sint8 volume[2];
	Uint8 pitch, repeat;
} UxnAudio;

static Uint32 advances[12] = {
	0x80000, 0x879c8, 0x8facd, 0x9837f, 0xa1451, 0xaadc1,
	0xb504f, 0xbfc88, 0xcb2ff, 0xd7450, 0xe411f, 0xf1a1c
};

static UxnAudio uxn_audio[POLYPHONY];

static Uint8 audio_finished;
int audio_render(int instance, Sint16 *sample, Sint16 *end);

static void
audio_finished_handler(int instance)
{
	audio_finished |= (Uint8)(1u << instance);
}

static Sint32
envelope(UxnAudio *c, Uint32 age)
{
	if(!c->r) return 0x0888;
	if(age < c->a) return 0x0888 * age / c->a;
	if(age < c->d) return 0x0444 * (2 * c->d - c->a - age) / (c->d - c->a);
	if(age < c->s) return 0x0444;
	if(age < c->r) return 0x0444 * (c->r - age) / (c->r - c->s);
	c->advance = 0;
	return 0x0000;
}

int
audio_render(int instance, Sint16 *sample, Sint16 *end)
{
	UxnAudio *c = &uxn_audio[instance];
	Sint32 s;
	if(!c->advance || !c->period) return 0;
	while(sample < end) {
		c->count += c->advance;
		c->i += c->count / c->period;
		c->count %= c->period;
		if(c->i >= c->len) {
			if(!c->repeat) {
				c->advance = 0;
				break;
			}
			c->i %= c->len;
		}
		s = (Sint8)(c->addr[c->i] + 0x80) * envelope(c, c->age++);
		*sample++ += s * c->volume[0] / 0x180;
		*sample++ += s * c->volume[1] / 0x180;
	}
	if(!c->advance) audio_finished_handler(instance);
	return 1;
}

static void
audio_start(int instance, Uint8 *d)
{
	UxnAudio *c = &uxn_audio[instance];
	Uint8 pitch = d[0xf] & 0x7f;
	Uint16 addr = peek2(d + 0xc);
	Uint16 adsr = peek2(d + 0x8);
	c->len = peek2(d + 0xa);
	if(c->len > 0x10000 - addr)
		c->len = 0x10000 - addr;
	c->addr = &ram[addr];
	c->volume[0] = d[0xe] >> 4;
	c->volume[1] = d[0xe] & 0xf;
	c->repeat = !(d[0xf] & 0x80);
	if(pitch < 108 && c->len)
		c->advance = advances[pitch % 12] >> (8 - pitch / 12);
	else {
		c->advance = 0;
		return;
	}
	c->a = ADSR_STEP * (adsr >> 12);
	c->d = ADSR_STEP * (adsr >> 8 & 0xf) + c->a;
	c->s = ADSR_STEP * (adsr >> 4 & 0xf) + c->d;
	c->r = ADSR_STEP * (adsr >> 0 & 0xf) + c->s;
	c->age = 0;
	c->i = 0;
	if(c->len <= 0x100)
		c->period = NOTE_PERIOD * 337 / 2 / c->len;
	else
		c->period = NOTE_PERIOD;
}

static void
audio_play(int instance, Uint8 *d)
{
	audio_start(instance, d);
}

Uint8
audio_get_vu(int instance)
{
	int i;
	UxnAudio *c = &uxn_audio[instance];
	Sint32 sum[2] = {0, 0};
	if(!c->advance || !c->period) return 0;
	for(i = 0; i < 2; i++) {
		if(!c->volume[i]) continue;
		sum[i] = 1 + envelope(c, c->age) * c->volume[i] / 0x800;
		if(sum[i] > 0xf) sum[i] = 0xf;
	}
	return (sum[0] << 4) | sum[1];
}

Uint16
audio_get_position(int instance)
{
	return uxn_audio[instance].i;
}

static Uint8 audio_get_vu0(void) { return audio_get_vu(0); }
static Uint8 audio_get_vu1(void) { return audio_get_vu(1); }
static Uint8 audio_get_vu2(void) { return audio_get_vu(2); }
static Uint8 audio_get_vu3(void) { return audio_get_vu(3); }
static void audio_play0(void) { audio_play(0, &dev[0x30]); }
static void audio_play1(void) { audio_play(1, &dev[0x40]); }
static void audio_play2(void) { audio_play(2, &dev[0x50]); }
static void audio_play3(void) { audio_play(3, &dev[0x60]); }

static void
controller_down(Uint8 mask)
{
	if(mask) {
		dev[0x82] |= mask;
		if(controller_vector)
			uxn_eval(controller_vector);
	}
}

static void
controller_up(Uint8 mask)
{
	if(mask) {
		dev[0x82] &= (~mask);
		if(controller_vector)
			uxn_eval(controller_vector);
	}
}

static void
controller_key(Uint8 key)
{
	if(key) {
		dev[0x83] = key;
		if(controller_vector)
			uxn_eval(controller_vector);
		dev[0x83] = 0;
	}
}

void
controller_deo_vector(void)
{
	controller_vector = peek2(&dev[0x80]);
}

static void
mouse_down(Uint8 mask)
{
	dev[0x96] |= mask;
	if(mouse_vector)
		uxn_eval(mouse_vector);
}

static void
mouse_up(Uint8 mask)
{
	dev[0x96] &= (~mask);
	if(mouse_vector)
		uxn_eval(mouse_vector);
}

static void
mouse_pos(Uint16 x, Uint16 y)
{
	dev[0x92] = x >> 8, dev[0x93] = x;
	dev[0x94] = y >> 8, dev[0x95] = y;
	if(mouse_vector)
		uxn_eval(mouse_vector);
}

static void
mouse_scroll(Uint16 x, Uint16 y)
{
	dev[0x9a] = x >> 8, dev[0x9b] = x;
	dev[0x9c] = -y >> 8, dev[0x9d] = -y;
	if(mouse_vector)
		uxn_eval(mouse_vector);
	dev[0x9a] = 0, dev[0x9b] = 0;
	dev[0x9c] = 0, dev[0x9d] = 0;
}

void
mouse_deo_vector(void)
{
	mouse_vector = peek2(&dev[0x90]);
}

typedef struct {
	union {
		FIL f;
		DIR dir;
	} h;
	char filepath[UXN_FILE_PATH_MAX];
	enum { IDLE,
		FILE_READ,
		FILE_WRITE,
		DIR_READ,
		DIR_WRITE } state;
} UxnFile;

static UxnFile ufs[2];

static inline unsigned int
put_text(Uint8 *dest, unsigned int *length, const char *text)
{
	Uint8 *anchor = dest;
	while(*text && *length) {
		*dest = *text++, dest++;
		*length = *length - 1;
	}
	if(*length) *dest = 0;
	return dest - anchor;
}

static inline unsigned int
put_size(Uint8 *dest, unsigned int *length, unsigned int len, unsigned int size)
{
	unsigned int i, write_len = len < *length ? len : *length;
	for(i = 0, dest += write_len; i < write_len; i++) {
		*(--dest) = "0123456789abcdef"[(Uint8)(size & 0xf)];
		size >>= 4;
		*length = *length - 1;
	}
	return write_len;
}

static unsigned int
put_fill(Uint8 *dest, unsigned int *length, unsigned int len, char c)
{
	unsigned int i, write_len = len < *length ? len : *length;
	for(i = 0; i < write_len; i++) {
		*dest++ = c;
		*length = *length - 1;
	}
	return write_len;
}

static unsigned int
put_stat(Uint8 *dest, unsigned int *length, unsigned int len, unsigned int size, unsigned int err, unsigned int dir)
{
	if(dir)
		return put_fill(dest, length, len, '-');
	else if(err)
		return put_fill(dest, length, len, '!');
	else if(len >= sizeof(size) * 2 || size < (1u << (len << 2)))
		return put_size(dest, length, len, size);
	return put_fill(dest, length, len, '?');
}

static unsigned int
put_statfile(Uint8 *dest, unsigned int *length, char *pathbuf, unsigned int prefix_len, unsigned int cap, const char *basename)
{
	unsigned int err, dir, size;
	FILINFO st;
	Uint8 *anchor = dest;
	char *p = pathbuf + prefix_len, *end = pathbuf + cap - 1;
	const char *b = basename;
	while(*b && p < end)
		*p++ = *b++;
	*p = 0;
	err = f_stat(pathbuf, &st) != FR_OK;
	dir = !err && (st.fattrib & AM_DIR);
	size = err ? 0 : (unsigned int)st.fsize;
	dest += put_stat(dest, length, 4, size, err, dir);
	dest += put_text(dest, length, "\t");
	dest += put_text(dest, length, basename);
	dest += put_text(dest, length, dir ? "/\n" : "\n");
	return dest - anchor;
}

static unsigned int
put_fdir(Uint8 *dest, unsigned int *length, const char *filepath, DIR *dir)
{
	Uint8 *p = dest, *end;
	FILINFO de;
	char pathbuf[0x200];
	unsigned int prefix_len = 0;
	char c = '/';
	const char *fp = filepath;
	while(*fp && prefix_len < sizeof pathbuf - 1)
		c = *fp++, pathbuf[prefix_len++] = c;
	if(c != '/' && prefix_len < sizeof pathbuf - 1)
		pathbuf[prefix_len++] = '/';
	if(!*length) return 0;
	end = dest + *length - 1;
	--*length;
	while(p < end && f_readdir(dir, &de) == FR_OK && de.fname[0]) {
		const char *name = de.fname;
		if(name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))
			continue;
		p += put_statfile(p, length, pathbuf, prefix_len, sizeof pathbuf, name);
	}
	*p = 0;
	return p - dest;
}

static unsigned int
is_dir_path(const char *p)
{
	const char *end = p;
	while(*end) end++;
	return end > p && end[-1] == '/';
}

static unsigned int
is_dir_real(char *p)
{
	FILINFO st;
	return f_stat(p, &st) == FR_OK && (st.fattrib & AM_DIR);
}

static unsigned int
file_write_dir(const char *p)
{
	char buf[0x200];
	char *s = buf, *end = buf + sizeof buf - 1;
	unsigned int ok = 1;
	while(*p && ok && s < end) {
		*s++ = *p;
		if(*p++ == '/') {
			s[-1] = '\0';
			ok = is_dir_real(buf) || f_mkdir(buf) == FR_OK;
			s[-1] = '/';
		}
	}
	return ok && !*p;
}

static void
file_reset(unsigned int id)
{
	switch(ufs[id].state) {
	case FILE_READ:
	case FILE_WRITE: f_close(&ufs[id].h.f); break;
	case DIR_READ: f_closedir(&ufs[id].h.dir); break;
	default: break;
	}
	ufs[id].state = IDLE;
}

static unsigned int
file_open(unsigned int id, unsigned int want_write, Uint8 flags)
{
	UxnFile *u = &ufs[id];
	if(want_write) {
		if(u->state == FILE_WRITE || u->state == DIR_WRITE)
			return 1;
		file_reset(id);
		file_write_dir(u->filepath);
		if(is_dir_path(u->filepath)) {
			u->state = is_dir_real(u->filepath) ? DIR_WRITE : IDLE;
		} else if(f_open(&u->h.f, u->filepath,
			FA_WRITE | ((flags & 0x01) ? FA_OPEN_APPEND : FA_CREATE_ALWAYS)) == FR_OK) {
			u->state = FILE_WRITE;
		}
	} else {
		if(u->state == FILE_READ || u->state == DIR_READ)
			return 1;
		file_reset(id);
		if(f_opendir(&u->h.dir, u->filepath) == FR_OK) {
			u->state = DIR_READ;
		} else if(f_open(&u->h.f, u->filepath, FA_READ) == FR_OK) {
			u->state = FILE_READ;
		}
	}
	return u->state != IDLE;
}

static unsigned int
file_init(unsigned int id, Uint16 addr)
{
	char source[UXN_FILE_PATH_MAX];
	const size_t available = 0x10000u - addr;
	const size_t length = strnlen((const char *)&ram[addr], available);
	file_reset(id);
	if(length == available || length >= sizeof(source)) {
		ufs[id].filepath[0] = 0;
		return 0;
	}
	memcpy(source, &ram[addr], length + 1);
	if(!uxn_platform_resolve_path(source, ufs[id].filepath, sizeof(ufs[id].filepath)))
		ufs[id].filepath[0] = 0;
	return 0;
}

static unsigned int
file_read(unsigned int id, Uint16 addr, unsigned int len)
{
	void *dest = &ram[addr];
	unsigned int n, length = len;
	if(!ufs[id].filepath[0])
		return 0;
	if(addr + len > 0x10000)
		length = 0x10000 - addr;
	if(!file_open(id, 0, 0))
		return 0;
	if(ufs[id].state == FILE_READ)
		{
			UINT got = 0;
			n = f_read(&ufs[id].h.f, dest, length, &got) == FR_OK ? got : 0;
		}
	else
		n = put_fdir(dest, &length, ufs[id].filepath, &ufs[id].h.dir);
	if(len > 0 && n == 0) file_reset(id);
	return n;
}

static unsigned int
file_write(unsigned int id, Uint16 addr, unsigned int len, Uint8 flags)
{
	if(!ufs[id].filepath[0])
		return 0;
	if(addr + len > 0x10000)
		len = 0x10000 - addr;
	if(!file_open(id, 1, flags))
		return 0;
	if(ufs[id].state == FILE_WRITE) {
		UINT written = 0;
		if(f_write(&ufs[id].h.f, &ram[addr], len, &written) != FR_OK)
			return 0;
		return f_sync(&ufs[id].h.f) == FR_OK ? written : 0;
	}
	return is_dir_real(ufs[id].filepath);
}

static unsigned int
file_stat(unsigned int id, Uint16 addr, unsigned int len)
{
	unsigned int err, dir;
	unsigned int length;
	FILINFO st;
	if(!ufs[id].filepath[0])
		return 0;
	if(addr + len > 0x10000)
		len = 0x10000 - addr;
	length = len;
	err = f_stat(ufs[id].filepath, &st) != FR_OK;
	dir = !err && (st.fattrib & AM_DIR);
	return put_stat(&ram[addr], &length, len, err ? 0 : (unsigned int)st.fsize, err, dir);
}

static unsigned int
file_delete(unsigned int id)
{
	if(!ufs[id].filepath[0])
		return 0;
	return f_unlink(ufs[id].filepath) == FR_OK;
}

static void filea_deo_stat(void) { poke2(&dev[0xa2], file_stat(0, peek2(&dev[0xa4]), rL1)); }
static void filea_deo_delete(void) { poke2(&dev[0xa2], file_delete(0)); }
static void filea_deo_name(void) { poke2(&dev[0xa2], file_init(0, peek2(&dev[0xa8]))); }
static void filea_deo_length(void) { rL1 = peek2(&dev[0xaa]); }
static void filea_deo_read(void) { poke2(&dev[0xa2], file_read(0, peek2(&dev[0xac]), rL1)); }
static void filea_deo_write(void) { poke2(&dev[0xa2], file_write(0, peek2(&dev[0xae]), rL1, dev[0xa7])); }
static void fileb_deo_stat(void) { poke2(&dev[0xb2], file_stat(1, peek2(&dev[0xb4]), rL2)); }
static void fileb_deo_delete(void) { poke2(&dev[0xb2], file_delete(1)); }
static void fileb_deo_name(void) { poke2(&dev[0xb2], file_init(1, peek2(&dev[0xb8]))); }
static void fileb_deo_length(void) { rL2 = peek2(&dev[0xba]); }
static void fileb_deo_read(void) { poke2(&dev[0xb2], file_read(1, peek2(&dev[0xbc]), rL2)); }
static void fileb_deo_write(void) { poke2(&dev[0xb2], file_write(1, peek2(&dev[0xbe]), rL2, dev[0xb7])); }

static UxnDateTime datetime_t;

void
datetime_update(void)
{
	uxn_platform_datetime(&datetime_t);
}

static Uint8 datetime_dei_y(void) {
	datetime_update();
	const int v = datetime_t.year;
	dev[0xc1] = v;
	return v >> 8; }
static Uint8 datetime_dei_mon(void) { datetime_update(); return datetime_t.month; }
static Uint8 datetime_dei_day(void) { datetime_update(); return datetime_t.day; }
static Uint8 datetime_dei_hou(void) { datetime_update(); return datetime_t.hour; }
static Uint8 datetime_dei_min(void) { datetime_update(); return datetime_t.minute; }
static Uint8 datetime_dei_sec(void) { datetime_update(); return datetime_t.second; }
static Uint8 datetime_dei_wday(void) { datetime_update(); return datetime_t.weekday; }
static Uint8 datetime_dei_yday(void) {
	datetime_update();
	const int v = datetime_t.yearday;
	dev[0xc9] = v;
	return v >> 8; }
static Uint8 datetime_dei_dst(void) { datetime_update(); return datetime_t.is_dst; }

static const dei_handler dei_handlers[256] = {
	[0x04] = system_dei_wst,
	[0x05] = system_dei_rst,
	[0x28] = screen_dei_rx,
	[0x2a] = screen_dei_ry,
	[0x2c] = screen_dei_ra,
	[0x34] = audio_get_vu0,
	[0x44] = audio_get_vu1,
	[0x54] = audio_get_vu2,
	[0x64] = audio_get_vu3,
	[0xc0] = datetime_dei_y,
	[0xc2] = datetime_dei_mon,
	[0xc3] = datetime_dei_day,
	[0xc4] = datetime_dei_hou,
	[0xc5] = datetime_dei_min,
	[0xc6] = datetime_dei_sec,
	[0xc7] = datetime_dei_wday,
	[0xc8] = datetime_dei_yday,
	[0xca] = datetime_dei_dst};

static const deo_handler deo_handlers[256] = {
	[0x03] = system_deo_expansion,
	[0x04] = system_deo_wst,
	[0x05] = system_deo_rst,
	[0x09] = system_deo_colorize,
	[0x0b] = system_deo_colorize,
	[0x0d] = system_deo_colorize,
	[0x0e] = system_deo_print,
	[0x11] = console_deo_vector,
	[0x18] = console_deo_stdout,
	[0x19] = console_deo_stderr,
	[0x21] = screen_deo_vector,
	[0x23] = screen_deo_width,
	[0x25] = screen_deo_height,
	[0x26] = screen_deo_auto,
	[0x29] = screen_deo_x,
	[0x2b] = screen_deo_y,
	[0x2d] = screen_deo_addr,
	[0x2e] = screen_deo_pixel,
	[0x2f] = screen_deo_sprite,
	[0x3f] = audio_play0,
	[0x4f] = audio_play1,
	[0x5f] = audio_play2,
	[0x6f] = audio_play3,
	[0x81] = controller_deo_vector,
	[0x91] = mouse_deo_vector,
	[0xa5] = filea_deo_stat,
	[0xa6] = filea_deo_delete,
	[0xa9] = filea_deo_name,
	[0xab] = filea_deo_length,
	[0xad] = filea_deo_read,
	[0xaf] = filea_deo_write,
	[0xb5] = fileb_deo_stat,
	[0xb6] = fileb_deo_delete,
	[0xb9] = fileb_deo_name,
	[0xbb] = fileb_deo_length,
	[0xbd] = fileb_deo_read,
	[0xbf] = fileb_deo_write};

static inline Uint8
emu_dei(const Uint8 port)
{
	dei_handler h = dei_handlers[port];
	return h ? h() : dev[port];
}

static inline void
emu_deo(const Uint8 port, const Uint8 value)
{
	deo_handler h = deo_handlers[port];
	dev[port] = value;
	if(h) h();
}

#define Lb(o)       stk[r][(Uint8)(ptr[r]-o)]
#define Lx(m,o,o2)  m ? Lb(o2) << 8 | Lb(o2+1) : Lb(o)
#define Pb(s,v)     stk[s][ptr[s]++] = v;
#define Px(m,s,v)   if(m) {const int t=(v);Pb(s,t>>8) Pb(s,t)} else Pb(s,v)
#define Lda(m,u,o)  Pb(r,ram[o]) if(m) Pb(r,ram[(u)(o+1)])
#define Sta(u,o,v)  if(d) ram[o]=v>>8, ram[(u)(o+1)]=v; else ram[o]=v;
#define MUTE        (void)x;(void)y;(void)z;(void)r;(void)d;

#define OP(opc,X,Y,Z,k1,k2,P) \
	case opc|0x00: { const int d=0,r=0,x=X,y=Y,z=Z; ptr[r]-=d?k2:k1; P MUTE } break; \
	case opc|0x20: { const int d=1,r=0,x=X,y=Y,z=Z; ptr[r]-=d?k2:k1; P MUTE } break; \
	case opc|0x40: { const int d=0,r=1,x=X,y=Y,z=Z; ptr[r]-=d?k2:k1; P MUTE } break; \
	case opc|0x60: { const int d=1,r=1,x=X,y=Y,z=Z; ptr[r]-=d?k2:k1; P MUTE } break; \
	case opc|0x80: { const int d=0,r=0,x=X,y=Y,z=Z; P MUTE } break; \
	case opc|0xa0: { const int d=1,r=0,x=X,y=Y,z=Z; P MUTE } break; \
	case opc|0xc0: { const int d=0,r=1,x=X,y=Y,z=Z; P MUTE } break; \
	case opc|0xe0: { const int d=1,r=1,x=X,y=Y,z=Z; P MUTE } break;

static unsigned int
uxn_eval(Uint16 start_pc)
{
	Uint16 pc = start_pc;
	for(;;) {
		instruction_count++;
		switch(ram[pc++]) {
	 case 0x00: return 1;
	 case 0x20: { const Uint16 a=ram[pc]<<8|ram[(Uint16)(pc+1)]; pc+=2; if(stk[0][--ptr[0]]) pc+=a; } break;
	 case 0x40: { const Uint16 a=ram[pc]<<8|ram[(Uint16)(pc+1)]; pc+=2+a; } break;
	 case 0x60: { const Uint16 a=ram[pc]<<8|ram[(Uint16)(pc+1)]; pc+=2; Px(1,1,pc); pc+=a; } break;
	 case 0x80: { const Uint8 r=0; Lda(0,Uint16,pc); pc+=1; } break;
	 case 0xa0: { const Uint8 r=0; Lda(1,Uint16,pc); pc+=2; } break;
	 case 0xc0: { const Uint8 r=1; Lda(0,Uint16,pc); pc+=1; } break;
	 case 0xe0: { const Uint8 r=1; Lda(1,Uint16,pc); pc+=2; } break;
	 OP(0x01, Lx(d,1,2), 0,         0,1,2, Px(d,r,x+1))
	 OP(0x02, 0,         0,         0,1,2, {})
	 OP(0x03, Lx(d,1,2), 0,         0,2,4, Px(d,r,x))
	 OP(0x04, Lx(d,1,2), Lx(d,2,4), 0,2,4, Px(d,r,x) Px(d,r,y))
	 OP(0x05, Lx(d,1,2), Lx(d,2,4), Lx(d,3,6), 3,6, Px(d,r,y) Px(d,r,x) Px(d,r,z))
	 OP(0x06, Lx(d,1,2), 0,         0,1,2, Px(d,r,x) Px(d,r,x))
	 OP(0x07, Lx(d,1,2), Lx(d,2,4), 0,2,4, Px(d,r,y) Px(d,r,x) Px(d,r,y))
	 OP(0x08, Lx(d,1,2), Lx(d,2,4), 0,2,4, Pb(r,x==y))
	 OP(0x09, Lx(d,1,2), Lx(d,2,4), 0,2,4, Pb(r,y!=x))
	 OP(0x0a, Lx(d,1,2), Lx(d,2,4), 0,2,4, Pb(r,y>x))
	 OP(0x0b, Lx(d,1,2), Lx(d,2,4), 0,2,4, Pb(r,y<x))
	 OP(0x0c, Lx(d,1,2), 0,         0,1,2, pc=d?x:pc+(Sint8)x;)
	 OP(0x0d, Lx(d,1,2), Lb((d+2)), 0,2,3, if(y) pc=d?x:pc+(Sint8)x;)
	 OP(0x0e, Lx(d,1,2), 0,         0,1,2, Px(1,!r,pc) pc=d?x:pc+(Sint8)x;)
	 OP(0x0f, Lx(d,1,2), 0,         0,1,2, Px(d,!r,x))
	 OP(0x10, Lx(0,1,1), 0,         0,1,1, Lda(d,Uint8,x))
	 OP(0x11, Lx(0,1,1), Lx(d,2,3), 0,2,3, Sta(Uint8,x,y))
	 OP(0x12, Lx(0,1,1), 0,         0,1,1, Uint16 t=pc+(Sint8)x; Lda(d,Uint16,t))
	 OP(0x13, Lx(0,1,1), Lx(d,2,3), 0,2,3, Uint16 t=pc+(Sint8)x; Sta(Uint16,t,y))
	 OP(0x14, Lx(1,2,2), 0,         0,2,2, Lda(d,Uint16,x))
	 OP(0x15, Lx(1,2,2), Lx(d,3,4), 0,3,4, Sta(Uint16,x,y))
	 OP(0x16, Lx(0,1,1), 0,         0,1,1, Pb(r,emu_dei(x)) if(d) Pb(r,dev[(x+d)&0xff]))
	 OP(0x17, Lx(0,1,1), Lx(d,2,3), 0,2,3, if(d) dev[x]=y>>8; emu_deo(x+d,y);)
	 OP(0x18, Lx(d,1,2), Lx(d,2,4), 0,2,4, Px(d,r,y+x))
	 OP(0x19, Lx(d,1,2), Lx(d,2,4), 0,2,4, Px(d,r,y-x))
	 OP(0x1a, Lx(d,1,2), Lx(d,2,4), 0,2,4, Px(d,r,y*x))
	 OP(0x1b, Lx(d,1,2), Lx(d,2,4), 0,2,4, Px(d,r,x?y/x:0))
	 OP(0x1c, Lx(d,1,2), Lx(d,2,4), 0,2,4, Px(d,r,y&x))
	 OP(0x1d, Lx(d,1,2), Lx(d,2,4), 0,2,4, Px(d,r,y|x))
	 OP(0x1e, Lx(d,1,2), Lx(d,2,4), 0,2,4, Px(d,r,y^x))
	 OP(0x1f, Lx(0,1,1), Lx(d,2,3), 0,2,3, Px(d,r,(y>>(x&0xf))<<(x>>4)))
		}
	}
	return 0;
}

#if 0
void
emu_resize(void)
{
	if(emu_texture != NULL)
		SDL_DestroyTexture(emu_texture);
	SDL_RenderSetLogicalSize(emu_renderer, screen_width, screen_height);
	emu_texture = SDL_CreateTexture(emu_renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STATIC, screen_width, screen_height);
	if(emu_texture == NULL || SDL_SetTextureBlendMode(emu_texture, SDL_BLENDMODE_NONE))
		fprintf(stderr, "SDL_SetTextureBlendMode: %s\n", SDL_GetError());
	if(SDL_UpdateTexture(emu_texture, NULL, screen_pixels, sizeof(Uint32)) != 0)
		fprintf(stderr, "SDL_UpdateTexture: %s\n", SDL_GetError());
	emu_viewport.x = 0;
	emu_viewport.y = 0;
	emu_viewport.w = screen_width;
	emu_viewport.h = screen_height;
	SDL_SetWindowSize(emu_window, screen_width * emu_zoom, screen_height * emu_zoom);
	screen_resize(screen_width, screen_height);
}

void
emu_redraw(void)
{
	if(SDL_UpdateTexture(emu_texture, NULL, screen_pixels, screen_width * sizeof(Uint32)) != 0)
		fprintf(stderr, "SDL_UpdateTexture: %s\n", SDL_GetError());
	SDL_RenderClear(emu_renderer);
	SDL_RenderCopy(emu_renderer, emu_texture, NULL, &emu_viewport);
	SDL_RenderPresent(emu_renderer);
}

static void
emu_restart(unsigned int soft)
{
	system_reboot(soft);
	screen_width = screen_height = 0;
	screen_resize(WIDTH, HEIGHT);
	uxn_eval(0x100);
}

static int
stdin_handler(void *p)
{
	SDL_Event event;
	USED(p);
	event.type = stdin_event;
	event.cbutton.state = CONSOLE_STD;
	while(read(0, &event.cbutton.button, 1) > 0) {
		while(SDL_PushEvent(&event) < 0)
			SDL_Delay(25);
	}

	event.cbutton.button = '\n';
	event.cbutton.state = CONSOLE_END;
	while(SDL_PushEvent(&event) < 0)
		SDL_Delay(25);
	return 0;
}

static void
set_fullscreen(int value, int win)
{
	Uint32 flags = 0;
	emu_full = value;
	if(emu_full)
		flags = SDL_WINDOW_FULLSCREEN_DESKTOP;
	if(win)
		SDL_SetWindowFullscreen(emu_window, flags);
}

static void
set_borderless(int value)
{
	if(emu_full) return;
	borderless = value;
	SDL_SetWindowBordered(emu_window, !value);
}

static Uint8
get_button(SDL_Event *event)
{
	switch(event->key.keysym.sym) {
	case SDLK_LCTRL: return 0x01;
	case SDLK_LALT: return 0x02;
	case SDLK_LSHIFT: return 0x04;
	case SDLK_HOME: return 0x08;
	case SDLK_UP: return 0x10;
	case SDLK_DOWN: return 0x20;
	case SDLK_LEFT: return 0x40;
	case SDLK_RIGHT: return 0x80;
	}
	return 0x00;
}

static Uint8
get_mouse_button(Uint8 event)
{
	if(event == 2) return 4;
	if(event == 4) return 2;
	return event;
}

static Uint8
get_button_joystick(SDL_Event *event)
{
	return 0x01 << (event->jbutton.button & 0x3);
}

static Uint8
get_vector_joystick(SDL_Event *event)
{
	if(event->jaxis.value < -3200)
		return 1;
	if(event->jaxis.value > 3200)
		return 2;
	return 0;
}

static Uint8
get_key(SDL_Event *event)
{
	int sym = event->key.keysym.sym;
	SDL_Keymod mods = SDL_GetModState();
	if(sym < 0x20 || sym < SDLK_a || sym == SDLK_DELETE)
		return sym;
	if(sym <= SDLK_z)
		return sym - (mods & KMOD_SHIFT) * 0x20;
	return 0x00;
}

static int
emu_event(void)
{
	SDL_Event event;
	while(SDL_PollEvent(&event)) {

		if(event.type == SDL_QUIT)
			return 0;
		else if(event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_EXPOSED)
			screen_reqdraw = 1;

		else if(event.type == stdin_event)
			console_input(event.cbutton.button, event.cbutton.state);

		else if(event.type == SDL_MOUSEMOTION)
			mouse_pos(event.motion.x, event.motion.y);
		else if(event.type == SDL_MOUSEBUTTONUP)
			mouse_up(get_mouse_button(SDL_BUTTON(event.button.button)));
		else if(event.type == SDL_MOUSEBUTTONDOWN)
			mouse_down(get_mouse_button(SDL_BUTTON(event.button.button)));
		else if(event.type == SDL_MOUSEWHEEL)
			mouse_scroll(event.wheel.x, event.wheel.y);

		else if(event.type >= audio0_event && event.type < audio0_event + POLYPHONY) {
			Uint8 *port_value = &dev[0x30 + 0x10 * (event.type - audio0_event)];
			Uint16 vector = port_value[0] << 8 | port_value[1];
			if(vector)
				uxn_eval(vector);
		}

		else if(event.type == SDL_TEXTINPUT) {
			char *c;
			for(c = event.text.text; *c; c++)
				controller_key(*c);
		} else if(event.type == SDL_KEYDOWN) {
			int ksym, key;
			if((key = get_key(&event)) && (key < 0x20 || (SDL_GetModState() & (KMOD_ALT | KMOD_CTRL))))
				controller_key(key);
			else if(get_button(&event))
				controller_down(get_button(&event));
			else if(event.key.keysym.sym == SDLK_F1)
				emu_zoom = (emu_zoom % 3) + 1, screen_reqsize = screen_reqdraw = 1;
			else if(event.key.keysym.sym == SDLK_F2)
				emu_deo(0xe, 0x1);
			else if(event.key.keysym.sym == SDLK_F3)
				dev[0x0f] = 0xff;
			else if(event.key.keysym.sym == SDLK_F4)
				emu_restart(0);
			else if(event.key.keysym.sym == SDLK_F5)
				emu_restart(1);
			else if(event.key.keysym.sym == SDLK_F11)
				set_fullscreen(!emu_full, 1);
			else if(event.key.keysym.sym == SDLK_F12)
				set_borderless(!borderless);
			ksym = event.key.keysym.sym;
			if(SDL_PeepEvents(&event, 1, SDL_PEEKEVENT, SDL_KEYUP, SDL_KEYUP) == 1 && ksym == event.key.keysym.sym)
				return 1;
		} else if(event.type == SDL_KEYUP)
			controller_up(get_button(&event));
		else if(event.type == SDL_JOYAXISMOTION) {
			Uint8 vec = get_vector_joystick(&event);
			if(!vec)
				controller_up((3 << (!event.jaxis.axis * 2)) << 4);
			else
				controller_down((1 << ((vec + !event.jaxis.axis * 2) - 1)) << 4);
		} else if(event.type == SDL_JOYBUTTONDOWN)
			controller_down(get_button_joystick(&event));
		else if(event.type == SDL_JOYBUTTONUP)
			controller_up(get_button_joystick(&event));
		else if(event.type == SDL_JOYHATMOTION) {
			switch(event.jhat.value) {
			case SDL_HAT_UP: controller_down(0x10); break;
			case SDL_HAT_DOWN: controller_down(0x20); break;
			case SDL_HAT_LEFT: controller_down(0x40); break;
			case SDL_HAT_RIGHT: controller_down(0x80); break;
			case SDL_HAT_LEFTDOWN: controller_down(0x40 | 0x20); break;
			case SDL_HAT_LEFTUP: controller_down(0x40 | 0x10); break;
			case SDL_HAT_RIGHTDOWN: controller_down(0x80 | 0x20); break;
			case SDL_HAT_RIGHTUP: controller_down(0x80 | 0x10); break;
			case SDL_HAT_CENTERED: controller_up(0x10 | 0x20 | 0x40 | 0x80); break;
			}
		}
	}
	return 1;
}

static void
emu_init_audio(void)
{
	SDL_AudioSpec as;
	SDL_zero(as);
	as.freq = SAMPLE_FREQUENCY;
	as.format = AUDIO_S16SYS;
	as.channels = 2;
	as.callback = audio_callback;
	as.samples = 512;
	as.userdata = NULL;
	audio_id = SDL_OpenAudioDevice(NULL, 0, &as, NULL, 0);
	if(!audio_id)
		fprintf(stderr, "sdl_audio: %s\n", SDL_GetError());
	audio0_event = SDL_RegisterEvents(POLYPHONY);
	SDL_PauseAudioDevice(audio_id, 1);
}

static int
emu_init(void)
{
	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0)
		return !fprintf(stderr, "sdl: %s\n", SDL_GetError());
	emu_init_audio();
	if(SDL_NumJoysticks() > 0 && SDL_JoystickOpen(0) == NULL)
		fprintf(stderr, "sdl_joystick: %s\n", SDL_GetError());
	stdin_event = SDL_RegisterEvents(1);
	SDL_DetachThread(stdin_thread = SDL_CreateThread(stdin_handler, "stdin", NULL));
	SDL_StartTextInput();
	SDL_ShowCursor(SDL_DISABLE);
	SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

	Uint32 window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
	if(emu_full)
		window_flags = window_flags | SDL_WINDOW_FULLSCREEN_DESKTOP;
	emu_window = SDL_CreateWindow("Uxn2",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		screen_width * emu_zoom,
		screen_height * emu_zoom,
		window_flags);
	if(emu_window == NULL)
		return !fprintf(stderr, "sdl_window: %s\n", SDL_GetError());
	emu_renderer = SDL_CreateRenderer(emu_window, -1, SDL_RENDERER_ACCELERATED);
	if(emu_renderer == NULL)
		return !fprintf(stderr, "sdl_renderer: %s\n", SDL_GetError());
	SDL_SetRenderDrawColor(emu_renderer, 0x00, 0x00, 0x00, 0xff);
	return 1;
}

static void
emu_run(void)
{
	Uint64 next_refresh = 0;
	Uint64 perf_freq = SDL_GetPerformanceFrequency();
	Uint64 frame_interval = perf_freq / 60;
	Uint64 ms_interval = perf_freq / 1000;

	for(;;) {
		Uint64 now = SDL_GetPerformanceCounter();
		if(!emu_event() || dev[0x0f])
			break;
		if(now >= next_refresh) {
			next_refresh = now + frame_interval;
			screen_update();
		}
		if(screen_vector) {
			now = SDL_GetPerformanceCounter();
			if(now < next_refresh) {
				Uint64 delay_ms = (next_refresh - now) / ms_interval;
				if(delay_ms > 0) SDL_Delay(delay_ms);
			}
		} else
			SDL_WaitEvent(NULL);
	}

	SDL_CloseAudioDevice(audio_id);
#ifdef _WIN32
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
	TerminateThread((HANDLE)SDL_GetThreadID(stdin_thread), 0);
#elif !defined(__APPLE__)
	close(0);
#endif
	SDL_Quit();
}

int
main(int argc, char **argv)
{
	int i = 0;
	if(argc == 2 && argv[1][0] == '-' && argv[1][1] == 'v')
		return !fprintf(stdout, "%s - Varvara(79K) Emulator, 18 Aug 2026.\n", argv[0]);

	while(argv[++i] && argv[i][0] == '-' && argv[i][1] != '\0' && argv[i][2] == '\0') {
		switch(argv[i][1]) {
		case 'f': emu_full = 1; break;
		case '2': emu_zoom = 2; break;
		case '-': i++;
		default: goto end_argparse; break;
		}
	}
end_argparse:
	if(i == argc)
		return !fprintf(stdout, "usage: %s -v | [-f -2] file.rom [args..]\n", argv[0]);
	if(!system_boot(argv[i], argc > i))
		return !fprintf(stdout, "Could not load %s.\n", argv[i]);
	screen_resize(WIDTH, HEIGHT);
	if(uxn_eval(0x100) && console_vector) {
		for(++i; i < argc; i++) {
			char *p = argv[i];
			while(*p)
				console_input(*p++, CONSOLE_ARG);
			console_input('\n', i == argc - 1 ? CONSOLE_END : CONSOLE_EOA);
		}
	}
	if(!dev[0x0f]) {
		if(!emu_init())
			return !fprintf(stdout, "Could not initialize %s.\n", argv[0]);
		emu_run();
	}
	return dev[0x0f] & 0x7f;
}
#endif

bool
uxn_runtime_boot(const char *rom_path)
{
	uxn_runtime_shutdown();
	memset(dev, 0, sizeof(dev));
	memset(stk, 0, sizeof(stk));
	memset(ptr, 0, sizeof(ptr));
	memset(uxn_audio, 0, sizeof(uxn_audio));
	memset(screen_palette, 0, sizeof(screen_palette));
	audio_finished = 0;
	instruction_count = 0;
	console_vector = screen_vector = controller_vector = mouse_vector = 0;
	rX = rY = rA = rMX = rMY = rMA = rML = rDX = rDY = rL1 = rL2 = 0;
	if(!system_boot(rom_path, 0)) {
		uxn_runtime_shutdown();
		return false;
	}
	screen_resize(WIDTH, HEIGHT);
	if(!screen_layers) {
		uxn_runtime_shutdown();
		return false;
	}
	if(!uxn_eval(0x100) || dev[0x0f]) {
		uxn_runtime_shutdown();
		return false;
	}
	return true;
}

void
uxn_runtime_shutdown(void)
{
	unsigned int i;
	for(i = 0; i < 2; i++) file_reset(i);
	memset(ufs, 0, sizeof(ufs));
	if(screen_layers) uxn_platform_free(screen_layers);
	if(ram) uxn_platform_free(ram);
	screen_layers = NULL;
	ram = NULL;
	screen_width = screen_height = screen_wmar2 = screen_hmar2 = 0;
}

bool
uxn_runtime_frame(void)
{
	if(!ram || dev[0x0f]) return false;
	screen_update();
	return !dev[0x0f];
}

void
uxn_runtime_request_redraw(void)
{
	memset(screen_damage, 0xff, sizeof(screen_damage));
	screen_reqdraw = 1;
}

void
uxn_runtime_controller(Uint8 buttons)
{
	Uint8 old = dev[0x82];
	controller_up(old & (Uint8)~buttons);
	controller_down(buttons & (Uint8)~old);
}

void
uxn_runtime_key(Uint8 key)
{
	controller_key(key);
}

void
uxn_runtime_mouse(Uint16 x, Uint16 y, Uint8 buttons, Sint16 scroll_x, Sint16 scroll_y)
{
	Uint8 old = dev[0x96];
	mouse_pos(x, y);
	mouse_up(old & (Uint8)~buttons);
	mouse_down(buttons & (Uint8)~old);
	if(scroll_x || scroll_y) mouse_scroll((Uint16)scroll_x, (Uint16)scroll_y);
}

void
uxn_runtime_audio(Sint16 *samples, unsigned int frames)
{
	unsigned int instance;
	memset(samples, 0, frames * 2 * sizeof(*samples));
	for(instance = 0; instance < POLYPHONY; instance++)
		audio_render(instance, samples, samples + frames * 2);
	for(instance = 0; instance < POLYPHONY; instance++) {
		if(audio_finished & (1u << instance)) {
			Uint8 *port_value = &dev[0x30 + 0x10 * instance];
			Uint16 vector = peek2(port_value);
			audio_finished &= (Uint8)~(1u << instance);
			if(vector) uxn_eval(vector);
		}
	}
}

bool
uxn_runtime_reset(bool soft)
{
	unsigned int i;
	for(i = 0; i < 2; i++) file_reset(i);
	memset(ufs, 0, sizeof(ufs));
	memset(uxn_audio, 0, sizeof(uxn_audio));
	memset(screen_palette, 0, sizeof(screen_palette));
	audio_finished = 0;
	if(!ram || !system_reboot(soft)) return false;
	screen_width = screen_height = 0;
	screen_resize(WIDTH, HEIGHT);
	return uxn_eval(0x100) && !dev[0x0f];
}

unsigned int
uxn_runtime_width(void)
{
	return (unsigned int)screen_width;
}

unsigned int
uxn_runtime_height(void)
{
	return (unsigned int)screen_height;
}

uint64_t
uxn_runtime_instruction_count(void)
{
	return instruction_count;
}
