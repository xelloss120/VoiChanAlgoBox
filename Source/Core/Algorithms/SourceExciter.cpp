#include "SourceExciter.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr double kVoicingSeconds = 0.010; // 有声音と無声音の切り替えの時間
constexpr double kHopSeconds = 0.005;     // 混合励振の倍音らしさを測り直す間隔
} // namespace

void SourceExciter::prepare (double sampleRate, int)
{
    sr = sampleRate;
    detector.prepare (sr);
    mixed.prepare (sr);
    pulses.prepare();
    hop = std::max (1, static_cast<int> (std::lround (kHopSeconds * sr)));
    reset();
}

void SourceExciter::reset()
{
    detector.reset();
    mixed.reset();
    pulses.reset();
    intonation.reset();
    voicedMix = 0;
    lastHz = 100;
    sinceHop = 0;
}

int SourceExciter::latencySamples (const ParamValues& params) const
{
    return PitchDetector::analysisDelayFor (sr, params);
}

void SourceExciter::process (float* data, int n, const ParamValues& params)
{
    const float* voice = detectionSource != nullptr ? detectionSource : data;
    detectionSource = nullptr;
    detector.push (voice, n, params);

    // 声の高さは、今から遅延の分だけ前の時刻のもの。作る音も、その時刻の音として扱われる
    const auto& est = detector.latest();
    const bool voiced = est.voiced && est.hz > 0;
    if (voiced)
        lastHz = est.hz;
    const double emphasis = intonation.factor (est.hz, voiced, params.get (P::intonation) * 0.01, n / sr); // 抑揚の強調
    const double f0 = std::clamp (lastHz * semitonesToRatio (params.get (P::pitch)) * emphasis, 20.0, sr * 0.45);
    const double dt = f0 / sr;
    const double voiceCoeff = 1.0 - std::exp (-1.0 / (kVoicingSeconds * sr));
    const bool useMixed = params.on (P::mixedExcitation);
    const int delay = detector.analysisDelaySamples();
    const int shape = params.index (P::glottalShape);

    for (int i = 0; i < n; ++i)
    {
        const double pulse = pulses.next (dt, shape);
        seed = seed * 1664525u + 1013904223u;
        const double noise = (static_cast<double> (seed >> 8) / 8388608.0 - 1.0) * 1.7320508;

        mixed.pushVoice (voice[i]); // 切り替えてすぐ使えるように、いつもためておく
        if (useMixed)
        {
            // 混合励振: 元の声の帯域ごとの倍音らしさ（声の高さと同じ時刻で測る）で、帯域ごとに混ぜる
            if (++sinceHop >= hop)
            {
                sinceHop = 0;
                mixed.analyze (voiced ? sr / est.hz : 0.0, delay, voiced);
            }
            data[i] = static_cast<float> (mixed.process (pulse, noise));
        }
        else
        {
            // 有声か無声かで切り替える。2つは無関係な音なので、パワーが 1 のままになるように、平方根で混ぜる
            voicedMix += voiceCoeff * ((voiced ? 1.0 : 0.0) - voicedMix);
            data[i] = static_cast<float> (std::sqrt (voicedMix) * pulse + std::sqrt (1.0 - voicedMix) * noise);
        }
    }
}

} // namespace vcab
