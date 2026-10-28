// SPDX-License-Identifier: GPL-3.0-only
// The host's transport as MIDI clock for the core (FM1-VST-PLAN.md 4.6: "MIDI Clock = In follows Live's clock, which the
// plugin generates from the host transport"). From the host's tempo and PPQ position, once per audio block, it makes the
// system real-time messages a firmware's MIDI clock input follows, each at the sample it falls on:
//   0xF8  timing clock, 24 per quarter note. Every tick sits at its own PPQ (tick k at k/24) and every block starts again
//         from the host's position, so nothing is counted forward and the clock cannot drift from the host.
//   0xFA  start, 0xFB continue, 0xFC stop (Mode ClockTransport), and 0xF2 song position (in sixteenths) before a continue.
// Where a start lands (the transport starts, is moved, or the core was just switched or booted):
//   - at the song's start: 0xFA with the tick at PPQ 0 (a count-in's negative PPQ waits for it)
//   - after a pause, on the same position: 0xFB on the next tick, no position
//   - elsewhere: 0xF2 for the next sixteenth, then 0xFB on that sixteenth's tick (a start is never between sixteenths)
//   - startOnBar: 0xFA on the next bar line instead. Not every core reads 0xF2 (ChoralRoot and fm1-x0x, for one, take
//     only 0xF8 and 0xFA-0xFC), and for those a start in the middle of a bar would run out of step with the host's bar.
//     A bar is numerator * 4 / denominator quarter notes, counted from the host's last bar start.
// The transport moves while it plays (a locate, a loop): Clock re-times only; ClockTransport sends 0xFC, 0xF2 and 0xFB on the
// next sixteenth (a song position is for a sequencer that is stopped), or with startOnBar 0xFC and 0xFA on the next bar
// line, nothing at all if the jump is whole bars (the core's own bar count is still right). A clock that already arrives in the host's MIDI (the track's input) wins: this one is
// silent for a second after the last such message and starts again as a fresh start.
// Without a tempo and a position (the Standalone, a host that gives none) there is nothing to say: no messages.
// Pure code, no JUCE: tests/host_clock_test.cpp runs it on a simulated playhead.
#pragma once
#include <cstdint>

namespace hostclock {

enum class Mode : int {
    Off = 0,             // no messages
    Clock = 1,           // 0xF8 while the transport plays, nothing else
    ClockTransport = 2,  // 0xF8, and start / continue / stop and the song position
};

constexpr int kMaxEvents = 64;     // the most one block hands back (more is dropped: only a block of hundreds of ms)

// what the host's AudioPlayHead says about this block's first sample
struct Transport {
    bool valid = false;            // the host gave a position at all
    bool playing = false;          // playing or recording
    bool hasBpm = false;
    double bpm = 120.0;
    bool hasPpq = false;
    double ppq = 0.0;              // quarter notes from the song's start; negative in a count-in
    bool hasBarStart = false;
    double barStartPpq = 0.0;      // PPQ of the bar line at or before ppq
    int num = 4, den = 4;          // time signature
    bool hasSamples = false;
    int64_t timeInSamples = 0;     // used for the position when the host gives no PPQ (a steady tempo only)
};

struct Event {
    int offset = 0;                // sample in the block, 0 .. n - 1, never less than the event before
    int len = 0;                   // 1 or 3
    uint8_t b[3] = {0, 0, 0};
};

class HostClock {
public:
    // One block of n samples: the events for it into out (at most cap), their number returned. externalClock: the host's
    // own MIDI for this block carries a clock message (0xF8 .. 0xFC). Audio thread; no allocation.
    int process(const Transport &t, int n, double sampleRate, Mode mode, bool startOnBar, bool externalClock, Event *out,
                int cap);

    // The core behind it is new (a switch, a boot) or the clock was unavailable: the next playing block is a fresh start.
    void resync();

private:
    bool running_ = false;         // a block has played since the last (re)start
    bool started_ = false;         // the core has been sent its start / continue (a pause before that is not a pause)
    double expect_ = 0.0;          // the PPQ the next block should start at
    int64_t lastTick_ = -1;        // the last tick given out
    int pend_ = 0;                 // 0: none, else the status byte (0xFA / 0xFB) waiting for tick pendTick_
    int64_t pendTick_ = 0;
    bool haveStop_ = false;
    double stopPpq_ = 0.0;         // where the transport stopped
    Mode lastMode_ = Mode::Off;
    double extHold_ = 0.0;         // samples left of the silence after an external clock
};

}   // namespace hostclock
