#include "ChannelVocoder.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace vcab
{

namespace
{
constexpr double kLowHz = 100, kHighHz = 10000;
constexpr double kEnvelopeSeconds = 0.010;  // 声の包絡をならす時間
// 音源の包絡をならす時間。音源は声ほど速く変わらないので、ゆっくりにする。速いと、狭い帯域での音源の細かい揺れに
// 追従して「大きい瞬間に下げ、小さい瞬間に上げる」ことになり、平均すると音が小さくなる
constexpr double kCarrierEnvelopeSeconds = 0.030;
constexpr double kVoicingSeconds = 0.010;   // 有声音と無声音の切り替えの時間

// polyBLEP: のこぎり波の段差のまわりを滑らかにして、エイリアス（折り返しの雑音）を抑える
double polyBlep (double t, double dt)
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}
} // namespace

void ChannelVocoder::prepare (double sampleRate, int)
{
    sr = sampleRate;
    detector.prepare (sr);
    mixed.prepare (sr);
    pulses.prepare();
    hop = std::max (1, static_cast<int> (std::lround (0.005 * sr)));
    numBands = 0;
    reset();
}

void ChannelVocoder::reset()
{
    detector.reset();
    for (auto& b : bands)
    {
        for (auto* f : { &b.analysis[0], &b.analysis[1], &b.carrier[0], &b.carrier[1] })
            f->reset();
        b.voiceEnv = b.carrierEnv = 0;
    }
    sawPhase = voicedMix = 0;
    intonation.reset();
    pulses.reset();
    mixed.reset();
    sinceHop = 0;
}

void ChannelVocoder::configure (int count, double formantRatio)
{
    // 帯域の数が変わったときと、フォルマントを動かしたときに、フィルタの係数を作り直す（状態は残す）
    const double top = std::min (kHighHz, sr * 0.45);
    const double ratio = std::pow (top / kLowHz, 1.0 / count);
    const double q = std::sqrt (ratio) / (ratio - 1.0); // 帯域の幅（隣の帯域との境目で -3dB ほど）

    for (int i = 0; i < count; ++i)
    {
        auto& b = bands[static_cast<size_t> (i)];
        const double centre = kLowHz * std::pow (ratio, i + 0.5);
        const auto ac = BiquadCoeffs::bandPass (sr, centre, q);
        b.analysis[0].setCoeffs (ac);
        b.analysis[1].setCoeffs (ac);

        // 合成側は、中心をフォルマントの倍率だけずらす（動かし方が「低い側を残す」のときは、低い帯域ほど少なく）
        const double synthCentre = warp.forward (centre);
        b.active = synthCentre < sr * 0.45;
        if (b.active)
        {
            const auto sc = BiquadCoeffs::bandPass (sr, synthCentre, q);
            for (auto* f : { &b.carrier[0], &b.carrier[1] })
                f->setCoeffs (sc);
        }
    }
    // 隣り合う帯域は、境目で位相が逆向きにずれていて、同じ向きで足すと打ち消し合う（平均 -6dB、谷は -15dB 以上）。
    // 1つおきに符号を反転して足すと、ほぼ平らにつながる。残りの差は、全帯域のフィルタ（2段）を位相込みで足した
    // 特性を範囲の中で平均して補う
    double sum = 0;
    constexpr int points = 200;
    for (int p = 0; p < points; ++p)
    {
        const double f = kLowHz * std::pow (top / kLowHz, (p + 0.5) / points);
        const auto z = std::polar (1.0, -2 * kPi * f / sr); // z^-1
        std::complex<double> total;
        for (int i = 0; i < count; ++i)
        {
            const double centre = kLowHz * std::pow (ratio, i + 0.5);
            const auto c = BiquadCoeffs::bandPass (sr, centre, q);
            const auto h = (c.b0 + c.b1 * z + c.b2 * z * z) / (1.0 + c.a1 * z + c.a2 * z * z);
            total += (i % 2 == 0 ? 1.0 : -1.0) * h * h;
        }
        sum += std::norm (total);
    }
    makeup = 1.0 / std::sqrt (std::max (sum / points, 1.0e-6));

    numBands = count;
    configuredFormant = formantRatio;
}

