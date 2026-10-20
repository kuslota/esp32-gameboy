#include "gb_emulator.h"
#include <setjmp.h>
#include <string.h>
#include <stdlib.h>
#define PGB_CGB 0
#define ENABLE_SOUND 1
#ifndef MINIGB_APU_AUDIO_FORMAT_S16SYS
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#endif
#define ENABLE_LCD 1
#define PGB_IMPL
#include "vendor/peanut_gb.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

int preferences_cgb_speed, preferences_ppu_timing, audio_enabled;
static gb_s machine;
static struct minigb_apu_ctx apu;
static int16_t stereo[AUDIO_SAMPLES_TOTAL];
static int16_t pending_audio[AUDIO_SAMPLES];
static bool pending_audio_ready;
static size_t pending_audio_count;
static uint32_t sample_phase;

uint8_t audio_read(audio_data *audio, uint16_t addr)
{
    (void)audio;
    return minigb_apu_audio_read(&apu, addr);
}
void audio_write(audio_data *audio, uint16_t addr, uint8_t value)
{
    (void)audio;
    minigb_apu_audio_write(&apu, addr, value);
}

/* Heap-allocated per game so the memory is free outside gameplay. */
static uint8_t *wram, *vram, *lcd;
static uint8_t *cart_ram;
static size_t cart_ram_size;
static int error_code;
static gb_emulator_diagnostics_t diagnostics;
static bool ready, guard_active;
static jmp_buf error_guard;
/* MBC3 clock: rtc_wall is the clock second the registers were last brought up to date
 * (0 = unknown); rtc_cycles accumulates emulated time when no clock is available. */
static bool has_rtc;
static gb_emulator_clock_fn clock_source;
static int64_t rtc_wall;
static uint32_t rtc_cycles;
typedef struct { int64_t wall; uint32_t cycles, reserved; } rtc_state;

#ifdef ESP_PLATFORM
static void *caps_alloc(size_t size, bool zero, uint32_t caps)
{
    return zero ? heap_caps_calloc(1, size, caps | MALLOC_CAP_8BIT) :
                  heap_caps_malloc(size, caps | MALLOC_CAP_8BIT);
}
#endif

static void *large_alloc(size_t size, bool zero)
{
#ifdef ESP_PLATFORM
    return caps_alloc(size, zero, MALLOC_CAP_SPIRAM);
#else
    return zero ? calloc(1, size) : malloc(size);
#endif
}

static void *graphics_alloc(size_t size)
{
#if defined(ESP_PLATFORM) && defined(CONFIG_GAMEBOY_GRAPHICS_INTERNAL_RAM)
    void *memory = caps_alloc(size, false, MALLOC_CAP_INTERNAL);
    return memory ? memory : caps_alloc(size, false, MALLOC_CAP_SPIRAM);
#else
    return large_alloc(size, false);
#endif
}

static bool unused_dmg_io(uint16_t address)
{
    /* Returning from the core's callback makes its fallback ignore writes and read FF.
     * Deliberately not a blanket FF00-FF7F pass, which would hide real errors. */
    return address == 0xff03 || (address >= 0xff08 && address <= 0xff0e) ||
           address == 0xff4e || (address >= 0xff60 && address <= 0xff67) ||
           (address >= 0xff6d && address <= 0xff6f) || address == 0xff71 ||
           (address >= 0xff78 && address <= 0xff7f);
}

static void core_error(gb_s *gb, enum gb_error_e error, uint16_t address)
{
    if ((error == GB_INVALID_READ || error == GB_INVALID_WRITE) && unused_dmg_io(address)) {
        if (!diagnostics.ignored_io_reads && !diagnostics.ignored_io_writes) {
            diagnostics.first_ignored_io_address = address;
            diagnostics.first_ignored_io_pc = gb->cpu_reg.pc;
        }
        uint32_t *count = error == GB_INVALID_READ ? &diagnostics.ignored_io_reads :
                                                   &diagnostics.ignored_io_writes;
        if (*count != UINT32_MAX) ++*count;
        return;
    }
    error_code = (int)error + 1;
    diagnostics.error_argument = address;
    diagnostics.error_pc = gb->cpu_reg.pc;
    ready = false;
    if (guard_active) longjmp(error_guard, 1);
}
void __gb_on_breakpoint(gb_s *gb, int number)
{
    (void)gb; (void)number;
    core_error(gb, GB_UNKNOWN_ERROR, 0);
}

