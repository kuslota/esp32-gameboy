#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define GB_WIDTH 160
#define GB_HEIGHT 144
#define GB_PACKED_ROW 40
#define GB_PACKED_BYTES (GB_PACKED_ROW * GB_HEIGHT)
#define GB_CLOCK_HZ 4194304u
#define GB_FRAME_CYCLES 70224u
#define GB_MAX_ROM_BYTES (2u * 1024u * 1024u)
#define GB_BTN_A 1u
#define GB_BTN_B 2u
#define GB_BTN_SELECT 4u
#define GB_BTN_START 8u
#define GB_BTN_RIGHT 16u
#define GB_BTN_LEFT 32u
#define GB_BTN_UP 64u
#define GB_BTN_DOWN 128u
/* Single-owner API. Caller keeps ROM storage alive until the next init. */
bool gb_emulator_validate_rom(const uint8_t *rom, size_t size);
size_t gb_emulator_rom_save_size(const uint8_t *rom, size_t size);
bool gb_emulator_init(const uint8_t *rom, size_t size);
bool gb_emulator_frame(bool skip_render);
const uint8_t *gb_emulator_pixels(void);
void gb_emulator_buttons(uint8_t pressed);
uint8_t gb_emulator_demo_counter(void);
int gb_emulator_error(void);
typedef struct {
    int error;
    /* Core callback argument: memory address for I/O, byte for illegal opcode. */
    uint16_t error_argument, error_pc;
    uint32_t ignored_io_reads, ignored_io_writes;
    uint16_t first_ignored_io_address, first_ignored_io_pc;
} gb_emulator_diagnostics_t;
gb_emulator_diagnostics_t gb_emulator_diagnostics(void);
void gb_emulator_shutdown(void);
size_t gb_emulator_save_size(void);
bool gb_emulator_save_dirty(void);
bool gb_emulator_save_copy(uint8_t *dst, size_t size);
bool gb_emulator_save_load(const uint8_t *src, size_t size);
void gb_emulator_save_clean(void);
/* MBC3 clock cartridges (0x0F, 0x10) append the 48-byte BGB/VBA-M clock footer to the save.
 * save_load also accepts the RAM-only and 44-byte-footer forms other emulators write. */
#define GB_RTC_FOOTER_BYTES 48u
bool gb_emulator_rom_save_accepted(const uint8_t *rom, size_t rom_size, size_t save_size);
/* Seconds on a clock that keeps running while the device is off, or false if there is none.
 * The cartridge clock follows it, so time passes across pauses and power-off. Without one the
 * cartridge clock advances with emulated time only. Called once per frame from the core task. */
typedef bool (*gb_emulator_clock_fn)(int64_t *seconds);
void gb_emulator_set_clock(gb_emulator_clock_fn clock);

/* One 535/536-sample mono PCM block per frame, exactly 32 kHz over time. */
size_t gb_emulator_audio_samples(int16_t *dst, size_t capacity);
size_t gb_emulator_state_size(void);
bool gb_emulator_state_copy(uint8_t *dst, size_t size);
bool gb_emulator_state_load(const uint8_t *src, size_t size);
