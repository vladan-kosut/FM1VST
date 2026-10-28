// SPDX-License-Identifier: GPL-3.0-only
// The host's transport as MIDI clock (plugin/HostClock.h) on a simulated playhead, no JUCE and no DAW: the tick count
// and every tick's sample against the ideal grid (block sizes 32 .. 2048, 44.1 / 48 / 96 kHz, steady, stepped and ramped
// tempo), a start at the song's start / in the middle of a bar / on a bar line (Standard: 0xF2 + 0xFB on a sixteenth;
// startOnBar: 0xFA on the bar line; 3/4), stop, pause and continue, locate, loop, a count-in, a host that gives no tempo or
// position (or only the sample time), a clock arriving in the host's MIDI, resync (a core switch), the modes, and a block
// longer than the event list. Every playing block also has to carry exactly the ticks that fall in it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "HostClock.h"

using namespace hostclock;

static int fails = 0;
static void check(bool c, const std::string &what)
{
    std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what.c_str());
    fails += !c;
}

struct Rec {
    int64_t pos;               // sample since the start of the run
    int len;
    uint8_t b[3];
};

// a host: a playhead that moves by the block's length at the block's tempo, and the clock behind it
struct Sim {
    HostClock hc;
    Mode mode = Mode::ClockTransport;
    bool bar = false;
    double sr = 48000.0;
    int bs = 512;
    // the playhead
    double ppq = 0.0, bpm = 120.0;
    bool playing = false;
    int num = 4, den = 4;
    bool hasBar = true, hasPpq = true;
    int64_t samples = 0;       // the run so far
    // what came out
    std::vector<Rec> log;
    bool badBlock = false;     // an offset out of the block or going back, or a count that isn't the ticks that fall in it
    Event ev[kMaxEvents];

    Transport make() const
    {
        Transport t;
        t.valid = true;
        t.playing = playing;
        t.hasBpm = true;
        t.bpm = bpm;
        t.hasPpq = hasPpq;
        t.ppq = ppq;
        t.num = num;
        t.den = den;
        const double barLen = 4.0 * num / den;
        t.hasBarStart = hasBar;
        t.barStartPpq = std::floor(ppq / barLen) * barLen;
        t.hasSamples = true;
        t.timeInSamples = samples;
        return t;
    }
    // one block; the playhead moves on after it
    int step(bool ext = false, int n = 0)
    {
        if (n <= 0)
            n = bs;
        const Transport t = make();
        const double p0 = ppq, p1 = ppq + n * bpm / (60.0 * sr);
        const int c = hc.process(t, n, sr, mode, bar, ext, ev, kMaxEvents);
        int prev = 0, ticks = 0;
        for (int i = 0; i < c; i++) {
            if (ev[i].offset < 0 || ev[i].offset >= n || ev[i].offset < prev)
                badBlock = true;
            prev = ev[i].offset;
            ticks += ev[i].len == 1 && ev[i].b[0] == 0xF8;
            log.push_back({samples + ev[i].offset, ev[i].len, {ev[i].b[0], ev[i].b[1], ev[i].b[2]}});
        }
        if (playing && !ext && mode != Mode::Off && c < kMaxEvents - 4) {
            // (the ticks k with p0 <= k / 24 < p1 and k >= 0, from the first at or after p0)
            const long a = std::max(0L, (long)std::ceil(p0 * 24.0 - 1e-6)), b = (long)std::ceil(p1 * 24.0 - 1e-6);
            if (ticks != std::max(0L, b - a) && hc_expectsTicks)
                badBlock = true;
        }
        samples += n;
        if (playing)
            ppq = p1;
        return c;
    }
    void skip(int n)                                  // a block the core never got: the playhead moved on without it
    {
        samples += n;
        if (playing)
            ppq += n * bpm / (60.0 * sr);
    }
    void run(double ppqLen, double tempo = 0.0)       // blocks until the playhead has moved ppqLen
    {
        const double end = ppq + ppqLen;
        while (ppq < end - 1e-12) {
            if (tempo > 0)
                bpm = tempo;
            step();
        }
    }
    bool hc_expectsTicks = true;
};

