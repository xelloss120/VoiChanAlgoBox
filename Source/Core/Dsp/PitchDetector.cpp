#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr int kFftSize = 1024;      // ケプストラム法の FFT の大きさ（窓は最長でも約 800 サンプルなので、残りは 0 で埋める）
constexpr float kSilenceDb = -55;   // これより小さい窓は、調べずに無音として扱う

// 方式ごとの、結果の時刻が今からどれだけ前か（元のサンプルレートのサンプル数）
int delayFor (double sr, int dec, double fsDec, double minHz, int method, int frameLength)
{
    const int hop = std::max (1, static_cast<int> (std::lround (0.005 * fsDec)));
    switch (method)
    {
        case PitchDetector::rapt:    return (frameLength / 2 + PitchDetector::kRaptLookahead * hop) * dec;
        case PitchDetector::pyin:    return (frameLength / 2 + PitchDetector::kPyinLookahead * hop) * dec;
        case PitchDetector::dio:     return frameLength * dec; // frameLength に、遅れのフレーム分を入れてある
        case PitchDetector::harvest: return static_cast<int> (std::ceil (HarvestStream::delayMsFor (minHz) * sr / 1000.0));
        default:                     return frameLength * dec / 2;
    }
}

// 3点を通る放物線の頂点の位置（-0.5〜0.5 のずれ）。山と谷の位置を、サンプルの間まで細かく求める
double parabolicOffset (double a, double b, double c)
{
    const double d = a - 2 * b + c;
    return std::abs (d) < 1.0e-12 ? 0.0 : std::clamp (0.5 * (a - c) / d, -0.5, 0.5);
}
} // namespace

void PitchDetector::prepare (double sampleRate)
{
    sr = sampleRate;
    decimation = std::max (1, static_cast<int> (std::lround (sr / 12000.0)));
    fs = sr / decimation;
    hop = std::max (1, static_cast<int> (std::lround (0.005 * fs)));

    // 間引く前に、間引いた後のナイキスト周波数より上を落とす（4次のバターワース）
    const double cutoff = 0.4 * fs;
    antiAlias[0].setCoeffs (BiquadCoeffs::lowPass (sr, cutoff, 0.5412));
    antiAlias[1].setCoeffs (BiquadCoeffs::lowPass (sr, cutoff, 1.3066));

    ring.assign (kRingSize, 0.0f);
    harvestStream.prepare (sr);
    frame.assign (std::max (kFftSize, kRingSize / 2), 0.0f);
    work.assign (kFftSize, 0.0);
    spectrum.assign (kFftSize, {});
    fft.prepare (kFftSize);

    curMinHz = curMaxHz = 0;
    configuredMethod = -1;
    reset();
}

void PitchDetector::reset()
{
    for (auto& f : antiAlias)
        f.reset();
    std::fill (ring.begin(), ring.end(), 0.0f);
    ringPos = phase = sinceLast = 0;
    estimate = {};
    raptCount = 0;
    raptHasPrev = false;
    hopCount = 0;
    std::fill (std::begin (dioOut), std::end (dioOut), 0.0);
    harvestStream.reset();
    inputCount = harvestOrigin = 0;
}

void PitchDetector::configure (double minHz, double maxHz, int forMethod)
{
    curMinHz = minHz;
    curMaxHz = maxHz;
    configuredMethod = forMethod;
    minLag = std::max (2, static_cast<int> (std::floor (fs / maxHz)));
    maxLag = static_cast<int> (std::ceil (fs / minHz)) + 1;
    frameLength = std::min (kFftSize, 2 * maxLag);
    if (forMethod == swipe)
    {
        // SWIPE は、下限の周期の 8 つ分の窓を使う（設定が変わったときだけ、くしの重みなどを作り直す）
        frameLength = swipeLongestWindow (fs, minHz);
        configureSwipe (minHz, maxHz);
    }
    if (forMethod == rapt)
    {
        // RAPT は、7.5ms の窓と、それを一番長い周期までずらした分
        frameLength = raptWindow (fs, minHz) + maxLag;
        raptCount = 0;
        raptHasPrev = false;
    }
    if (forMethod == dio)
    {
        frameLength = dioDelayHops() * hop; // 音量を測る窓を兼ねる
        hopCount = 0;
    }
    if (forMethod == pyin)
        configurePyin (minHz, maxHz);
    if (forMethod == harvest)
    {
        harvestStream.setRange (minHz, maxHz);
        harvestStream.reset();
        harvestOrigin = inputCount;
    }
    delaySamples = delayFor (sr, decimation, fs, minHz, forMethod, frameLength);
}

