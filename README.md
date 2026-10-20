# ESP32 Game Boy

A Game Boy (DMG) emulator core for the ESP32-S3, with cartridge, battery-save
and save-state storage and a USB serial gamepad host. The core is based on
[CrankBoy](https://github.com/CrankBoyHQ/crankboy-app) and
[Peanut-GB](https://github.com/deltabeard/Peanut-GB). The emulator and storage
code are plain C with no display, audio or input dependencies; your firmware
supplies those.

The repository is both an ESP-IDF component and a PlatformIO/Arduino library.
No commercial cartridge is included.

## Supported cartridges and limits

- DMG and dual-mode cartridges up to 2 MiB: ROM-only, MBC1, MBC2, MBC3 with or
  without its clock (0x0F, 0x10), and MBC5 without rumble.
- CGB-only cartridges are rejected.
- The ROM catalog lists `.gb` and `.gbc` files; dual-mode cartridges run in DMG
  mode.
- Save states identify their ROM by CRC32 and exact size, and carry the core
  serializer version. A state written by a different core build is rejected.
- Battery `.sav` files are raw cartridge RAM; clock cartridges append the 48-byte
  BGB/VBA-M clock footer. `game.gb` saves to `game.sav`;
  other names, such as `game.gbc`, append `.sav`. The checkpoint is
  `<rom file>.state`. Both keep a `.bak` copy of the previous version.
- Only one emulator can run at a time; the core is a singleton.

## Memory budget

At the largest supported cartridge, budget about 2.8 MiB of PSRAM during a
checkpoint:

| Use | Size |
| --- | --- |
| ROM | 2 MiB |
| WRAM, VRAM and LCD buffer | 54,912 bytes |
| Live cartridge RAM | up to 128 KiB |
| Battery-write snapshot | up to 128 KiB |
| State buffer | about 188 KiB |
| Transactional state-file copy | about 188 KiB |

The WRAM/VRAM/LCD block is allocated by `gb_emulator_init` and freed by
`gb_emulator_shutdown`. The core's CPU and APU structs stay in internal RAM. The
`gameboy::Session` emulator and save tasks use 8 KiB and 4 KiB internal stacks
while a game is active.

## Using it from ESP-IDF

The component targets the ESP32-S3 and is built and checked against ESP-IDF
v6.1. The IDF v5.x `usb`/`hal` branch of `CMakeLists.txt` has not been
exercised. Enable octal or quad PSRAM in your project: the ROM, cartridge RAM
and state buffers live there.

Add the repository as a submodule under your project's `components/` directory
and require it:

```sh
git submodule add https://github.com/kuslota/esp32-gameboy.git components/esp32-gameboy
```

```cmake
idf_component_register(SRCS "main.c" REQUIRES esp32-gameboy)
```

`gameboy::Session` and `gameboy::Controller` are PlatformIO/Arduino only. The C
APIs below work in plain ESP-IDF. Their headers carry no `extern "C"` guards,
except `usb_gamepad.h`, so wrap the others yourself when including them from
C++.

### Kconfig options

Both options are under `ESP32 Game Boy` in `menuconfig`.

- `GAMEBOY_USB_SERIAL_HOST` (default on, chips with USB OTG only) builds the USB
  serial gamepad host. On IDF 6.x it pulls the managed `espressif/usb`
  component through `idf_component.yml`; on 5.x it uses IDF's built-in `usb`.
- `GAMEBOY_GRAPHICS_INTERNAL_RAM` (default on) tries internal RAM for the
  54,912-byte WRAM/VRAM/LCD block and falls back to PSRAM. Without it, or in a
  PlatformIO build that does not define `CONFIG_GAMEBOY_GRAPHICS_INTERNAL_RAM`,
  the block is allocated in PSRAM.

### Emulator (`gb_emulator.h`)

One task must own the emulator; its functions are not safe to call
concurrently. `gb_emulator_init(rom, size)` borrows the ROM buffer, which must
stay valid until the next `init` or `gb_emulator_shutdown`. On failure it returns
false and sets `gb_emulator_error()`: `-1` unsupported ROM, `-2` out of memory,
`-3` core initialization failed, `-4` cartridge RAM allocation failed.

Per frame, call `gb_emulator_buttons(mask)` with the `GB_BTN_*` bits (A, B,
Select, Start, Right, Left, Up, Down), then `gb_emulator_frame(skip_render)`.
It returns false once the core has stopped on an illegal opcode;
`gb_emulator_diagnostics()` reports the cause. Pace frames with
`gb_frame_time_us(n)`, the exact cumulative time of `n` frames at the DMG rate.

- `gb_emulator_pixels()` returns the 160x144 frame: 2 bits per pixel, four
  pixels per byte with the leftmost pixel in the low bits, 40 bytes per row
  (`GB_PACKED_BYTES` in total). Shade 0 is lightest and 3 darkest.
- `gb_emulator_audio_samples(dst, capacity)` copies the mono signed 16-bit PCM
  of the last frame: 535 or 536 samples, exactly 32 kHz over time. Provide room
  for 536. It returns 0 when there is nothing new.
- `gb_emulator_validate_rom`, `gb_emulator_rom_save_size` and
  `gb_emulator_rom_save_accepted` inspect a ROM without starting it. A clock
  cartridge writes RAM plus `GB_RTC_FOOTER_BYTES`, but also loads RAM-only and
  44-byte-footer saves from other emulators.
- `gb_emulator_set_clock(fn)` supplies seconds from a clock that keeps running
  while the device is off. The cartridge clock follows it, catching up after
  pauses, power-off and checkpoint restores. Without one, or while `fn` returns
  false, the cartridge clock advances with emulated time.
- `gb_emulator_save_size/dirty/copy/load/clean` expose battery RAM. `save_dirty`
  turns true when the game writes cartridge RAM, or when the cartridge clock
  advances on emulated time (there is no wall-clock anchor to recover it from);
  call `save_clean` after copying it out.
- `gb_emulator_state_size/copy/load` capture and restore a full snapshot.

### Video conversion (`gb_video.h`)

Converts the emulator frame for a 1-bit panel. The target buffer is
`GB_PANEL_BYTES` (5,000 bytes): 200x200 pixels, 25 bytes per row, most
significant bit first, a set bit is white. The 160x144 image is dithered with a
4x4 Bayer matrix and centered at pixel (20, 28). Zero-initialize the
`gb_video_t`; it remembers the previous frame so only changed rows are written,
and the return value is the number of rows that changed. The pixels outside the
image are never touched.

- `gb_video_convert` is the reference implementation.
- `gb_video_convert_fast` produces identical output from a lookup table built on
  first use. Call it from one display task only.
- `gb_video_prepare_gray` tracks changed rows the same way but fills the image
  area with black instead of dithering, for panels that draw grays in a separate
  pass.

### Storage

- `game_catalog.h`: `game_catalog_scan` lists `.gb` and `.gbc` files in one
  directory, sorted case-insensitively and ignoring hidden entries and
  subdirectories; `truncated` is set when more than `capacity` files exist.
  `game_catalog_paths` derives a ROM's save path.
- `rom_file.h`: `rom_file_read` reads a file that must be exactly the expected
  size and never writes. `rom_file_read_upto` accepts any non-empty file up to
  the capacity and reports its length.
- `save_file.h`: `save_file_replace` writes a `.tmp` file, flushes it, rotates
  the old file to `.bak` and renames the new one into place. A failure leaves
  the previous file intact.
- `state_file.h`: `state_file_write/read` store a checkpoint with a checksum and
  the ROM identity. A read falls back to the `.bak` file unless the primary
  belongs to a different ROM or core layout. `state_file_crc32` computes the ROM
  identity.

### USB gamepad (`usb_gamepad.h`)

`#include "gameboy/usb_gamepad.h"` works from C and C++, with or without
Arduino. The controller must be an ESP32-C3 on its native USB Serial/JTAG port
(VID:PID `303A:1001`) sending `GP1` frames, whose format is in
`include/GamepadSerialProtocol.h`. Other USB devices are ignored.

`usb_gamepad_start(&config)` copies the config, runs the host on its own task
(core 0, priority 2, 8 KiB stack) and returns once USB host startup has
succeeded or failed. A controller may be plugged in later. It returns:

- `ESP_OK`, also when the host is already running (the new config is ignored)
- `ESP_ERR_INVALID_ARG` for a null config or `report`
- `ESP_ERR_NO_MEM` if the task could not be created
- `ESP_ERR_INVALID_STATE` while a stop is in progress
- `ESP_FAIL` if the USB host stack failed to install
- `ESP_ERR_TIMEOUT` if startup did not finish within 1.5 s

On any error the host is stopped again before returning.

The callbacks in `usb_gamepad_config_t` all run on the host task and receive
`ctx`:

- `report(mask, ctx)` is required, runs for every valid report and must not
  block. Bit 0 of `mask` is controller button 1, the C3's GPIO0.
- `disconnected(ctx)` is optional. It means "release every button". It can run
  several times, and once more as the task exits.
- `release_console(ctx)` and `restore_console(ctx)` are an optional pair, called
  around the interval that owns the native USB port. With both NULL, an Arduino
  build calls `Serial.end()` and `Serial.begin(115200)`, and an ESP-IDF build
  does nothing.

`usb_gamepad_stop()` blocks until the host has released USB, routed the PHY back
to USB Serial/JTAG and called `restore_console`. It returns `ESP_ERR_TIMEOUT`
after 3 s if the host is still draining; call it again to keep waiting. It
returns `ESP_OK` at once when the host is stopped. Do not call start or stop from
a callback, and do not call them concurrently. `usb_gamepad_running()` reports
whether the host task is up.

The ESP32-S3 has one USB PHY, shared by USB Serial/JTAG and the USB OTG host.
While the host runs, the USB log console and USB flashing are unavailable. If
your app logs over USB Serial/JTAG, pass `release_console` and `restore_console`
to stop and restart that console around the host. Stopping the host routes the
PHY back to USB Serial/JTAG. A firmware that starts the host at boot cannot be
reflashed over USB until the host stops: switch the app to another input, or
hold BOOT while plugging in to enter download mode.

## Using it from Arduino/PlatformIO

`library.json` builds the whole `src/` tree with `-O3` for the `espressif32`
platform. Include `<gameboy/GameBoy.h>`; its classes exist only when
`ARDUINO_ARCH_ESP32` is defined. The Bluetooth controller needs the
NimBLE-Arduino library (tested with 2.5.1) in your `lib_deps`.

### `gameboy::Session`

A `Session` owns one emulator task, the cartridge image, and the battery-save
and checkpoint buffers. Your firmware owns SD mounting, the display, the audio
hardware and Bluetooth.

1. Mount the card at `/sdcard`. ROMs live in `/sdcard/roms`; override with
   `-DGAMEBOY_ROM_DIRECTORY="..."`.
2. `begin(callbacks)` stores the callbacks and allocates nothing. The button,
   frame and audio callbacks are all required.
3. `list(names, capacity, &count, &truncated)` enumerates ROMs, at most 64.
4. `hasCheckpoint(path)` validates the ROM identity and checkpoint checksum; use
   it before offering a Continue choice. Call it from the UI task, never while a
   game is running.
5. `start(path, resumeState)` begins asynchronous loading and starts play
   automatically after preflight. Poll `status()` for the `Phase` and errors.
6. `requestPause()`, `requestResume()`, `requestSave()`, `requestRetrySave()` and
   `requestStop()` only set flags for the emulator task. Pause, save and stop
   write battery RAM and a full checkpoint.
7. `join(timeoutMs)` waits for the task to finish. After a stop or an error,
   call it before destroying or reusing the `Session`.

Callbacks run on the emulator task and must copy data before returning:

- The frame callback gets a borrowed 5,760-byte 160x144 image in the
  `gb_emulator_pixels` format. It fires for every emulated frame, about 60 times
  a second; a display task can take the latest frame at its own refresh rate.
- The audio callback gets 535 or 536 signed mono samples at 32 kHz per frame.
- The button callback returns the `gameboy::Button` bits: A, B, Select, Start,
  Right, Left, Up, Down.

Dirty battery RAM is also written about every 30 seconds
(`-DGAMEBOY_AUTOSAVE_US=...`) by a low-priority worker. The emulator copies at
most 128 KiB of cartridge RAM into a PSRAM snapshot, and the worker owns it
until its `.tmp` file is flushed and the `.bak` rotation finishes. Pause and
stop drain that worker before checkpointing or freeing memory. If a write fails,
the phase becomes `SaveFailed` and the ROM, emulator and failed snapshot stay
alive. Keep the game open and call `requestRetrySave()` when the user retries.
A successful stop releases all dynamic cartridge resources and reaches `Idle`.

### `gameboy::Controller`

`Controller` reads a gamepad on its own worker and exposes the result for
polling from the game task.

- `Controller::start()` uses Bluetooth; `start(Transport::UsbSerial)` uses the
  USB host described above. `supportsTransport` says which transports the
  current build offers.
- `buttons()` returns Game Boy bit order and `rawButtons()` the device bits.
  `connected()` requires both a live link and a recent report. `reportCount()`
  and `lastReportAgeMs()` expose report timing.
- Input is released after 2 seconds without a report, which the controller's
  250 ms heartbeat prevents while a button is held. A disconnect releases all
  buttons.
- `stop(timeoutMs)` waits at most 3 seconds. If it returns false, keep any
  setup radio off and retry until `active()` is false.
- `setMapping(mapping, persist)` takes eight distinct single-bit HID masks in
  Game Boy order and stores them in the `gamepad`/`buttons` NVS blob. With no
  valid stored mapping, the default is `40 20 10 80 08 02 04 01` (hex), the
  InkBoy controller wiring.
  Controller start-up never erases NVS.

The Bluetooth and USB details are in
[`src/controller/README.md`](src/controller/README.md).

## Host checks

`./check-host.sh` compiles the real C sources and the `Session` against host
shims and runs the whole suite: emulation and audio, state and battery file
recovery, catalog path rules, pause boundaries, asynchronous saves, save-failure
retries, and the USB frame, descriptor and console lifecycle logic. It needs
`python3`, a C compiler (`cc`) and a C++17 compiler. Board builds and hardware validation belong to
your own firmware.

## Credits and licenses

This repository is MIT-licensed; see [`LICENSE`](LICENSE). The core under
`src/gb_core/vendor` is derived from CrankBoy (MIT) and Peanut-GB (MIT), whose
MiniGB APU it also uses, and was obtained through Wenting Zhang's PaperBoy port.
File hashes, local modifications and the upstream chain are recorded in
[`vendor-provenance/core.json`](vendor-provenance/core.json). Attribution and
license texts are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

`tests/fixtures/eink_demo.gb` and its generator, `tests/tools/build_demo_rom.py`,
are original CC0-1.0 test material.
