// ボコーダの音源（声帯の音の代わり）を作る部品。LPC ボコーダ、ケプストラムボコーダ、チャンネルボコーダで共有する。
//
// パルス列: のこぎり波（段差を polyBLEP でなめらかにしたもの）の差分。段差のところだけが残る。倍音が同じ大きさに
//           並ぶ音で、平均のパワーは 1。
// 混合励振: 実際の声は、低い帯域は倍音がはっきりしていて、高い帯域ほど息（雑音）が混ざる。有声か無声かの二択で
//           パルス列と雑音を切り替えると、その中間がブザーか雑音のどちらかに倒れてしまう。そこで、音源を 5 つの帯域
//           （500 / 1000 / 2000 / 4000Hz で区切る。MELP というボコーダと同じ）に分け、帯域ごとに、元の声のその帯域が
//           どれだけ「1 周期ずらすとよく重なる」か（倍音らしさ）に応じて、パルス列と雑音を混ぜる。
//           帯域は「上の区切りの低域通過 − 下の区切りの低域通過」で分けるので、全部の帯域を足すと、フィルタの位相に
//           関係なく、ちょうど元に戻る（パルス列だけ、雑音だけなら、分けない場合と同じ音になる）。
#pragma once

#include "Biquad.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vcab
{

// パルス列（平均のパワー 1）
class PulseTrain
{
public:
    void reset() { phase = prevSaw = 0; }

    // dt: 1 サンプルあたりに進む周期の割合（声の高さ ÷ サンプルレート）
    double next (double dt)
    {
        phase += dt;
        if (phase >= 1.0)
            phase -= 1.0;
        const double saw = 2.0 * phase - 1.0 - polyBlep (phase, dt);
        const double pulse = -((saw - prevSaw) - 2.0 * dt) * 0.5 / std::sqrt (dt);
        prevSaw = saw;
        return pulse;
    }

    static double polyBlep (double t, double dt)
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

private:
    double phase = 0, prevSaw = 0;
};

class MixedExcitation
{
public:
    static constexpr int kBands = 5;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        static const double edges[kBands - 1] = { 500, 1000, 2000, 4000 };
        for (int k = 0; k < kBands - 1; ++k)
        {
            const double f = std::min (edges[k], sr * 0.45);
            for (auto* set : { &voiceSplit, &pulseSplit, &noiseSplit })
            {
                (*set)[static_cast<size_t> (k)][0].setCoeffs (BiquadCoeffs::lowPass (sr, f, 0.5412));
                (*set)[static_cast<size_t> (k)][1].setCoeffs (BiquadCoeffs::lowPass (sr, f, 1.3066));
            }
        }
        int size = 1;
        while (size < static_cast<int> (sr * 0.25)) // 一番長い周期 + 窓 + ピッチ検出の遅れ（最大 0.15 秒ほど）より長く
            size <<= 1;
        for (auto& r : rings)
            r.assign (static_cast<size_t> (size), 0.0f);
        mask = size - 1;
        smoothCoeff = 1.0 - std::exp (-1.0 / (0.010 * sr));
        reset();
    }

    void reset()
    {
        for (auto* set : { &voiceSplit, &pulseSplit, &noiseSplit })
            for (auto& f : *set)
            {
                f[0].reset();
                f[1].reset();
            }
        for (auto& r : rings)
            std::fill (r.begin(), r.end(), 0.0f);
        now = 0;
        target.fill (0.0);
        mix.fill (0.0);
    }

    // 元の声を 1 サンプル渡す（帯域に分けて、倍音らしさを測るためにためておく）
    void pushVoice (float x)
    {
        double bands[kBands];
        split (voiceSplit, x, bands);
        for (int b = 0; b < kBands; ++b)
            rings[static_cast<size_t> (b)][static_cast<size_t> (now & mask)] = static_cast<float> (bands[b]);
        ++now;
    }

    // 帯域ごとの倍音らしさを測り直す。period: 元の声の周期（サンプル数）。centreBack: 測る時刻が、今から何サンプル前か
    void analyze (double period, int centreBack, bool voiced)
    {
        if (! voiced || period < 2)
        {
            target.fill (0.0);
            return;
        }
        const int lag = static_cast<int> (std::lround (period));
        const int len = std::max (2 * lag, static_cast<int> (0.010 * sr)); // 2 周期か 10ms の長い方
        const std::int64_t end = std::min (now, now - centreBack + len / 2);
        const std::int64_t start = end - len;
        if (start - lag - 1 < now - (mask + 1))
            return;
        for (int b = 0; b < kBands; ++b)
        {
            const auto& r = rings[static_cast<size_t> (b)];
            double best = 0;
            for (int l = lag - 1; l <= lag + 1; ++l) // 周期の端数のずれを、前後 1 サンプルで吸収する
            {
                double c = 0, e0 = 0, e1 = 0;
                for (std::int64_t t = start; t < end; ++t)
                {
                    const double x0 = r[static_cast<size_t> (t & mask)], x1 = r[static_cast<size_t> ((t - l) & mask)];
                    c += x0 * x1;
                    e0 += x0 * x0;
                    e1 += x1 * x1;
                }
                best = std::max (best, c / std::sqrt (e0 * e1 + 1.0e-20));
            }
            // 相関 0.3 以下は雑音だけ、0.8 以上はパルス列だけ。その間はなめらかに混ぜる
            target[static_cast<size_t> (b)] = std::clamp ((best - 0.3) / 0.5, 0.0, 1.0);
        }
    }

    // パルス列と雑音（どちらもパワー 1）を、帯域ごとの倍音らしさで混ぜる
    double process (double pulse, double noise)
    {
        double p[kBands], n[kBands];
        split (pulseSplit, pulse, p);
        split (noiseSplit, noise, n);
        double out = 0;
        for (int b = 0; b < kBands; ++b)
        {
            auto& m = mix[static_cast<size_t> (b)];
            m += smoothCoeff * (target[static_cast<size_t> (b)] - m);
            out += std::sqrt (m) * p[b] + std::sqrt (1.0 - m) * n[b];
        }
        return out;
    }

    // 画面などに出すため（0〜1）
    double bandVoicing (int band) const { return mix[static_cast<size_t> (band)]; }

private:
    using Split = std::array<std::array<Biquad, 2>, kBands - 1>;

    static void split (Split& lp, double x, double* bands)
    {
        // 低域通過を区切りごとにかけて、隣どうしの差を帯域にする（足すと x に戻る）
        double below = 0;
        for (int k = 0; k < kBands - 1; ++k)
        {
            const double l = lp[static_cast<size_t> (k)][1].process (lp[static_cast<size_t> (k)][0].process (static_cast<float> (x)));
            bands[k] = l - below;
            below = l;
        }
        bands[kBands - 1] = x - below;
    }

    double sr = 48000, smoothCoeff = 0.002;
    Split voiceSplit {}, pulseSplit {}, noiseSplit {};
    std::array<std::vector<float>, kBands> rings;
    std::int64_t now = 0, mask = 0;
    std::array<double, kBands> target {}, mix {};
};

} // namespace vcab