int PitchDetector::analysisDelayFor (double sampleRate, const ParamValues& params)
{
    const int dec = std::max (1, static_cast<int> (std::lround (sampleRate / 12000.0)));
    const double fsDec = sampleRate / dec;
    const double minHz = params.get (P::pitchMinHz);
    const int maxLagDec = static_cast<int> (std::ceil (fsDec / minHz)) + 1;
    const int hopDec = std::max (1, static_cast<int> (std::lround (0.005 * fsDec)));
    const int method = std::clamp (params.index (P::pitchMethod), 0, numMethods - 1);
    int length = std::min (kFftSize, 2 * maxLagDec);
    if (method == swipe)
        length = swipeLongestWindow (fsDec, minHz);
    if (method == rapt)
        length = raptWindow (fsDec, minHz) + maxLagDec;
    if (method == dio)
        length = dioDelayHops() * hopDec;
    return delayFor (sampleRate, dec, fsDec, minHz, method, length);
}

void PitchDetector::push (const float* x, int n, const ParamValues& params)
{
    method = std::clamp (params.index (P::pitchMethod), 0, numMethods - 1);
    const double minHz = params.get (P::pitchMinHz);
    const double maxHz = std::max (minHz * 1.5, static_cast<double> (params.get (P::pitchMaxHz)));
    if (minHz != curMinHz || maxHz != curMaxHz || method != configuredMethod)
        configure (minHz, maxHz, method);
    if (method == harvest)
        harvestStream.push (x, n); // 元のサンプルレートのまま渡す（中で約 8kHz に間引く）

    const auto size = static_cast<int> (ring.size());
    for (int i = 0; i < n; ++i)
    {
        ++inputCount;
        const float y = antiAlias[1].process (antiAlias[0].process (x[i]));
        if (++phase < decimation)
            continue;
        phase = 0;

        ring[static_cast<size_t> (ringPos)] = y;
        ringPos = (ringPos + 1) % size;

        if (++sinceLast >= hop)
        {
            sinceLast = 0;
            analyze();
        }
    }
}

void PitchDetector::analyze()
{
    // 直近の窓を、古い順に取り出す
    const auto size = static_cast<int> (ring.size());
    double energy = 0;
    for (int k = 0; k < frameLength; ++k)
    {
        const float v = ring[static_cast<size_t> ((ringPos - frameLength + k + size) % size)];
        frame[static_cast<size_t> (k)] = v;
        energy += v * v;
    }
    ++count;

    estimate.levelDb = static_cast<float> (10 * std::log10 (energy / frameLength + 1.0e-12));

    // 前後のフレームを見て決める方式は、無音のときも毎回計算して、記録を進める
    switch (method)
    {
        case rapt:    analyzeRapt (estimate.levelDb < kSilenceDb); return;
        case pyin:    analyzePyin (estimate.levelDb < kSilenceDb); return;
        case dio:     analyzeDio(); return;
        case harvest: analyzeHarvest(); return;
        default:      break;
    }

    if (estimate.levelDb < kSilenceDb)
    {
        estimate.hz = 0;
        estimate.voiced = false;
        estimate.clarity = 0;
        return;
    }

    float clarity = 0;
    double period = 0;
    bool voiced = false;
    switch (method)
    {
        case autocorrelation: period = detectAutocorrelation (clarity); voiced = clarity > 0.7f; break;
        case yin:             period = detectYin (clarity);             voiced = clarity > 0.7f; break;
        case mcleod:          period = detectMcLeod (clarity);          voiced = clarity > 0.7f; break;
        case cepstrum:        period = detectCepstrum (clarity);        voiced = clarity > 0.5f; break;
        case sift:            period = detectSift (clarity);            voiced = clarity > 0.35f; break; // 合成音の評価で、見逃しと誤りの釣り合いが一番よかった値
        case swipe:           period = detectSwipe (clarity);           voiced = swipeVoiced; break;
        default: break;
    }

    estimate.hz = period > 0 ? static_cast<float> (fs / period) : 0.0f;
    estimate.voiced = voiced && period > 0;
    estimate.clarity = clarity;
}