bool gb_emulator_validate_rom(const uint8_t *rom, size_t size)
{
    if (!rom || size < 32768 || size > GB_MAX_ROM_BYTES || rom[0x148] > 6 ||
        size != (32768u << rom[0x148]) || rom[0x149] > 5 || rom[0x149] == 1 ||
        (rom[0x143] & 0xc0) == 0xc0) return false;
    bool ram;
    /* Rumble (0x1c-0x1e) mappers are deliberately absent. */
    switch (rom[0x147]) {
    case 0x0f: ram = false; break;
    case 0x10: ram = true; break;
    case 0: if (size != 32768) return false; ram = false; break;
    case 8: case 9: if (size != 32768) return false; ram = true; break;
    case 1: case 0x11: case 0x19: ram = false; break;
    case 2: case 3: case 0x12: case 0x13: case 0x1a: case 0x1b: ram = true; break;
    case 5: case 6: if (size > 262144) return false; ram = false; break;
    default: return false;
    }
    if (ram != (rom[0x149] != 0)) return false;
    if (rom[0x147] < 0x19 && rom[0x149] > 3) return false;
    uint8_t checksum = 0;
    for (unsigned i = 0x134; i <= 0x14c; ++i) checksum = checksum - rom[i] - 1;
    return checksum == rom[0x14d];
}

size_t gb_emulator_rom_save_size(const uint8_t *rom, size_t size)
{
    if (!gb_emulator_validate_rom(rom, size) || !gb_get_rom_uses_battery((uint8_t *)rom)) return 0;
    if (rom[0x147] == 6) return 512;
    static const size_t sizes[] = {0, 2048, 8192, 32768, 131072, 65536};
    const bool rtc = rom[0x147] == 0x0f || rom[0x147] == 0x10;
    return sizes[rom[0x149]] + (rtc ? GB_RTC_FOOTER_BYTES : 0);
}

void gb_emulator_set_clock(gb_emulator_clock_fn clock) { clock_source = clock; }

static void rtc_advance(void)
{
    int64_t now;
    if (clock_source && clock_source(&now)) {
        /* A clock that went backwards (or a save from the future) just re-anchors. */
        if (rtc_wall > 0 && now > rtc_wall) {
            int64_t elapsed = now - rtc_wall;
            gb_catch_up_rtc_direct(&machine, elapsed > UINT32_MAX ? UINT32_MAX : (unsigned)elapsed);
        }
        rtc_wall = now;
        return;
    }
    rtc_wall = 0;
    rtc_cycles += GB_FRAME_CYCLES;
    if (rtc_cycles >= GB_CLOCK_HZ) {
        rtc_cycles -= GB_CLOCK_HZ; gb_catch_up_rtc_direct(&machine, 1);
        /* With no wall-clock anchor the registers are the only record of this second. */
        machine.direct.sram_updated = 1;
    }
}

static void put_le(uint8_t *dst, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) dst[i] = (uint8_t)(value >> (8 * i));
}
static uint64_t get_le(const uint8_t *src, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= (uint64_t)src[i] << (8 * i);
    return value;
}

