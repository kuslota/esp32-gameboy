#!/usr/bin/env python3
"""Run the Arduino Session against host FreeRTOS shims and a CC0 cartridge."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="gb-session-") as tmp:
    work = Path(tmp)
    fake = work / "fake"
    (fake / "freertos").mkdir(parents=True)
    romdir = work / "roms"
    romdir.mkdir()
    rom = romdir / "eink_demo.gb"
    demo = (root / "tests/fixtures/eink_demo.gb").read_bytes()
    rom.write_bytes(demo)
    battery = romdir / "battery.gb"
    battery_data = bytearray(demo)
    battery_data[0x147] = 3  # MBC1 with RAM and battery.
    battery_data[0x149] = 2  # 8 KiB SRAM.
    battery_data[0x150:0x150 + 12] = bytes([
        0x3e, 0x0a, 0xea, 0x00, 0x00, 0x3e, 0x5a, 0xea, 0x00, 0xa0, 0x18, 0xfe,
    ])
    battery_data[0x14d] = (-sum(battery_data[0x134:0x14d]) - 25) & 255
    battery.write_bytes(battery_data)
    (fake / "freertos/FreeRTOS.h").write_text(r'''
#pragma once
#include <stdint.h>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
struct portMUX_TYPE { std::mutex lock; };
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock.lock()
#define portEXIT_CRITICAL(m) (m)->lock.unlock()
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
''')
    (fake / "freertos/semphr.h").write_text(r'''
#pragma once
#include "FreeRTOS.h"
struct FakeSemaphore { std::mutex mutex; std::condition_variable cv; unsigned count = 0; };
using SemaphoreHandle_t = FakeSemaphore*;
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return new FakeSemaphore; }
inline int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ms) {
  std::unique_lock<std::mutex> lock(sem->mutex);
  if (!sem->cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return sem->count; })) return 0;
  --sem->count; return pdTRUE;
}
inline int xSemaphoreGive(SemaphoreHandle_t sem) {
  { std::lock_guard<std::mutex> lock(sem->mutex); ++sem->count; }
  sem->cv.notify_one(); return pdTRUE;
}
inline void vSemaphoreDelete(SemaphoreHandle_t sem) { delete sem; }
''')
    (fake / "freertos/task.h").write_text(r'''
#pragma once
#include "FreeRTOS.h"
using TaskHandle_t = void*;
inline int xTaskCreatePinnedToCore(void (*fn)(void*), const char*, unsigned, void* arg,
                                   unsigned, TaskHandle_t* handle, int) {
  *handle = reinterpret_cast<void*>(1);
  std::thread([=] { fn(arg); }).detach();
  return pdPASS;
}
inline void vTaskDelay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline void vTaskDelete(void*) {}
''')
    (fake / "esp_heap_caps.h").write_text(r'''
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void* heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void* heap_caps_calloc(size_t count, size_t n, int caps) { (void)caps; return calloc(count, n); }
static inline void heap_caps_free(void* p) { free(p); }
''')
    (fake / "esp_attr.h").write_text('#pragma once\n#define IRAM_ATTR\n')
    (fake / "esp_timer.h").write_text(r'''
#pragma once
#include <stdint.h>
#include <chrono>
inline int64_t esp_timer_get_time() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
''')
    (work / "session_test.cpp").write_text(r'''
#include <gameboy/GameBoy.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
using namespace std::chrono_literals;
using gameboy::Phase;
gameboy::Session* activeSession = nullptr;
struct Signals { std::atomic<int> frames{0}, audio{0}; };
uint8_t buttons(void*) { return 0; }
void frame(void* context, const uint8_t*, size_t size) {
  if (size != 5760) std::abort();
  static_cast<Signals*>(context)->frames++;
}
void audio(void* context, const int16_t*, size_t count) {
  if (count != 523 && count != 524) std::abort();
  static_cast<Signals*>(context)->audio++;
}
template <typename F> void until(F predicate) {
  for (int i = 0; i < 500; ++i) {
    if (predicate()) return;
    std::this_thread::sleep_for(10ms);
  }
  const auto state = activeSession->status();
  std::fprintf(stderr, "timed out: phase=%u error=%d message=%s frames=%u\n",
      unsigned(state.phase), state.error, state.errorMessage, state.frames); std::abort();
}
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  Signals signals;
  gameboy::Session session;
  activeSession = &session;
  if (session.hasCheckpoint(argv[1])) return 11;
  if (!session.begin({&signals, buttons, frame, audio}) || !session.start(argv[1])) return 3;
  until([&] { return session.status().phase == Phase::Playing; });
  until([&] { return signals.frames >= 4 && signals.audio >= 8; });
  session.requestPause();
  until([&] { return session.status().phase == Phase::Paused; });
  int before = signals.frames.load();
  // The producer hands off each completed emulator frame; the display
  // consumer is responsible for coalescing to its own refresh cadence.
  if (before != int(session.status().frames)) return 22;
  std::this_thread::sleep_for(100ms);
  if (signals.frames != before || !session.status().stateAvailable) return 4;
  session.requestResume();
  until([&] { return session.status().phase == Phase::Playing && signals.frames > before; });
  session.requestStop();
  until([&] { return session.status().phase == Phase::Idle; });
  if (!session.join(1000)) return 5;
  if (!session.hasCheckpoint(argv[1])) return 12;
  if (!session.start(argv[1], true)) return 6;
  until([&] { return session.status().phase == Phase::Playing; });
  until([&] { return signals.frames > before + 3; });
  std::filesystem::create_directory(std::string(argv[1]) + ".state.tmp");
  session.requestPause();
  until([&] { return session.status().phase == Phase::SaveFailed; });
  before = signals.frames.load();
  std::this_thread::sleep_for(100ms);
  if (signals.frames != before) return 7;
  std::filesystem::remove(std::string(argv[1]) + ".state.tmp");
  session.requestRetrySave();
  until([&] { return session.status().phase == Phase::Paused; });
  session.requestStop();
  until([&] { return session.status().phase == Phase::Idle; });
  if (!session.join(1000)) return 8;
  if (!session.start(argv[1])) return 9;
  until([&] { return session.status().phase == Phase::Playing; });
  until([&] { return signals.frames > before + 7; });
  std::filesystem::create_directory(std::string(argv[1]) + ".state.tmp");
  session.requestStop();
  until([&] { return session.status().phase == Phase::SaveFailed; });
  std::this_thread::sleep_for(50ms);
  std::filesystem::remove(std::string(argv[1]) + ".state.tmp");
  session.requestRetrySave();
  until([&] {
    const auto phase = session.status().phase;
    if (phase == Phase::Paused) std::abort();
    return phase == Phase::Idle;
  });
  if (!session.join(1000)) return 10;
  // The battery writer sleeps for 250 ms before its SD write in this host
  // build. The emulation callbacks must continue during that delay, and a
  // stop must wait for the writer before freeing its snapshot buffer.
  if (!session.start(argv[2])) return 13;
  until([&] { return session.status().phase == Phase::Playing; });
  until([&] { return session.status().frames >= 12; });
  const auto save = std::string(argv[2]).substr(0, std::string(argv[2]).size() - 2) + "sav";
  if (std::filesystem::exists(save)) return 14;
  before = signals.frames.load();
  std::this_thread::sleep_for(70ms);
  if (signals.frames <= before) return 15;
  session.requestStop();
  if (session.join(0)) return 16;
  until([&] { return session.status().phase == Phase::Idle; });
  if (!session.join(1000) || !std::filesystem::exists(save)) return 17;
  { auto* file = std::fopen(argv[2], "r+b");
    if (!file || std::fseek(file, 0x156, SEEK_SET) || std::fputc(0x5b, file) == EOF ||
        std::fclose(file)) return 21;
  }
  std::filesystem::create_directory(save + ".tmp");
  if (!session.start(argv[2])) return 18;
  until([&] { return session.status().phase == Phase::SaveFailed; });
  before = signals.frames.load();
  std::this_thread::sleep_for(70ms);
  if (signals.frames != before) return 19;
  std::filesystem::remove(save + ".tmp");
  session.requestRetrySave();
  until([&] { return session.status().phase == Phase::Paused; });
  session.requestStop();
  until([&] { return session.status().phase == Phase::Idle; });
  if (!session.join(1000)) return 20;
  std::puts("PASS: autostart, pause boundary, async battery autosave, drain, failure retention, retry, stop");
}
''')
    objects = []
    for source in ["gb_core/gb_emulator.c", "gb_core/gb_video.c",
                   "gb_core/vendor/minigb_apu/minigb_apu.c",
                   "storage/game_catalog.c", "storage/rom_file.c",
                   "storage/save_file.c", "storage/state_file.c"]:
        path = root / "src" / source
        obj = work / (path.name + ".o")
        subprocess.run(["cc", "-std=gnu11", "-O3", "-Wno-unknown-attributes",
                        "-DESP_PLATFORM", "-I" + str(fake),
                        "-DMINIGB_APU_AUDIO_FORMAT_S16SYS",
                        "-I" + str(root / "src/gb_core"), "-I" + str(root / "src/storage"),
                        "-c", str(path), "-o", str(obj)], check=True)
        objects.append(str(obj))
    executable = work / "session_test"
    subprocess.run(["c++", "-std=gnu++17", "-pthread", "-DARDUINO_ARCH_ESP32",
                    "-DGAMEBOY_AUTOSAVE_US=100000", "-DGAMEBOY_TEST_SAVE_DELAY_MS=250",
                    '-DGAMEBOY_ROM_DIRECTORY="' + str(romdir) + '"',
                    "-I" + str(fake), "-I" + str(root / "include"), "-I" + str(root / "src"),
                    str(root / "src/GameBoy.cpp"), str(work / "session_test.cpp"), *objects,
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable), str(rom), str(battery)], check=True)
