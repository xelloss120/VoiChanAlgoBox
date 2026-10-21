#include "BandDenoise.h"

#include "DenoiseDetail.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace vcab
{

using namespace denoise_detail;



void BandSynth::prepare (double sampleRate)
{
    sr = sampleRate;
    // 境目は、ERB の目盛り（Glasberg・Moore）で等間隔に、100Hz から min (16kHz, 上限の 0.8 倍) まで
    const double top = std::min (16000.0, std::min (20000.0, sr * 0.45) * 0.8);
    const auto erb = [] (double f) { return 21.4 * std::log10 (1.0 + 0.00437 * f); };
    const auto fromErb = [] (double e) { return (std::pow (10.0, e / 21.4) - 1.0) / 0.00437; };
    const int edges = kMaxBands - 1;
    numBands = kMaxBands;
    std::array<double, kMaxBands - 1> edge {};
    for (int k = 0; k < edges; ++k)
        edge[static_cast<size_t> (k)] = fromErb (erb (100.0) + (erb (top) - erb (100.0)) * k / (edges - 1));
    for (int k = 0; k < edges; ++k)
    {
        splitCoeffs[static_cast<size_t> (k)] = BiquadCoeffs::lowPass (sr, edge[static_cast<size_t> (k)], 0.70710678);
        split[static_cast<size_t> (k)].setCoeffs (splitCoeffs[static_cast<size_t> (k)]);
    }
    // 立ち上がりの保護の時間
    const auto coef = [this] (double seconds) { return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * sr))); };
    fastUp = coef (0.001);
    fastDown = coef (0.010);
    slowCoef = coef (0.040);
    openRelease = coef (0.020);
    holdSamples = static_cast<int> (0.030 * sr);
    for (int k = 0; k < numBands; ++k)
    {
        lo[static_cast<size_t> (k)] = k == 0 ? 0.0 : edge[static_cast<size_t> (k - 1)];
        hi[static_cast<size_t> (k)] = k == numBands - 1 ? sr * 0.5 : edge[static_cast<size_t> (k)];
    }
    weightBins = 0; // 重みは、使うときに作り直す
    reset();
}


