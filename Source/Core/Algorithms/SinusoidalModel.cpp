#include "SinusoidalModel.h"

#include "../Dsp/Biquad.h"
#include "PhaseVocoder.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr int kOverlap = 4;
constexpr double kTwoPi = 2 * kPi;
constexpr int kPeakHalfWidth = 2; // ハン窓の山（メインローブ）の半分の幅（ビン）

double wrapPhase (double p) { return p - kTwoPi * std::round (p / kTwoPi); }

double envAt (const std::vector<double>& env, double bin)
{
    const int last = static_cast<int> (env.size()) - 1;
    if (bin <= 0) return env[0];
    if (bin >= last) return env[static_cast<size_t> (last)];
    const int i = static_cast<int> (bin);
    const double f = bin - i;
    return std::exp (std::log (env[static_cast<size_t> (i)]) * (1 - f) + std::log (env[static_cast<size_t> (i + 1)]) * f);
}
} // namespace

bool SinusoidalModel::supportsFormant (const ParamValues& p) const
{
    return p.index (P::formantMethod) != SpectralEnvelope::none;
}

void SinusoidalModel::prepare (double sampleRate, int)
{
    sr = sampleRate;
    size = 0;
    configure (1024);
}

void SinusoidalModel::configure (int fftSize)
{
    size = fftSize;
    half = size / 2;
    hop = size / kOverlap;
    latency = size - hop;

    fft.prepare (size);
    envelope.prepare (size, sr);
    window.resize (static_cast<size_t> (size));
    for (int i = 0; i < size; ++i)
        window[static_cast<size_t> (i)] = 0.5 - 0.5 * std::cos (kTwoPi * i / size);

    inFifo.assign (static_cast<size_t> (size), 0.0f);
    outFifo.assign (static_cast<size_t> (size), 0.0f);
    outAccum.assign (static_cast<size_t> (2 * size), 0.0);
    spec.assign (static_cast<size_t> (size), {});
    residualSpec.assign (static_cast<size_t> (size), {});
    for (auto* v : { &mag, &phase, &freq, &lastPhase, &env, &residual, &smoothed })
        v->assign (static_cast<size_t> (half + 1), 0.0);
    peaks.reserve (static_cast<size_t> (half + 1));
    reset();
}

void SinusoidalModel::reset()
{
    std::fill (inFifo.begin(), inFifo.end(), 0.0f);
    std::fill (outFifo.begin(), outFifo.end(), 0.0f);
    std::fill (outAccum.begin(), outAccum.end(), 0.0);
    std::fill (lastPhase.begin(), lastPhase.end(), 0.0);
    for (auto& t : tracks)
        t = {};
    rover = latency;
}

void SinusoidalModel::process (float* data, int n, const ParamValues& params)
{
    const int wanted = PhaseVocoder::fftSizeFor (params);
    if (wanted != size)
        configure (wanted);

    for (int i = 0; i < n; ++i)
    {
        inFifo[static_cast<size_t> (rover)] = data[i];
        data[i] = outFifo[static_cast<size_t> (rover - latency)];
        if (++rover >= size)
        {
            rover = latency;
            processFrame (params);
        }
    }
}

