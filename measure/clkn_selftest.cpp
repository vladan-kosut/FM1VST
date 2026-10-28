// SPDX-License-Identifier: GPL-3.0-only
// clkn (clkn_core.c) on a Device, no JUCE and no DAW, at several host rates: every message makes one burst of its own
// frequency and length, not before the message and within a Device block (plus a resampler's few samples) after it, and
// nothing else makes a sound. Studio branch only; run through measure/build_clkn.sh selftest.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "device.h"

extern "C" const fm1core_t *fm1core_get(uint32_t abi);

static int fails = 0;
static void check(bool c, const std::string &what)
{
    std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what.c_str());
    fails += !c;
}

struct Msg {
    const char *name;
    uint32_t pkt;
    double hz, ms;
    double at_ms;                               // when the host gives it, in ms from the run's start
};

static uint32_t pkt1(uint8_t st) { return 0xFu | (uint32_t)st << 8; }
static uint32_t pkt3(uint8_t st, uint8_t a, uint8_t b) { return 0x3u | (uint32_t)st << 8 | (uint32_t)a << 16 | (uint32_t)b << 24; }
static uint32_t noteOn(uint8_t n, uint8_t v) { return 0x9u | 0x90u << 8 | (uint32_t)n << 16 | (uint32_t)v << 24; }

int main()
{
    const std::vector<Msg> msgs = {
        {"0xF8", pkt1(0xF8), 6000, 2, 100},  {"0xFA", pkt1(0xFA), 440, 60, 300}, {"0xFB", pkt1(0xFB), 660, 60, 500},
        {"0xFC", pkt1(0xFC), 220, 60, 700},  {"0xF2", pkt3(0xF2, 38, 0), 880, 40, 900}, {"note 100", noteOn(100, 90), 4000, 3, 1100},
        {"0xF8 again", pkt1(0xF8), 6000, 2, 1303.7},
    };
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        std::printf("[host rate %.0f Hz, blocks of 256]\n", sr);
        const fm1core_t *c = fm1core_get(FM1CORE_ABI);
        Device dev(c);
        dev.boot_from(nullptr, 0);
        const int bs = 256;
        const long total = (long)(1.5 * sr) / bs * bs;
        std::vector<float> L((size_t)total), R((size_t)total);
        // a note with velocity 0, a CC and a SysEx byte make no sound
        std::vector<std::pair<long, uint32_t>> ev;
        for (const Msg &m : msgs)
            ev.push_back({(long)std::floor(m.at_ms * sr / 1000.0 + 0.5), m.pkt});
        ev.push_back({(long)(1.4 * sr), noteOn(100, 0)});
        ev.push_back({(long)(1.41 * sr), 0x3u | 0xB0u << 8 | 1u << 16 | 5u << 24});
        std::sort(ev.begin(), ev.end(), [](auto &a, auto &b) { return a.first < b.first; });
        size_t ei = 0;
        long pos = 0;
        while (pos < total) {
            const long end = std::min(total, (pos / bs + 1) * bs);
            while (pos < end) {
                const long upto = ei < ev.size() && ev[ei].first < end ? std::max(pos, ev[ei].first) : end;
                if (upto > pos) {
                    dev.render(L.data() + pos, R.data() + pos, (uint32_t)(upto - pos), sr);
                    pos = upto;
                }
                while (ei < ev.size() && ev[ei].first <= pos && pos < end + 1) {
                    dev.midi_in(ev[ei].second);
                    ei++;
                    if (ei < ev.size() && ev[ei].first > pos)
                        break;
                }
                if (pos >= end)
                    break;
            }
        }
        // each message: the burst after it
        std::vector<char> claimed((size_t)total, 0);
        for (const Msg &m : msgs) {
            const long s0 = (long)std::floor(m.at_ms * sr / 1000.0 + 0.5);
            const long win = (long)((m.ms + 20.0) * sr / 1000.0);
            long first = -1, last = -1;
            for (long n = s0; n < std::min(total, s0 + win); n++)
                if (std::fabs(L[(size_t)n]) > 0.03f) {
                    if (first < 0)
                        first = n;
                    last = n;
                    claimed[(size_t)n] = 1;
                }
            if (first < 0) {
                check(false, std::string(m.name) + ": no sound after the message");
                continue;
            }
            // frequency from the zero crossings
            int cross = 0;
            for (long n = first + 1; n <= last; n++)
                cross += (L[(size_t)n - 1] < 0) != (L[(size_t)n] < 0);
            const double dur = (double)(last - first + 1) * 1000.0 / sr;
            const double hz = cross > 1 ? 0.5 * cross * sr / (double)(last - first) : 0;
            const double late = (double)(first - s0) * 1000.0 / sr;
            const double blockMs = 1000.0 * EMU_BLOCK / EMU_FS;
            char b[200];
            std::snprintf(b, sizeof b, "%s: burst %.0f Hz (want %.0f), %.2f ms long (want %.0f), starts %.2f ms after the message", m.name, hz,
                          m.hz, dur, m.ms, late);
            // a burst starts at the first audio block made after the message: never before it, and within a block (and a few
            // resampler samples) of it; its first samples are the attack, below the threshold, so up to 0.3 ms are lost
            check(late >= 0.0 && late <= blockMs + 0.3 + 4.0 * 1000.0 / sr && std::fabs(hz - m.hz) <= 0.04 * m.hz &&
                      std::fabs(dur - m.ms) <= 0.7,
                  b);
        }
        long stray = 0;
        for (long n = 0; n < total; n++)
            stray += !claimed[(size_t)n] && std::fabs(L[(size_t)n]) > 0.03f;
        check(stray == 0, "no sound that no message made (" + std::to_string(stray) + " samples; velocity 0, a CC: silent)");
        double lr = 0;
        for (long n = 0; n < total; n++)
            lr = std::max(lr, (double)std::fabs(L[(size_t)n] - R[(size_t)n]));
        check(lr < 1e-6, "both channels the same");
    }
    std::printf("clkn_selftest: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
