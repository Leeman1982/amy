#include "patch.h"
#include <AMY-Arduino.h>
#include <SD.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static TrackPatch g_trackPatch[NUM_TRACKS];

// One AMY PCM preset number per (track, zone slot) — wide enough apart that
// no two tracks' zones can ever collide in AMY's preset table.
static inline int16_t presetIdFor(uint8_t track, uint8_t zoneIdx) {
    return (int16_t)(100 + (int)track * 100 + (int)zoneIdx);
}

static void buildPatchDirPath(const char* dirName, char* out, size_t n) {
    snprintf(out, n, "%s%s/%s", SD_MOUNT_POINT, PATCH_DIR, dirName);
}

// ─── patch.cfg parsing ───────────────────────────────────────────────────────
static void trimNewline(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

// Reads name=/category=/poly= directives and every "zone ..." line. Silently
// skips malformed or over-limit lines rather than aborting the whole patch —
// a typo in one zone shouldn't cost you the rest of the kit.
static bool parsePatchCfg(const char* cfgPath, PatchInfo& info, PatchZone* zones,
                           uint8_t maxZones, uint8_t& numZones) {
    FILE* f = fopen(cfgPath, "r");
    if (!f) return false;

    char line[160];
    numZones = 0;
    while (fgets(line, sizeof(line), f)) {
        trimNewline(line);
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == 0) continue;

        if (strncmp(s, "name=", 5) == 0) {
            strncpy(info.name, s + 5, sizeof(info.name) - 1);
        } else if (strncmp(s, "category=", 9) == 0) {
            strncpy(info.category, s + 9, sizeof(info.category) - 1);
        } else if (strncmp(s, "poly=", 5) == 0) {
            int p = atoi(s + 5);
            if (p > 0 && p <= 24) info.polyphony = (uint8_t)p;
        } else if (strncmp(s, "zone", 4) == 0 && (s[4] == ' ' || s[4] == '\t')) {
            if (numZones >= maxZones) continue;
            int noteLo, noteHi, velLo, velHi, root, chromatic, mode, loop, gainPct, panPct;
            char file[40];
            int got = sscanf(s + 4, "%d %d %d %d %d %d %d %d %d %d %39s",
                              &noteLo, &noteHi, &velLo, &velHi, &root,
                              &chromatic, &mode, &loop, &gainPct, &panPct, file);
            if (got != 11) continue;
            PatchZone& z = zones[numZones++];
            z.noteLo = (uint8_t)constrain(noteLo, 0, 127);
            z.noteHi = (uint8_t)constrain(noteHi, 0, 127);
            z.velLo  = (uint8_t)constrain(velLo, 0, 127);
            z.velHi  = (uint8_t)constrain(velHi, 0, 127);
            z.rootNote  = (uint8_t)constrain(root, 0, 127);
            z.chromatic = (chromatic != 0);
            z.streaming = (mode != 0);
            z.loop      = (loop != 0);
            z.gain      = constrain((float)gainPct / 100.0f, 0.0f, 2.0f);
            z.pan       = constrain((float)panPct / 100.0f, -1.0f, 1.0f);
            strncpy(z.file, file, sizeof(z.file) - 1);
        }
    }
    fclose(f);
    return true;
}

// ─── Minimal RIFF/WAVE walker, for RAM-mode zones only ─────────────────────
// AMY's own WAV parser (transfer.c) is what handles streamed zones — this is
// only needed here because a RAM zone's samples must be fread() straight into
// the buffer pcm_load() hands back, which happens on the application side of
// AMY's file hooks.
struct WavInfo { uint16_t channels; uint32_t sampleRate; uint32_t dataOffset, dataBytes; };

static bool wavParse(FILE* f, WavInfo& info) {
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12) return false;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    bool haveFmt = false;
    while (true) {
        uint8_t chunkHdr[8];
        if (fread(chunkHdr, 1, 8, f) != 8) break;
        uint32_t chunkSize = (uint32_t)chunkHdr[4] | ((uint32_t)chunkHdr[5] << 8) |
                              ((uint32_t)chunkHdr[6] << 16) | ((uint32_t)chunkHdr[7] << 24);
        if (memcmp(chunkHdr, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (chunkSize < 16 || fread(fmt, 1, 16, f) != 16) return false;
            info.channels   = (uint16_t)fmt[2] | ((uint16_t)fmt[3] << 8);
            info.sampleRate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                               ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            if (chunkSize > 16) fseek(f, chunkSize - 16, SEEK_CUR);
            haveFmt = true;
        } else if (memcmp(chunkHdr, "data", 4) == 0) {
            info.dataOffset = (uint32_t)ftell(f);
            info.dataBytes  = chunkSize;
            return haveFmt;
        } else {
            fseek(f, chunkSize + (chunkSize & 1), SEEK_CUR);   // chunks are word-aligned
        }
    }
    return false;
}

static bool loadZoneToRam(const char* samplePath, PatchZone& zone, int16_t presetId) {
    FILE* f = fopen(samplePath, "rb");
    if (!f) return false;
    WavInfo info;
    if (!wavParse(f, info) || info.channels == 0 || info.channels > 2) { fclose(f); return false; }

    // Loop POINTS live on the preset, but whether a note actually loops is
    // decided per note-on via amy_event.feedback (see rompler_engine.cpp) —
    // these are just "loop the whole sample" bounds, harmless to set even for
    // zones that never loop.
    uint32_t totalFrames = info.dataBytes / (info.channels * 2);
    int16_t* buf = pcm_load(presetId, totalFrames, info.sampleRate, (uint8_t)info.channels,
                             zone.rootNote, 0, totalFrames);
    if (!buf) { fclose(f); return false; }

    fseek(f, info.dataOffset, SEEK_SET);
    size_t need = (size_t)info.dataBytes;
    size_t got = fread(buf, 1, need, f);
    fclose(f);
    return got == need;
}

