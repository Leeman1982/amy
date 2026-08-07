#include "sd_storage.h"
#include <AMY-Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <stdio.h>
#include <string.h>

// ─── AMY file hooks ──────────────────────────────────────────────────────────
// AMY core never calls fopen()/fread() etc. itself (src/pcm.c, src/transfer.c
// only ever go through these function pointers), precisely so a platform like
// this one can point them at whatever filesystem it has. Once SD.begin() has
// mounted the card at SD_MOUNT_POINT, plain stdio against that path prefix
// works directly — the ESP32 Arduino core's SD library and the SD_MOUNT_POINT
// path both sit on the same underlying VFS FATFS mount, so no bridging code
// is needed beyond these four thin wrappers (mirroring transfer.c's own
// posix_external_*_hook reference implementation).
static uint32_t sdFopenHook(char* filename, const char* mode) {
    FILE* f = fopen(filename, mode);
    return (uint32_t)(uintptr_t)f;
}
static uint32_t sdFreadHook(uint32_t handle, uint8_t* bytes, uint32_t len) {
    FILE* f = (FILE*)(uintptr_t)handle;
    if (!f) return 0;
    return (uint32_t)fread(bytes, 1, len, f);
}
static uint32_t sdFwriteHook(uint32_t handle, uint8_t* bytes, uint32_t len) {
    FILE* f = (FILE*)(uintptr_t)handle;
    if (!f) return 0;
    return (uint32_t)fwrite(bytes, 1, len, f);
}
static void sdFseekHook(uint32_t handle, uint32_t pos) {
    FILE* f = (FILE*)(uintptr_t)handle;
    if (f) fseek(f, (long)pos, SEEK_SET);
}
static void sdFcloseHook(uint32_t handle) {
    FILE* f = (FILE*)(uintptr_t)handle;
    if (f) fclose(f);
}

// Called from the .ino BEFORE amy_start(), so the hook pointers are already
// in place on the config struct amy_start() copies into amy_global.
void sdInstallAmyFileHooks(amy_config_t& cfg) {
    cfg.amy_external_fopen_hook  = sdFopenHook;
    cfg.amy_external_fread_hook  = sdFreadHook;
    cfg.amy_external_fwrite_hook = sdFwriteHook;
    cfg.amy_external_fseek_hook  = sdFseekHook;
    cfg.amy_external_fclose_hook = sdFcloseHook;
}

// ─── Mount ──────────────────────────────────────────────────────────────────
bool SdStorage::begin() {
    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS, SPI, SD_SPI_HZ, SD_MOUNT_POINT)) {
        _mounted = false;
        return false;
    }
    _mounted = true;
    if (!SD.exists(PATCH_DIR))            SD.mkdir(PATCH_DIR);
    if (!SD.exists("/patterns"))          SD.mkdir("/patterns");
    if (!SD.exists(KIT_DIR))              SD.mkdir(KIT_DIR);
    return true;
}

void SdStorage::patternPath(uint8_t track, uint8_t idx, char* buf, size_t n) {
    snprintf(buf, n, "/patterns/t%up%u.bin", (unsigned)(track % NUM_TRACKS),
             (unsigned)(idx % NUM_PATTERNS));
}
void SdStorage::kitPath(uint8_t slot, char* buf, size_t n) {
    snprintf(buf, n, "%s/k%02u.bin", KIT_DIR, (unsigned)(slot % KIT_SLOTS));
}

// ─── Patterns ───────────────────────────────────────────────────────────────
// Every save removes the old file first: some Arduino-ESP32 core versions
// open FILE_WRITE at end-of-file (append) rather than truncating, which would
// otherwise corrupt these fixed-size structs on a second save.
bool SdStorage::savePattern(uint8_t track, uint8_t idx, const Pattern& p) {
    if (!_mounted) return false;
    char path[40]; patternPath(track, idx, path, sizeof(path));
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    bool ok = (f.write((const uint8_t*)&p, sizeof(Pattern)) == sizeof(Pattern));
    f.close();
    return ok;
}

bool SdStorage::loadPattern(uint8_t track, uint8_t idx, Pattern& p) {
    if (!_mounted) return false;
    char path[40]; patternPath(track, idx, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    bool ok = (f.read((uint8_t*)&p, sizeof(Pattern)) == (int)sizeof(Pattern));
    f.close();
    if (ok && (p.length == 0 || p.length > NUM_STEPS)) p.length = DEFAULT_STEPS;
    return ok;
}

bool SdStorage::patternExists(uint8_t track, uint8_t idx) {
    if (!_mounted) return false;
    char path[40]; patternPath(track, idx, path, sizeof(path));
    return SD.exists(path);
}

// ─── Settings ───────────────────────────────────────────────────────────────
bool SdStorage::saveSettings(const GlobalSettings& s) {
    if (!_mounted) return false;
    SD.remove("/settings.bin");
    File f = SD.open("/settings.bin", FILE_WRITE);
    if (!f) return false;
    bool ok = (f.write((const uint8_t*)&s, sizeof(s)) == sizeof(s));
    f.close();
    return ok;
}

bool SdStorage::loadSettings(GlobalSettings& s) {
    if (!_mounted) return false;
    File f = SD.open("/settings.bin", FILE_READ);
    if (!f) return false;
    GlobalSettings tmp;
    bool ok = (f.read((uint8_t*)&tmp, sizeof(tmp)) == (int)sizeof(tmp));
    f.close();
    // Reject anything written by a different build rather than restoring a
    // struct whose fields have since moved.
    if (!ok || tmp.magic != STORAGE_MAGIC || tmp.version != STORAGE_VERSION) return false;
    s = tmp;
    if (s.bpm < BPM_MIN || s.bpm > BPM_MAX) s.bpm = BPM_DEFAULT;
    return true;
}

// ─── Kits ───────────────────────────────────────────────────────────────────
bool SdStorage::saveKit(uint8_t slot, const Kit& k) {
    if (!_mounted) return false;
    char path[24]; kitPath(slot, path, sizeof(path));
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    bool ok = (f.write((const uint8_t*)&k, sizeof(k)) == sizeof(k));
    f.close();
    return ok;
}

bool SdStorage::loadKit(uint8_t slot, Kit& k) {
    if (!_mounted) return false;
    char path[24]; kitPath(slot, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    bool ok = (f.read((uint8_t*)&k, sizeof(k)) == (int)sizeof(k));
    f.close();
    if (ok) k.name[sizeof(k.name) - 1] = 0;
    return ok;
}

bool SdStorage::kitExists(uint8_t slot) {
    if (!_mounted) return false;
    char path[24]; kitPath(slot, path, sizeof(path));
    return SD.exists(path);
}