void ChannelVocoder::process (float* data, int n, const ParamValues& params)
{
    const int count = std::clamp (static_cast<int> (params.get (P::cvBands)), 4, kMaxBands);
    const double formantRatio = semitonesToRatio (params.get (P::formant));
    // 帯域の割り当てを決め打ちするので、フォルマントごと（追いかけ）は、帯域ごと（固定の目印）と同じに扱う
    const int warpMode = std::min (std::clamp (params.index (P::formantWarp), 0, 3), static_cast<int> (FormantWarp::bands));
    const double offs[3] = { params.get (P::formantF1), params.get (P::formantF2), params.get (P::formantF3) };
    const bool offsChanged = warpMode >= FormantWarp::bands && (offs[0] != configuredOffs[0] || offs[1] != configuredOffs[1] || offs[2] != configuredOffs[2]);
    if (count != numBands || std::abs (formantRatio - configuredFormant) > 1.0e-6 || warpMode != configuredWarp || offsChanged)
    {
        if (warpMode >= FormantWarp::bands)
            warp.set (formantRatio, warpMode, offs, FormantWarp::kBandHz);
        else
            warp.set (formantRatio, warpMode);
        std::copy (offs, offs + 3, configuredOffs);
        configuredWarp = warpMode;
        configure (count, formantRatio);
    }

    detector.push (data, n, params);
    const auto& est = detector.latest();
    const bool voiced = est.voiced && est.hz > 0;
    const double emphasis = intonation.factor (est.hz, voiced, params.get (P::intonation) * 0.01, n / sr); // 抑揚の強調
    const double f0 = std::min (sr * 0.45, voiced ? est.hz * semitonesToRatio (params.get (P::pitch)) * emphasis : 100.0);
    const double dt = f0 / sr;

    const double envCoeff = 1.0 - std::exp (-1.0 / (kEnvelopeSeconds * sr));
    const double carrierCoeff = 1.0 - std::exp (-1.0 / (kCarrierEnvelopeSeconds * sr));
    const double voiceCoeff = 1.0 - std::exp (-1.0 / (kVoicingSeconds * sr));
    const bool useMixed = params.on (P::mixedExcitation);
    const int shape = params.index (P::glottalShape);

    for (int i = 0; i < n; ++i)
    {
        const double x = data[i];
        double excitation;
        mixed.pushVoice (static_cast<float> (x)); // 切り替えてすぐ使えるように、いつもためておく
        if (useMixed)
        {
            // ---- 音源（混合励振）: パルス列と白色雑音（どちらもパワー 1）を、帯域ごとの倍音らしさで混ぜる ----
            // この方式は音を遅らせないので、倍音らしさは直近の音で測る
            if (++sinceHop >= hop)
            {
                sinceHop = 0;
                mixed.analyze (voiced ? sr / est.hz : 0.0, 0, voiced);
            }
            noiseSeed = noiseSeed * 1664525u + 1013904223u;
            const double unitNoise = (static_cast<double> (noiseSeed >> 8) / 8388608.0 - 1.0) * 1.7320508;
            excitation = mixed.process (pulses.next (dt, shape), unitNoise);
        }
        else
        {
            // ---- 音源: のこぎり波と白色雑音を、有声音の度合いで混ぜる ----
            voicedMix += voiceCoeff * ((voiced ? 1.0 : 0.0) - voicedMix);
            sawPhase += dt;
            if (sawPhase >= 1.0)
                sawPhase -= 1.0;
            // 声帯の波形を選んだときは、のこぎり波の代わりにその波形（パワーをのこぎり波と同じ 1/3 に）
            const double saw = shape == GlottalSource::pulseTrain ? 2.0 * sawPhase - 1.0 - polyBlep (sawPhase, dt)
                                                                  : pulses.next (dt, shape) * 0.57735027;
            noiseSeed = noiseSeed * 1664525u + 1013904223u;
            const double noise = (static_cast<double> (noiseSeed >> 8) / 8388608.0 - 1.0) * 1.7; // 一様乱数。のこぎり波と同じくらいの大きさ
            excitation = voicedMix * saw + (1.0 - voicedMix) * noise;
        }

        // ---- 帯域ごとに、声の包絡を音源に乗せる ----
        double out = 0;
        for (int b = 0; b < numBands; ++b)
        {
            auto& band = bands[static_cast<size_t> (b)];
            const double v = band.analysis[1].process (static_cast<float> (band.analysis[0].process (static_cast<float> (x))));
            band.voiceEnv += envCoeff * (v * v - band.voiceEnv);
            if (! band.active)
                continue;

            const double c = band.carrier[1].process (band.carrier[0].process (static_cast<float> (excitation)));
            band.carrierEnv += carrierCoeff * (c * c - band.carrierEnv);
            const double gain = std::sqrt (band.voiceEnv / (band.carrierEnv + 1.0e-12));
            out += (b % 2 == 0 ? c : -c) * std::min (gain, 1000.0); // 1つおきに符号を反転（configure を参照）
        }
        data[i] = static_cast<float> (out * makeup);
    }
}

int ChannelVocoder::latencySamples (const ParamValues&) const
{
    // バンドパスの遅れ（低い帯域ほど大きい）と包絡をならす遅れの、おおよその平均（合成音での実測で約 5ms）
    return static_cast<int> (0.005 * sr);
}

} // namespace vcab