void SinusoidalModel::processFrame (const ParamValues& params)
{
    const double ratio = semitonesToRatio (params.get (P::pitch));
    const double formantRatio = semitonesToRatio (params.get (P::formant));
    warp.set (params, formantTrack);
    const double binHz = sr / size;
    // 動かした後の位置 bin に来る、元の包絡の位置（「低い側を残す」のときは FormantWarp で求める）
    auto sourceBin = [&] (double bin) { return warp.isUniform() ? bin / formantRatio : warp.inverse (bin * binHz) / binHz; };
    const int envMethod = std::clamp (params.index (P::formantMethod), 0, 3);
    const int maxPeaks = std::clamp (static_cast<int> (params.get (P::smMaxSines)), 1, kMaxTracks);
    const double expected = kTwoPi * hop / size;

    // ---- 分析 ----
    for (int i = 0; i < size; ++i)
        spec[static_cast<size_t> (i)] = { inFifo[static_cast<size_t> (i)] * window[static_cast<size_t> (i)], 0.0 };
    fft.forward (spec.data());

    double maxMag = 0;
    for (int k = 0; k <= half; ++k)
    {
        const auto kk = static_cast<size_t> (k);
        mag[kk] = std::abs (spec[kk]);
        phase[kk] = std::arg (spec[kk]);
        const double delta = wrapPhase (phase[kk] - lastPhase[kk] - k * expected);
        lastPhase[kk] = phase[kk];
        freq[kk] = k + delta / expected;
        maxMag = std::max (maxMag, mag[kk]);
    }

    // 山を探す（両隣より大きく、最大から -70dB 以内、正弦波の振幅で約 -100dB 以上）
    const double ampScale = 4.0 / size; // ハン窓をかけた正弦波の山の高さは、振幅 × N/4
    peaks.clear();
    for (int k = 2; k < half - 1; ++k)
    {
        const auto kk = static_cast<size_t> (k);
        if (mag[kk] > mag[kk - 1] && mag[kk] >= mag[kk + 1] && mag[kk] > maxMag * 3.0e-4 && mag[kk] * ampScale > 1.0e-5)
            peaks.push_back (k);
    }
    // 大きい順に、上限の数まで
    std::sort (peaks.begin(), peaks.end(), [this] (int a, int b) { return mag[static_cast<size_t> (a)] > mag[static_cast<size_t> (b)]; });
    if (static_cast<int> (peaks.size()) > maxPeaks)
        peaks.resize (static_cast<size_t> (maxPeaks));

    if (envMethod != SpectralEnvelope::none)
        envelope.estimate (envMethod, mag.data(), env.data());

    // 雑音成分のために、元のスペクトルを取っておく（後で正弦波の分を引く）
    std::copy (spec.begin(), spec.begin() + half + 1, residualSpec.begin());

    // ---- 線をつなぐ ----
    for (auto& t : tracks)
        t.matched = false;

    for (int k : peaks)
    {
        const auto kk = static_cast<size_t> (k);
        // 山の高さは、対数の放物線で、ビンの間まで補間する
        const double a = std::log (mag[kk - 1] + 1.0e-12), b = std::log (mag[kk] + 1.0e-12), c = std::log (mag[kk + 1] + 1.0e-12);
        const double d = a - 2 * b + c;
        const double offset = std::abs (d) > 1.0e-12 ? std::clamp (0.5 * (a - c) / d, -0.5, 0.5) : 0.0;
        const double amp = std::exp (b - 0.25 * (a - c) * offset) * ampScale;
        const double f = freq[kk];

        // 前の窓で、周波数が一番近い（1 ビン以内の）線につなぐ
        Track* best = nullptr;
        double bestDist = 1.0;
        Track* freeSlot = nullptr;
        for (auto& t : tracks)
        {
            if (! t.active)
            {
                if (freeSlot == nullptr)
                    freeSlot = &t;
                continue;
            }
            if (t.matched)
                continue;
            const double dist = std::abs (t.freq - f);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = &t;
            }
        }
        if (best == nullptr)
        {
            // 新しい線: 音量 0 から始める
            if (freeSlot == nullptr)
                continue;
            best = freeSlot;
            *best = {};
            best->active = true;
            best->freq = f;
            best->phase = phase[kk];
        }
        best->matched = true;
        best->newFreq = f;
        best->newAmp = amp;
    }

    // ---- 正弦波を鳴らす（前の窓の中心から、今の窓の中心まで） ----
    // 窓の中心は、今ためている窓の先頭から half の位置。その手前 hop サンプル分を書く
    const int start = half - hop;
    const double density = envMethod != SpectralEnvelope::none ? std::sqrt (ratio) : 1.0;
    for (auto& t : tracks)
    {
        if (! t.active)
            continue;
        if (! t.matched)
        {
            // つながらなかった線は、同じ周波数のまま音を絞って消す
            t.newFreq = t.freq;
            t.newAmp = 0;
        }

        // 鳴らす大きさ。フォルマント処理では、移した先の周波数での包絡に合わせて大きさを決め直す
        double gain = density;
        const double outBin = t.newFreq * ratio;
        if (envMethod != SpectralEnvelope::none)
            gain *= std::min (16.0, envAt (env, sourceBin (outBin)) / envAt (env, t.newFreq));
        t.outAmpEnd = outBin < half * 0.98 ? t.newAmp * gain : 0.0;

        const double f0 = t.freq * ratio * kTwoPi / size, f1 = t.newFreq * ratio * kTwoPi / size; // 1サンプルあたりの位相の進み
        for (int i = 0; i < hop; ++i)
        {
            const double x = static_cast<double> (i) / hop;
            const double amp = t.outAmpStart + (t.outAmpEnd - t.outAmpStart) * x;
            t.phase += f0 + (f1 - f0) * x;
            outAccum[static_cast<size_t> (start + i)] += amp * std::sin (t.phase);
        }
        t.phase = wrapPhase (t.phase);

        t.freq = t.newFreq;
        t.amp = t.newAmp;
        t.outAmpStart = t.outAmpEnd;
        if (! t.matched)
            t.active = false;
    }

    // ---- 雑音成分 ----
    if (params.on (P::smNoiseOn))
        synthesizeNoise (params, envMethod, formantRatio, ratio);

    for (int i = 0; i < hop; ++i)
        outFifo[static_cast<size_t> (i)] = static_cast<float> (outAccum[static_cast<size_t> (i)]);
    std::copy (outAccum.begin() + hop, outAccum.end(), outAccum.begin());
    std::fill (outAccum.end() - hop, outAccum.end(), 0.0);
    std::copy (inFifo.begin() + hop, inFifo.end(), inFifo.begin());
}

