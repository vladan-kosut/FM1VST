// SPDX-License-Identifier: GPL-3.0-only
// The host's transport as MIDI clock (plugin/HostClock.h) through the processor, headless: a fake AudioPlayHead and a fake
// core that gives back, as SysEx on its MIDI out, each clock message it was handed and the device millisecond it came in
// (tests/clock_core.c). Steady tempo from the start (the first message 0xFA, the ticks one per 1/24 quarter, 20.83 ms
// apart at 120 BPM, 0xFC at the stop); Off; Clock only; no playhead; a start in the middle of a song (0xF2, 0xFB) and
// with Start on the bar line (0xFA on the bar line); a core switched while playing (a fresh start, no 0xFC); a clock in
// the host's MIDI wins; a block the processor skipped (no channels: the early return) under half a tick and over; the
// settings in the state (a new instance: Clock + transport; a state saved before the setting: Off); and the clock against
// a host MIDI event on the very same sample (the core reads the same device millisecond for both). FM1EMU_HOME points at
// a scratch folder under the build.
//
// THE CORE'S MILLISECONDS are the engine's, not the clock's: the Device runs the core by the host's demand (an unpaced
// native core, as here: device.h PACING), a 128-frame block at 44.1 kHz (2.9 ms) at a time, so the millisecond a message
// meets runs ahead of the host's sample by 0 .. 2.9 ms and is a whole number. Two messages on a perfect grid are read up to a
// block plus a millisecond off it (3.9 ms), whatever sent them. That bound is the tolerance of the spacing check, which is
// only a sanity check; the check of the clock's own sample accuracy is the one against a host event on the same sample.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "Processor.h"

static int fails = 0;
static void check(bool c, const std::string &what)
{
    std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what.c_str());
    fails += !c;
}

struct Rec {
    int st, d1, d2;
    double ms;
};

static int count(const std::vector<Rec> &r, int st, size_t from = 0)
{
    int c = 0;
    for (size_t i = from; i < r.size(); i++)
        c += r[i].st == st;
    return c;
}
static int firstOf(const std::vector<Rec> &r, int st, size_t from = 0)
{
    for (size_t i = from; i < r.size(); i++)
        if (r[i].st == st)
            return (int)i;
    return -1;
}

// a host's transport
struct Head : juce::AudioPlayHead {
    bool playing = false;
    double bpm = 120.0, ppq = 0.0;
    int num = 4, den = 4;
    int64_t samples = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo pi;
        pi.setIsPlaying(playing);
        pi.setBpm(bpm);
        pi.setPpqPosition(ppq);
        const double barLen = 4.0 * num / den;
        pi.setPpqPositionOfLastBarStart(std::floor(ppq / barLen) * barLen);
        pi.setTimeSignature(TimeSignature{num, den});
        pi.setTimeInSamples(samples);
        return pi;
    }
};

// one processor driven block by block under a Head; what the clock core gave back is kept
struct Run {
    FM1Processor &p;
    Head head;
    int bs;
    double sr;
    juce::AudioBuffer<float> buf;
    std::vector<Rec> recs;

    Run(FM1Processor &proc, int block = 256, double rate = 48000.0) : p(proc), bs(block), sr(rate), buf(2, block)
    {
        p.setRateAndBufferSizeDetails(sr, bs);
        p.prepareToPlay(sr, bs);
        p.setPlayHead(&head);
    }
    ~Run() { p.setPlayHead(nullptr); }

    // one block; hostMidi: the host's own MIDI for it; skipped: a block the processor returns from early (no channels)
    void block(const juce::MidiBuffer *hostMidi = nullptr, bool skipped = false)
    {
        juce::MidiBuffer m;
        if (hostMidi)
            m = *hostMidi;
        if (skipped) {
            juce::AudioBuffer<float> none(0, bs);
            p.processBlock(none, m);
        } else {
            buf.clear();
            p.processBlock(buf, m);
        }
        for (const auto e : m) {
            const uint8_t *d = e.data;
            if (e.numBytes == 12 && d[0] == 0xF0 && d[1] == 0x7D && d[2] == 0x43 && d[3] == 0x4B)
                recs.push_back({d[4] == 0x10 ? 0xB0 : 0xF0 + d[4], d[5], d[6], (double)((uint32_t)d[7] | (uint32_t)d[8] << 7 | (uint32_t)d[9] << 14 | (uint32_t)d[10] << 21)});
        }
        if (head.playing)
            head.ppq += bs * head.bpm / (60.0 * sr);
        head.samples += bs;
    }
    void blocks(int n)
    {
        for (int i = 0; i < n; i++)
            block();
    }
    void untilPpq(double ppq)                    // blocks until the playhead has reached it
    {
        while (head.ppq < ppq)
            block();
    }
};

