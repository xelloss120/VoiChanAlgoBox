#include "Limiter.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

void Limiter::prepare (double sampleRate)
{
    sr = sampleRate;
    lookahead = std::max (1, static_cast<int> (std::lround (0.001 * sr)));
    delay.assign (static_cast<size_t> (lookahead), 0.0f);
    required.assign (static_cast<size_t> (lookahead + 1), 1.0f);
    held.assign (static_cast<size_t> (lookahead), 1.0f);
    reset();
}

void Limiter::reset()
{
    std::fill (delay.begin(), delay.end(), 0.0f);
    std::fill (required.begin(), required.end(), 1.0f);
    std::fill (held.begin(), held.end(), 1.0f);
    heldSum = lookahead;
    pos = 0;
    env = 1;
    grDb = 0;
}

void Limiter::setParams (float ceilingDb, float releaseMs) noexcept
{
    ceiling = std::pow (10.0f, ceilingDb / 20.0f);
    releaseCoef = static_cast<float> (std::exp (-1.0 / (std::max (1.0f, releaseMs) * 0.001 * sr)));
}

void Limiter::process (float* data, int n) noexcept
{
    // 仕組み:
    //  1. 入ってきた音のピークに対して「必要な倍率」を出す
    //  2. 直近 lookahead+1 サンプルの最小値をとる（ピークの手前から下げ始めるため）
    //  3. それを lookahead サンプルの移動平均でなめらかにする。
    //     ピークが遅延から出てくる時点では、平均の中身がすべてピークの倍率以下になっているので、確実に止まる
    //  4. 戻るときはリリースの時間でゆっくり戻す
    const int L = lookahead;
    const auto reqSize = static_cast<int> (required.size());
    float minGain = 1;

    for (int i = 0; i < n; ++i)
    {
        const float x = data[i];
        const float ax = std::abs (x);
        const float req = ax > ceiling ? ceiling / ax : 1.0f;

        required[static_cast<size_t> (pos % reqSize)] = req;

        float m = 1;
        for (float r : required)
            m = std::min (m, r);

        const auto hIdx = static_cast<size_t> (pos % L);
        heldSum += m - held[hIdx];
        held[hIdx] = m;
        const float smooth = static_cast<float> (heldSum / L);

        if (smooth < env)
            env = smooth;
        else
            env = releaseCoef * env + (1 - releaseCoef) * smooth;

        const auto dIdx = static_cast<size_t> (pos % L);
        const float delayed = delay[dIdx];
        delay[dIdx] = x;

        float y = delayed * env;
        y = std::clamp (y, -ceiling, ceiling); // 最後の安全装置
        data[i] = y;

        minGain = std::min (minGain, env);
        pos = (pos + 1) % (L * reqSize); // L と reqSize の両方で割り切れる周期で回す
    }

    grDb = 20.0f * std::log10 (std::max (minGain, 1.0e-6f));
}

} // namespace vcab
