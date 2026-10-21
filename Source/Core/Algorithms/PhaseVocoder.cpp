#include "PhaseVocoder.h"

#include "../Dsp/Biquad.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr int kOverlap = 4;
constexpr double kTwoPi = 2 * kPi;

double wrapPhase (double p) { return p - kTwoPi * std::round (p / kTwoPi); }

// 包絡を、小数のビンの位置で読む（対数で直線補間）
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

int PhaseVocoder::fftSizeFor (const ParamValues& p)
{
    return 512 << std::clamp (p.index (P::pvFftSize), 0, 2); // 512 / 1024 / 2048
}

bool PhaseVocoder::supportsFormant (const ParamValues& p) const
{
    return p.index (P::formantMethod) != SpectralEnvelope::none;
}

void PhaseVocoder::prepare (double sampleRate, int)
{
    sr = sampleRate;
    size = 0;
    configure (1024);
}

void PhaseVocoder::configure (int fftSize)
{
    // 窓の長さを変えたときだけ呼ばれる（つまみを動かした瞬間に、一度だけ配列を作り直す）
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
    for (auto* v : { &mag, &phase, &freq, &lastPhase, &outMag, &outPhase, &outFreq, &lastOutPhase, &env })
        v->assign (static_cast<size_t> (half + 1), 0.0);
    peaks.reserve (static_cast<size_t> (half + 1));
    reset();
}

void PhaseVocoder::reset()
{
    std::fill (inFifo.begin(), inFifo.end(), 0.0f);
    std::fill (outFifo.begin(), outFifo.end(), 0.0f);
    std::fill (outAccum.begin(), outAccum.end(), 0.0);
    std::fill (lastPhase.begin(), lastPhase.end(), 0.0);
    std::fill (lastOutPhase.begin(), lastOutPhase.end(), 0.0);
    rover = latency;
}

void PhaseVocoder::process (float* data, int n, const ParamValues& params)
{
    const int wanted = fftSizeFor (params);
    if (wanted != size)
        configure (wanted);

    for (int i = 0; i < n; ++i)
    {
        // 入力をためて、出力は窓の 3/4 だけ遅れたものを出す
        inFifo[static_cast<size_t> (rover)] = data[i];
        data[i] = outFifo[static_cast<size_t> (rover - latency)];
        if (++rover >= size)
        {
            rover = latency;
            processFrame (params);
        }
    }
}