const float* BandSynth::weights (int bins, double binHz) const
{
    if (bins == weightBins && binHz == weightHz)
        return weightTable.data();
    weightBins = bins;
    weightHz = binHz;
    weightTable.assign (static_cast<size_t> (numBands * bins), 0.0f);
    for (int b = 0; b < bins; ++b)
    {
        const double w = 2.0 * kPi * b * binHz / sr;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
        std::complex<double> prev = 0;
        for (int k = 0; k < numBands; ++k)
        {
            std::complex<double> lp = 1.0;
            if (k < numBands - 1)
            {
                const auto& c = splitCoeffs[static_cast<size_t> (k)];
                lp = (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
            }
            weightTable[static_cast<size_t> (k * bins + b)] = static_cast<float> (std::norm (lp - prev));
            prev = lp;
        }
    }
    return weightTable.data();
}


void BandSynth::reset()
{
    for (auto& b : split)
        b.reset();
    gain.fill (1.0f);
    step.fill (0.0f);
    rampLeft = 0;
    noiseValid = false;
    fastEnv.fill (0.0f);
    slowEnv.fill (0.0f);
    opened.fill (0.0f);
    holdLeft.fill (0);
}


void BandSynth::setNoise (const float* binNoise, int bins, double binHz, double powerToVariance)
{
    // 区画のノイズに、帯域の通り方の重みをかけて足し、帯域の音の 1 サンプルあたりの 2 乗に直す
    const float* wt = weights (bins, binHz);
    for (int k = 0; k < numBands; ++k)
    {
        double sum = 0;
        for (int b = 0; b < bins; ++b)
            sum += binNoise[b] * wt[k * bins + b];
        noiseVar[static_cast<size_t> (k)] = static_cast<float> (sum * powerToVariance);
    }
    noiseValid = true;
}


void BandSynth::setTargets (const float* targets, int rampSamples)
{
    // 今の倍率から、rampSamples かけて直線で次の倍率へ
    rampLeft = std::max (1, rampSamples);
    for (int k = 0; k < numBands; ++k)
        step[static_cast<size_t> (k)] = (targets[k] - gain[static_cast<size_t> (k)]) / static_cast<float> (rampLeft);
}


float BandSynth::process (float x) noexcept
{
    if (rampLeft > 0)
    {
        for (int k = 0; k < numBands; ++k)
            gain[static_cast<size_t> (k)] += step[static_cast<size_t> (k)];
        --rampLeft;
    }
    // 帯域 k = LP_k − LP_{k−1}（最後は 元 − LP）。全部 1 倍なら元と同じ
    const bool guard = protect && noiseValid;
    const auto bandGain = [&] (size_t i, float band) -> float
    {
        if (! guard)
            return gain[i];
        // 立ち上がりの保護: 急に大きくなった帯域を、しばらく開ける
        const float e = band * band;
        fastEnv[i] += (e > fastEnv[i] ? fastUp : fastDown) * (e - fastEnv[i]);
        slowEnv[i] += slowCoef * (e - slowEnv[i]);
        const float nz = noiseVar[i] + 1.0e-20f;
        if (fastEnv[i] > nz * 10.0f && fastEnv[i] > slowEnv[i] * 2.0f)
            holdLeft[i] = holdSamples;
        if (holdLeft[i] > 0)
        {
            --holdLeft[i];
            opened[i] = 1.0f;
        }
        else
            opened[i] -= openRelease * opened[i];
        // 開ける量は、SNR が +9dB（8 倍）〜 +15dB（32 倍）の間でなだらかに（+6〜+12dB より、話している最中のノイズが 0.5dB ほど多く減る）
        const float amount = opened[i] * std::clamp ((fastEnv[i] / nz - 8.0f) / 24.0f, 0.0f, 1.0f);
        return gain[i] + amount * (1.0f - gain[i]);
    };
    float prev = 0, y = 0;
    const int last = numBands - 1;
    for (int k = 0; k < last; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const float lp = split[i].process (x);
        const float band = lp - prev;
        y += bandGain (i, band) * band;
        prev = lp;
    }
    const float band = x - prev;
    return y + bandGain (static_cast<size_t> (last), band) * band;
}


void BandSynth::binsToBands (const float* binGain, const float* binPower, int bins, double binHz, float floorGain, float* out) const
{
    // 帯域に入る区画の、大きさ（と帯域の通り方）で重み付けした「残す割合の 2 乗」の平均の平方根
    // （帯域の中で、下げた後の大きさが合うように）
    const float* wt = weights (bins, binHz);
    for (int k = 0; k < numBands; ++k)
    {
        double num = 0, den = 0, plain = 0;
        int count = 0;
        for (int b = 0; b < bins; ++b)
        {
            const double w = wt[k * bins + b];
            if (w < 0.01)
                continue;
            const double g = binGain[b];
            num += g * g * binPower[b] * w;
            den += binPower[b] * w;
            plain += g;
            ++count;
        }
        double g = 1.0;
        if (count == 0)
        {
            const int b = std::clamp (static_cast<int> (std::lround (0.5 * (lo[static_cast<size_t> (k)] + hi[static_cast<size_t> (k)]) / binHz)), 0, bins - 1);
            g = binGain[b];
        }
        else if (den > 1.0e-20)
            g = std::sqrt (num / den);
        else
            g = plain / count;
        out[k] = std::clamp (static_cast<float> (g), floorGain, 1.0f);
    }
}



void BandDenoiser::prepare (double sampleRate)
{
    sr = sampleRate;
    const double top = std::min (20000.0, sr * 0.45);
    int edges = 0;
    for (double e : kBandEdges)
        if (e < top * 0.8)
            ++edges;
    numBands = edges + 1;

    for (int k = 0; k < edges; ++k)
    {
        // 4次のバターワース（2次を2段。Q は 0.5412 と 1.3066）
        split[static_cast<size_t> (k)][0].setCoeffs (BiquadCoeffs::lowPass (sr, kBandEdges[k], 0.5412));
        split[static_cast<size_t> (k)][1].setCoeffs (BiquadCoeffs::lowPass (sr, kBandEdges[k], 1.3066));
    }
    for (int k = 0; k < numBands; ++k)
    {
        const double lo = k == 0 ? 50.0 : kBandEdges[k - 1];
        const double hi = k == numBands - 1 ? top : kBandEdges[k];
        bandLo[static_cast<size_t> (k)] = lo;
        bandHi[static_cast<size_t> (k)] = hi;
        const auto hp = BiquadCoeffs::highPass (sr, lo, 0.7071), lp = BiquadCoeffs::lowPass (sr, hi, 0.7071);
        detect[static_cast<size_t> (k)][0].setCoeffs (hp);
        detect[static_cast<size_t> (k)][1].setCoeffs (lp);
        // 等価雑音帯域幅（通る大きさの 2 乗を周波数で足したもの）。2 次のフィルタは裾が広いので、境目の差より広い
        {
            double sum = 0;
            const double step = 2.0;
            for (double f = step * 0.5; f < sr * 0.5; f += step)
            {
                const double g = hp.magnitudeAt (sr, f) * lp.magnitudeAt (sr, f);
                sum += g * g * step;
            }
            bandWidth[static_cast<size_t> (k)] = sum;
        }
        // 音量をならす時間: 低い帯域ほど長く（2 周期分。下限 4ms）
        const double tau = std::max (0.004, 2.0 / std::sqrt (lo * hi));
        envCoeff[static_cast<size_t> (k)] = 1.0 - std::exp (-1.0 / (tau * sr));
        // 上がるときは速く（声の出だしに早く反応するように。1ms、低い帯域は半周期）
        envAttack[static_cast<size_t> (k)] = 1.0 - std::exp (-1.0 / (std::max (0.001, 0.5 / std::sqrt (lo * hi)) * sr));
    }

    noise.prepare (numBands, sr / kBlock);
    noise.setSettleSamples (sr, static_cast<int> (0.05 * sr)); // 音量の追いかけ（一番低い帯域で約 6ms で上がる）が落ち着くまで
    reverb.prepare (numBands, sr / kBlock);
    calibrate (*this, noise, sr, sr / kBlock, false);

    // 表示のため: 大きさの分かっている白色雑音（一様分布 ±0.1 → 分散 0.01 / 3）を通して、帯域ごとの音量の平均と、
    // 等価雑音帯域幅から計算した値との比を測る
    {
        reset();
        WhiteNoise wn;
        std::array<double, kMaxBands> sum {};
        const int total = static_cast<int> (2.0 * sr), warm = static_cast<int> (1.0 * sr);
        for (int j = 0; j < total; ++j)
        {
            analyzeSample (wn.next());
            if (j >= warm)
                for (int k = 0; k < numBands; ++k)
                    sum[static_cast<size_t> (k)] += env[static_cast<size_t> (k)];
        }
        const double variance = 0.01 / 3.0;
        for (int k = 0; k < numBands; ++k)
        {
            const auto i = static_cast<size_t> (k);
            const double expected = variance * bandWidth[i] * 2.0 / sr;
            envScale[i] = expected > 0 ? sum[i] / (total - warm) / expected : 1.0;
        }
    }
    reset();
}


void BandDenoiser::reset()
{
    for (auto& s : split)
        for (auto& b : s)
            b.reset();
    for (auto& d : detect)
        for (auto& b : d)
            b.reset();
    env.fill (0.0f);
    gainDb.fill (0.0f);
    gainFrom.fill (1.0f);
    gainTo.fill (1.0f);
    gateOpen.fill (true);
    blockPos = 0;
    noise.reset();
    reverb.reset();
    voiceSnrDb = -60;
}


void BandDenoiser::analyzeSample (float x) noexcept
{
    for (int k = 0; k < numBands; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const float b = detect[i][1].process (detect[i][0].process (x));
        const float p = b * b;
        env[i] += static_cast<float> (p > env[i] ? envAttack[i] : envCoeff[i]) * (p - env[i]);
    }
}


void BandDenoiser::endBlock (const DenoiseSettings& s)
{
    noise.push (env.data());
    reverb.push (env.data());
    const float* est = noise.estimate (s.noiseMode);
    if (s.dereverb)
    {
        const float* r = reverb.estimate (s.rt60);
        // SNR 式はノイズの見積もりに「強さ」をかけるので、残響の分はその分を割っておく
        const auto reverbScale = s.rule == BandRule::snr ? static_cast<float> (std::pow (10.0, -s.strengthDb / 10.0)) : 1.0f;
        for (int k = 0; k < numBands; ++k)
            noiseTotal[static_cast<size_t> (k)] = est[k] + r[k] * reverbScale;
        est = noiseTotal.data();
    }
    {
        double sig = 0, nz = 0;
        for (int k = 0; k < numBands; ++k)
            if (bandHi[static_cast<size_t> (k)] > 300 && bandLo[static_cast<size_t> (k)] < 4000)
            {
                sig += env[static_cast<size_t> (k)];
                nz += est[k];
            }
        voiceSnrDb = static_cast<float> (10.0 * std::log10 ((sig + 1.0e-20) / (nz + 1.0e-20)));
    }
    const double floorDb = -s.maxReductionDb;
    const double blockSec = kBlock / sr;
    const auto attack = static_cast<float> (1.0 - std::exp (-blockSec / (std::max (0.1, s.attackMs) * 0.001)));
    const auto release = static_cast<float> (1.0 - std::exp (-blockSec / (std::max (1.0, s.releaseMs) * 0.001)));
    const double strength = std::pow (10.0, s.strengthDb / 10.0);

    for (int k = 0; k < numBands; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const double snrDb = 10.0 * std::log10 (std::max (env[i], kTiny) / std::max (est[i], kTiny));
        double target = 0;
        switch (s.rule)
        {
            case BandRule::gate: // ゲート式: ノイズより「しきい値」大きければ開く（閉じるのは 3dB 下回ってから）
                gateOpen[i] = gateOpen[i] ? snrDb > s.thresholdDb - 3.0 : snrDb > s.thresholdDb;
                target = gateOpen[i] ? 0.0 : floorDb;
                break;
            case BandRule::expander: // エキスパンダー式: しきい値より下は、1dB 下がるごとに「比率 − 1」dB 下げる
                target = snrDb < s.thresholdDb ? (snrDb - s.thresholdDb) * (s.ratio - 1.0) : 0.0;
                break;
            default: // SNR 式（ウィーナー型）: 残す割合（2乗）= 1 − ノイズ / 今の音
            {
                const double keep = 1.0 - strength / std::pow (10.0, snrDb / 10.0);
                target = keep > 1.0e-6 ? 10.0 * std::log10 (keep) : floorDb;
                break;
            }
        }
        target = std::max (target, floorDb);
        gainDb[i] += (static_cast<float> (target) - gainDb[i]) * (target > gainDb[i] ? attack : release);
        gainFrom[i] = gainTo[i];
        gainTo[i] = std::pow (10.0f, gainDb[i] / 20.0f);
    }
}


void BandDenoiser::process (float* data, int n, const DenoiseSettings& s)
{
    const int last = numBands - 1;
    for (int j = 0; j < n; ++j)
    {
        const float x = data[j];
        noise.observe (x);
        analyzeSample (x);

        // 足し戻す: 帯域 k = LP_k − LP_{k−1}（最後は 元 − LP）。下げる量は、ブロックの中で直線でつなぐ
        const float t = static_cast<float> (blockPos + 1) / kBlock;
        float prev = 0, y = 0;
        for (int k = 0; k < last; ++k)
        {
            const auto i = static_cast<size_t> (k);
            const float lp = split[i][1].process (split[i][0].process (x));
            y += (gainFrom[i] + (gainTo[i] - gainFrom[i]) * t) * (lp - prev);
            prev = lp;
        }
        const auto li = static_cast<size_t> (last);
        y += (gainFrom[li] + (gainTo[li] - gainFrom[li]) * t) * (x - prev);
        data[j] = y;

        if (++blockPos == kBlock)
        {
            blockPos = 0;
            endBlock (s);
        }
    }
}


float BandDenoiser::noiseDensity (double hz, NoiseMode mode) const
{
    const float* est = noise.estimate (mode);
    for (int k = 0; k < numBands; ++k)
        if (hz < bandHi[static_cast<size_t> (k)] || k == numBands - 1)
        {
            // 等価雑音帯域幅 B のフィルタを通した白色雑音の 2 乗は、全体（0〜sr/2）の B / (sr/2)
            return static_cast<float> (est[k] * sr / (2.0 * std::max (bandWidth[static_cast<size_t> (k)], 1.0) * envScale[static_cast<size_t> (k)]));
        }
    return 0;
}


void BandDenoiser::analyzeOnly (const float* data, int n)
{
    for (int j = 0; j < n; ++j)
    {
        noise.observe (data[j]);
        analyzeSample (data[j]);
        if (++blockPos == kBlock)
        {
            blockPos = 0;
            noise.push (env.data());
        }
    }
}



void HarmonicComb::prepare (double sampleRate)
{
    sr = sampleRate;
    // 40Hz の周期 × 4 つ分 ＋ 余裕
    int len = 1;
    while (len < static_cast<int> (sr / 40.0 * kTaps) + 8)
        len <<= 1;
    ring.assign (static_cast<size_t> (len), 0.0f);
    periodCoeff = 1.0 - std::exp (-1.0 / (0.005 * sr)); // 周期は 5ms でなめらかに追う
    mixCoeff = 1.0 - std::exp (-1.0 / (0.010 * sr));    // かけ始めと外すときは 10ms
    for (auto& b : lowPass)
        b.setCoeffs (BiquadCoeffs::lowPass (sr, 4000.0, 0.7071));
    for (auto& b : highPass)
        b.setCoeffs (BiquadCoeffs::highPass (sr, 4000.0, 0.7071));
    reset();
}


void HarmonicComb::reset()
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    pos = 0;
    period = 0;
    mix = 0;
    for (auto& b : lowPass)
        b.reset();
    for (auto& b : highPass)
        b.reset();
}


