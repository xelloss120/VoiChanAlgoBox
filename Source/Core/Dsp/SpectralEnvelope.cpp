#include "SpectralEnvelope.h"

#include "Lpc.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr double kFloor = 1.0e-9;
constexpr int kTrueEnvelopeIterations = 12;
} // namespace

void SpectralEnvelope::prepare (int fftSize, double sampleRate)
{
    n = fftSize;
    half = n / 2;
    sr = sampleRate;
    // 声の高さの周期の半分より短いところだけを残す（倍音のギザギザを消し、フォルマントの山並みは残す）。
    // 2.5ms は、声の高さ 200Hz（周期 5ms）までの入力に合う。男性の声を入力する前提の値
    lifter = std::min (half - 1, static_cast<int> (sr * 0.0025));
    // LPC の次数: 1kHz ごとにフォルマント1つ分（山と谷で2次）が目安。高すぎると倍音を拾う
    lpcOrder = std::clamp (static_cast<int> (sr / 1200.0), 8, 48);

    fft.prepare (n);
    buf.assign (static_cast<size_t> (n), {});
    logMag.assign (static_cast<size_t> (half + 1), 0.0);
    smooth.assign (static_cast<size_t> (half + 1), 0.0);
    lifted.assign (static_cast<size_t> (half + 1), 0.0);
    autocorr.assign (static_cast<size_t> (lpcOrder + 1), 0.0);
    coeffs.assign (static_cast<size_t> (lpcOrder + 1), 0.0);
    tmp.assign (static_cast<size_t> (lpcOrder + 1), 0.0);
    reflection.assign (static_cast<size_t> (lpcOrder + 1), 0.0);
}

void SpectralEnvelope::cepstralSmooth (const double* in, double* out)
{
    // 対数振幅を左右対称に並べて逆 FFT → ケプストラム。低いケフレンシーだけ残して FFT で戻す
    for (int k = 0; k <= half; ++k)
        buf[static_cast<size_t> (k)] = { in[k], 0.0 };
    for (int k = half + 1; k < n; ++k)
        buf[static_cast<size_t> (k)] = { in[n - k], 0.0 };
    fft.inverse (buf.data());
    for (int q = lifter + 1; q < n - lifter; ++q)
        buf[static_cast<size_t> (q)] = {};
    fft.forward (buf.data());
    for (int k = 0; k <= half; ++k)
        out[k] = buf[static_cast<size_t> (k)].real();
}

void SpectralEnvelope::lpcEnvelope (const double* mag, double* env)
{
    // パワースペクトルを逆 FFT すると自己相関になる（ウィーナー・ヒンチンの定理）
    for (int k = 0; k <= half; ++k)
        buf[static_cast<size_t> (k)] = { mag[k] * mag[k], 0.0 };
    for (int k = half + 1; k < n; ++k)
        buf[static_cast<size_t> (k)] = { mag[n - k] * mag[n - k], 0.0 };
    fft.inverse (buf.data());
    for (int i = 0; i <= lpcOrder; ++i)
        autocorr[static_cast<size_t> (i)] = buf[static_cast<size_t> (i)].real();
    autocorr[0] *= 1.0 + 1.0e-6; // わずかに持ち上げて、計算を安定させる

    // レビンソン・ダービンの再帰で、予測係数 a を求める（LPC の方式と共有）
    lpc::levinson (autocorr.data(), lpcOrder, coeffs.data(), reflection.data(), tmp.data());

    // 包絡は 1 / |A| の形。A の周波数特性は、係数を並べて FFT すれば求まる
    for (int i = 0; i < n; ++i)
        buf[static_cast<size_t> (i)] = { i <= lpcOrder ? coeffs[static_cast<size_t> (i)] : 0.0, 0.0 };
    fft.forward (buf.data());
    double sumMag = 0, sumEnv = 0;
    for (int k = 0; k <= half; ++k)
    {
        env[k] = 1.0 / std::max (std::abs (buf[static_cast<size_t> (k)]), 1.0e-6);
        sumMag += mag[k] * mag[k];
        sumEnv += env[k] * env[k];
    }
    // 大きさは、元のスペクトルと全体のエネルギーが同じになるように合わせる
    const double scale = sumEnv > 0 ? std::sqrt (sumMag / sumEnv) : 0.0;
    for (int k = 0; k <= half; ++k)
        env[k] = std::max (env[k] * scale, kFloor);
}

void SpectralEnvelope::estimate (int method, const double* mag, double* env)
{
    switch (method)
    {
        case cepstrum:
        {
            for (int k = 0; k <= half; ++k)
                logMag[static_cast<size_t> (k)] = std::log (mag[k] + kFloor);
            cepstralSmooth (logMag.data(), smooth.data());
            for (int k = 0; k <= half; ++k)
                env[k] = std::exp (smooth[static_cast<size_t> (k)]);
            break;
        }
        case lpc:
            lpcEnvelope (mag, env);
            break;
        case trueEnvelope:
        {
            // 滑らかにしては、元より低いところを元の値まで持ち上げる。倍音の頂点をなぞるようになる
            for (int k = 0; k <= half; ++k)
                logMag[static_cast<size_t> (k)] = std::log (mag[k] + kFloor);
            auto& target = lifted; // 持ち上げた対数振幅
            std::copy (logMag.begin(), logMag.end(), target.begin());
            for (int it = 0; it < kTrueEnvelopeIterations; ++it)
            {
                cepstralSmooth (target.data(), smooth.data());
                for (int k = 0; k <= half; ++k)
                    target[static_cast<size_t> (k)] = std::max (logMag[static_cast<size_t> (k)], smooth[static_cast<size_t> (k)]);
            }
            for (int k = 0; k <= half; ++k)
                env[k] = std::exp (smooth[static_cast<size_t> (k)]);
            break;
        }
        default:
            for (int k = 0; k <= half; ++k)
                env[k] = 1.0;
            break;
    }
}

} // namespace vcab
