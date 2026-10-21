// デノイズの部品が共有する小道具（帯域の境目、FFT の大きさ、倍率を測るための白色雑音、特殊関数など）
#pragma once

#include "Biquad.h"
#include "NoiseTracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

namespace vcab::denoise_detail
{

constexpr double kSmoothSeconds = 0.05;   // 最小値を追う前に、大きさをならす時間
// 倍率を測る白色雑音の長さ。最小値を探す長さが初期値（1.6 秒）より長い分は、窓が埋まるのを待つ分だけ足した所まで測る
constexpr double kCalibSeconds = 6.0, kCalibWarmup = 2.0;
constexpr double kCalibExtra = NoiseTracker::kWindowLengths[std::size (NoiseTracker::kWindowLengths) - 1] - NoiseTracker::kDefaultMinWindow;
constexpr float kTiny = 1.0e-20f;

// 帯域分割式の境目（Hz）。500Hz より上は 1/3 オクターブ
constexpr double kBandEdges[] = { 150, 250, 350, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150,
                                  4000, 5000, 6300, 8000, 10000, 12500, 16000 };

// 48kHz で 256 / 512 / 1024 点。ほかのサンプルレートでは、時間がほぼ同じになる 2 のべき乗
inline int fftSizeFor (double sr, int index)
{
    const double target = 256.0 * (1 << std::clamp (index, 0, 2)) * sr / 48000.0;
    int size = 64;
    while (size * 1.5 < target)
        size <<= 1;
    return size;
}

// 倍率を測るための白色雑音（毎回同じ並び）
struct WhiteNoise
{
    uint32_t state = 0x12345678u;
    float next() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (static_cast<float> (state) / 4294967296.0f * 2.0f - 1.0f) * 0.1f;
    }
};

template <typename Analyzer>
void calibrate (Analyzer& a, NoiseTracker& t, double sr, double frameRate, bool uniform)
{
    WhiteNoise noise;
    std::vector<float> buf (1024);
    t.beginCalibration (static_cast<int> (kCalibWarmup * frameRate));
    const int base = static_cast<int> (kCalibSeconds * sr);
    for (int done = 0; done < static_cast<int> ((kCalibSeconds + kCalibExtra) * sr); done += static_cast<int> (buf.size()))
    {
        if (done >= base)
            t.markCalibrationEnd();
        for (auto& x : buf)
            x = noise.next();
        a.analyzeOnly (buf.data(), static_cast<int> (buf.size()));
    }
    t.finishCalibration (uniform);
}

inline std::vector<double> periodicHann (int size, bool squareRoot)
{
    std::vector<double> w (static_cast<size_t> (size));
    for (int i = 0; i < size; ++i)
    {
        const double h = 0.5 - 0.5 * std::cos (2 * kPi * i / size);
        w[static_cast<size_t> (i)] = squareRoot ? std::sqrt (h) : h;
    }
    return w;
}

// 指数で割った変形ベッセル関数 e^(−x) I0(x)、e^(−x) I1(x)（MMSE-STSA に使う）
inline double besselI0e (double x)
{
    if (x < 15.0)
    {
        double sum = 1, term = 1;
        const double q = x * x / 4;
        for (int k = 1; k < 60; ++k)
        {
            term *= q / (static_cast<double> (k) * k);
            sum += term;
            if (term < sum * 1.0e-16)
                break;
        }
        return sum * std::exp (-x);
    }
    return (1.0 + 1.0 / (8 * x) + 9.0 / (128 * x * x)) / std::sqrt (2 * kPi * x);
}

inline double besselI1e (double x)
{
    if (x < 15.0)
    {
        double term = x / 2, sum = term;
        const double q = x * x / 4;
        for (int k = 1; k < 60; ++k)
        {
            term *= q / (static_cast<double> (k) * (k + 1));
            sum += term;
            if (term < sum * 1.0e-16)
                break;
        }
        return sum * std::exp (-x);
    }
    return (1.0 - 3.0 / (8 * x) - 15.0 / (128 * x * x)) / std::sqrt (2 * kPi * x);
}

// 指数積分 E1(x)（log-MMSE に使う）。式は Abramowitz と Stegun の「Handbook of Mathematical Functions」から
inline double expIntE1 (double x)
{
    x = std::max (x, 1.0e-10);
    if (x < 1.0)
    {
        // 級数（5.1.11）: E1(x) = -γ - ln x - Σ (-x)^k / (k・k!)
        double sum = 0, term = 1;
        for (int k = 1; k < 30; ++k)
        {
            term *= -x / k;
            sum -= term / k;
        }
        return -0.5772156649015329 - std::log (x) + sum;
    }
    // 有理式の近似（5.1.56）: x e^x E1(x) ≒ P(x) / Q(x)。1 以上で誤差 2e-8 未満
    const double p = (((x + 8.5733287401) * x + 18.0590169730) * x + 8.6347608925) * x + 0.2677737343;
    const double q = (((x + 9.5733223454) * x + 25.6329561486) * x + 21.0996530827) * x + 3.9584969228;
    return p / (q * x) * std::exp (-x);
}

// 声のありそうさ（0〜1）。300Hz〜4kHz の、ノイズに対する音量（dB）から
inline float presenceFromSnr (double snrDb)
{
    return static_cast<float> (1.0 / (1.0 + std::exp (-(snrDb - 6.0) / 1.5)));
}

} // namespace vcab::denoise_detail
