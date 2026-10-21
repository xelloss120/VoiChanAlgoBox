// リバーブ（残響）。Jezar の Freeverb（パブリックドメイン）の形をモノラルで作る。
//
// 8本の櫛形フィルタ（遅れた音を減らしながら何度も戻す。戻すたびに高い音を少し削る）を並べて足し、
// 4本の全域通過フィルタ（音色を変えずに、音を細かくばらまく）に直列に通す。櫛形フィルタの遅れの長さを
// 互いに割り切れない値にして、響きが特定の音程に聞こえないようにしてある。
//   部屋の大きさ  櫛形フィルタで戻す割合（大きいほど響きが長く続く）
//   高域の減衰    戻すたびに高い音を削る量（大きいほど、響きがこもって柔らかくなる）
//   プリディレイ  元の音から響きが始まるまでの間（部屋の広さの感じ）
// 元の音はそのまま残し、響きを「混ぜる量」だけ足す。
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace vcab
{

class Reverb
{
public:
    struct Settings
    {
        double size = 0.5;       // 0〜1
        double damping = 0.5;    // 0〜1
        double preDelayMs = 20;
        double mix = 0.2;        // 響きを足す量（0〜1）
    };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        // 元の値は 44.1kHz のときのサンプル数。サンプルレートに合わせて伸ばす
        constexpr int combTuning[] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        constexpr int allpassTuning[] = { 556, 441, 341, 225 };
        const double scale = sr / 44100.0;
        for (size_t i = 0; i < combs.size(); ++i)
            combs[i].buffer.assign (static_cast<size_t> (combTuning[i] * scale), 0.0f);
        for (size_t i = 0; i < allpasses.size(); ++i)
            allpasses[i].buffer.assign (static_cast<size_t> (allpassTuning[i] * scale), 0.0f);
        preDelay.assign (static_cast<size_t> (sr * 0.11) + 2, 0.0f);
        reset();
    }

    void reset()
    {
        for (auto& c : combs)
        {
            std::fill (c.buffer.begin(), c.buffer.end(), 0.0f);
            c.pos = 0;
            c.store = 0;
        }
        for (auto& a : allpasses)
        {
            std::fill (a.buffer.begin(), a.buffer.end(), 0.0f);
            a.pos = 0;
        }
        std::fill (preDelay.begin(), preDelay.end(), 0.0f);
        prePos = 0;
    }

    void process (float* data, int n, const Settings& s)
    {
        // Freeverb の定数（部屋の大きさ → 戻す割合 0.7〜0.98、高域の減衰 → 0〜0.4、入力の倍率、出力の倍率）
        const double feedback = 0.7 + 0.28 * std::clamp (s.size, 0.0, 1.0);
        const double damp = 0.4 * std::clamp (s.damping, 0.0, 1.0);
        constexpr double inputGain = 0.015, wetScale = 3.0;
        const auto preSize = static_cast<int> (preDelay.size());
        const int preSamples = std::clamp (static_cast<int> (s.preDelayMs * 0.001 * sr), 0, preSize - 1);

        for (int i = 0; i < n; ++i)
        {
            preDelay[static_cast<size_t> (prePos)] = data[i];
            const float delayedIn = preDelay[static_cast<size_t> ((prePos - preSamples + preSize) % preSize)];
            prePos = (prePos + 1) % preSize;

            const double in = delayedIn * inputGain;
            double out = 0;
            for (auto& c : combs)
            {
                const float y = c.buffer[static_cast<size_t> (c.pos)];
                c.store = y * (1 - damp) + c.store * damp;
                c.buffer[static_cast<size_t> (c.pos)] = static_cast<float> (in + c.store * feedback);
                c.pos = (c.pos + 1) % static_cast<int> (c.buffer.size());
                out += y;
            }
            for (auto& a : allpasses)
            {
                const float b = a.buffer[static_cast<size_t> (a.pos)];
                const double y = -out + b;
                a.buffer[static_cast<size_t> (a.pos)] = static_cast<float> (out + b * 0.5);
                a.pos = (a.pos + 1) % static_cast<int> (a.buffer.size());
                out = y;
            }
            data[i] = static_cast<float> (data[i] + s.mix * wetScale * out);
        }
    }

private:
    struct Comb
    {
        std::vector<float> buffer;
        int pos = 0;
        double store = 0; // 高域を削るための、1つ前の値
    };
    struct Allpass
    {
        std::vector<float> buffer;
        int pos = 0;
    };

    double sr = 48000;
    std::array<Comb, 8> combs;
    std::array<Allpass, 4> allpasses;
    std::vector<float> preDelay;
    int prePos = 0;
};

} // namespace vcab