static bool loadZoneStreaming(const char* samplePath, PatchZone& zone, int16_t presetId) {
    char msg[128];
    // zF<preset>,<filename>,<midinote> — see src/parse.c. Filenames may not
    // contain commas (the wire parser splits on them).
    snprintf(msg, sizeof(msg), "zF%d,%s,%d", (int)presetId, samplePath, (int)zone.rootNote);
    amy_add_message(msg);
    return true;   // pcm_load_file() logs its own failures; nothing else to check here
}

// ─── Public API ──────────────────────────────────────────────────────────────
void patchUnload(uint8_t track) {
    if (track >= NUM_TRACKS) return;
    TrackPatch& tp = g_trackPatch[track];
    for (uint8_t i = 0; i < tp.numZones; i++) {
        if (tp.zones[i].presetId >= 0) pcm_unload_preset((uint16_t)tp.zones[i].presetId);
    }
    tp = TrackPatch();
}

bool patchLoad(uint8_t track, const char* dirName) {
    if (track >= NUM_TRACKS || !dirName || !dirName[0]) return false;
    patchUnload(track);

    TrackPatch& tp = g_trackPatch[track];
    strncpy(tp.info.dirName, dirName, sizeof(tp.info.dirName) - 1);
    strncpy(tp.info.name, dirName, sizeof(tp.info.name) - 1);   // fallback if patch.cfg has no name=

    char patchDir[80], cfgPath[96];
    buildPatchDirPath(dirName, patchDir, sizeof(patchDir));
    snprintf(cfgPath, sizeof(cfgPath), "%s/patch.cfg", patchDir);

    if (!parsePatchCfg(cfgPath, tp.info, tp.zones, MAX_ZONES_PER_PATCH, tp.numZones)) {
        tp = TrackPatch();
        return false;
    }

    uint8_t loadedCount = 0;
    for (uint8_t i = 0; i < tp.numZones; i++) {
        PatchZone& z = tp.zones[i];
        char samplePath[128];
        snprintf(samplePath, sizeof(samplePath), "%s/%s", patchDir, z.file);
        int16_t presetId = presetIdFor(track, i);
        bool ok = z.streaming ? loadZoneStreaming(samplePath, z, presetId)
                               : loadZoneToRam(samplePath, z, presetId);
        if (ok) { z.presetId = presetId; loadedCount++; }
        else    { z.presetId = -1; }
    }

    // (Re)configure this track's AMY synth for PCM playback at the patch's
    // requested polyphony — same pattern AMY's own amy_default_synths() uses
    // (src/api.c) to stand up an instrument.
    amy_event e = amy_default_event();
    e.synth = track;
    e.num_voices = tp.info.polyphony;
    e.oscs_per_voice = 1;
    e.wave = PCM;
    amy_add_event(&e);

    tp.loaded = loadedCount > 0;
    return tp.loaded;
}

const PatchZone* patchFindZone(uint8_t track, uint8_t note, uint8_t velocity) {
    if (track >= NUM_TRACKS) return nullptr;
    TrackPatch& tp = g_trackPatch[track];
    for (uint8_t i = 0; i < tp.numZones; i++) {
        PatchZone& z = tp.zones[i];
        if (z.presetId < 0) continue;
        if (note >= z.noteLo && note <= z.noteHi && velocity >= z.velLo && velocity <= z.velHi)
            return &z;
    }
    return nullptr;
}

TrackPatch& patchForTrack(uint8_t track) {
    static TrackPatch empty;
    if (track >= NUM_TRACKS) return empty;
    return g_trackPatch[track];
}

uint8_t patchScan(PatchInfo* list, uint8_t max) {
    uint8_t count = 0;
    File dir = SD.open(PATCH_DIR);
    if (!dir || !dir.isDirectory()) return 0;

    File entry;
    while (count < max && (entry = dir.openNextFile())) {
        if (entry.isDirectory()) {
            const char* dirName = entry.name();
            // SD.h may hand back either "name" or "/patches/name" depending on
            // core version — keep only the last path component.
            const char* slash = strrchr(dirName, '/');
            if (slash) dirName = slash + 1;

            PatchInfo& info = list[count];
            info = PatchInfo();
            strncpy(info.dirName, dirName, sizeof(info.dirName) - 1);
            strncpy(info.name, dirName, sizeof(info.name) - 1);

            char cfgPath[96];
            snprintf(cfgPath, sizeof(cfgPath), "%s%s/%s/patch.cfg", SD_MOUNT_POINT, PATCH_DIR, dirName);
            FILE* f = fopen(cfgPath, "r");
            if (f) {
                char line[96];
                // Only the header fields matter for the browser list — bail
                // out early rather than reading the whole (possibly long)
                // zone table just to show a name.
                for (int i = 0; i < 8 && fgets(line, sizeof(line), f); i++) {
                    trimNewline(line);
                    if (strncmp(line, "name=", 5) == 0)
                        strncpy(info.name, line + 5, sizeof(info.name) - 1);
                    else if (strncmp(line, "category=", 9) == 0)
                        strncpy(info.category, line + 9, sizeof(info.category) - 1);
                }
                fclose(f);
            }
            count++;
        }
        entry.close();
    }
    dir.close();
    return count;
}