bool gb_emulator_init(const uint8_t *rom, size_t size)
{
    ready = false; error_code = 0;
    memset(&diagnostics, 0, sizeof(diagnostics));
    has_rtc = false; rtc_wall = 0; rtc_cycles = 0;
    free(cart_ram); cart_ram = NULL; cart_ram_size = 0;
    if (!gb_emulator_validate_rom(rom, size)) { error_code = -1; return false; }
    if (!wram) wram = graphics_alloc(WRAM_SIZE_CGB);
    if (!vram) vram = graphics_alloc(VRAM_SIZE_CGB);
    if (!lcd) lcd = graphics_alloc(GB_PACKED_BYTES);
    if (!wram || !vram || !lcd) {
        free(wram); free(vram); free(lcd);
        wram = vram = lcd = NULL;
        error_code = -2; return false;
    }
    memset(&machine, 0, sizeof(machine));
    minigb_apu_audio_init(&apu);
    pending_audio_ready = false;
    pending_audio_count = 0;
    sample_phase = 0;
    memset(wram, 0, WRAM_SIZE_CGB); memset(vram, 0, VRAM_SIZE_CGB);
    memset(lcd, 0, GB_PACKED_BYTES);
    enum gb_init_error_e result = gb_init(&machine, wram, vram, lcd, (uint8_t *)rom,
                                         size, core_error, NULL, false);
    /* The core returns REQUIRES_CGB for DMG carts too; validate_rom already rejects CGB-only ones. */
    if (result != GB_INIT_NO_ERROR && result != GB_INIT_NO_ERROR_BUT_REQUIRES_CGB) {
        error_code = -3; return false;
    }
    gb_reset(&machine, false);
    machine.audio.audio_mem = apu.audio_mem;
    cart_ram_size = gb_get_save_size(&machine);
    if (cart_ram_size) {
        cart_ram = large_alloc(cart_ram_size, true);
        if (!cart_ram) { error_code = -4; cart_ram_size = 0; return false; }
        machine.gb_cart_ram = cart_ram;
        machine.gb_cart_ram_size = cart_ram_size;
        __gb_update_map_pointers(&machine);
    }
    gb_init_lcd(&machine);
    machine.direct.joypad = 0xff;
    has_rtc = rom[0x147] == 0x0f || rom[0x147] == 0x10;
    ready = true;
    return true;
}
bool gb_emulator_frame(bool skip_render)
{
    if (!ready) return false;
    /* Catch up before the game runs so boot-time reads and latches see the current time. */
    if (has_rtc) rtc_advance();
    if (setjmp(error_guard)) { guard_active = false; return false; }
    guard_active = true;
    machine.direct.frame_skip = skip_render;
    gb_run_frame__dmg(&machine);
    machine.direct.frame_skip = false;
    guard_active = false;
    sample_phase += 32000u * GB_FRAME_CYCLES;
    pending_audio_count = sample_phase / GB_CLOCK_HZ;
    sample_phase %= GB_CLOCK_HZ;
    minigb_apu_audio_callback(&apu, stereo, pending_audio_count);
    for (size_t i = 0; i < pending_audio_count; ++i) {
        pending_audio[i] = (int16_t)(((int32_t)stereo[2*i] + stereo[2*i+1]) / 2);
    }
    pending_audio_ready = true;
    return ready;
}
const uint8_t *gb_emulator_pixels(void) { return lcd; }
void gb_emulator_buttons(uint8_t pressed) { machine.direct.joypad = (uint8_t)~pressed; }
uint8_t gb_emulator_demo_counter(void) { return wram ? wram[0] : 0; }
int gb_emulator_error(void) { return error_code; }
gb_emulator_diagnostics_t gb_emulator_diagnostics(void)
{
    gb_emulator_diagnostics_t result = diagnostics;
    result.error = error_code;
    return result;
}
void gb_emulator_shutdown(void)
{
    ready = false;
    guard_active = false;
    free(cart_ram);
    cart_ram = NULL;
    cart_ram_size = 0;
    free(wram); free(vram); free(lcd);
    wram = vram = lcd = NULL;
    pending_audio_ready = false;
    pending_audio_count = 0;
    memset(&machine, 0, sizeof(machine));
}
size_t gb_emulator_save_size(void)
{
    return machine.cart_battery ? cart_ram_size + (has_rtc ? GB_RTC_FOOTER_BYTES : 0) : 0;
}
static bool save_accepted(size_t ram, bool rtc, size_t size)
{
    if (!rtc) return size && size == ram;
    return (size && size == ram) || size == ram + 44 || size == ram + GB_RTC_FOOTER_BYTES;
}
bool gb_emulator_rom_save_accepted(const uint8_t *rom, size_t rom_size, size_t save_size)
{
    const size_t full = gb_emulator_rom_save_size(rom, rom_size);
    if (!full) return false;
    const bool rtc = rom[0x147] == 0x0f || rom[0x147] == 0x10;
    return save_accepted(full - (rtc ? GB_RTC_FOOTER_BYTES : 0), rtc, save_size);
}
bool gb_emulator_save_dirty(void) { return ready && machine.direct.sram_updated; }
bool gb_emulator_save_copy(uint8_t *dst, size_t size)
{
    if (!ready || !dst || !size || size != gb_emulator_save_size()) return false;
    memcpy(dst, cart_ram, cart_ram_size);
    if (has_rtc) {
        /* Current then latched seconds, minutes, hours, day low, day high as 32-bit words,
         * then the 64-bit clock second they were valid at. */
        uint8_t *footer = dst + cart_ram_size;
        for (unsigned i = 0; i < 5; ++i) {
            put_le(footer + 4 * i, machine.cart_rtc[i], 4);
            put_le(footer + 20 + 4 * i, machine.latched_rtc[i], 4);
        }
        put_le(footer + 40, rtc_wall > 0 ? (uint64_t)rtc_wall : 0, 8);
    }
    return true;
}
bool gb_emulator_save_load(const uint8_t *src, size_t size)
{
    if (!ready || !src || !gb_emulator_save_size() || !save_accepted(cart_ram_size, has_rtc, size)) return false;
    memcpy(cart_ram, src, cart_ram_size);
    if (has_rtc && size > cart_ram_size) {
        static const uint8_t mask[5] = {0x3f, 0x3f, 0x1f, 0xff, 0xc1};
        const uint8_t *footer = src + cart_ram_size;
        for (unsigned i = 0; i < 5; ++i) {
            machine.cart_rtc[i] = (uint8_t)get_le(footer + 4 * i, 4) & mask[i];
            machine.latched_rtc[i] = (uint8_t)get_le(footer + 20 + 4 * i, 4) & mask[i];
        }
        const uint64_t wall = get_le(footer + 40, size - cart_ram_size == 44 ? 4 : 8);
        rtc_wall = wall <= INT64_MAX ? (int64_t)wall : 0;
    }
    machine.direct.sram_dirty = 0; machine.direct.sram_updated = 0; return true;
}
void gb_emulator_save_clean(void) { machine.direct.sram_dirty = 0; machine.direct.sram_updated = 0; }