static int count(const std::vector<Rec> &l, uint8_t b0, size_t from = 0)
{
    int c = 0;
    for (size_t i = from; i < l.size(); i++)
        c += l[i].b[0] == b0 && (b0 != 0xF2 || l[i].len == 3);
    return c;
}
static int firstOf(const std::vector<Rec> &l, uint8_t b0, size_t from = 0)
{
    for (size_t i = from; i < l.size(); i++)
        if (l[i].b[0] == b0)
            return (int)i;
    return -1;
}
static bool near(int64_t a, double b, double tol = 1.0) { return std::fabs((double)a - b) <= tol; }

int main()
{
    // 1. a steady tempo, from the song's start: every tick's sample on the ideal grid, the count exact, FA then F8 at 0
    {
        int cases = 0, bad = 0, badPos = 0, badCount = 0;
        const int sizes[] = {32, 64, 100, 256, 480, 512, 1000, 1024, 2048};
        const double rates[] = {44100.0, 48000.0, 96000.0};
        const double tempi[] = {90.07, 120.0, 123.45, 128.0, 174.0};
        for (int bs : sizes)
            for (double sr : rates)
                for (double bpm : tempi) {
                    Sim s;
                    s.bs = bs, s.sr = sr, s.bpm = bpm, s.playing = true;
                    s.run(64.0);
                    cases++;
                    bad += s.badBlock;
                    const double tickLen = 60.0 * sr / (bpm * 24.0);
                    int k = 0;
                    for (const Rec &r : s.log)
                        if (r.b[0] == 0xF8) {
                            badPos += !near(r.pos, k * tickLen);
                            k++;
                        }
                    const long want = (long)std::ceil(s.ppq * 24.0 - 1e-6);
                    badCount += k != want;
                    const bool head = s.log.size() > 1 && s.log[0].b[0] == 0xFA && s.log[0].pos == 0 &&
                                      s.log[1].b[0] == 0xF8 && s.log[1].pos == 0 && count(s.log, 0xFA) == 1 &&
                                      count(s.log, 0xF2) == 0 && count(s.log, 0xFC) == 0;
                    bad += !head;
                }
        check(cases == 9 * 3 * 5, "steady tempo: " + std::to_string(cases) + " runs (9 block sizes x 3 rates x 5 tempi)");
        check(bad == 0, "steady tempo: every block in range, in order and with exactly its ticks; FA then F8 at sample 0");
        check(badPos == 0, "steady tempo: every tick within 1 sample of k * (60 * sr / bpm / 24)");
        check(badCount == 0, "steady tempo: the tick count is the host's PPQ * 24");
    }

    // 2. a tempo stepped 120 -> 128 at a block boundary, and a ramp: spacing follows the tempo, nothing doubled or lost
    {
        Sim s;
        s.playing = true;
        s.run(32.0, 120.0);
        const size_t at = s.log.size();
        s.run(32.0, 128.0);
        double lo = 1e9, hi = 0;
        bool order = true;
        for (size_t i = at + 1; i < s.log.size(); i++)
            if (s.log[i].b[0] == 0xF8 && s.log[i - 1].b[0] == 0xF8) {
                const double d = (double)(s.log[i].pos - s.log[i - 1].pos);
                lo = std::min(lo, d), hi = std::max(hi, d);
                order = order && d > 0;
            }
        check(!s.badBlock && order, "stepped tempo: blocks in order, each with its ticks");
        check(lo >= 937.5 - 1.5 && hi <= 937.5 + 1.5, "stepped tempo (128 BPM): spacing 937.5 samples +/- 1.5 (" +
                                                          std::to_string(lo) + " .. " + std::to_string(hi) + ")");
        Sim r;
        r.playing = true;
        double rlo = 1e9, rhi = 0, bmax = 0;
        for (int i = 0; i < 600; i++) {
            r.bpm = 120.0 + 0.05 * i;
            bmax = r.bpm;
            r.step();
        }
        for (size_t i = 1; i < r.log.size(); i++)
            if (r.log[i].b[0] == 0xF8 && r.log[i - 1].b[0] == 0xF8) {
                const double d = (double)(r.log[i].pos - r.log[i - 1].pos);
                rlo = std::min(rlo, d), rhi = std::max(rhi, d);
            }
        check(!r.badBlock && rlo >= 60.0 * 48000.0 / (bmax * 24.0) - 1.5 && rhi <= 1000.0 + 1.5,
              "ramped tempo (120 -> " + std::to_string((int)bmax) + " BPM): spacing between the two tempi's, nothing doubled or lost");
    }

    // 3. a start in the middle of a bar, Standard: 0xF2 (the next sixteenth), 0xFB on its tick, F8 from the next tick on
    {
        Sim s;
        s.playing = true, s.ppq = 9.3;
        s.run(2.0);
        const int sp = firstOf(s.log, 0xF2), fb = firstOf(s.log, 0xFB);
        check(sp == 0 && s.log[0].pos == 0 && s.log[0].len == 3 && s.log[0].b[1] == 38 && s.log[0].b[2] == 0,
              "mid-bar start: SPP 38 (PPQ 9.5, the next sixteenth) first, at sample 0");
        check(fb > 0 && near(s.log[fb].pos, 0.2 * 24000.0) && s.log[fb + 1].b[0] == 0xF8 && s.log[fb + 1].pos == s.log[fb].pos,
              "mid-bar start: 0xFB at PPQ 9.5 (sample 4800), the tick right behind it");
        check(count(s.log, 0xFA) == 0 && count(s.log, 0xFC) == 0 && count(s.log, 0xF8) > 0 && !s.badBlock,
              "mid-bar start: no 0xFA / 0xFC; the clock runs before the continue too");
        int before = 0;
        for (int i = 0; i < fb; i++)
            before += s.log[i].b[0] == 0xF8;
        check(before == 4, "mid-bar start: the ticks 224 .. 227 come before the 0xFB (" + std::to_string(before) + ")");
        Sim hi;                                   // the song position is 14 bits: 1000 bars in = sixteenth 16000
        hi.playing = true, hi.ppq = 4000.0;
        hi.step();
        const int h = firstOf(hi.log, 0xF2);
        check(h == 0 && hi.log[0].b[1] == (16000 & 0x7F) && hi.log[0].b[2] == (16000 >> 7), "SPP: 7 + 7 bits (sixteenth 16000)");
    }

    // 4. startOnBar: 0xFA on the next bar line, no 0xF2 or 0xFB; 4/4, 3/4, on the line, without the host's bar start
    {
        Sim s;
        s.bar = true, s.playing = true, s.ppq = 9.3;
        s.run(4.0);
        const int fa = firstOf(s.log, 0xFA);
        check(fa >= 0 && near(s.log[fa].pos, 2.7 * 24000.0) && s.log[fa + 1].b[0] == 0xF8 && s.log[fa + 1].pos == s.log[fa].pos,
              "startOnBar 4/4 from PPQ 9.3: 0xFA at PPQ 12 (sample 64800), the tick behind it");
        check(count(s.log, 0xF2) == 0 && count(s.log, 0xFB) == 0 && count(s.log, 0xFA) == 1 && !s.badBlock,
              "startOnBar: no 0xF2, no 0xFB, one 0xFA; F8 runs before it");
        Sim l;
        l.bar = true, l.playing = true, l.ppq = 8.0;
        l.step();
        check(l.log.size() > 1 && l.log[0].b[0] == 0xFA && l.log[0].pos == 0 && l.log[1].b[0] == 0xF8 && l.log[1].pos == 0,
              "startOnBar exactly on a bar line (PPQ 8): 0xFA, F8 at sample 0");
        Sim t3;
        t3.bar = true, t3.playing = true, t3.num = 3, t3.den = 4, t3.ppq = 7.0;
        t3.run(4.0);
        const int f3 = firstOf(t3.log, 0xFA);
        check(f3 >= 0 && near(t3.log[f3].pos, 2.0 * 24000.0), "startOnBar 3/4 from PPQ 7: 0xFA at PPQ 9 (the bar line)");
        Sim o;
        o.bar = true, o.playing = true, o.hasBar = false, o.ppq = 9.3;
        o.run(4.0);
        const int fo = firstOf(o.log, 0xFA);
        check(fo >= 0 && near(o.log[fo].pos, 2.7 * 24000.0), "startOnBar without the host's bar start: bars counted from PPQ 0");
        Sim e;
        e.bar = true, e.playing = true, e.num = 7, e.den = 8, e.ppq = 3.6;
        e.run(4.0);
        const int fe = firstOf(e.log, 0xFA);
        check(fe >= 0 && near(e.log[fe].pos, 3.4 * 24000.0), "startOnBar 7/8 (a bar is 3.5 quarters) from PPQ 3.6: 0xFA at PPQ 7");
    }

    // 5. stop, pause and continue on the same position, resume somewhere else
    {
        Sim s;
        s.playing = true, s.ppq = 10.0;
        s.run(2.0);
        check(count(s.log, 0xF2) == 1 && count(s.log, 0xFB) == 1 && s.log[1].b[0] == 0xFB && s.log[1].pos == 0,
              "start on a sixteenth (PPQ 10): 0xF2 and 0xFB at sample 0");
        s.playing = false;
        const size_t at = s.log.size();
        s.step();
        check(s.log.size() == at + 1 && s.log[at].b[0] == 0xFC && s.log[at].pos == s.samples - s.bs,
              "stop: one 0xFC at the first stopped block's sample 0");
        s.step();
        s.step();
        check(s.log.size() == at + 1, "stopped: nothing more (no F8)");
        s.playing = true;
        const size_t at2 = s.log.size();
        s.step();
        s.step();                                  // (the 0xFB sits on the next tick, up to a sixteenth's 1/6 quarter away)
        s.step();
        check(count(s.log, 0xFB, at2) == 1 && count(s.log, 0xF2, at2) == 0 && count(s.log, 0xFA, at2) == 0,
              "resume on the same position: 0xFB on the next tick, no 0xF2");
        s.playing = false;
        s.step();
        s.ppq += 7.77;
        s.playing = true;
        const size_t at3 = s.log.size();
        s.step();
        check(count(s.log, 0xF2, at3) == 1 && count(s.log, 0xFB, at3) <= 1, "resume somewhere else: 0xF2 first");
        Sim b;
        b.bar = true, b.playing = true, b.ppq = 10.0;
        b.run(4.0);                                // (past the bar line at 12: 0xFA has gone out)
        b.playing = false;
        b.step();
        b.playing = true;
        const size_t at4 = b.log.size();
        b.step();
        b.step();
        b.step();
        check(count(b.log, 0xFB, at4) == 1 && count(b.log, 0xFA, at4) == 0,
              "startOnBar: a pause resumed in place is 0xFB, not a wait for the bar");
        Sim w;                                     // stopped while the start still waits for its bar line: the core never started
        w.bar = true, w.playing = true, w.ppq = 10.0;
        w.run(1.0);
        w.playing = false;
        w.step();
        w.playing = true;
        const size_t at5 = w.log.size();
        w.run(3.0);
        check(count(w.log, 0xFC) == 0 && count(w.log, 0xFB) == 0 && count(w.log, 0xFA, at5) == 1,
              "stopped before the start's bar line: no 0xFC, and the resume still waits for the bar (0xFA, no 0xFB)");
        check(!s.badBlock && !b.badBlock && !w.badBlock, "stop / continue: blocks in order, each with its ticks");
    }

    // 6. locate while playing and a loop
    {
        Sim s;
        s.playing = true;
        s.run(10.0);
        const size_t at = s.log.size();
        s.ppq = 50.0;
        s.run(2.0);
        {
            const int fc = firstOf(s.log, 0xFC, at), sp = firstOf(s.log, 0xF2, at), fb = firstOf(s.log, 0xFB, at);
            check(count(s.log, 0xFC, at) == 1 && count(s.log, 0xF2, at) == 1 && count(s.log, 0xFB, at) == 1 && fc >= 0 && sp > fc && fb > sp &&
                      s.log[fc].pos == s.log[sp].pos,
                  "locate (Standard): 0xFC, 0xF2, 0xFB in that order (stop, position, continue)");
        }
        Sim c;
        c.mode = Mode::Clock, c.playing = true;
        c.run(10.0);
        const size_t cat = c.log.size();
        c.ppq = 50.0;
        c.run(2.0);
        check(count(c.log, 0xF2, cat) + count(c.log, 0xFB, cat) + count(c.log, 0xFA, cat) + count(c.log, 0xFC, cat) == 0 && !c.badBlock,
              "locate (Clock): only ticks, re-timed to the new position");
        Sim b;
        b.bar = true, b.playing = true;
        b.run(16.0);
        const size_t bat = b.log.size();
        b.ppq -= 8.0;                              // a loop of two bars
        b.run(8.0);
        check(count(b.log, 0xFA, bat) + count(b.log, 0xFC, bat) + count(b.log, 0xF2, bat) == 0 && !b.badBlock,
              "loop of whole bars (startOnBar): nothing but ticks");
        const size_t bat2 = b.log.size();
        b.ppq += 5.5;                              // not whole bars
        b.run(8.0);
        const int fc = firstOf(b.log, 0xFC, bat2), fa = firstOf(b.log, 0xFA, bat2);
        check(fc >= 0 && fa > fc && !b.badBlock, "locate off the bar grid (startOnBar): 0xFC, then 0xFA on a bar line");
    }

    // 6b. small moves: a locate of 1/16 or 1/12 quarter while playing is detected; one under half a tick is not (a tempo
    // change and rounding move the expected position that much); a transport that stops and plays again a little way off
    // is not a pause resumed
    {
        const double jumps[] = {1.0 / 16.0, 1.0 / 12.0, 0.25, -0.25};
        for (double j : jumps) {
            Sim s;
            s.playing = true, s.ppq = 12.0;
            s.run(2.0);
            const size_t at = s.log.size();
            s.ppq += j;
            s.hc_expectsTicks = false;
            s.step();
            s.step();
            s.step();
            s.step();
            s.step();
            s.step();
            check(count(s.log, 0xF2, at) == 1 && count(s.log, 0xFC, at) == 1,
                  "playing, moved by " + std::to_string(j) + " quarters: a locate (0xFC, 0xF2, 0xFB)");
        }
        Sim q;
        q.playing = true, q.ppq = 12.0;
        q.run(2.0);
        const size_t at = q.log.size();
        q.ppq += 0.01;
        q.hc_expectsTicks = false;
        q.step();
        q.step();
        check(count(q.log, 0xF2, at) + count(q.log, 0xFA, at) + count(q.log, 0xFB, at) + count(q.log, 0xFC, at) == 0,
              "playing, moved by 0.01 quarter (under half a tick): not a locate");
        const double offs[] = {0.25, 0.5, 1.0 / 12.0};
        for (double j : offs) {
            Sim s;
            s.playing = true, s.ppq = 10.0;
            s.run(2.0);
            s.playing = false;
            s.step();
            s.ppq += j;
            s.playing = true;
            const size_t a2 = s.log.size();
            for (int i = 0; i < 12; i++)
                s.step();
            const int sp = firstOf(s.log, 0xF2, a2), fb = firstOf(s.log, 0xFB, a2);
            check(sp >= 0 && fb > sp && count(s.log, 0xFB, a2) == 1,
                  "stopped, played again " + std::to_string(j) + " quarters off: 0xF2 then 0xFB, not a bare 0xFB");
            Sim b;
            b.bar = true, b.playing = true, b.ppq = 10.0;
            b.run(4.0);
            b.playing = false;
            b.step();
            b.ppq += j;
            b.playing = true;
            const size_t a3 = b.log.size();
            b.run(2.5);                            // (the bar line at 16 is up to 1.75 quarters away)
            check(count(b.log, 0xFB, a3) == 0 && count(b.log, 0xFA, a3) == 1,
                  "startOnBar, stopped, played again " + std::to_string(j) + " off: 0xFA on the bar line, no 0xFB");
        }
    }

    // 6c. a block the processor skipped (the lock was taken, no core booted): under half a tick it is a tick late and
    // nothing else; more, the core lost its ticks and the move is a locate
    {
        Sim a;
        a.bs = 256, a.playing = true;
        a.run(8.0);
        const size_t at = a.log.size();
        a.skip(256);
        a.hc_expectsTicks = false;
        a.run(2.0);
        check(count(a.log, 0xFC, at) + count(a.log, 0xF2, at) + count(a.log, 0xFB, at) + count(a.log, 0xFA, at) == 0 &&
                  count(a.log, 0xF8) == (int)std::ceil(a.ppq * 24.0 - 1e-6) && !a.badBlock,
              "a skipped block of 256 (a quarter of a tick): no transport messages, no tick lost");
        Sim b;
        b.bs = 2048, b.playing = true;
        b.run(8.0);
        const size_t bt = b.log.size();
        b.skip(2048);
        b.hc_expectsTicks = false;
        b.run(2.0);
        check(count(b.log, 0xFC, bt) == 1 && count(b.log, 0xF2, bt) == 1 && count(b.log, 0xFB, bt) == 1 && !b.badBlock,
              "a skipped block of 2048 (two ticks): a locate (0xFC, 0xF2, 0xFB)");
    }

    // 7. a count-in: nothing until PPQ 0, then 0xFA and the tick there
    {
        Sim s;
        s.playing = true, s.ppq = -4.0;
        s.run(6.0);
        check(s.log.size() > 1 && s.log[0].b[0] == 0xFA && near(s.log[0].pos, 4.0 * 24000.0) && s.log[1].b[0] == 0xF8 &&
                  s.log[1].pos == s.log[0].pos && !s.badBlock,
              "count-in: nothing before PPQ 0, then 0xFA and F8 at sample 96000");
        Sim b;
        b.bar = true, b.playing = true, b.ppq = -4.0;
        b.run(6.0);
        check(b.log.size() > 1 && b.log[0].b[0] == 0xFA && near(b.log[0].pos, 4.0 * 24000.0), "count-in (startOnBar): the same");
    }

    // 8. a host with no tempo, no position, or only the sample time
    {
        Sim a;
        a.playing = true;
        Transport t = a.make();
        t.valid = false;
        Event ev[kMaxEvents];
        HostClock hc;
        check(hc.process(t, 512, 48000.0, Mode::ClockTransport, false, false, ev, kMaxEvents) == 0, "no position at all: no events");
        t = a.make();
        t.hasBpm = false;
        check(hc.process(t, 512, 48000.0, Mode::ClockTransport, false, false, ev, kMaxEvents) == 0, "no tempo: no events");
        t = a.make();
        t.hasPpq = false;
        t.hasSamples = false;
        check(hc.process(t, 512, 48000.0, Mode::ClockTransport, false, false, ev, kMaxEvents) == 0, "no PPQ and no sample time: no events");
        Sim p, q;
        p.playing = q.playing = true;
        q.hasPpq = false;                          // only timeInSamples, at a steady tempo
        p.run(16.0);
        q.run(16.0);
        bool same = p.log.size() == q.log.size();
        for (size_t i = 0; same && i < p.log.size(); i++)
            same = p.log[i].pos == q.log[i].pos && p.log[i].b[0] == q.log[i].b[0];
        check(same && !p.log.empty(), "no PPQ: derived from the sample time, the same events as with it");
    }

    // 9. a clock in the host's MIDI wins; afterwards ours starts afresh
    {
        Sim s;
        s.playing = true;
        s.run(4.0);
        const size_t at = s.log.size();
        s.step(true);
        const int blocks = (int)(48000.0 / s.bs) - 3;
        s.hc_expectsTicks = false;                 // (the silence is the point: the playhead plays on)
        for (int i = 0; i < blocks + 4; i++)
            s.step();
        check(s.log.size() == at, "an external clock: ours is silent for the second after it (" + std::to_string(blocks + 5) + " blocks)");
        s.hc_expectsTicks = true;
        for (int i = 0; i < 16; i++)
            s.step();
        check(s.log.size() > at && count(s.log, 0xF2, at) == 1 && count(s.log, 0xFB, at) == 1 && count(s.log, 0xFC, at) == 0,
              "after it: a fresh start (0xF2, 0xFB), no 0xFC");
        check(!s.badBlock, "external clock: blocks in order, each with its ticks");
    }

    // 10. resync (a core switched while playing): a fresh start, no stop
    {
        Sim s;
        s.playing = true;
        s.run(20.3);
        const size_t at = s.log.size();
        s.hc.resync();
        s.step();
        check(count(s.log, 0xFC, at) == 0 && count(s.log, 0xF2, at) == 1, "resync: 0xF2 for the position, no 0xFC");
        s.run(2.0);
        check(count(s.log, 0xFB, at) == 1 && !s.badBlock, "resync: then 0xFB on the sixteenth");
    }

    // 11. the modes
    {
        Sim o;
        o.mode = Mode::Off, o.playing = true;
        o.run(8.0);
        check(o.log.empty(), "Off: no events");
        Sim c;
        c.mode = Mode::Clock, c.playing = true;
        c.run(8.0);
        c.playing = false;
        c.step();
        c.step();
        check(count(c.log, 0xF8) == (int)std::ceil(c.ppq * 24.0 - 1e-6) && count(c.log, 0xFA) + count(c.log, 0xFB) + count(c.log, 0xFC) + count(c.log, 0xF2) == 0 &&
                  !c.badBlock,
              "Clock: one tick per 1/24 quarter of the playhead, no start / continue / stop / SPP, also through a stop");
        Sim m;
        m.playing = true;
        m.run(4.0);
        const size_t at = m.log.size();
        m.mode = Mode::Off;
        m.step();
        m.step();
        check(m.log.size() == at + 1 && m.log[at].b[0] == 0xFC && m.log[at].pos == m.samples - 2 * m.bs,
              "switched to Off while running: one 0xFC, then silence");
    }

    // 12. a block longer than the event list: capped, in range, and the next block is on the grid again
    {
        Sim s;
        s.playing = true, s.bpm = 300.0;
        s.hc_expectsTicks = false;
        s.step(false, 100000);
        const size_t first = s.log.size();
        check(first <= (size_t)kMaxEvents && !s.badBlock, "a block of 100000 samples at 300 BPM: at most " + std::to_string(kMaxEvents) + " events, in range");
        s.step(false, 512);
        bool onGrid = s.log.size() > first;
        const double tick = 60.0 * 48000.0 / (300.0 * 24.0);
        for (size_t i = first; i < s.log.size(); i++)
            if (s.log[i].b[0] == 0xF8)
                onGrid = onGrid && near(s.log[i].pos, std::round(s.log[i].pos / tick) * tick, 1.0);
        const size_t second = s.log.size() - first;     // (512 samples at 300 BPM: 1 or 2 ticks, not the dropped ones piled up)
        check(onGrid && second >= 1 && second <= 2 && !s.badBlock, "the next block: its ticks are on the grid, none late, none piled up");
    }

    // 13. the same input twice: the same output (an offline render)
    {
        auto go = [](Sim &s) {
            s.playing = true, s.ppq = 3.3, s.bar = false;
            s.run(5.0, 121.0);
            s.playing = false;
            s.step();
            s.playing = true;
            s.run(3.0, 127.5);
            s.ppq = 30.0;
            s.run(3.0);
        };
        Sim a, b;
        go(a);
        go(b);
        bool same = a.log.size() == b.log.size() && !a.log.empty();
        for (size_t i = 0; same && i < a.log.size(); i++)
            same = a.log[i].pos == b.log[i].pos && a.log[i].len == b.log[i].len && a.log[i].b[0] == b.log[i].b[0] &&
                   a.log[i].b[1] == b.log[i].b[1] && a.log[i].b[2] == b.log[i].b[2];
        check(same, "the same playhead twice: identical events");
    }

    std::printf("host_clock_test: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
