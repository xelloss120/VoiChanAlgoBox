#include "LatencyMeter.h"

#include "Dsp/Biquad.h"
#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vcab
{

namespace
{
constexpr double kSeconds = 2.0;
constexpr double kMaxLagMs = 300.0;   // これより大きな遅延は探さない
constexpr int kBlock = 128;

// 声に近いテスト音
std::vector<float> makeTestSignal (double sr)
{
    const int n = static_cast<int> (sr * kSeconds);
    std::vector<float> x (static_cast<size_t> (n), 0.0f);

    struct Resonator
    {
        double a1, a2, g, y1 = 0, y2 = 0;
        Resonator (double f, double bw, double sr)
        {
            const double r = std::exp (-kPi * bw / sr);
            a1 = 2 * r * std::cos (2 * kPi * f / sr);
            a2 = -r * r;
            g = 1 - r;
        }
        double process (double v) { const double y = g * v + a1 * y1 + a2 * y2; y2 = y1; y1 = y; return y; }
    };
    Resonator formants[] = { { 700, 90, sr }, { 1200, 100, sr }, { 2600, 140, sr } };

    std::uint32_t seed = 1;
    double phase = 0, hissPrev = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double cycle = std::fmod (t, 0.5);       // 0.5 秒ごとに: 母音 150ms、休み 100ms、息 100ms、休み 150ms
        double v = 0;
        if (t > 0.2 && cycle < 0.15)
        {
            phase += 130.0 / sr;
            if (phase >= 1.0)
            {
                phase -= 1.0;
                v = 1.0;
            }
        }
        for (auto& r : formants)
            v = r.process (v);
        double out = v * 100.0; // 母音の RMS が約 -21dBFS（ゲートの初期のしきい値より十分大きい）
        if (t > 0.2 && cycle >= 0.25 && cycle < 0.35)
        {
            seed = seed * 1664525u + 1013904223u;
            const double w = static_cast<double> (seed >> 8) / 8388608.0 - 1.0;
            out += 0.1 * (w - hissPrev);
            hissPrev = w;
        }
        x[static_cast<size_t> (i)] = static_cast<float> (out);
    }
    return x;
}

// 音量の包絡（対数）を、4kHz ほどに間引いて求める
std::vector<double> envelope (const std::vector<float>& x, double sr, int decimation)
{
    const double coeff = 1.0 - std::exp (-1.0 / (0.002 * sr)); // 約 2ms でならす
    std::vector<double> env;
    env.reserve (x.size() / static_cast<size_t> (decimation) + 1);
    double e = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        e += coeff * (static_cast<double> (x[i]) * x[i] - e);
        if (i % static_cast<size_t> (decimation) == 0)
            env.push_back (std::log10 (e + 1.0e-9));
    }
    return env;
}
} // namespace

double measureLatencyMs (const ParamValues& params, double sampleRate)
{
    const auto input = makeTestSignal (sampleRate);
    auto output = input;

    Engine engine;
    engine.prepare (sampleRate, kBlock);
    engine.setRealtime (false);
    const int n = static_cast<int> (output.size());
    for (int pos = 0; pos < n; pos += kBlock)
        engine.process (output.data() + pos, std::min (kBlock, n - pos), params);

    double peak = 0;
    for (float v : output)
        peak = std::max (peak, static_cast<double> (std::abs (v)));
    if (peak < 1.0e-4)
        return -1.0;

    const int decimation = std::max (1, static_cast<int> (sampleRate / 4000.0));
    const double envRate = sampleRate / decimation;
    const auto a = envelope (input, sampleRate, decimation);
    const auto b = envelope (output, sampleRate, decimation);
    const int maxLag = static_cast<int> (kMaxLagMs * 0.001 * envRate);
    const int len = static_cast<int> (a.size()) - maxLag;
    if (len <= 0)
        return -1.0;

    // ずらしながら相関係数を求め、一番大きいずれを探す
    std::vector<double> corr (static_cast<size_t> (maxLag + 1), -2.0);
    double meanA = 0, varA = 0;
    for (int i = 0; i < len; ++i)
        meanA += a[static_cast<size_t> (i)];
    meanA /= len;
    for (int i = 0; i < len; ++i)
        varA += (a[static_cast<size_t> (i)] - meanA) * (a[static_cast<size_t> (i)] - meanA);

    int best = 0;
    for (int lag = 0; lag <= maxLag; ++lag)
    {
        double meanB = 0;
        for (int i = 0; i < len; ++i)
            meanB += b[static_cast<size_t> (i + lag)];
        meanB /= len;
        double cov = 0, varB = 0;
        for (int i = 0; i < len; ++i)
        {
            const double da = a[static_cast<size_t> (i)] - meanA;
            const double db = b[static_cast<size_t> (i + lag)] - meanB;
            cov += da * db;
            varB += db * db;
        }
        corr[static_cast<size_t> (lag)] = cov / std::sqrt (varA * varB + 1.0e-12);
        if (corr[static_cast<size_t> (lag)] > corr[static_cast<size_t> (best)])
            best = lag;
    }

    // 山の位置を、放物線でサンプルの間まで細かく求める
    double offset = 0;
    if (best > 0 && best < maxLag)
    {
        const double y0 = corr[static_cast<size_t> (best - 1)], y1 = corr[static_cast<size_t> (best)], y2 = corr[static_cast<size_t> (best + 1)];
        const double d = y0 - 2 * y1 + y2;
        if (std::abs (d) > 1.0e-12)
            offset = std::clamp (0.5 * (y0 - y2) / d, -0.5, 0.5);
    }
    return (best + offset) / envRate * 1000.0;
}

} // namespace vcab
