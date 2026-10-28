/* SPDX-License-Identifier: GPL-3.0-only */
/* A fake core for host_clock_plugin_test (tests/host_clock_plugin_test.cpp): no firmware, only the ABI. It writes down
 * the MIDI clock messages it is given and gives each back as a SysEx on its MIDI out, so the test sees, in the host's
 * MIDI, what reached the core and when (in the core's own device milliseconds):
 *   - 0xF8 .. 0xFC (USB-MIDI CIN 0xF) and 0xF2 (CIN 3) through midi_in are kept, with the device millisecond they
 *     arrived in (tick() counts them up from 0 at each init);
 *   - at the next tick() each becomes F0 7D 43 4B <status - 0xF0> <data1> <data2> <ms in 4 x 7 bits, low first> F7.
 * Everything else (notes, SysEx) is dropped. Silence, a blank screen, a 64 KiB flash. */
#include <stdint.h>
#include <string.h>

#include "fm1core.h"

static fm1panel_t panel;
static int8_t keymap[6 * FM1P_NCOL];
static uint8_t flash[65536];
static uint32_t now_ms;

#define NREC 4096
static struct {
    uint8_t st, d1, d2;
    uint32_t ms;
} rec[NREC];
static uint32_t rec_w, rec_r;

#define NOUT 8192
static uint32_t outq[NOUT];
static uint32_t out_w, out_r;

static void out_pkt(uint32_t p)
{
    if (out_w - out_r < NOUT)
        outq[out_w++ % NOUT] = p;
}
static void out_sysex(const uint8_t *d, uint32_t n)   /* d: F0 .. F7 */
{
    uint32_t i = 0;
    while (n - i > 3) {
        out_pkt(4u | (uint32_t)d[i] << 8 | (uint32_t)d[i + 1] << 16 | (uint32_t)d[i + 2] << 24);
        i += 3;
    }
    {
        const uint32_t r = n - i;
        out_pkt((r == 1 ? 5u : r == 2 ? 6u : 7u) | (uint32_t)d[i] << 8 | (r > 1 ? (uint32_t)d[i + 1] << 16 : 0u) |
                (r > 2 ? (uint32_t)d[i + 2] << 24 : 0u));
    }
}

static void keep(uint8_t st, uint8_t d1, uint8_t d2)
{
    if (rec_w - rec_r < NREC) {
        rec[rec_w % NREC].st = st, rec[rec_w % NREC].d1 = d1, rec[rec_w % NREC].d2 = d2;
        rec[rec_w % NREC].ms = now_ms;
        rec_w++;
    }
}

static int midi_in(uint32_t p)
{
    const uint32_t cin = p & 15u;
    const uint8_t b0 = (uint8_t)(p >> 8), b1 = (uint8_t)(p >> 16), b2 = (uint8_t)(p >> 24);
    if (cin == 0xF && b0 >= 0xF8 && b0 <= 0xFC)
        keep(b0, 0, 0);
    else if (cin == 3 && b0 == 0xF2)
        keep(b0, b1, b2);
    return 1;
}

static int midi_out_take(uint32_t *p)
{
    if (out_r == out_w)
        return 0;
    *p = outq[out_r++ % NOUT];
    return 1;
}

static void tick(uint32_t ms)
{
    now_ms = ms;
    while (rec_r != rec_w) {
        const uint32_t i = rec_r++ % NREC;
        uint8_t r[16];
        uint32_t k = 0;
        r[k++] = 0xF0, r[k++] = 0x7D, r[k++] = 0x43, r[k++] = 0x4B;
        r[k++] = (uint8_t)(rec[i].st - 0xF0u), r[k++] = rec[i].d1 & 0x7F, r[k++] = rec[i].d2 & 0x7F;
        r[k++] = (uint8_t)(rec[i].ms & 0x7F), r[k++] = (uint8_t)((rec[i].ms >> 7) & 0x7F);
        r[k++] = (uint8_t)((rec[i].ms >> 14) & 0x7F), r[k++] = (uint8_t)((rec[i].ms >> 21) & 0x7F);
        r[k++] = 0xF7;
        out_sysex(r, k);
    }
}

static void options(const char *f, int nf, int s, int h) { (void)f, (void)nf, (void)s, (void)h; }
static void init(int demo)
{
    (void)demo;
    for (int i = 0; i < FM1P_NB; i++)
        panel.btn_id[i] = (uint8_t)i;
    for (int i = 0; i < FM1P_NE - 1; i++)
        panel.enc_id[i] = (uint8_t)i, panel.enc_dir[i] = 1;
    panel.lcd_writes++;
    now_ms = 0, rec_w = rec_r = 0, out_w = out_r = 0;
}
static void frame(void) {}
static void idle(void) {}
static void audio(int16_t *st, uint32_t n) { memset(st, 0, (size_t)n * 4u); }
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
    .id = "clk",
    .name = "Clock recorder (test)",
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
