// SPDX-License-Identifier: GPL-3.0-only
// The host's transport as MIDI clock for the core: see HostClock.h.
#include "HostClock.h"

#include <algorithm>
#include <cmath>

namespace hostclock {

namespace {

constexpr double kTickEps = 1e-6;              // in ticks: a position this close under a tick is on it
constexpr double kJump = 1.0 / 48.0;           // quarter notes: half a tick; a position further off the expected one is a locate
constexpr double kZero = 1e-6;                 // quarter notes: the song's start
constexpr uint8_t kClock = 0xF8, kStart = 0xFA, kContinue = 0xFB, kStop = 0xFC, kSpp = 0xF2;

// the first tick at or after a position
int64_t tickAtOrAfter(double ppq) { return (int64_t)std::ceil(ppq * 24.0 - kTickEps); }

}   // namespace

void HostClock::resync()
{
    running_ = false;
    started_ = false;
    pend_ = 0;
    haveStop_ = false;
}

int HostClock::process(const Transport &t, int n, double sampleRate, Mode mode, bool startOnBar, bool externalClock,
                       Event *out, int cap)
{
    int cnt = 0;
    auto put = [&](int off, uint8_t a, int len, uint8_t b = 0, uint8_t c = 0) {
        if (cnt < cap) {
            out[cnt].offset = off;
            out[cnt].len = len;
            out[cnt].b[0] = a;
            out[cnt].b[1] = b;
            out[cnt].b[2] = c;
            cnt++;
        }
    };

    if (externalClock) {                         // the host's own clock: ours stays out of the way for a second
        extHold_ = sampleRate;
        resync();
        return 0;
    }
    if (extHold_ > 0) {
        extHold_ -= n;
        resync();
        return 0;
    }
    if (mode != lastMode_) {                     // the setting changed: the core is told it stops, the rest starts afresh
        const bool announced = started_ && lastMode_ == Mode::ClockTransport;
        resync();
        lastMode_ = mode;
        if (announced)
            put(0, kStop, 1);
    }
    if (mode == Mode::Off)
        return cnt;

    const double bpm = t.bpm;
    bool have = t.valid && t.hasBpm && bpm > 0.0 && n > 0 && sampleRate > 0.0;
    double ppq0 = 0.0;
    if (have) {
        if (t.hasPpq)
            ppq0 = t.ppq;
        else if (t.hasSamples)
            ppq0 = (double)t.timeInSamples * bpm / (60.0 * sampleRate);
        else
            have = false;
    }
    if (!have) {                                 // nothing to follow: whatever starts next starts afresh
        resync();
        return cnt;
    }

    const bool transport = mode == Mode::ClockTransport;
    if (!t.playing) {
        if (running_) {
            if (transport && started_)           // (a start still waiting for its tick never reached the core: no stop)
                put(0, kStop, 1);
            haveStop_ = started_;
            stopPpq_ = ppq0;
        }
        running_ = false;
        started_ = false;
        pend_ = 0;
        return cnt;
    }

    const double barLen = (t.num > 0 && t.den > 0) ? 4.0 * t.num / t.den : 4.0;
    const double barStart = t.hasBarStart ? t.barStartPpq : 0.0;

    bool begin = !running_, locate = false;
    if (running_ && std::fabs(ppq0 - expect_) > kJump) {
        begin = true;
        locate = true;
    }
    if (begin) {
        pend_ = 0;
        if (!locate)
            started_ = false;
        const int64_t first = std::max<int64_t>(0, tickAtOrAfter(ppq0));
        lastTick_ = first - 1;
        if (transport) {
            if (ppq0 <= kZero) {                 // the song's start (or a count-in before it)
                pend_ = kStart;
                pendTick_ = 0;
            } else if (!locate && haveStop_ && std::fabs(ppq0 - stopPpq_) < kJump) {
                pend_ = kContinue;               // a pause resumed where it stopped
                pendTick_ = first;
            } else if (startOnBar) {
                const double d = ppq0 - expect_;
                const double bars = d / barLen;
                const bool whole = locate && std::fabs(bars - std::round(bars)) * barLen < kJump;
                if (!whole) {                    // (a jump of whole bars leaves the core's own bar count right)
                    if (locate)
                        put(0, kStop, 1);
                    started_ = false;
                    const double k = std::ceil((ppq0 - barStart) / barLen - kTickEps / 24.0);
                    pend_ = kStart;
                    pendTick_ = std::max<int64_t>(0, tickAtOrAfter(barStart + k * barLen));
                }
            } else {
                if (locate) {                    // (a song position is for a sequencer that is stopped: stop, position, continue)
                    put(0, kStop, 1);
                    started_ = false;
                }
                const int64_t s = (int64_t)std::ceil(ppq0 * 4.0 - kTickEps / 6.0);   // the next sixteenth
                put(0, kSpp, 3, (uint8_t)(s & 0x7F), (uint8_t)((s >> 7) & 0x7F));
                pend_ = kContinue;
                pendTick_ = 6 * s;
            }
        }
        running_ = true;
    }
    if (pend_ && pendTick_ <= lastTick_) {       // (its tick has gone by: now)
        put(0, (uint8_t)pend_, 1);
        pend_ = 0;
        started_ = true;
    }

    const double ppq1 = ppq0 + (double)n * bpm / (60.0 * sampleRate);
    const double samplesPerPpq = 60.0 * sampleRate / bpm;
    for (int64_t k = lastTick_ + 1; (double)k / 24.0 < ppq1 - kTickEps / 24.0 && cnt + 2 <= cap; k++) {
        const double at = ((double)k / 24.0 - ppq0) * samplesPerPpq;
        const int off = (int)std::min<double>(n - 1, std::max<double>(0.0, std::floor(at + 0.5)));
        if (pend_ && k >= pendTick_) {           // (start / continue and the tick it falls on: the message first)
            put(off, (uint8_t)pend_, 1);
            pend_ = 0;
            started_ = true;
        }
        put(off, kClock, 1);
        lastTick_ = k;
    }
    lastTick_ = std::max(lastTick_, tickAtOrAfter(ppq1) - 1);   // (a block too long for cap: the rest is dropped, not late)
    expect_ = ppq1;
    return cnt;
}

}   // namespace hostclock