double PitchDetector::detectAutocorrelation (float& clarity)
{
    // 正規化した相互相関（-1〜1）。前半の窓と、tau だけずらした窓が、どれだけ似ているか
    const int w = frameLength - maxLag;
    const float* x = frame.data();

    double e0 = 0;
    for (int j = 0; j < w; ++j)
        e0 += x[j] * x[j];

    double e1 = 0;
    for (int j = 0; j < w; ++j)
        e1 += x[j + minLag - 1] * x[j + minLag - 1];

    int best = -1;
    double bestValue = -2;
    for (int tau = minLag - 1; tau <= maxLag; ++tau)
    {
        if (tau > minLag - 1)
            e1 += x[tau + w - 1] * x[tau + w - 1] - x[tau - 1] * x[tau - 1]; // ずらした窓の大きさを1つずつ更新
        double r = 0;
        for (int j = 0; j < w; ++j)
            r += x[j] * x[j + tau];
        const double value = r / std::sqrt (e0 * e1 + 1.0e-12);
        work[static_cast<size_t> (tau)] = value;
        if (tau >= minLag && tau < maxLag && value > bestValue)
        {
            bestValue = value;
            best = tau;
        }
    }
    if (best < 0)
        return 0;

    // 一番高い山を選ぶだけ。周期の2倍、3倍のずれも同じくらい高くなるので、どれを選ぶかが揺れやすい
    clarity = static_cast<float> (bestValue);
    return best + parabolicOffset (work[static_cast<size_t> (best - 1)], work[static_cast<size_t> (best)], work[static_cast<size_t> (best + 1)]);
}

