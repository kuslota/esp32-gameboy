#include <gameboy/GameBoy.h>

#ifdef ARDUINO_ARCH_ESP32
extern "C" {
#include "gb_core/gb_emulator.h"
#include "gb_core/gb_video.h"
#include "storage/game_catalog.h"
#include "storage/rom_file.h"
#include "storage/save_file.h"
#include "storage/state_file.h"
}

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

namespace gameboy {
namespace {
#ifndef GAMEBOY_ROM_DIRECTORY
#define GAMEBOY_ROM_DIRECTORY "/sdcard/roms"
#endif
constexpr char kRomDirectory[] = GAMEBOY_ROM_DIRECTORY;
#ifndef GAMEBOY_AUTOSAVE_US
#define GAMEBOY_AUTOSAVE_US 30000000u
#endif
constexpr uint32_t kAutosaveUs = GAMEBOY_AUTOSAVE_US;

uint8_t* psram(size_t bytes) {
  return static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

bool regularFile(const char* path, size_t& size) {
  struct stat info{};
  if (stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size < 0) return false;
  size = static_cast<size_t>(info.st_size);
  return true;
}

uint32_t crcStream(const char* path, size_t exactSize) {
  FILE* file = fopen(path, "rb");
  if (!file) return 0;
  uint32_t crc = 0xffffffffu;
  size_t total = 0;
  uint8_t chunk[1024];
  while (total < exactSize) {
    size_t wanted = exactSize - total < sizeof(chunk) ? exactSize - total : sizeof(chunk);
    size_t got = fread(chunk, 1, wanted, file);
    if (got != wanted) { fclose(file); return 0; }
    for (size_t i = 0; i < got; ++i) {
      crc ^= chunk[i];
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    total += got;
  }
  bool exact = fgetc(file) == EOF && !ferror(file);
  bool closed = fclose(file) == 0;
  return exact && closed ? crc ^ 0xffffffffu : 0;
}

size_t statePayloadSize(const char* path) {
  FILE* file = fopen(path, "rb");
  if (!file) return 0;
  uint8_t header[24];
  bool read = fread(header, 1, sizeof(header), file) == sizeof(header);
  fclose(file);
  if (!read || memcmp(header, "GBST", 4) || header[4] != 1 || header[5] != 0 ||
      header[6] != 24 || header[7] != 0) return 0;
  size_t size = static_cast<size_t>(header[8]) |
      (static_cast<size_t>(header[9]) << 8) |
      (static_cast<size_t>(header[10]) << 16) |
      (static_cast<size_t>(header[11]) << 24);
  return size && size <= STATE_FILE_MAX_PAYLOAD ? size : 0;
}

bool pathFor(const char* requested, char rom[288], char save[288], char state[300]) {
  if (!requested) return false;
  constexpr size_t prefix = sizeof(kRomDirectory) - 1;
  if (strncmp(requested, kRomDirectory, prefix) || requested[prefix] != '/') return false;
  game_entry_t entry{};
  const char* name = requested + prefix + 1;
  if (!*name || strchr(name, '/') || strlen(name) >= sizeof(entry.name)) return false;
  memcpy(entry.name, name, strlen(name) + 1);
  if (!game_catalog_paths(kRomDirectory, &entry, rom, save)) return false;
  if (strcmp(rom, requested)) return false;
  int n = snprintf(state, 300, "%s.state", rom);
  return n > 0 && n < 300;
}

}

bool Session::begin(Callbacks callbacks) {
  if (task_ || !callbacks.buttons || !callbacks.frame || !callbacks.audio) return false;
  callbacks_ = callbacks;
  return true;
}

bool Session::list(char names[][256], size_t capacity, size_t* count, bool* truncated) {
  if (!names || !capacity || !count || !truncated || capacity > GAME_CATALOG_MAX) return false;
  auto* entries = reinterpret_cast<game_entry_t*>(psram(sizeof(game_entry_t) * capacity));
  if (!entries) return false;
  bool ok = game_catalog_scan(kRomDirectory, entries, capacity, count, truncated);
  if (!ok) { heap_caps_free(entries); return false; }
  for (size_t i = 0; i < *count; ++i) memcpy(names[i], entries[i].name, sizeof(entries[i].name));
  heap_caps_free(entries);
  return true;
}

bool Session::hasCheckpoint(const char* path) {
  if (task_) return false;
  char rom[288], save[288], state[300];
  if (!pathFor(path, rom, save, state)) return false;
  size_t romSize = 0;
  if (!regularFile(rom, romSize) || romSize < 32768 || romSize > GB_MAX_ROM_BYTES) return false;
  size_t payloadSize = statePayloadSize(state);
  if (!payloadSize) {
    char backup[304];
    int n = snprintf(backup, sizeof(backup), "%s.bak", state);
    if (n <= 0 || n >= static_cast<int>(sizeof(backup))) return false;
    payloadSize = statePayloadSize(backup);
  }
  if (!payloadSize) return false;
  const uint32_t romCrc = crcStream(rom, romSize);
  auto* payload = psram(payloadSize);
  if (!payload) return false;
  const bool valid = state_file_read(state, romCrc, static_cast<uint32_t>(romSize),
                                     payload, payloadSize) == STATE_FILE_OK;
  heap_caps_free(payload);
  return valid;
}

bool Session::start(const char* path, bool resumeState) {
  if (task_ && !join(0)) return false;
  if (!callbacks_.buttons || !callbacks_.frame || !callbacks_.audio ||
      !pathFor(path, romPath_, savePath_, statePath_)) return false;
  resumeState_ = resumeState;
  pause_.store(false); resume_.store(false); save_.store(false);
  retry_.store(false); stop_.store(false);
  portENTER_CRITICAL(&statusMux_);
  status_ = {};
  status_.phase = Phase::Loading;
  strncpy(status_.game, strrchr(romPath_, '/') + 1, sizeof(status_.game) - 1);
  portEXIT_CRITICAL(&statusMux_);
  done_ = xSemaphoreCreateBinary();
  if (!done_) { update(Phase::Error, -1, "Not enough memory for player task"); return false; }
  if (xTaskCreatePinnedToCore(taskEntry, "gb_player", 8192, this, 4, &task_, 1) != pdPASS) {
    vSemaphoreDelete(done_); done_ = nullptr;
    update(Phase::Error, -1, "Could not start player task");
    return false;
  }
  return true;
}

void Session::requestPause() { pause_ = true; }
void Session::requestResume() { resume_ = true; }
void Session::requestSave() { save_ = true; }
void Session::requestRetrySave() { retry_ = true; }
void Session::requestStop() { stop_ = true; retry_ = true; }

Status Session::status() const {
  portENTER_CRITICAL(&statusMux_);
  Status copy = status_;
  portEXIT_CRITICAL(&statusMux_);
  return copy;
}

void Session::update(Phase phase, int error, const char* message) {
  portENTER_CRITICAL(&statusMux_);
  status_.phase = phase;
  status_.error = error;
  if (message) {
    strncpy(status_.errorMessage, message, sizeof(status_.errorMessage) - 1);
    status_.errorMessage[sizeof(status_.errorMessage) - 1] = 0;
  } else status_.errorMessage[0] = 0;
  portEXIT_CRITICAL(&statusMux_);
}

void Session::setDirty(bool dirty) {
  portENTER_CRITICAL(&statusMux_);
  status_.saveDirty = dirty;
  portEXIT_CRITICAL(&statusMux_);
}

bool Session::startSaveWorker() {
  if (!saveSize_) return true;
  saveWake_ = xSemaphoreCreateBinary();
  saveDone_ = xSemaphoreCreateBinary();
  saveExit_ = xSemaphoreCreateBinary();
  if (!saveWake_ || !saveDone_ || !saveExit_) return false;
  saveWorkerStop_.store(false);
  saveWorkerBusy_.store(false);
  saveWorkerPending_.store(false);
  saveWorkerFailed_.store(false);
  return xTaskCreatePinnedToCore(saveEntry, "gb_save", 4096, this, 1,
                                  &saveTask_, 0) == pdPASS;
}

void Session::saveEntry(void* context) {
  auto* self = static_cast<Session*>(context);
  for (;;) {
    xSemaphoreTake(self->saveWake_, portMAX_DELAY);
    if (self->saveWorkerStop_.load()) break;
#ifdef GAMEBOY_TEST_SAVE_DELAY_MS
    vTaskDelay(pdMS_TO_TICKS(GAMEBOY_TEST_SAVE_DELAY_MS));
#endif
    const bool written = save_file_replace(self->savePath_, self->saveBuffer_, self->saveSize_);
    self->saveWorkerFailed_.store(!written);
    self->saveWorkerPending_.store(!written);
    self->saveWorkerBusy_.store(false);
    xSemaphoreGive(self->saveDone_);
  }
  xSemaphoreGive(self->saveExit_);
  vTaskDelete(nullptr);
}

bool Session::submitBatteryBuffer() {
  if (!saveTask_ || !saveWorkerPending_.load() || saveWorkerBusy_.load()) return false;
  saveWorkerFailed_.store(false);
  saveWorkerBusy_.store(true);
  if (xSemaphoreGive(saveWake_) == pdTRUE) return true;
  saveWorkerBusy_.store(false);
  saveWorkerFailed_.store(true);
  return false;
}

bool Session::queueBatterySnapshot() {
  if (!saveSize_ || !saveBuffer_ || saveWorkerBusy_.load() || saveWorkerPending_.load() ||
      !gb_emulator_save_dirty()) return false;
  if (!gb_emulator_save_copy(saveBuffer_, saveSize_)) return false;
  // Later cartridge writes mark SRAM dirty again while the worker owns this copy.
  gb_emulator_save_clean();
  saveWorkerPending_.store(true);
  return submitBatteryBuffer();
}

void Session::waitBatteryWrite() {
  while (saveWorkerBusy_.load()) xSemaphoreTake(saveDone_, portMAX_DELAY);
}

bool Session::persist(bool checkpoint) {
  update(Phase::Saving);
  if (saveSize_) {
    waitBatteryWrite();
    // A failed worker retains its immutable snapshot for retry. Flush it
    // before taking a newer snapshot from cartridge RAM.
    if (saveWorkerPending_.load()) {
      if (!submitBatteryBuffer()) {
        update(Phase::SaveFailed, -5, "Could not queue battery save"); return false;
      }
      waitBatteryWrite();
      if (saveWorkerFailed_.load()) {
        update(Phase::SaveFailed, -5, "Could not write battery save to SD card"); return false;
      }
    }
    if (gb_emulator_save_dirty()) {
      if (!queueBatterySnapshot()) {
        update(Phase::SaveFailed, -4, "Could not copy game progress"); return false;
      }
      waitBatteryWrite();
      if (saveWorkerFailed_.load()) {
        update(Phase::SaveFailed, -5, "Could not write battery save to SD card"); return false;
      }
    }
  }
  if (checkpoint && stateDirty_) {
    if (!stateBuffer_ || !gb_emulator_state_copy(stateBuffer_, stateSize_) ||
        state_file_write(statePath_, romCrc_, static_cast<uint32_t>(romSize_),
                         stateBuffer_, stateSize_) != STATE_FILE_OK) {
      update(Phase::SaveFailed, -6, "Could not write checkpoint to SD card"); return false;
    }
    stateDirty_ = false;
    portENTER_CRITICAL(&statusMux_);
    status_.stateAvailable = true;
    portEXIT_CRITICAL(&statusMux_);
  }
  setDirty(gb_emulator_save_dirty() || stateDirty_);
  return true;
}

void Session::cleanup() {
  if (saveTask_) {
    saveWorkerStop_.store(true);
    xSemaphoreGive(saveWake_);
    xSemaphoreTake(saveExit_, portMAX_DELAY);
    saveTask_ = nullptr;
  }
  if (saveWake_) { vSemaphoreDelete(saveWake_); saveWake_ = nullptr; }
  if (saveDone_) { vSemaphoreDelete(saveDone_); saveDone_ = nullptr; }
  if (saveExit_) { vSemaphoreDelete(saveExit_); saveExit_ = nullptr; }
  saveWorkerBusy_.store(false); saveWorkerPending_.store(false);
  saveWorkerFailed_.store(false); saveWorkerStop_.store(false);
  gb_emulator_shutdown();
  heap_caps_free(rom_); rom_ = nullptr; romSize_ = 0;
  heap_caps_free(saveBuffer_); saveBuffer_ = nullptr; saveSize_ = 0;
  heap_caps_free(stateBuffer_); stateBuffer_ = nullptr; stateSize_ = 0;
  romCrc_ = 0; stateDirty_ = false; batteryLoaded_ = false;
}

void Session::taskEntry(void* context) {
  auto* self = static_cast<Session*>(context);
  self->run();
  xSemaphoreGive(self->done_);
  vTaskDelete(nullptr);
}

void Session::run() {
  size_t size = 0;
  if (!regularFile(romPath_, size) || size < 32768 || size > GB_MAX_ROM_BYTES) {
    update(Phase::Error, -10, "ROM missing or outside 32 KiB to 2 MiB"); goto finished;
  }
  rom_ = psram(size);
  if (!rom_) { update(Phase::Error, -11, "Not enough PSRAM for ROM"); goto finished; }
  romSize_ = size;
  if (rom_file_read(romPath_, rom_, size) != ROM_FILE_OK ||
      !gb_emulator_validate_rom(rom_, size)) {
    update(Phase::Error, -12, "ROM unreadable or unsupported"); goto finished;
  }
  romCrc_ = state_file_crc32(rom_, size);
  saveSize_ = gb_emulator_rom_save_size(rom_, size);
  if (saveSize_) {
    saveBuffer_ = psram(saveSize_);
    if (!saveBuffer_) { update(Phase::Error, -13, "Not enough PSRAM for battery save"); goto finished; }
    const auto primary = rom_file_read(savePath_, saveBuffer_, saveSize_);
    if (primary == ROM_FILE_OK) batteryLoaded_ = true;
    else {
      char backup[300];
      snprintf(backup, sizeof(backup), "%s.bak", savePath_);
      const auto recovered = rom_file_read(backup, saveBuffer_, saveSize_);
      if (recovered == ROM_FILE_OK) batteryLoaded_ = true;
      else if (primary != ROM_FILE_MISSING || recovered != ROM_FILE_MISSING) {
        update(Phase::Error, -14, "Battery save unreadable; original retained"); goto finished;
      }
    }
  }
  if (!gb_emulator_init(rom_, romSize_) || gb_emulator_save_size() != saveSize_) {
    update(Phase::Error, -15, "Cartridge could not start"); goto finished;
  }
  if (batteryLoaded_ && !gb_emulator_save_load(saveBuffer_, saveSize_)) {
    update(Phase::Error, -16, "Battery save could not load"); goto finished;
  }
  batteryLoaded_ = false;
  stateSize_ = gb_emulator_state_size();
  if (!stateSize_ || stateSize_ > STATE_FILE_MAX_PAYLOAD || !(stateBuffer_ = psram(stateSize_))) {
    update(Phase::Error, -17, "Not enough PSRAM for checkpoint"); goto finished;
  }
  {
    const auto state = state_file_read(statePath_, romCrc_, static_cast<uint32_t>(romSize_),
                                       stateBuffer_, stateSize_);
    portENTER_CRITICAL(&statusMux_);
    status_.stateAvailable = state == STATE_FILE_OK;
    portEXIT_CRITICAL(&statusMux_);
    if (resumeState_) {
      if (state != STATE_FILE_OK || !gb_emulator_state_load(stateBuffer_, stateSize_)) {
        update(Phase::Error, -18, "Checkpoint unavailable or incompatible"); goto finished;
      }
    }
  }
  if (!startSaveWorker()) {
    update(Phase::Error, -19, "Could not start battery save worker"); goto finished;
  }
  // A pause before the first emulated frame still needs a resumable checkpoint.
  stateDirty_ = true;
  update(Phase::Playing);
  {
    uint64_t epoch = static_cast<uint64_t>(esp_timer_get_time());
    uint32_t epochFrames = 0;
    uint64_t lastBatterySave = epoch;
    int16_t audio[GB_AUDIO_FRAME_SAMPLES_MAX];
    bool playing = true;
    for (;;) {
      if (saveWorkerFailed_.load() && status().phase != Phase::SaveFailed &&
          status().phase != Phase::Saving) {
        playing = false;
        update(Phase::SaveFailed, -5, "Could not write battery save to SD card");
      }
      if (status().phase == Phase::SaveFailed) {
        if (!retry_.exchange(false)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (!persist(true)) continue;
        // The stop path owns the next transition. Do not publish Paused while
        // the host is preparing to release display and audio resources.
        if (!stop_.load()) update(Phase::Paused);
      }
      if (stop_.load()) {
        if (!persist(true)) continue;
        update(Phase::Stopping);
        break;
      }
      if (pause_.exchange(false)) {
        playing = false;
        if (!persist(true)) continue;
        update(Phase::Paused);
      }
      if (resume_.exchange(false) && status().phase == Phase::Paused) {
        playing = true;
        epoch = static_cast<uint64_t>(esp_timer_get_time()); epochFrames = 0;
        update(Phase::Playing);
      }
      if (save_.exchange(false)) {
        if (!persist(true)) { playing = false; continue; }
        update(playing ? Phase::Playing : Phase::Paused);
      }
      if (!playing || status().phase == Phase::SaveFailed) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      gb_emulator_buttons(callbacks_.buttons(callbacks_.context));
      if (!gb_emulator_frame(false)) {
        update(Phase::Error, gb_emulator_error(), "Game Boy core stopped"); break;
      }
      size_t samples = gb_emulator_audio_samples(audio, GB_AUDIO_FRAME_SAMPLES_MAX);
      if (samples) callbacks_.audio(callbacks_.context, audio, samples);
      portENTER_CRITICAL(&statusMux_);
      ++status_.frames;
      portEXIT_CRITICAL(&statusMux_);
      callbacks_.frame(callbacks_.context, gb_emulator_pixels(), GB_PACKED_BYTES);
      stateDirty_ = true;
      setDirty(true);
      uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
      if (saveSize_ && gb_emulator_save_dirty() && !saveWorkerBusy_.load() &&
          !saveWorkerPending_.load() && now - lastBatterySave >= kAutosaveUs) {
        if (!queueBatterySnapshot()) {
          playing = false;
          update(Phase::SaveFailed, -4, "Could not queue battery autosave");
          continue;
        }
        lastBatterySave = now;
      }
      uint64_t due = epoch + gb_frame_time_us(++epochFrames);
      now = static_cast<uint64_t>(esp_timer_get_time());
      if (due > now) vTaskDelay(pdMS_TO_TICKS((due - now + 999) / 1000));
      else vTaskDelay(1);
    }
  }
finished:
  cleanup();
  if (status().phase != Phase::Error) update(Phase::Idle);
}

bool Session::join(uint32_t timeoutMs) {
  if (!task_) return true;
  if (xSemaphoreTake(done_, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
  vSemaphoreDelete(done_); done_ = nullptr;
  task_ = nullptr;
  return true;
}

}
#endif
