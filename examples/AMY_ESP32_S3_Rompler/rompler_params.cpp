#include "rompler_params.h"
#include <string.h>
#include <stdio.h>

const RomplerParamInfo ROMPLER_PARAM_INFO[NUM_ROMPLER_PARAMS] = {
    { "Gain",  7  },   // CC7  = channel volume
    { "Pan",   10 },   // CC10 = pan
    { "Tune",  0  },
};

static float trackParam[NUM_TRACKS][NUM_ROMPLER_PARAMS];

void romplerParamsInit() {
    for (uint8_t t = 0; t < NUM_TRACKS; t++)
        for (uint8_t i = 0; i < NUM_ROMPLER_PARAMS; i++)
            trackParam[t][i] = 0.5f;   // unity gain, center pan, no tune
}

void setTrackParam(uint8_t track, uint8_t id, float value01) {
    if (track >= NUM_TRACKS || id >= NUM_ROMPLER_PARAMS) return;
    trackParam[track][id] = constrain(value01, 0.0f, 1.0f);
}

float getTrackParam(uint8_t track, uint8_t id) {
    if (track >= NUM_TRACKS || id >= NUM_ROMPLER_PARAMS) return 0.5f;
    return trackParam[track][id];
}

bool paramIsLockable(uint8_t id) { return id < NUM_ROMPLER_PARAMS; }

void formatTrackParam(uint8_t track, uint8_t id, char* buf, size_t n) {
    float v = getTrackParam(track, id);
    switch (id) {
    case RP_GAIN: snprintf(buf, n, "%d%%", (int)(v * 200.0f)); break;
    case RP_PAN: {
        int p = (int)((v - 0.5f) * 200.0f);
        if (p == 0) snprintf(buf, n, "C");
        else        snprintf(buf, n, "%c%d", p < 0 ? 'L' : 'R', abs(p));
        break;
    }
    case RP_TUNE: snprintf(buf, n, "%+dc", (int)((v - 0.5f) * 2400.0f)); break;
    default: buf[0] = 0; break;
    }
}

float trackGainMul(uint8_t track)   { return getTrackParam(track, RP_GAIN) * 2.0f; }
float trackPanBias(uint8_t track)   { return (getTrackParam(track, RP_PAN) - 0.5f) * 2.0f; }
float trackTuneCents(uint8_t track) { return (getTrackParam(track, RP_TUNE) - 0.5f) * 2400.0f; }

void snapshotTrackParams(uint8_t track, uint8_t* out) {
    if (track >= NUM_TRACKS) { memset(out, 128, NUM_ROMPLER_PARAMS); return; }
    for (uint8_t i = 0; i < NUM_ROMPLER_PARAMS; i++)
        out[i] = (uint8_t)(trackParam[track][i] * 255.0f);
}

void restoreTrackParams(uint8_t track, const uint8_t* in) {
    if (track >= NUM_TRACKS) return;
    for (uint8_t i = 0; i < NUM_ROMPLER_PARAMS; i++)
        trackParam[track][i] = (float)in[i] * (1.0f / 255.0f);
}