void PhaseVocoder::processFrame (const ParamValues& params)
{
    const double ratio = semitonesToRatio (params.get (P::pitch));
    const double formantRatio = semitonesToRatio (params.get (P::formant));
    const int envMethod = std::clamp (params.index (P::formantMethod), 0, 3);
    const bool phaseLock = params.on (P::pvPhaseLock);
    const double expected = kTwoPi * hop / size; // 1つ隣のビンの、1回ずらす間の位相の進み

    // ---- 分析: 窓をかけて FFT、振幅と本当の周波数 ----
    for (int i = 0; i < size; ++i)
        spec[static_cast<size_t> (i)] = { inFifo[static_cast<size_t> (i)] * window[static_cast<size_t> (i)], 0.0 };
    fft.forward (spec.data());

    double maxMag = 0;
    for (int k = 0; k <= half; ++k)
    {
        const auto kk = static_cast<size_t> (k);
        mag[kk] = std::abs (spec[kk]);
        phase[kk] = std::arg (spec[kk]);
        // 位相の進みから、ビンの中心からのずれを求める
        const double delta = wrapPhase (phase[kk] - lastPhase[kk] - k * expected);
        lastPhase[kk] = phase[kk];
        freq[kk] = k + delta / expected; // 本当の周波数（ビンの単位）
        maxMag = std::max (maxMag, mag[kk]);
    }

    // ---- 移す ----
    std::fill (outMag.begin(), outMag.end(), 0.0);
    std::fill (outFreq.begin(), outFreq.end(), -1.0);

    if (phaseLock)
    {
        // 倍音の山を探す（両隣より大きく、最大から -80dB 以内）
        peaks.clear();
        for (int k = 1; k < half; ++k)
        {
            const auto kk = static_cast<size_t> (k);
            if (mag[kk] > mag[kk - 1] && mag[kk] >= mag[kk + 1] && mag[kk] > maxMag * 1.0e-4)
                peaks.push_back (k);
        }

        // 山ごとに、隣の山との谷までを1つの組として、形を崩さずに移す
        for (size_t p = 0; p < peaks.size(); ++p)
        {
            const int pk = peaks[p];
            int lo = 0, hi = half;
            if (p > 0)
            {
                lo = peaks[p - 1];
                for (int k = peaks[p - 1]; k < pk; ++k)
                    if (mag[static_cast<size_t> (k)] < mag[static_cast<size_t> (lo)])
                        lo = k;
                ++lo;
            }
            if (p + 1 < peaks.size())
            {
                hi = pk;
                for (int k = pk; k < peaks[p + 1]; ++k)
                    if (mag[static_cast<size_t> (k)] < mag[static_cast<size_t> (hi)])
                        hi = k;
            }

            const int target = static_cast<int> (std::lround (pk * ratio));
            if (target < 0 || target > half)
                continue;
            // 山の位相は、前の窓の同じ位置の位相から、移した後の周波数で進める
            const double peakPhase = lastOutPhase[static_cast<size_t> (target)] + kTwoPi * freq[static_cast<size_t> (pk)] * ratio * hop / size;
            for (int k = lo; k <= hi; ++k)
            {
                const int j = target + (k - pk);
                if (j < 0 || j > half || mag[static_cast<size_t> (k)] <= outMag[static_cast<size_t> (j)])
                    continue;
                outMag[static_cast<size_t> (j)] = mag[static_cast<size_t> (k)];
                // 周りのビンは、元の山との位相の差を保つ
                outPhase[static_cast<size_t> (j)] = peakPhase + (phase[static_cast<size_t> (k)] - phase[static_cast<size_t> (pk)]);
                outFreq[static_cast<size_t> (j)] = 0; // 位相を決めた印
            }
        }
        for (int j = 0; j <= half; ++j)
            if (outFreq[static_cast<size_t> (j)] < 0) // 何も来なかったビンは、中心の周波数で位相を進めておく
                outPhase[static_cast<size_t> (j)] = lastOutPhase[static_cast<size_t> (j)] + j * expected;
    }
    else
    {
        // ビンごとに移す。同じ位置に複数来たら、大きいほうを使う
        for (int k = 0; k <= half; ++k)
        {
            const int j = static_cast<int> (std::lround (k * ratio));
            if (j > half)
                break;
            if (mag[static_cast<size_t> (k)] > outMag[static_cast<size_t> (j)])
            {
                outMag[static_cast<size_t> (j)] = mag[static_cast<size_t> (k)];
                outFreq[static_cast<size_t> (j)] = freq[static_cast<size_t> (k)] * ratio;
            }
        }
        for (int j = 0; j <= half; ++j)
        {
            const auto jj = static_cast<size_t> (j);
            const double f = outFreq[jj] >= 0 ? outFreq[jj] : j;
            outPhase[jj] = lastOutPhase[jj] + kTwoPi * f * hop / size;
        }
    }

    // ---- フォルマント処理 ----
    if (envMethod != SpectralEnvelope::none)
    {
        envelope.estimate (envMethod, mag.data(), env.data());
        warp.set (params, formantTrack);
        const double binHz = sr / size;
        // ピッチを上げると、フォルマントの山に入る倍音の数が減って音が小さくなる（下げると逆）。その分を補う
        const double density = std::sqrt (ratio);
        for (int j = 0; j <= half; ++j)
        {
            // 移した後のスペクトルの包絡は env(j / ratio)。それを外して、env(j / formantRatio) をかけ直す
            // （動かし方が「低い側を残す」のときは、j に来る元の位置を FormantWarp で求める）
            const double from = warp.isUniform() ? j / formantRatio : warp.inverse (j * binHz) / binHz;
            const double gain = envAt (env, from) / envAt (env, j / ratio);
            outMag[static_cast<size_t> (j)] *= std::min (gain, 16.0) * density; // 持ち上げすぎない（+24dB まで）
        }
    }

    // ---- 合成: 逆 FFT して、窓をかけて重ね足す ----
    for (int j = 0; j <= half; ++j)
    {
        const auto jj = static_cast<size_t> (j);
        outPhase[jj] = wrapPhase (outPhase[jj]);
        lastOutPhase[jj] = outPhase[jj];
        spec[jj] = std::polar (outMag[jj], outPhase[jj]);
    }
    spec[0] = { spec[0].real(), 0.0 };
    spec[static_cast<size_t> (half)] = { spec[static_cast<size_t> (half)].real(), 0.0 };
    for (int j = half + 1; j < size; ++j)
        spec[static_cast<size_t> (j)] = std::conj (spec[static_cast<size_t> (size - j)]);
    fft.inverse (spec.data());

    // ハン窓を2回（分析と合成）かけて 4 倍で重ねると、和が 1.5 になるので割る
    const double norm = 1.0 / 1.5;
    for (int i = 0; i < size; ++i)
        outAccum[static_cast<size_t> (i)] += spec[static_cast<size_t> (i)].real() * window[static_cast<size_t> (i)] * norm;

    for (int i = 0; i < hop; ++i)
        outFifo[static_cast<size_t> (i)] = static_cast<float> (outAccum[static_cast<size_t> (i)]);
    std::copy (outAccum.begin() + hop, outAccum.end(), outAccum.begin());
    std::fill (outAccum.end() - hop, outAccum.end(), 0.0);
    std::copy (inFifo.begin() + hop, inFifo.end(), inFifo.begin());
}

int PhaseVocoder::latencySamples (const ParamValues& params) const
{
    const int n = fftSizeFor (params);
    return n - n / kOverlap;
}

} // namespace vcab