Fft::Complex SinusoidalModel::windowKernel (double delta) const noexcept
{
    // 長さ N の矩形窓のスペクトル D(δ) = e^{-jπδ(N-1)/N} sin(πδ) / sin(πδ/N) を使うと、
    // ハン窓（0.5 - 0.5cos）は 0.5 D(δ) - 0.25 D(δ-1) - 0.25 D(δ+1)
    auto dirichlet = [this] (double d) -> Fft::Complex
    {
        const double s = std::sin (kPi * d / size);
        const double value = std::abs (s) < 1.0e-12 ? static_cast<double> (size) : std::sin (kPi * d) / s;
        return std::polar (value, -kPi * d * (size - 1) / size);
    };
    return 0.5 * dirichlet (delta) - 0.25 * dirichlet (delta - 1) - 0.25 * dirichlet (delta + 1);
}

void SinusoidalModel::synthesizeNoise (const ParamValues&, int envMethod, double formantRatio, double ratio)
{
    // 鳴らした正弦波の分を、元のスペクトルから引く。山のビンの値と、窓をかけた正弦波の形から、
    // 正弦波の複素振幅を求め、その形を周りのビンから引く → 残りが雑音成分
    for (int k : peaks)
    {
        const double centre = freq[static_cast<size_t> (k)];
        // 周波数の推定が山のビンから半分以上ずれるものは、安定した正弦波ではない（雑音の山など）ので引かない
        if (std::abs (k - centre) > 0.5)
            continue;
        const auto kernelAtPeak = windowKernel (k - centre);
        const auto amplitude = spec[static_cast<size_t> (k)] / kernelAtPeak;
        for (int j = std::max (0, k - kPeakHalfWidth * 2); j <= std::min (half, k + kPeakHalfWidth * 2); ++j)
            residualSpec[static_cast<size_t> (j)] -= amplitude * windowKernel (j - centre);
    }
    for (int k = 0; k <= half; ++k)
        residual[static_cast<size_t> (k)] = std::abs (residualSpec[static_cast<size_t> (k)]);
    // ならす（前後 2 ビンの平均）
    for (int k = 0; k <= half; ++k)
    {
        double sum = 0;
        int count = 0;
        for (int j = std::max (0, k - 2); j <= std::min (half, k + 2); ++j, ++count)
            sum += residual[static_cast<size_t> (j)];
        smoothed[static_cast<size_t> (k)] = sum / count;
    }

    // 雑音の形もフォルマントと一緒に動かす。フォルマント処理なしのときは、ピッチと一緒に動かす
    const double shift = envMethod != SpectralEnvelope::none ? formantRatio : ratio;
    const bool curved = envMethod != SpectralEnvelope::none && ! warp.isUniform();
    const double binHz = sr / size;
    for (int j = 0; j <= half; ++j)
    {
        const double src = curved ? warp.inverse (j * binHz) / binHz : j / shift;
        double m = 0;
        if (src < half)
        {
            const int i = static_cast<int> (src);
            const double f = src - i;
            m = smoothed[static_cast<size_t> (i)] * (1 - f) + smoothed[static_cast<size_t> (i + 1)] * f;
        }
        // 乱数で位相を決める（線形合同法。再現性があり、処理の中で確保しない）
        noiseSeed = noiseSeed * 1664525u + 1013904223u;
        const double ph = kTwoPi * (noiseSeed >> 8) / 16777216.0;
        spec[static_cast<size_t> (j)] = std::polar (m, ph);
    }
    spec[0] = { spec[0].real(), 0.0 };
    spec[static_cast<size_t> (half)] = { spec[static_cast<size_t> (half)].real(), 0.0 };
    for (int j = half + 1; j < size; ++j)
        spec[static_cast<size_t> (j)] = std::conj (spec[static_cast<size_t> (size - j)]);
    fft.inverse (spec.data());

    // ランダムな位相の窓どうしは打ち消し合わないので、大きさ（分散）で合わせる:
    // 窓の中の1サンプルあたりの分散は元の 3/8、合成の窓の2乗を4つ重ねた和は 1.5 → 0.5625 倍なので、4/3 倍する
    const double norm = 4.0 / 3.0;
    for (int i = 0; i < size; ++i)
        outAccum[static_cast<size_t> (i)] += spec[static_cast<size_t> (i)].real() * window[static_cast<size_t> (i)] * norm;
}

int SinusoidalModel::latencySamples (const ParamValues& params) const
{
    const int n = PhaseVocoder::fftSizeFor (params);
    return n - n / kOverlap;
}

} // namespace vcab
