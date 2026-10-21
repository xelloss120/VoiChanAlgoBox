#include "Transient.h"


#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace vcab
{



void TransientSuppressor::prepare (double sampleRate)
{
    sr = sampleRate;
    lookahead = static_cast<int> (std::lround (0.005 * sr));
    holdExtra = static_cast<int> (std::lround (0.005 * sr));
    delay.assign (static_cast<size_t> (lookahead + 1), 0.0f);
    fastRing.assign (static_cast<size_t> (lookahead + 1), 0.0f);
    onsetRing.assign (static_cast<size_t> (lookahead + 1), 0);
    for (auto& b : highPass)
        b.setCoeffs (BiquadCoeffs::highPass (sr, 2000.0, 0.7071));
    auto coeff = [this] (double seconds) { return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * sr))); };
    fastAttack = coeff (0.0001);
    fastRelease = coeff (0.001);
    slowAttack = coeff (0.1);
    slowRelease = coeff (0.4);
    closeCoeff = coeff (0.0005);
    openCoeff = coeff (0.02);
    reset();
}


void TransientSuppressor::reset()
{
    std::fill (delay.begin(), delay.end(), 0.0f);
    std::fill (fastRing.begin(), fastRing.end(), 0.0f);
    std::fill (onsetRing.begin(), onsetRing.end(), static_cast<unsigned char> (0));
    for (auto& b : highPass)
        b.reset();
    fast = slow = 0;
    gainDb = 0;
    prevScore = -100;
    hold = 0;
    pos = 0;
}


void TransientSuppressor::process (float* data, int n, double thresholdDb, double depthDb)
{
    const int size = lookahead + 1;
    for (int i = 0; i < n; ++i)
    {
        const float x = data[i];
        // 今の音で調べる（出すのは lookahead 前の音なので、その分先を見ていることになる）
        const float h = highPass[1].process (highPass[0].process (x));
        const float p = h * h;
        fast += (p > fast ? fastAttack : fastRelease) * (p - fast);
        slow += (p > slow ? slowAttack : slowRelease) * (p - slow);
        const float score = 10.0f * std::log10 ((fast + 1.0e-12f) / (slow + 1.0e-10f));
        // 周りより「しきい値」以上大きくなった瞬間（-70dBFS より小さい音は対象外）
        const bool onset = score > thresholdDb && prevScore <= thresholdDb && fast > 1.0e-7f;
        prevScore = score;

        delay[static_cast<size_t> (pos)] = x;
        fastRing[static_cast<size_t> (pos)] = fast;
        onsetRing[static_cast<size_t> (pos)] = onset ? 1 : 0;

        // lookahead 前に立ち上がった音が、今までに 6dB 以上小さくなっていれば、短い音（突発音）とみなす
        const int oldest = (pos + 1) % size;
        if (onsetRing[static_cast<size_t> (oldest)] != 0)
        {
            float peak = 0;
            for (float f : fastRing)
                peak = std::max (peak, f);
            if (fast < peak * 0.25f)
            {
                hold = lookahead + holdExtra;
                ++count;
            }
        }

        const float target = hold > 0 ? static_cast<float> (-depthDb) : 0.0f;
        if (hold > 0)
            --hold;
        gainDb += (target - gainDb) * (target < gainDb ? closeCoeff : openCoeff);
        data[i] = delay[static_cast<size_t> (oldest)] * std::pow (10.0f, gainDb / 20.0f);
        pos = oldest;
    }
}

} // namespace vcab
