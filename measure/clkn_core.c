/* SPDX-License-Identifier: GPL-3.0-only */
/* clkn: a measurement core for the host clock (studio branch only, not part of any upstream change). No firmware, only
 * the ABI, and a sound for every message it is handed, so that what a DAW gives the core and when can be read from the
 * audio the plugin makes (exported per track, compared with a reference click): the whole chain from the host's transport
 * to the audio, including the engine's own granularity (the Device runs an unpaced core a 128-frame block at a time).
 *
 *   message (USB-MIDI packet)      sound (a sine burst)
 *   0xF8 timing clock (CIN 0xF)    6000 Hz,  2 ms   (one per tick)
 *   0xFA start                      440 Hz, 60 ms
 *   0xFB continue                   660 Hz, 60 ms
 *   0xFC stop                       220 Hz, 60 ms
 *   0xF2 song position (CIN 3)      880 Hz, 40 ms
 *   note on (CIN 9, velocity > 0)  4000 Hz,  3 ms   (for the calibration: the host's own MIDI, no clock)
 *
 * A burst starts at the first audio() after the message (a block of the Device, as for any core). It has a 0.25 ms linear
 * attack and release so that its onset reads cleanly. Everything else (CCs, SysEx, other notes) is dropped. Silence,
 * a blank screen, a 64 KiB flash. Playing the keys (notes 53 .. 79) does not reach it: use a note outside them
 * (the calibration uses 100) or turn "... and reach the firmware's MIDI in" on. */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "fm1core.h"

static fm1panel_t panel;
static int8_t keymap[6 * FM1P_NCOL];
static uint8_t flash[65536];

#define FS 44100.0
#define NVOICE 16
#define NPEND 256

static struct {
    double freq, phase;
    uint32_t left, total, edge;
} voice[NVOICE];

/* what midi_in saw since the last audio(): voice kinds to start */
enum { K_CLOCK, K_START, K_CONT, K_STOP, K_SPP, K_NOTE, K_N };
static const struct {
    double hz, ms;
} kind[K_N] = {{6000, 2}, {440, 60}, {660, 60}, {220, 60}, {880, 40}, {4000, 3}};
static uint8_t pend[NPEND];
static uint32_t pend_w, pend_r;

static int midi_in(uint32_t p)
{
    const uint32_t cin = p & 15u;
    const uint8_t b0 = (uint8_t)(p >> 8), b2 = (uint8_t)(p >> 24);
    int k = -1;
    if (cin == 0xF)
        k = b0 == 0xF8 ? K_CLOCK : b0 == 0xFA ? K_START : b0 == 0xFB ? K_CONT : b0 == 0xFC ? K_STOP : -1;
    else if (cin == 3 && b0 == 0xF2)
        k = K_SPP;
    else if (cin == 9 && (b0 & 0xF0) == 0x90 && b2 > 0)
        k = K_NOTE;
    if (k >= 0 && pend_w - pend_r < NPEND)
        pend[pend_w++ % NPEND] = (uint8_t)k;
    return 1;
}

static int midi_out_take(uint32_t *p)
{
    (void)p;
    return 0;
}

static void start_voice(int k)
{
    for (int i = 0; i < NVOICE; i++)
        if (voice[i].left == 0) {
            voice[i].freq = kind[k].hz;
            voice[i].phase = 0.0;
            voice[i].total = voice[i].left = (uint32_t)(kind[k].ms * FS / 1000.0 + 0.5);
            voice[i].edge = (uint32_t)(0.25 * FS / 1000.0 + 0.5);
            return;
        }
}

/* the Device calls this for each 128-frame block it makes (int16 stereo, interleaved) */
static void audio(int16_t *st, uint32_t n)
{
    while (pend_r != pend_w)
        start_voice(pend[pend_r++ % NPEND]);
    for (uint32_t f = 0; f < n; f++) {
        double s = 0.0;
        for (int i = 0; i < NVOICE; i++) {
            if (voice[i].left == 0)
                continue;
            const uint32_t at = voice[i].total - voice[i].left;      /* frames played */
            double env = 1.0;
            if (at < voice[i].edge)
                env = (double)at / (double)voice[i].edge;
            else if (voice[i].left <= voice[i].edge)
                env = (double)voice[i].left / (double)voice[i].edge;
            s += 0.4 * env * sin(voice[i].phase);
            voice[i].phase += 2.0 * 3.14159265358979323846 * voice[i].freq / FS;
            voice[i].left--;
        }
        if (s > 1.0)
            s = 1.0;
        if (s < -1.0)
            s = -1.0;
        st[2 * f] = st[2 * f + 1] = (int16_t)(s * 32000.0);
    }
}

static void tick(uint32_t ms) { (void)ms; }
static void options(const char *f, int nf, int s, int h) { (void)f, (void)nf, (void)s, (void)h; }
static void init(int demo)
{
    (void)demo;
    for (int i = 0; i < FM1P_NB; i++)
        panel.btn_id[i] = (uint8_t)i;
    for (int i = 0; i < FM1P_NE - 1; i++)
        panel.enc_id[i] = (uint8_t)i, panel.enc_dir[i] = 1;
    panel.lcd_writes++;
    memset(voice, 0, sizeof voice);
    pend_w = pend_r = 0;
}
static void frame(void) {}
static void idle(void) {}
static void nop(void) {}
static void stats(uint32_t *a, uint32_t *b) { *a = 0, *b = 0; }
static void ui_info(char *buf, uint32_t n)
{
    if (n)
        buf[0] = 0;
}
static int halted(void) { return 0; }
static int flash_stage(const uint8_t *b, uint32_t n)
{
    memset(flash, 0xFF, sizeof flash);
    if (b)
        memcpy(flash, b, n < sizeof flash ? n : sizeof flash);
    return 1;
}
static uint8_t *flash_ptr(void) { return flash; }
static uint32_t flash_dirty(void) { return 0; }
static int flash_sync(void) { return 1; }

static fm1core_t desc = {
    .abi_version = FM1CORE_ABI,
    .id = "clkn",
    .name = "Clock clicks (measurement)",
    .version = "1",
    .source_url = "",
    .family = "test",
    .license = "GPL-3.0-only",
    .gated = 0,
    .flash_size = sizeof flash,
    .panel = &panel,
    .keymap = keymap,
    .options = options,
    .init = init,
    .tick = tick,
    .frame = frame,
    .idle = idle,
    .audio = audio,
    .midi_in = midi_in,
    .midi_out_take = midi_out_take,
    .dump = nop,
    .stats = stats,
    .ui_info = ui_info,
    .shutdown = nop,
    .halted = halted,
    .flash_stage = flash_stage,
    .flash = flash_ptr,
    .flash_dirty = flash_dirty,
    .flash_sync = flash_sync,
};

FM1CORE_EXPORT const fm1core_t *fm1core_get(uint32_t abi)
{
    if (abi != FM1CORE_ABI)
        return 0;
    memset(keymap, -1, sizeof keymap);
    return &desc;
}