double PitchDetector::detectYin (float& clarity)
{
    // 差分関数 d(tau): tau だけずらした波形との差の2乗の和。周期で 0 に近づく
    const int w = frameLength - maxLag;
    const float* x = frame.data();
    auto& d = work;

    d[0] = 1;
    double running = 0;
    for (int tau = 1; tau <= maxLag; ++tau)
    {
        double sum = 0;
        for (int j = 0; j < w; ++j)
        {
            const double diff = x[j] - x[j + tau];
            sum += diff * diff;
        }
        // 累積平均で割って正規化する（ずれが小さいところで d が小さくなる偏りを消す）
        running += sum;
        d[static_cast<size_t> (tau)] = running > 0 ? sum * tau / running : 1;
    }

    // しきい値を下回る最初の谷を選ぶ。なければ、一番深い谷
    constexpr double threshold = 0.15;
    int best = -1;
    for (int tau = minLag; tau < maxLag; ++tau)
    {
        if (d[static_cast<size_t> (tau)] < threshold)
        {
            while (tau + 1 < maxLag && d[static_cast<size_t> (tau + 1)] < d[static_cast<size_t> (tau)])
                ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0)
    {
        best = minLag;
        for (int tau = minLag; tau < maxLag; ++tau)
            if (d[static_cast<size_t> (tau)] < d[static_cast<size_t> (best)])
                best = tau;
    }

    const auto b = static_cast<size_t> (best);
    clarity = static_cast<float> (std::clamp (1.0 - d[b], 0.0, 1.0));
    return best + parabolicOffset (d[b - 1], d[b], d[b + 1]);
}

double PitchDetector::detectMcLeod (float& clarity)
{
    // NSDF: 2 × 相関 ÷ 2つの窓の大きさの和（-1〜1）。重ねる長さは、ずらすほど短くなる
    const int n = frameLength;
    const float* x = frame.data();
    auto& nsdf = work;

    for (int tau = 0; tau <= maxLag; ++tau)
    {
        double r = 0, m = 0;
        for (int j = 0; j < n - tau; ++j)
        {
            r += x[j] * x[j + tau];
            m += x[j] * x[j] + x[j + tau] * x[j + tau];
        }
        nsdf[static_cast<size_t> (tau)] = m > 0 ? 2 * r / m : 0;
    }

    // 0 を下から上に横切ってから、上から下に横切るまでの区間ごとに、一番高い点（山）を拾う
    int peaks[64];
    int numPeaks = 0;
    double highest = 0;
    int tau = 1;
    while (tau < maxLag && nsdf[static_cast<size_t> (tau)] > 0) // 最初の山（ずれ 0 のまわり）を飛ばす
        ++tau;
    while (tau < maxLag && numPeaks < 64)
    {
        while (tau < maxLag && nsdf[static_cast<size_t> (tau)] <= 0)
            ++tau;
        int top = -1;
        while (tau < maxLag && nsdf[static_cast<size_t> (tau)] > 0)
        {
            if (top < 0 || nsdf[static_cast<size_t> (tau)] > nsdf[static_cast<size_t> (top)])
                top = tau;
            ++tau;
        }
        if (top >= minLag && top < maxLag)
        {
            peaks[numPeaks++] = top;
            highest = std::max (highest, nsdf[static_cast<size_t> (top)]);
        }
    }
    if (numPeaks == 0)
        return 0;

    // 一番高い山の 0.9 倍以上ある、最初の山を選ぶ
    int best = peaks[0];
    for (int i = 0; i < numPeaks; ++i)
        if (nsdf[static_cast<size_t> (peaks[i])] >= 0.9 * highest)
        {
            best = peaks[i];
            break;
        }

    const auto b = static_cast<size_t> (best);
    clarity = static_cast<float> (std::clamp (nsdf[b], 0.0, 1.0));
    return best + parabolicOffset (nsdf[b - 1], nsdf[b], nsdf[b + 1]);
}

double PitchDetector::detectCepstrum (float& clarity)
{
    // ハン窓をかけて FFT → 振幅の対数 → 逆 FFT。倍音が等間隔に並ぶと、その間隔（= 周期）に山が出る
    const int n = frameLength;
    for (int k = 0; k < kFftSize; ++k)
    {
        const double win = k < n ? 0.5 - 0.5 * std::cos (2 * kPi * k / (n - 1)) : 0.0;
        spectrum[static_cast<size_t> (k)] = { k < n ? frame[static_cast<size_t> (k)] * win : 0.0, 0.0 };
    }
    fft.forward (spectrum.data());
    for (auto& s : spectrum)
        s = { std::log (std::abs (s) + 1.0e-9), 0.0 };
    fft.inverse (spectrum.data());

    int best = -1;
    double bestValue = -1.0e9;
    for (int q = minLag; q < maxLag; ++q)
    {
        const double v = spectrum[static_cast<size_t> (q)].real();
        if (v > bestValue)
        {
            bestValue = v;
            best = q;
        }
    }
    if (best < 0)
        return 0;

    const auto b = static_cast<size_t> (best);
    // 山の高さを確からしさの目安にする（周期のはっきりした声で 0.5〜1 ほどになるように倍率をかける）
    clarity = static_cast<float> (std::clamp (bestValue * 4.0, 0.0, 1.0));
    return best + parabolicOffset (spectrum[b - 1].real(), spectrum[b].real(), spectrum[b + 1].real());
}

} // namespace vcab
