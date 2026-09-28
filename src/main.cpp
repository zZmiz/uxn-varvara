
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/uart.h"
#include "pico/stdio/driver.h"
#include "tusb.h"

#include "FrensHelpers.h"
#include "external_audio.h"
#include "gamepad.h"
#include "menu.h"
#include "menu_settings.h"
#include "nespad.h"
#include "settings.h"
#include "uxn_runtime.h"
#include "file_path.h"
#include "audio_gain.h"
#define UXN_SCALE_HOT __attribute__((section(".time_critical.uxn_scale"), noinline))
#define UXN_SCALE_CLOCK() time_us_32()
#include "screen_scale.h"

#if !HSTX
#error This Uxn port requires RP2350 HSTX output
#endif

namespace {

constexpr uint32_t CPU_FREQ_KHZ = 252000;
constexpr int OUTPUT_WIDTH = 320;
constexpr int OUTPUT_HEIGHT = 240;
constexpr int AUDIO_FRAMES_PER_FRAME = 44100 / 60;
constexpr int AUDIO_PRIME_FRAMES = 1472;
constexpr char FILE_ROOT_DEFAULT[] = "/roms/UXN";

alignas(4) uint16_t videoFrameAlt[OUTPUT_WIDTH * OUTPUT_HEIGHT];
uint16_t *videoFrames[2] = {};
unsigned videoWriteIndex = 1;
UxnScaleCache scaleCaches[2] = {};
uint16_t pendingDamage[2][16] = {};
int viewportX = 0;
int viewportY = 0;
int viewportWidth = OUTPUT_WIDTH;
int viewportHeight = OUTPUT_HEIGHT;
char fileRoot[FF_MAX_LFN] = "/roms/UXN";
uint64_t instructionCountAtLastReport = 0;
struct TimingCounter {
    uint64_t total = 0;
    uint32_t count = 0, maximum = 0;
    void add(uint32_t elapsed) {
        total += elapsed;
        ++count;
        maximum = std::max(maximum, elapsed);
    }
    void report(const char *name) {
        printf(" %s=%lu/%lu", name, static_cast<unsigned long>(count ? total / count : 0),
            static_cast<unsigned long>(maximum));
        total = count = maximum = 0;
    }
};
TimingCounter inputTiming, vmTiming, waitTiming, scaleTiming, audioTiming, queueTiming;
TimingCounter tileCounts;
TimingCounter scanTiming, renderTiming;
uint32_t presentElapsed = 0;

uint32_t previousButtons = 0;
uint8_t previousKeys[6] = {};
int mouseX = OUTPUT_WIDTH / 2;
int mouseY = OUTPUT_HEIGHT / 2;
uint8_t mouseButtons = 0;
bool leaveRuntime = false;

char reportBuffer[1024];
size_t reportLength = 0, reportSent = 0;
bool capturingReport = false;
void captureReport(const char *data, int length)
{
    if (!capturingReport) return;
    const size_t count = std::min(static_cast<size_t>(length),
                                  sizeof(reportBuffer) - reportLength);
    memcpy(reportBuffer + reportLength, data, count);
    reportLength += count;
}
stdio_driver_t reportDriver = {};

void drainReport()
{
    while (reportSent < reportLength && uart_is_writable(uart0))
        uart_putc_raw(uart0, reportBuffer[reportSent++]);
}

uint32_t nextVideoFrame = 0;
void paceRuntime()
{
    ++nextVideoFrame;
    while (static_cast<int32_t>(hstx_getframecounter() - nextVideoFrame) < 0) {
        drainReport();
        tight_loop_contents();
    }
    const uint32_t now = hstx_getframecounter();
    if (static_cast<int32_t>(now - nextVideoFrame) > 0) nextVideoFrame = now;
    drainReport();
}

void primeAudioOutputs()
{
    if (!settings.flags.audioEnabled) return;

    for (int i = 0; i < AUDIO_PRIME_FRAMES; ++i) {
        if (hstx_di_queue_get_level() < AUDIO_PRIME_FRAMES / 4)
            hstx_push_audio_sample(0, 0);
        if (EXT_AUDIO_GET_FREE() > I2S_AUDIO_RING_SIZE - 1 - AUDIO_PRIME_FRAMES)
            EXT_AUDIO_ENQUEUE_SAMPLE(0, 0);
    }
}

enum InputBits : uint32_t {
    INPUT_A = 1u << 0,
    INPUT_B = 1u << 1,
    INPUT_SELECT = 1u << 2,
    INPUT_START = 1u << 3,
    INPUT_UP = 1u << 4,
    INPUT_DOWN = 1u << 5,
    INPUT_LEFT = 1u << 6,
    INPUT_RIGHT = 1u << 7,
};

uint32_t usbButtons()
{
    const uint32_t buttons = io::getCurrentGamePadState(0).buttons |
                             io::getCurrentGamePadState(1).buttons;
    return
        (buttons & io::GamePadState::Button::A ? INPUT_A : 0) |
        (buttons & io::GamePadState::Button::B ? INPUT_B : 0) |
        (buttons & io::GamePadState::Button::SELECT ? INPUT_SELECT : 0) |
        (buttons & io::GamePadState::Button::START ? INPUT_START : 0) |
        (buttons & io::GamePadState::Button::UP ? INPUT_UP : 0) |
        (buttons & io::GamePadState::Button::DOWN ? INPUT_DOWN : 0) |
        (buttons & io::GamePadState::Button::LEFT ? INPUT_LEFT : 0) |
        (buttons & io::GamePadState::Button::RIGHT ? INPUT_RIGHT : 0);
}

uint32_t vintageButtons(int player)
{
#if NES_PIN_CLK == -1
    (void)player;
    return 0;
#else
    const uint16_t bits = nespad_states_ext[player];
    uint32_t result =
        (bits & (1u << 2) ? INPUT_SELECT : 0) |
        (bits & (1u << 3) ? INPUT_START : 0) |
        (bits & (1u << 4) ? INPUT_UP : 0) |
        (bits & (1u << 5) ? INPUT_DOWN : 0) |
        (bits & (1u << 6) ? INPUT_LEFT : 0) |
        (bits & (1u << 7) ? INPUT_RIGHT : 0);
    if (nespad_padtype[player] == NESPAD_TYPE_SNES) {
        result |= bits & (1u << 8) ? INPUT_A : 0;
        result |= bits & (1u << 0) ? INPUT_B : 0;
    } else {
        result |= bits & (1u << 0) ? INPUT_A : 0;
        result |= bits & (1u << 1) ? INPUT_B : 0;
    }
    return result;
#endif
}

uint32_t keyboardButtons()
{
    const io::KeyboardState &keyboard = io::getCurrentKeyboardState();
    uint32_t result =
        (keyboard.modifier & 0x11 ? INPUT_A : 0) |
        (keyboard.modifier & 0x44 ? INPUT_B : 0) |
        (keyboard.modifier & 0x22 ? INPUT_SELECT : 0);
    for (uint8_t key : keyboard.keycode) {
        switch (key) {
        case 0x4a: result |= INPUT_START; break;
        case 0x52: result |= INPUT_UP; break;
        case 0x51: result |= INPUT_DOWN; break;
        case 0x50: result |= INPUT_LEFT; break;
        case 0x4f: result |= INPUT_RIGHT; break;
        default: break;
        }
    }
    return result;
}

bool keyWasPressed(uint8_t key, const uint8_t oldKeys[6])
{
    if (!key) return false;
    for (int i = 0; i < 6; ++i)
        if (oldKeys[i] == key) return false;
    return true;
}

uint8_t hidToAscii(uint8_t key, uint8_t modifier)
{
    const bool shift = modifier & 0x22;
    if (key >= 0x04 && key <= 0x1d)
        return static_cast<uint8_t>((shift ? 'A' : 'a') + key - 0x04);
    if (key >= 0x1e && key <= 0x27) {
        static const char normal[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        return static_cast<uint8_t>((shift ? shifted : normal)[key - 0x1e]);
    }
    switch (key) {
    case 0x28: return '\n';
    case 0x29: return 0x1b;
    case 0x2a: return 0x08;
    case 0x2b: return '\t';
    case 0x2c: return ' ';
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    case 0x4c: return 0x7f;
    default: return 0;
    }
}

void pollKeyboard()
{
    const io::KeyboardState &keyboard = io::getCurrentKeyboardState();
    for (int i = 0; i < 6; ++i) {
        if (keyWasPressed(keyboard.keycode[i], previousKeys)) {
            const uint8_t ascii = hidToAscii(keyboard.keycode[i], keyboard.modifier);
            if (ascii) uxn_runtime_key(ascii);
        }
    }
    memcpy(previousKeys, keyboard.keycode, sizeof(previousKeys));
}

void pollMouse()
{
    io::MouseState &mouse = io::getCurrentMouseState();
    if (!mouse.connected) return;
    const int32_t dx = mouse.dx;
    const int32_t dy = mouse.dy;
    const int32_t wheel = mouse.wheel;
    mouse.dx = mouse.dy = mouse.wheel = 0;
    if (!dx && !dy && !wheel && mouse.buttons == mouseButtons) return;

    mouseX = std::clamp(mouseX + static_cast<int>(dx), 0, OUTPUT_WIDTH - 1);
    mouseY = std::clamp(mouseY + static_cast<int>(dy), 0, OUTPUT_HEIGHT - 1);
    const int localX = std::clamp(mouseX - viewportX, 0, viewportWidth - 1);
    const int localY = std::clamp(mouseY - viewportY, 0, viewportHeight - 1);
    const uint16_t uxnX = static_cast<uint16_t>(localX * uxn_runtime_width() / viewportWidth);
    const uint16_t uxnY = static_cast<uint16_t>(localY * uxn_runtime_height() / viewportHeight);
    const uint8_t rawButtons = mouse.buttons;
    mouseButtons = (rawButtons & 0x01) |
                   (rawButtons & 0x02 ? 0x04 : 0) |
                   (rawButtons & 0x04 ? 0x02 : 0);
    uxn_runtime_mouse(uxnX, uxnY, mouseButtons, 0, static_cast<int16_t>(wheel));
}

void pollInput()
{
#if NES_PIN_CLK != -1
    nespad_read_start();
#endif
    tuh_task();
#if NES_PIN_CLK != -1
    nespad_read_finish();
#endif
    pollKeyboard();
    pollMouse();

    const uint32_t buttons = usbButtons() | keyboardButtons() |
                             vintageButtons(0) | vintageButtons(1);
    const uint32_t pushed = buttons & ~previousButtons;
    previousButtons = buttons;

    if ((buttons & (INPUT_START | INPUT_SELECT)) == (INPUT_START | INPUT_SELECT) &&
        (pushed & (INPUT_START | INPUT_SELECT))) {
        hstx_disable_viewport();
        const int result = showSettingsMenu(true);
        if (result == 3) leaveRuntime = true;
        else if (result == 5 && !uxn_runtime_reset(false)) {
            snprintf(ErrorMessage, ERRORMESSAGESIZE, "Could not reset Uxn ROM");
            leaveRuntime = true;
        }
        Frens::applyScreenMode(ScreenMode::NOSCANLINE_1_1);
        videoWriteIndex = 1;
        hstx_configure_viewport(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT);
        if (!leaveRuntime) primeAudioOutputs();
        nextVideoFrame = hstx_getframecounter();
        scaleCaches[0].valid = scaleCaches[1].valid = 0;
        uxn_runtime_request_redraw();
        previousButtons = buttons;
    }
    uxn_runtime_controller(static_cast<uint8_t>(buttons));
}

void setFileRoot(const char *romPath)
{
    strncpy(fileRoot, romPath, sizeof(fileRoot) - 1);
    fileRoot[sizeof(fileRoot) - 1] = 0;
    if (!strncmp(fileRoot, "0:", 2)) memmove(fileRoot, fileRoot + 2, strlen(fileRoot + 2) + 1);
    char *slash = strrchr(fileRoot, '/');
    if (slash && slash != fileRoot) *slash = 0;
    else strcpy(fileRoot, FILE_ROOT_DEFAULT);
}

void runRom(const char *romPath)
{
    setFileRoot(romPath);
    if (ROM_FILE_ADDR) {
        Frens::f_free(reinterpret_cast<void *>(ROM_FILE_ADDR));
        ROM_FILE_ADDR = 0;
    }
    if (!uxn_runtime_boot(romPath)) {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Could not start Uxn ROM");
        return;
    }

    printf("[uxn] build=native-damage-v1 " __DATE__ " " __TIME__ " ROM=%s root=%s screen=%ux%u\n", romPath, fileRoot,
           uxn_runtime_width(), uxn_runtime_height());
    Frens::dumpHeapStats("uxn/start");
    pico_hdmi_set_audio_sample_rate(44100);
    Frens::applyScreenMode(ScreenMode::NOSCANLINE_1_1);
    memset(videoFrames[0], 0, OUTPUT_WIDTH * OUTPUT_HEIGHT * sizeof(uint16_t));
    memset(videoFrames[1], 0, OUTPUT_WIDTH * OUTPUT_HEIGHT * sizeof(uint16_t));
    videoWriteIndex = 1;
    scaleCaches[0].valid = scaleCaches[1].valid = 0;
    hstx_configure_viewport(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT);
    Frens::PaceFrames60fps(true);

    abSwapped = 1;
    EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
    primeAudioOutputs();
    nextVideoFrame = hstx_getframecounter();
    reportDriver.out_chars = captureReport;
    stdio_set_driver_enabled(&reportDriver, true);
    leaveRuntime = false;
    previousButtons = 0;
    memset(previousKeys, 0, sizeof(previousKeys));

    uint64_t reportStart = time_us_64();
    uint64_t frameTotal = 0;
    uint32_t frameMax = 0;
    uint32_t frameCount = 0;
    int externalAudioFreeMin = EXT_AUDIO_GET_FREE();
    int externalAudioFreeMax = externalAudioFreeMin;
    uint32_t hdmiUnderrunPrevious = hstx_di_queue_get_underrun_count();
    uint32_t i2sUnderrunPrevious = audio_i2s_get_underrun_count();
    uint32_t i2sOverflowPrevious = audio_i2s_get_overflow_count();
    uint32_t hdmiDropPrevious = hstx_audio_get_drop_count();
    instructionCountAtLastReport = uxn_runtime_instruction_count();
    uxn_runtime_take_draw_timing();
    static int16_t audio[AUDIO_FRAMES_PER_FRAME * 2];
    int pcmPeak = 0;
    unsigned hdmiClipped = 0;
    int hdmiPeak = 0;

    while (!leaveRuntime) {
        paceRuntime();
        const uint64_t frameStart = time_us_64();
        const int externalFreeBefore = EXT_AUDIO_GET_FREE();
        externalAudioFreeMin = std::min(externalAudioFreeMin, externalFreeBefore);
        externalAudioFreeMax = std::max(externalAudioFreeMax, externalFreeBefore);
        uint32_t stageStart = time_us_32();
        presentElapsed = 0;
        pollInput();
        inputTiming.add(time_us_32() - stageStart);
        stageStart = time_us_32();
        presentElapsed = 0;
        if (leaveRuntime || !uxn_runtime_frame()) break;
        vmTiming.add(time_us_32() - stageStart - presentElapsed);
        stageStart = time_us_32();
        uxn_runtime_audio(audio, AUDIO_FRAMES_PER_FRAME);
        audioTiming.add(time_us_32() - stageStart);
        stageStart = time_us_32();
        for (int i = 0; i < AUDIO_FRAMES_PER_FRAME * 2; ++i) {
            const int value = audio[i];
            pcmPeak = std::max(pcmPeak, value < 0 ? -value : value);
        }
        if (settings.flags.audioEnabled) {
            for (int i = 0; i < AUDIO_FRAMES_PER_FRAME; ++i) {
                const int left = uxn_hdmi_gain(audio[i * 2], &hdmiClipped);
                const int right = uxn_hdmi_gain(audio[i * 2 + 1], &hdmiClipped);
                hdmiPeak = std::max(hdmiPeak, std::max(left < 0 ? -left : left,
                    right < 0 ? -right : right));
                hstx_push_audio_sample(left, right);
                EXT_AUDIO_ENQUEUE_SAMPLE(audio[i * 2], audio[i * 2 + 1]);
            }
            const int externalFreeAfter = EXT_AUDIO_GET_FREE();
            externalAudioFreeMin = std::min(externalAudioFreeMin, externalFreeAfter);
            externalAudioFreeMax = std::max(externalAudioFreeMax, externalFreeAfter);
        }

        queueTiming.add(time_us_32() - stageStart);
        const uint32_t elapsed = static_cast<uint32_t>(time_us_64() - frameStart);
        frameTotal += elapsed;
        frameMax = std::max(frameMax, elapsed);
        frameCount++;
        const uint64_t now = time_us_64();
        if (now - reportStart >= 5000000) {
            const uint64_t count = uxn_runtime_instruction_count();
            const uint64_t delta = count - instructionCountAtLastReport;
            const uint64_t period = now - reportStart;
            const uint32_t hdmiUnderrunNow = hstx_di_queue_get_underrun_count();
            const uint32_t hdmiUnderrunDelta = hdmiUnderrunNow - hdmiUnderrunPrevious;
            const uint32_t i2sUnderrunNow = audio_i2s_get_underrun_count();
            const uint32_t i2sOverflowNow = audio_i2s_get_overflow_count();
            const uint32_t hdmiDropNow = hstx_audio_get_drop_count();

            reportLength = reportSent = 0;
            capturingReport = true;
            stdio_filter_driver(&reportDriver);
            printf("[uxn] ips=%llu frame_us(avg/max)=%llu/%lu "
                   "audio_di(level/underruns_delta/total)=%u/%lu/%lu "
                   "ext_free(min/max/now)=%d/%d/%d "
                   "i2s_delta(underrun/overflow)=%lu/%lu "
                   "hdmi_drop_delta=%lu "
                   "screen=%ux%u\n",
                   static_cast<unsigned long long>(delta * 1000000 / period),
                   static_cast<unsigned long long>(frameCount ? frameTotal / frameCount : 0),
                   static_cast<unsigned long>(frameMax),
                   static_cast<unsigned>(hstx_di_queue_get_level()),
                   static_cast<unsigned long>(hdmiUnderrunDelta),
                   static_cast<unsigned long>(hdmiUnderrunNow),
                   externalAudioFreeMin, externalAudioFreeMax, EXT_AUDIO_GET_FREE(),
                   static_cast<unsigned long>(i2sUnderrunNow - i2sUnderrunPrevious),
                   static_cast<unsigned long>(i2sOverflowNow - i2sOverflowPrevious),
                   static_cast<unsigned long>(hdmiDropNow - hdmiDropPrevious),
                   uxn_runtime_width(), uxn_runtime_height());
            Frens::dumpHeapStats("uxn/5s");
            printf("[timing_us avg/max]");
            inputTiming.report("input");
            vmTiming.report("vm");
            waitTiming.report("display_wait");
            scaleTiming.report("scale");
            audioTiming.report("audio");
            queueTiming.report("queue");
            tileCounts.report("tiles");
            scanTiming.report("scan");
            renderTiming.report("render");
            printf(" enabled=%u pcm_peak=%d\n", settings.flags.audioEnabled ? 1u : 0u, pcmPeak);
            pcmPeak = 0;
            printf("[hdmi-gain4-v1] peak=%d clipped=%u\n", hdmiPeak, hdmiClipped);
            hdmiPeak = 0;
            hdmiClipped = 0;
            const auto drawing = uxn_runtime_take_draw_timing();
            printf("[native-damage-v1 us/frame] fill=%lu sprite=%lu calls=%lu/%lu clock=%lu\n",
                static_cast<unsigned long>(frameCount ? drawing.fill_us / frameCount : 0),
                static_cast<unsigned long>(frameCount ? drawing.sprite_us / frameCount : 0),
                static_cast<unsigned long>(drawing.fill_calls),
                static_cast<unsigned long>(drawing.sprite_calls),
                static_cast<unsigned long>(clock_get_hz(clk_sys)));
            stdio_filter_driver(nullptr);
            capturingReport = false;
            reportStart = now;
            instructionCountAtLastReport = count;
            hdmiUnderrunPrevious = hdmiUnderrunNow;
            i2sUnderrunPrevious = i2sUnderrunNow;
            i2sOverflowPrevious = i2sOverflowNow;
            hdmiDropPrevious = hdmiDropNow;
            frameTotal = frameMax = frameCount = 0;
            externalAudioFreeMin = EXT_AUDIO_GET_FREE();
            externalAudioFreeMax = externalAudioFreeMin;
        }
    }
    while (reportSent < reportLength) drainReport();
    stdio_set_driver_enabled(&reportDriver, false);
    hstx_disable_viewport();
    uxn_runtime_shutdown();
    for (auto &cache : scaleCaches) {
        if (cache.pixels) Frens::f_free(cache.pixels);
        cache = {};
    }
    abSwapped = 1;
}

}

const int8_t g_settings_visibility_uxn[MOPT_COUNT] = {
    1, 1, BOOTLOADER_BUILD, 0,
    0, 0, 0, 0,
    1, 0, HSTX, 0,
    1, 1, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 1, 1, 1,
};

const uint8_t g_available_screen_modes_uxn[] = {0, 0, 0, 1};

extern "C" void *uxn_platform_alloc(size_t size)
{
    return Frens::f_malloc(size);
}

extern "C" void *uxn_platform_realloc(void *pointer, size_t size)
{
    return pointer ? Frens::f_realloc(pointer, size) : Frens::f_malloc(size);
}

extern "C" void uxn_platform_free(void *pointer)
{
    Frens::f_free(pointer);
}

extern "C" void uxn_platform_screen_resized(int width, int height)
{
    scaleCaches[0].valid = scaleCaches[1].valid = 0;
    printf("[uxn] screen resize %dx%d\n", width, height);
}

extern "C" void uxn_platform_present(const uint8_t *pixels, int stride, int width,
                                       int height, const uint32_t palette[16])
{
    const uint32_t presentStart = time_us_32();
    int outWidth = width;
    int outHeight = height;
    if (outWidth > OUTPUT_WIDTH || outHeight > OUTPUT_HEIGHT) {
        if (static_cast<int64_t>(OUTPUT_WIDTH) * height <=
            static_cast<int64_t>(OUTPUT_HEIGHT) * width) {
            outWidth = OUTPUT_WIDTH;
            outHeight = std::max(1, height * OUTPUT_WIDTH / width);
        } else {
            outHeight = OUTPUT_HEIGHT;
            outWidth = std::max(1, width * OUTPUT_HEIGHT / height);
        }
    }
    viewportWidth = outWidth;
    viewportHeight = outHeight;
    viewportX = (OUTPUT_WIDTH - outWidth) / 2;
    viewportY = (OUTPUT_HEIGHT - outHeight) / 2;

    uint16_t *output = videoFrames[videoWriteIndex];

    const uint32_t waitStart = time_us_32();
    while (hstx_viewport_buffer_in_use(output)) tight_loop_contents();
    waitTiming.add(time_us_32() - waitStart);
    const uint32_t scaleStart = time_us_32();
    uint16_t converted[16];
    for (int i = 0; i < 16; ++i) {
        const uint32_t c = palette[i];
        converted[i] = static_cast<uint16_t>(((c >> 19) & 0x1f) << 10 |
                                              ((c >> 11) & 0x1f) << 5 |
                                              ((c >> 3) & 0x1f));
    }
    auto &cache = scaleCaches[videoWriteIndex];
    const bool tiled = width == 256 && outHeight == 240 &&
        ((height == 256 && outWidth == 240) || (height == 240 && outWidth == 256));
    if (tiled && !cache.pixels)
        cache.pixels = static_cast<uint8_t *>(Frens::f_malloc(256 * 256));
    if (tiled && cache.pixels) {
        const uint16_t *damage = uxn_runtime_screen_damage();
        for (int y = 0; y < 16; ++y)
            for (int slot = 0; slot < 2; ++slot)
                pendingDamage[slot][y] |= damage ? damage[y] : 0xffffu;
        if (!cache.valid) memset(output, 0, OUTPUT_WIDTH * OUTPUT_HEIGHT * sizeof(*output));
        tileCounts.add(uxn_present_cached_256(pixels, stride,
            output + viewportY * OUTPUT_WIDTH + viewportX, OUTPUT_WIDTH, converted, &cache,
            pendingDamage[videoWriteIndex], height));
        memset(pendingDamage[videoWriteIndex], 0, sizeof(pendingDamage[videoWriteIndex]));
        scanTiming.add(cache.scan_us);
        renderTiming.add(cache.render_us);
    } else {
        scaleCaches[0].valid = scaleCaches[1].valid = 0;
        memset(output, 0, OUTPUT_WIDTH * OUTPUT_HEIGHT * sizeof(*output));
        uxn_scale_screen(pixels, stride, width, height,
            output + viewportY * OUTPUT_WIDTH + viewportX, OUTPUT_WIDTH,
            outWidth, outHeight, converted);
    }
    hstx_submit_viewport(output);
    videoWriteIndex ^= 1u;
    scaleTiming.add(time_us_32() - scaleStart);
    presentElapsed += time_us_32() - presentStart;
}

extern "C" bool uxn_platform_resolve_path(const char *source, char *dest, size_t capacity)
{
    return uxn_resolve_file_path(source, fileRoot, dest, capacity);
}

extern "C" void uxn_platform_datetime(UxnDateTime *result)
{

    *result = UxnDateTime{2000, 0, 1, 0, 0, 0, 6, 0, 0};
}

int main()
{
    char selectedRom[FF_MAX_LFN] = {};
    Frens::setClocksAndStartStdio(CPU_FREQ_KHZ, VREG_VOLTAGE_1_20);
    printf("Uxn2 / Varvara for Adafruit Fruit Jam\n");
    printf("CPU: %lu kHz\n", static_cast<unsigned long>(clock_get_hz(clk_sys) / 1000));

    FrensSettings::initSettings(FrensSettings::emulators::UXN);
    const bool fatal = !Frens::initAll(selectedRom, CPU_FREQ_KHZ, 0, 0, 1024, false, true);
    g_settings_visibility = g_settings_visibility_uxn;
    g_available_screen_modes = g_available_screen_modes_uxn;
    settings.screenMode = ScreenMode::NOSCANLINE_1_1;
    Frens::applyScreenMode(settings.screenMode);

    videoFrames[0] = reinterpret_cast<uint16_t *>(hstx_getframebuffer());
    videoFrames[1] = videoFrameAlt;
    memset(videoFrames[0], 0, OUTPUT_WIDTH * OUTPUT_HEIGHT * sizeof(uint16_t));
    memset(videoFrames[1], 0, OUTPUT_WIDTH * OUTPUT_HEIGHT * sizeof(uint16_t));
    printf("[uxn] SRAM HSTX buffers=%p/%p\n",
           static_cast<void *>(videoFrames[0]), static_cast<void *>(videoFrames[1]));

    bool showSplash = true;
    while (true) {
        selectedRom[0] = 0;
        menu("Uxn / Varvara", ErrorMessage, fatal, showSplash, ".rom", selectedRom);
        showSplash = false;
        FILINFO info{};
        if (f_stat(selectedRom, &info) != FR_OK || !info.fsize || info.fsize > 0xfff00) {
            snprintf(ErrorMessage, ERRORMESSAGESIZE, "ROM must be 1-1048320 bytes");
            if (ROM_FILE_ADDR) {
                Frens::f_free(reinterpret_cast<void *>(ROM_FILE_ADDR));
                ROM_FILE_ADDR = 0;
            }
            continue;
        }
        runRom(selectedRom);
    }
}
