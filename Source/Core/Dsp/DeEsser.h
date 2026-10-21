// ディエッサー: 高い帯域だけを下げる方式（声全体は下げない）。先読みなし（遅延 0）。
//
// 音を「周波数より下」と「上」に分け（上 = 元の音 - 下。足すと元に戻る）、
// 上の帯域の音量がしきい値を超えた分だけ、上の帯域を下げる（最大の削減量まで）。
#pragma once

#include "Biquad.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

class DeEsser
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        envCoef = static_cast<float> (std::exp (-1.0 / (0.005 * sr)));   // 判断用の音量は 5ms でならす
        attackCoef = static_cast<float> (std::exp (-1.0 / (0.001 * sr))); // 下げ始めは 1ms
        releaseCoef = static_cast<float> (std::exp (-1.0 / (0.06 * sr))); // 戻りは 60ms
        lastFreq = -1;
        reset();
    }

    void reset()
    {
        split.reset();
        detect.reset();
        envSq = 0;
        grDb = 0;
        maxGr = 0;
    }

    void setParams (float freq, float thresholdDb, float maxReductionDb)
    {
        freq = std::clamp (freq, 1000.0f, static_cast<float> (sr * 0.45));
        if (freq != lastFreq)
        {
            lastFreq = freq;
            split.setCoeffs (BiquadCoeffs::lowPass (sr, freq, 0.70710678));
            detect.setCoeffs (BiquadCoeffs::highPass (sr, freq, 0.70710678));
        }
        threshold = thresholdDb;
        maxReduction = maxReductionDb;
    }

    void process (float* data, int n) noexcept
    {
        maxGr = 0;
        for (int i = 0; i < n; ++i)
        {
            const float x = data[i];

            const float sc = detect.process (x);
            envSq = envCoef * envSq + (1 - envCoef) * sc * sc;
            const float levelDb = 10.0f * std::log10 (envSq + 1.0e-12f);

            const float target = -std::clamp (levelDb - threshold, 0.0f, maxReduction);
            const float coef = target < grDb ? attackCoef : releaseCoef;
            grDb = coef * grDb + (1 - coef) * target;

            const float low = split.process (x);
            const float high = x - low;
            data[i] = low + high * std::pow (10.0f, grDb / 20.0f);

            maxGr = std::min (maxGr, grDb);
        }
    }

    float gainReductionDb() const noexcept { return maxGr; } // 表示用（0 以下）

private:
    double sr = 48000;
    Biquad split, detect;
    float lastFreq = -1, threshold = -30, maxReduction = 10;
    float envSq = 0, envCoef = 0, attackCoef = 0, releaseCoef = 0;
    float grDb = 0, maxGr = 0;
};

} // namespace vcab
