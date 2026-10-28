// SPDX-License-Identifier: GPL-3.0-only
// The host's AudioPlayHead as the HostClock.h Transport (what the host says about this block's first sample).
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "HostClock.h"

namespace hostclock {

inline Transport fromPlayHead(juce::AudioPlayHead *ph)
{
    Transport t;
    if (ph == nullptr)
        return t;                                // (the Standalone, a host without one: nothing to follow)
    const auto pos = ph->getPosition();
    if (!pos)
        return t;
    t.valid = true;
    t.playing = pos->getIsPlaying() || pos->getIsRecording();
    if (const auto bpm = pos->getBpm()) {
        t.hasBpm = true;
        t.bpm = *bpm;
    }
    if (const auto ppq = pos->getPpqPosition()) {
        t.hasPpq = true;
        t.ppq = *ppq;
    }
    if (const auto bar = pos->getPpqPositionOfLastBarStart()) {
        t.hasBarStart = true;
        t.barStartPpq = *bar;
    }
    if (const auto ts = pos->getTimeSignature()) {
        t.num = ts->numerator;
        t.den = ts->denominator;
    }
    if (const auto samples = pos->getTimeInSamples()) {
        t.hasSamples = true;
        t.timeInSamples = *samples;
    }
    return t;
}

}   // namespace hostclock