size_t gb_emulator_audio_samples(int16_t *dst, size_t capacity)
{
    if (!ready || !pending_audio_ready || !dst || capacity < pending_audio_count) return 0;
    memcpy(dst, pending_audio, pending_audio_count * sizeof(int16_t));
    pending_audio_ready = false;
    return pending_audio_count;
}

typedef struct {
    uint8_t magic[8];
    uint32_t version;
    uint32_t core_size;
    uint32_t apu_size;
    uint32_t lcd_size;
    uint32_t sample_phase;
} frontend_state_header;
static const uint8_t state_magic[8] = {'G','B','E','S','T','A','T','E'};

size_t gb_emulator_state_size(void)
{
    /* Only clock cartridges carry the trailing clock block, so other games' checkpoints keep their size. */
    return ready ? sizeof(frontend_state_header) + gb_get_state_size(&machine) + sizeof(apu) + GB_PACKED_BYTES +
                   (has_rtc ? sizeof(rtc_state) : 0) : 0;
}
bool gb_emulator_state_copy(uint8_t *dst, size_t size)
{
    if (!ready || !dst || size != gb_emulator_state_size()) return false;
    frontend_state_header header = {{'G','B','E','S','T','A','T','E'}, 2,
                                    gb_get_state_size(&machine), sizeof(apu), GB_PACKED_BYTES, sample_phase};
    memcpy(dst, &header, sizeof(header));
    gb_state_save(&machine, (char *)(dst + sizeof(header)));
    memcpy(dst + sizeof(header) + header.core_size, &apu, sizeof(apu));
    memcpy(dst + sizeof(header) + header.core_size + sizeof(apu), lcd, GB_PACKED_BYTES);
    if (has_rtc) {
        const rtc_state clock = {rtc_wall, rtc_cycles, 0};
        memcpy(dst + sizeof(header) + header.core_size + sizeof(apu) + GB_PACKED_BYTES, &clock, sizeof(clock));
    }
    return true;
}
bool gb_emulator_state_load(const uint8_t *src, size_t size)
{
    if (!ready || !src || size != gb_emulator_state_size()) return false;
    frontend_state_header header;
    memcpy(&header, src, sizeof(header));
    if (memcmp(header.magic, state_magic, sizeof(state_magic)) || header.version != 2 ||
        header.core_size != gb_get_state_size(&machine) || header.apu_size != sizeof(apu) || header.lcd_size != GB_PACKED_BYTES ||
        header.sample_phase >= GB_CLOCK_HZ) return false;
    char *aligned_core = large_alloc(header.core_size, false);
    if (!aligned_core) return false;
    memcpy(aligned_core, src + sizeof(header), header.core_size);
    const char *error = gb_state_load(&machine, aligned_core, header.core_size);
    free(aligned_core);
    if (error) return false;
    memcpy(&apu, src + sizeof(header) + header.core_size, sizeof(apu));
    memcpy(lcd, src + sizeof(header) + header.core_size + sizeof(apu), GB_PACKED_BYTES);
    machine.audio.audio_mem = apu.audio_mem;
    if (machine.cart_battery && cart_ram_size) {
        machine.direct.sram_dirty = 1;
        machine.direct.sram_updated = 1;
    }
    pending_audio_ready = false;
    pending_audio_count = 0;
    sample_phase = header.sample_phase;
    if (has_rtc) {
        rtc_state clock;
        memcpy(&clock, src + sizeof(header) + header.core_size + sizeof(apu) + GB_PACKED_BYTES, sizeof(clock));
        /* The next frame catches the registers up from the checkpoint's clock second. */
        rtc_wall = clock.wall > 0 ? clock.wall : 0;
        rtc_cycles = clock.cycles < GB_CLOCK_HZ ? clock.cycles : 0;
    }
    return true;
}
