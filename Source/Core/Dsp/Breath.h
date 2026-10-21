// ブレスの付加（どのボイチェンの方式にも効く後段）
//
// 1. 声（ボイチェンの出力）から、5ms ごとに LPC で声色の形（包絡）と大きさを求める。
// 2. 白色雑音を、その声色のフィルタ（格子型の合成フィルタ）に通す。声と同じ母音の「ささやき声」になる。
//    大きさも、声の大きさに合わせて動く（LPC の予測の誤差から求める）。
// 3. ささやき声の高い側（「明るさ」の周波数より上）だけを、声に「量」だけ足す。
// 遅延は増えない。
#pragma once

#include "Biquad.h"
#include "Lpc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vcab
{

class Breath
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        hop = std::max (1, static_cast<int> (std::lround (0.005 * sr)));
        window = static_cast<int> (std::lround (0.020 * sr));
        history.assign (static_cast<size_t> (window), 0.0f);
        hann.resize (static_cast<size_t> (window));
        double sumSq = 0;
        for (int i = 0; i < window; ++i)
        {
            hann[static_cast<size_t> (i)] = 0.5 - 0.5 * std::cos (2 * kPi * (i + 0.5) / window);
            sumSq += hann[static_cast<size_t> (i)] * hann[static_cast<size_t> (i)];
        }
        windowPower = sumSq;
        reset();
    }

    void reset()
    {
        std::fill (history.begin(), history.end(), 0.0f);
        historyPos = sinceHop = 0;
        state.fill (0.0);
        prev = next = {};
        step = hop;
        for (auto& f : highPass)
            f.reset();
    }

    // amount: 0〜1、toneHz: 息を足し始める周波数
    void process (float* data, int n, double amount, double toneHz)
    {
        if (toneHz != currentTone)
        {
            currentTone = toneHz;
            highPass[0].setCoeffs (BiquadCoeffs::highPass (sr, toneHz, 0.5412));
            highPass[1].setCoeffs (BiquadCoeffs::highPass (sr, toneHz, 1.3066));
        }

        for (int i = 0; i < n; ++i)
        {
            const float x = data[i];
            history[static_cast<size_t> (historyPos)] = x;
            historyPos = (historyPos + 1) % window;
            if (++sinceHop >= hop)
            {
                sinceHop = 0;
                analyze();
            }

            // 前の係数から次の係数へ、hop の間に少しずつ近づける（切り替えのザラつきを防ぐ）
            const double t = std::min (1.0, static_cast<double> (++step) / hop);
            Coeffs now;
            for (int k = 1; k <= kOrder; ++k)
                now.k[k] = prev.k[k] + (next.k[k] - prev.k[k]) * t;
            const double gain = prev.gain + (next.gain - prev.gain) * t;

            seed = seed * 1664525u + 1013904223u;
            const double noise = (static_cast<double> (seed >> 8) / 8388608.0 - 1.0) * 1.7320508; // 一様乱数で分散 1
            double whisper = lpc::synthesisLattice (noise * gain, now.k, kOrder, state.data());
            whisper = highPass[1].process (highPass[0].process (static_cast<float> (whisper)));
            data[i] = static_cast<float> (x + amount * whisper);
        }
    }

private:
    static constexpr int kOrder = 16;

    struct Coeffs
    {
        double k[kOrder + 1] {};
        double gain = 0;
    };

    void analyze()
    {
        double frame[4096];
        const int len = std::min (window, 4096);
        for (int i = 0; i < len; ++i)
            frame[i] = history[static_cast<size_t> ((historyPos + i) % window)] * hann[static_cast<size_t> (i)];
        double r[kOrder + 1];
        for (int lag = 0; lag <= kOrder; ++lag)
        {
            double s = 0;
            for (int i = lag; i < len; ++i)
                s += frame[i] * frame[i - lag];
            r[lag] = s;
        }
        r[0] = r[0] * (1.0 + 1.0e-6) + 1.0e-12;
        double a[kOrder + 1], tmp[kOrder + 1];
        Coeffs c;
        const double err = lpc::levinson (r, kOrder, a, c.k, tmp);
        // 予測の誤差（窓をかけた区間の合計）を、1サンプルあたりの大きさに直す → 雑音にかける大きさ
        c.gain = std::sqrt (std::max (err, 0.0) / windowPower);
        prev = next;
        next = c;
        step = 0;
    }

    double sr = 48000, windowPower = 1, currentTone = -1;
    int hop = 240, window = 960, historyPos = 0, sinceHop = 0, step = 0;
    std::vector<float> history;
    std::vector<double> hann;
    std::array<double, kOrder> state {};
    Coeffs prev, next;
    Biquad highPass[2];
    std::uint32_t seed = 7;
};

} // namespace vcab
