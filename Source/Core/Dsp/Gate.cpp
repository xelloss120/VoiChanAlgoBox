#include "Gate.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
float dbToGain (float db) { return std::pow (10.0f, db / 20.0f); }
}

void Gate::prepare (double sampleRate)
{
    sr = sampleRate;

    scHighPass.setCoeffs (BiquadCoeffs::highPass (sr, 200, 0.70710678));
    scLowPass.setCoeffs (BiquadCoeffs::lowPass (sr, 4000, 0.70710678));

    // 判断用の音量は 5ms で追従
    envCoef = static_cast<float> (std::exp (-1.0 / (0.005 * sr)));

    reset();
}

void Gate::reset()
{
    scHighPass.reset();
    scLowPass.reset();
    envSq = 0;
    envDb = -120;
    open = false;
    holdCounter = 0;
    gain = dbToGain (settings.rangeDb);
}

void Gate::process (float* data, int n) noexcept
{
    const float rangeGain = dbToGain (settings.rangeDb);
    const int holdSamples = static_cast<int> (settings.holdMs * 0.001 * sr);
    const float openThresholdDb = settings.thresholdDb;
    const float closeThresholdDb = openThresholdDb - settings.hysteresisDb;

    // アタックとリリースは、倍率が 0 から 1 まで（またはその逆）動く時間として扱う
    const float attackStep = 1.0f / std::max (1.0f, static_cast<float> (settings.attackMs * 0.001 * sr));
    const float releaseStep = 1.0f / std::max (1.0f, static_cast<float> (settings.releaseMs * 0.001 * sr));

    for (int i = 0; i < n; ++i)
    {
        // 判断用の信号
        const float sc = scLowPass.process (scHighPass.process (data[i]));
        envSq = envCoef * envSq + (1 - envCoef) * sc * sc;
        envDb = 10.0f * std::log10 (envSq + 1.0e-12f);

        // 開閉の判断（ヒステリシスとホールド付き）。デノイズとつないでいるときは、声のありそうさ（0.6 で開き、0.4 で閉じ始める）
        const bool external = settings.voiceProbability >= 0;
        const bool above = external ? settings.voiceProbability >= 0.6f : envDb >= openThresholdDb;
        const bool below = external ? settings.voiceProbability < 0.4f : envDb < closeThresholdDb;
        if (above)
        {
            open = true;
            holdCounter = holdSamples;
        }
        else if (open && below)
        {
            if (holdCounter > 0)
                --holdCounter;
            else
                open = false;
        }
        else if (open)
        {
            // しきい値の間（ヒステリシスの帯）にいる間は、ホールドを数え直す
            holdCounter = holdSamples;
        }

        // 目標の倍率。開いている間は 1。閉じている間は、ゲートならレンジ、エキスパンダーならしきい値より下の分を
        // (比率 - 1) 倍した量だけ下げる（ニーの幅でなめらかにつなぐ。下限はレンジ）
        float target = 1.0f;
        if (! open)
            target = settings.expander ? std::clamp (dbToGain (expanderGainDb (envDb, settings)), rangeGain, 1.0f)
                                       : rangeGain;

        // 倍率をなめらかに動かす（上げるときはアタック、下げるときはリリースの速さで、目標まで）
        if (gain < target)
            gain = std::min (target, gain + attackStep);
        else
            gain = std::max (target, gain - releaseStep);

        data[i] *= gain;
    }
}

} // namespace vcab
