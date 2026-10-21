#include "BandShifter.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr double kLowHz = 60, kHighHz = 16000;
constexpr double kEnergySeconds = 0.010; // 帯域の大きさをならす時間
constexpr int kGainInterval = 32;        // 包絡の補正を計算し直す間隔（サンプル数）
} // namespace

void BandShifter::prepare (double sampleRate, int)
{
    sr = sampleRate;
    fft.prepare (kFftSize);
    envelope.prepare (kFftSize, sr);
    fftInput.assign (kFftSize, 0.0f);
    spec.assign (kFftSize, {});
    mag.assign (kFftSize / 2 + 1, 0.0);
    env.assign (kFftSize / 2 + 1, 1.0);
    window.resize (kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[static_cast<size_t> (i)] = 0.5 - 0.5 * std::cos (2 * kPi * i / kFftSize);
    numBands = 0;
    reset();
}

void BandShifter::reset()
{
    hilbert.reset();
    for (auto& b : bands)
    {
        for (auto* f : { &b.filterI[0], &b.filterI[1], &b.filterQ[0], &b.filterQ[1] })
            f->reset();
        b.rotor = { 1, 0 };
        b.energy = 0;
        b.gain = b.gainTarget = 1;
    }
    std::fill (fftInput.begin(), fftInput.end(), 0.0f);
    fftPos = sinceFft = sinceGains = 0;
    envReady = false;
}

void BandShifter::configure (int count)
{
    const double top = std::min (kHighHz, sr * 0.45);
    bandRatio = std::pow (top / kLowHz, 1.0 / count);
    const double q = std::sqrt (bandRatio) / (bandRatio - 1.0);

    for (int i = 0; i < count; ++i)
    {
        auto& b = bands[static_cast<size_t> (i)];
        b.centre = kLowHz * std::pow (bandRatio, i + 0.5);
        const auto c = BiquadCoeffs::bandPass (sr, b.centre, q);
        for (auto* f : { &b.filterI[0], &b.filterI[1], &b.filterQ[0], &b.filterQ[1] })
            f->setCoeffs (c);
    }

    // 隣り合う帯域は、境目で位相が逆向きにずれていて、同じ向きで足すと打ち消し合う。チャンネルボコーダと同じく、
    // 1つおきに符号を反転して足し、残りの差を、全帯域のフィルタを位相込みで足した特性の平均で補う
    double sum = 0;
    constexpr int points = 200;
    for (int p = 0; p < points; ++p)
    {
        const double f = kLowHz * std::pow (top / kLowHz, (p + 0.5) / points);
        const auto z = std::polar (1.0, -2 * kPi * f / sr);
        std::complex<double> total;
        for (int i = 0; i < count; ++i)
        {
            const auto c = BiquadCoeffs::bandPass (sr, bands[static_cast<size_t> (i)].centre, q);
            const auto h = (c.b0 + c.b1 * z + c.b2 * z * z) / (1.0 + c.a1 * z + c.a2 * z * z);
            total += (i % 2 == 0 ? 1.0 : -1.0) * h * h;
        }
        sum += std::norm (total);
    }
    makeup = 1.0 / std::sqrt (std::max (sum / points, 1.0e-6));
    numBands = count;
}

double BandShifter::bandEnvelopeAt (double hz) const
{
    // 帯域の番号（小数）に直して、隣り合う帯域の大きさを対数で補間する
    const double idx = std::log (hz / kLowHz) / std::log (bandRatio) - 0.5;
    const int last = numBands - 1;
    auto energyAt = [this] (int i) { return bands[static_cast<size_t> (i)].energy + 1.0e-14; };
    if (idx <= 0) return std::sqrt (energyAt (0));
    if (idx >= last) return std::sqrt (energyAt (last));
    const int i0 = static_cast<int> (idx);
    const double f = idx - i0;
    return std::exp (0.5 * (std::log (energyAt (i0)) * (1 - f) + std::log (energyAt (i0 + 1)) * f));
}

double BandShifter::fftEnvelopeAt (double hz) const
{
    const double bin = hz * kFftSize / sr;
    const int last = kFftSize / 2;
    if (bin <= 0) return env[0];
    if (bin >= last) return env[static_cast<size_t> (last)];
    const int i0 = static_cast<int> (bin);
    const double f = bin - i0;
    return std::exp (std::log (env[static_cast<size_t> (i0)]) * (1 - f) + std::log (env[static_cast<size_t> (i0 + 1)]) * f);
}

void BandShifter::analyzeFft (int method)
{
    // 直近 1024 サンプルに窓をかけて FFT → 包絡（音は通さない。分析だけ）
    for (int i = 0; i < kFftSize; ++i)
        spec[static_cast<size_t> (i)] = { fftInput[static_cast<size_t> ((fftPos + i) % kFftSize)] * window[static_cast<size_t> (i)], 0.0 };
    fft.forward (spec.data());
    for (int k = 0; k <= kFftSize / 2; ++k)
        mag[static_cast<size_t> (k)] = std::abs (spec[static_cast<size_t> (k)]);
    const int envMethod = method == envCepstrum ? SpectralEnvelope::cepstrum
                        : method == envLpc      ? SpectralEnvelope::lpc
                                                : SpectralEnvelope::trueEnvelope;
    envelope.estimate (envMethod, mag.data(), env.data());
    envReady = true;
}

void BandShifter::updateGains (const ParamValues& params, int method, double ratio, double formantRatio)
{
    warp.set (params, formantTrack);
    // 帯域 i の中身は、ずらした後に centre × ratio の位置に来る。そこに乗っている包絡は元の位置 centre のもの。
    // それを外して、centre × ratio / formantRatio の位置の包絡（フォルマントの倍率で動かした包絡）をかける。
    // ピッチを上げると、フォルマントの山に入る倍音の数が減って小さくなるので、その分も補う（フェーズボコーダと同じ）
    const double density = std::sqrt (ratio);
    for (int i = 0; i < numBands; ++i)
    {
        auto& b = bands[static_cast<size_t> (i)];
        if (method == envNone)
        {
            b.gainTarget = 1;
            continue;
        }
        // （動かし方が「低い側を残す」のときは、centre × ratio に来る元の位置を FormantWarp で求める）
        const double to = warp.isUniform() ? b.centre * ratio / formantRatio : warp.inverse (b.centre * ratio);
        double g = 1;
        if (method == envBands)
            g = bandEnvelopeAt (to) / bandEnvelopeAt (b.centre);
        else if (envReady)
            g = fftEnvelopeAt (to) / fftEnvelopeAt (b.centre);
        b.gainTarget = std::min (g, 16.0) * density; // 持ち上げすぎない（+24dB まで）
    }
}

void BandShifter::process (float* data, int n, const ParamValues& params)
{
    const int count = std::clamp (static_cast<int> (params.get (P::bsBands)), 8, kMaxBands);
    if (count != numBands)
        configure (count);

    const double ratio = semitonesToRatio (params.get (P::pitch));
    const double formantRatio = semitonesToRatio (params.get (P::formant));
    const int method = std::clamp (params.index (P::bsFormantMethod), 0, 4);
    const double energyCoeff = 1.0 - std::exp (-1.0 / (kEnergySeconds * sr));
    const double gainCoeff = 1.0 - std::exp (-1.0 / (0.005 * sr)); // 補正の変化を約 5ms でならす

    // 帯域ごとの、1サンプルあたりの回転（中心周波数 × (倍率 - 1) Hz）
    for (int i = 0; i < numBands; ++i)
    {
        auto& b = bands[static_cast<size_t> (i)];
        b.step = std::polar (1.0, 2 * kPi * b.centre * (ratio - 1.0) / sr);
    }

    for (int s = 0; s < n; ++s)
    {
        const double x = data[s];

        if (method >= envCepstrum)
        {
            fftInput[static_cast<size_t> (fftPos)] = static_cast<float> (x);
            fftPos = (fftPos + 1) % kFftSize;
            if (++sinceFft >= kFftHop)
            {
                sinceFft = 0;
                analyzeFft (method);
            }
        }
        if (++sinceGains >= kGainInterval)
        {
            sinceGains = 0;
            updateGains (params, method, ratio, formantRatio);
        }

        double inI = 0, inQ = 0;
        hilbert.process (x, inI, inQ);

        double out = 0;
        for (int i = 0; i < numBands; ++i)
        {
            auto& b = bands[static_cast<size_t> (i)];
            const double bi = b.filterI[1].process (b.filterI[0].process (static_cast<float> (inI)));
            const double bq = b.filterQ[1].process (b.filterQ[0].process (static_cast<float> (inQ)));
            b.energy += energyCoeff * (bi * bi + bq * bq - b.energy);

            // ずらす: I cos + Q sin（回転は、複素数をかけ続けて進める）
            b.rotor *= b.step;
            b.gain += gainCoeff * (b.gainTarget - b.gain);
            const double y = (bi * b.rotor.real() + bq * b.rotor.imag()) * b.gain;
            out += (i % 2 == 0) ? y : -y;
        }
        data[s] = static_cast<float> (out * makeup);
    }

    // 回転の大きさが計算誤差で 1 からずれていかないように、塊ごとに直す
    for (int i = 0; i < numBands; ++i)
    {
        auto& b = bands[static_cast<size_t> (i)];
        b.rotor /= std::abs (b.rotor);
    }
}

int BandShifter::latencySamples (const ParamValues&) const
{
    // 全体としての遅れ（雑音を通したときの波形の相互相関で約 0.4ms）。ただしバンドパスは低い帯域ほど細く、
    // 遅れが大きい（100Hz の帯域では数十 ms）ので、低い音ほど遅れて出る
    return static_cast<int> (0.001 * sr);
}

} // namespace vcab