static bool installCore(const char *path, const char *name)
{
    const juce::File src(path);
    const juce::File dst = FM1Processor::userCoresDir().getChildFile(juce::String(name) + FM1CORE_SUFFIX);
    FM1Processor::userCoresDir().createDirectory();
    return src.existsAsFile() && src.copyFileTo(dst);
}

static std::unique_ptr<FM1Processor> makeProcessor()
{
    auto p = std::make_unique<FM1Processor>();
    if (!p->switchCore("clk"))
        std::printf("  FAIL  the clock core loads\n"), fails++;
    return p;
}

int main()
{
    const juce::File scratch(FM1_SCRATCH_DIR);
    scratch.deleteRecursively();
    scratch.createDirectory();
    setenv("FM1EMU_HOME", scratch.getFullPathName().toRawUTF8(), 1);
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf("host_clock_plugin_test: FM1EMU_HOME=%s\n", scratch.getFullPathName().toRawUTF8());

    if (!installCore(FM1_CLK_CORE, "clk") || !installCore(FM1_ECHO_CORE, "echo")) {
        check(false, std::string("the test cores are built: ") + FM1_CLK_CORE + ", " + FM1_ECHO_CORE);
        return 1;
    }
    const double tickMs = 1000.0 * 60.0 / 120.0 / 24.0;   // one tick at 120 BPM

    std::printf("[a new instance]\n");
    {
        auto p = makeProcessor();
        check(p->getHostClockMode() == 2 && !p->getHostClockOnBar(), "default: Clock + transport, Start on the bar line off");
    }

    std::printf("[steady tempo from the start]\n");
    {
        auto p = makeProcessor();
        Run r(*p);
        r.head.playing = true;
        r.untilPpq(8.0);
        r.head.playing = false;
        r.blocks(8);
        const int ticks = count(r.recs, 0xF8);
        check(!r.recs.empty() && r.recs[0].st == 0xFA, "the first message is 0xFA");
        check(ticks == (int)std::ceil(r.head.ppq * 24.0 - 1e-6), "one tick per 1/24 quarter of the playhead (" + std::to_string(ticks) + ")");
        check(!r.recs.empty() && r.recs.back().st == 0xFC && count(r.recs, 0xFC) == 1, "0xFC at the stop, once");
        check(count(r.recs, 0xF2) == 0 && count(r.recs, 0xFB) == 0, "no 0xF2, no 0xFB");
        double worst = 0, first = -1, last = 0;
        int n = 0;
        for (size_t i = 0; i < r.recs.size(); i++)
            if (r.recs[i].st == 0xF8) {
                if (first < 0)
                    first = r.recs[i].ms;
                else if (r.recs[i - 1].st == 0xF8)
                    worst = std::max(worst, std::fabs(r.recs[i].ms - r.recs[i - 1].ms - tickMs));
                last = r.recs[i].ms;
                n++;
            }
        const double mean = n > 1 ? (last - first) / (n - 1) : 0;
        const double blockMs = 1000.0 * EMU_BLOCK / EMU_FS;   // (the Device's internal block: see the top)
        check(worst <= blockMs + 1.0, "every tick " + std::to_string(tickMs) + " ms after the one before, within a device block and a ms (" +
                                          std::to_string(blockMs + 1.0) + "): worst " + std::to_string(worst));
        check(std::fabs(mean - tickMs) <= 0.2, "the mean spacing " + std::to_string(mean) + " ms (no drift)");
    }

    std::printf("[the clock against a host event on the same sample]\n");
    {
        auto p = makeProcessor();
        p->setHostClockMode(1);
        Run r(*p);
        r.head.playing = true;
        // a tick, in samples: exactly 1000 at 120 BPM and 48 kHz, so the clock's tick and the host's event sit on the very same
        // sample (one sample apart they can be read a whole device block apart: the Device steps its clock between them)
        const double grid = r.sr * 60.0 / r.head.bpm / 24.0;
        const int ticks = 144;
        int next = 0;
        for (int64_t at = 0; next < ticks; at += r.bs) {
            juce::MidiBuffer m;
            for (; next < ticks; next++) {
                const int64_t pos = (int64_t)std::floor(next * grid + 0.5);
                if (pos >= at + r.bs)
                    break;
                m.addEvent(juce::MidiMessage::controllerEvent(1, 1, next & 0x7F), (int)(pos - at));
            }
            r.block(&m);
        }
        r.head.playing = false;
        r.blocks(8);
        std::vector<double> t8, tc;
        for (const Rec &x : r.recs) {
            if (x.st == 0xF8)
                t8.push_back(x.ms);
            else if (x.st == 0xB0 && x.d1 == 1)
                tc.push_back(x.ms);
        }
        double worst = 0;
        for (size_t i = 0; i < std::min(t8.size(), tc.size()); i++)
            worst = std::max(worst, std::fabs(t8[i] - tc[i]));
        check(t8.size() == (size_t)ticks && tc.size() == (size_t)ticks,
              "a tick and a control change per 1/24 quarter: " + std::to_string(t8.size()) + " and " + std::to_string(tc.size()));
        check(worst <= 1.0, "each tick is read at the same device ms as the host's control change on its sample, within 1 ms (the "
                            "engine's granularity is in both): worst " + std::to_string(worst));
    }

    std::printf("[the modes, no playhead]\n");
    {
        auto p = makeProcessor();
        p->setHostClockMode(0);
        Run r(*p);
        r.head.playing = true;
        r.untilPpq(2.0);
        check(r.recs.empty(), "Off: nothing reaches the core");
        auto q = makeProcessor();
        q->setHostClockMode(1);
        Run c(*q);
        c.head.playing = true;
        c.untilPpq(4.0);
        c.head.playing = false;
        c.blocks(8);
        check(count(c.recs, 0xF8) > 90 && count(c.recs, 0xFA) + count(c.recs, 0xFB) + count(c.recs, 0xFC) + count(c.recs, 0xF2) == 0,
              "Clock: ticks only, also through a stop (" + std::to_string(count(c.recs, 0xF8)) + " ticks)");
        auto n = makeProcessor();
        Run h(*n);
        n->setPlayHead(nullptr);                 // (no playhead at all: the Standalone)
        for (int i = 0; i < 200; i++)
            h.block();
        check(h.recs.empty(), "no playhead: nothing reaches the core");
    }

    std::printf("[a start in the middle of a song]\n");
    {
        auto p = makeProcessor();
        Run r(*p);
        r.head.playing = true, r.head.ppq = 9.3;
        r.untilPpq(9.7);
        const int sp = firstOf(r.recs, 0xF2), fb = firstOf(r.recs, 0xFB);
        check(sp == 0 && r.recs[0].d1 == 38 && r.recs[0].d2 == 0, "Clock + transport: 0xF2 first, song position 38 (PPQ 9.5)");
        int before = 0;
        for (int i = 0; i < fb; i++)
            before += r.recs[(size_t)i].st == 0xF8;
        check(fb > sp && before == 4 && count(r.recs, 0xFA) == 0, "then four ticks (224 .. 227), 0xFB on the tick of PPQ 9.5, no 0xFA");

        auto q = makeProcessor();
        q->setHostClockOnBar(true);
        Run b(*q);
        b.head.playing = true, b.head.ppq = 9.3;
        b.untilPpq(12.3);
        const int fa = firstOf(b.recs, 0xFA), f0 = firstOf(b.recs, 0xF8);
        check(fa > f0 && f0 >= 0 && count(b.recs, 0xF2) + count(b.recs, 0xFB) == 0,
              "Start on the bar line: ticks, then 0xFA; no 0xF2, no 0xFB");
        const double wait = fa > 0 && f0 >= 0 ? b.recs[(size_t)fa].ms - b.recs[(size_t)f0].ms : 0;
        check(std::fabs(wait - 64 * tickMs) <= 3.0, "0xFA 64 ticks after the first tick: the bar line at PPQ 12 (" + std::to_string(wait) + " ms)");
    }

    std::printf("[a core switched while playing]\n");
    {
        auto p = makeProcessor();
        Run r(*p);
        r.head.playing = true, r.head.ppq = 20.3;
        r.untilPpq(21.0);
        check(p->switchCore("echo"), "the echo core takes over");
        r.blocks(20);
        const size_t mark = r.recs.size();
        check(p->switchCore("clk"), "the clock core is back (a fresh boot)");
        r.untilPpq(r.head.ppq + 0.6);
        check(r.recs.size() > mark, "it hears the clock again");
        check(firstOf(r.recs, 0xF2, mark) == (int)mark && count(r.recs, 0xFC, mark) == 0 && count(r.recs, 0xFB, mark) == 1,
              "a fresh start (0xF2, then 0xFB), no 0xFC");
    }

    std::printf("[a clock in the host's MIDI wins]\n");
    {
        auto p = makeProcessor();
        Run r(*p);
        r.head.playing = true;
        r.untilPpq(2.0);
        const size_t mark = r.recs.size();
        juce::MidiBuffer m;
        m.addEvent(juce::MidiMessage(0xF8), 10);
        r.block(&m);
        r.blocks(120);                           // (0.64 s, inside the second ours keeps quiet)
        check(count(r.recs, 0xF8, mark) == 1 && count(r.recs, 0xF2, mark) + count(r.recs, 0xFB, mark) + count(r.recs, 0xFA, mark) == 0,
              "only the host's own 0xF8 reaches the core, ours is silent");
        r.blocks(150);
        check(count(r.recs, 0xF8, mark) > 20 && count(r.recs, 0xFB, mark) == 1, "after the second: ours again, as a fresh start");
    }

    std::printf("[a block the processor did not run]\n");
    for (int bsz : {256, 2048}) {
        auto p = makeProcessor();
        Run r(*p, bsz);
        r.head.playing = true;
        r.untilPpq(8.0);
        const size_t mark = r.recs.size();
        r.block(nullptr, true);
        r.untilPpq(r.head.ppq + 2.0);
        r.head.playing = false;
        r.blocks(8);
        const int ft = count(r.recs, 0xFC, mark), sp = count(r.recs, 0xF2, mark), fb = count(r.recs, 0xFB, mark);
        if (bsz == 256)
            check(ft == 1 && sp == 0 && fb == 0 && count(r.recs, 0xF8) == (int)std::ceil(r.head.ppq * 24.0 - 1e-6),
                  "256 samples skipped (a quarter of a tick): no transport messages (only the final stop), no tick lost");
        else
            check(ft == 2 && sp == 1 && fb == 1,
                  "2048 samples skipped (two ticks): a locate, 0xFC, 0xF2, 0xFB (then the final stop's 0xFC)");
    }

    std::printf("[the state]\n");
    {
        auto p = makeProcessor();
        p->setHostClockMode(1);
        p->setHostClockOnBar(true);
        juce::MemoryBlock st;
        p->getStateInformation(st);
        auto q = makeProcessor();
        q->setStateInformation(st.getData(), (int)st.getSize());
        check(q->getHostClockMode() == 1 && q->getHostClockOnBar(), "the settings round trip (Clock, Start on the bar line)");

        auto s = makeProcessor();
        juce::ValueTree t = juce::ValueTree::readFromData(st.getData(), st.getSize());
        check(t.hasProperty("hostClock") && t.hasProperty("hostClockOnBar"), "the state carries hostClock and hostClockOnBar");
        t.removeProperty("hostClock", nullptr);
        t.removeProperty("hostClockOnBar", nullptr);
        juce::MemoryBlock old;
        {
            juce::MemoryOutputStream os(old, false);
            t.writeToStream(os);
        }
        s->setHostClockMode(2);                  // (so a failure to reset it shows)
        s->setStateInformation(old.getData(), (int)old.getSize());
        check(s->getHostClockMode() == 0 && !s->getHostClockOnBar(), "a state saved before the setting: Off, as it behaved");
    }

    std::printf("host_clock_plugin_test: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