void HarmonicComb::process (float* data, int n, float hz, bool voiced, double depthDb)
{
    const int mask = static_cast<int> (ring.size()) - 1;
    const bool active = voiced && hz > 0;
    // 倍音と倍音のちょうど間では、4 つの平均が 0 になるので、残る割合は 1 − mix
    const double mixTarget = active ? 1.0 - std::pow (10.0, -depthDb / 20.0) : 0.0;
    const double periodTarget = active ? sr / hz : period;
    if (period <= 0 && active)
        period = periodTarget;

    for (int i = 0; i < n; ++i)
    {
        // 低い側（くし形をかける）と高い側（そのまま）に分ける。分けた2つは位相がそろっているので、
        // 低い側の中で引き算をすれば、打ち消し合いがずれない
        const float x = data[i];
        const float low = lowPass[1].process (lowPass[0].process (x));
        const float high = highPass[1].process (highPass[0].process (x));
        ring[static_cast<size_t> (pos)] = low;
        period += (periodTarget - period) * periodCoeff;
        mix += (mixTarget - mix) * mixCoeff;

        // 倍音の間の成分 = 低い側 − 周期 T ずつずらした 4 つの低い側の平均（小数の位置は直線で補間）
        float between = 0;
        if (period > 1 && mix > 1.0e-4)
        {
            float sum = low;
            for (int k = 1; k < kTaps; ++k)
            {
                const double d = period * k;
                const int di = static_cast<int> (d);
                const float frac = static_cast<float> (d - di);
                const float a = ring[static_cast<size_t> ((pos - di) & mask)];
                const float b = ring[static_cast<size_t> ((pos - di - 1) & mask)];
                sum += a + (b - a) * frac;
            }
            between = low - sum / kTaps;
        }
        data[i] = low + high - static_cast<float> (mix) * between;
        pos = (pos + 1) & mask;
    }
}

} // namespace vcab
