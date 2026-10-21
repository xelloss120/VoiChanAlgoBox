#include "SpectralDenoise.h"

#include "DenoiseDetail.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace vcab
{

using namespace denoise_detail;

namespace
{
void regenerateHarmonics (Fft& fft, int size, const std::vector<Fft::Complex>& spec, std::vector<Fft::Complex>& work,
                          std::vector<float>& gain, const std::vector<float>& power, const float* est, const DenoiseSettings& s)
{
    // HRNR（Plapous ら 2006）: ウィーナーで下げた音（窓をかけた 1 フレーム）を時間に戻し、半波整流して倍音を作り直す
    const int half = size / 2;
    for (int k = 0; k <= half; ++k)
        work[static_cast<size_t> (k)] = spec[static_cast<size_t> (k)] * static_cast<double> (gain[static_cast<size_t> (k)]);
    for (int k = 1; k < half; ++k)
        work[static_cast<size_t> (size - k)] = std::conj (work[static_cast<size_t> (k)]);
    fft.inverse (work.data());
    for (auto& v : work)
        v = { std::max (v.real(), 0.0), 0.0 };
    fft.forward (work.data());

    // 作り直した倍音と、ウィーナーの結果を半々に合わせた SNR で、下げる量を決め直す
    constexpr double rho = 0.5;
    const double strength = std::pow (10.0, s.strengthDb / 10.0);
    const auto floorGain = static_cast<float> (std::pow (10.0, -s.maxReductionDb / 20.0));
    for (int k = 0; k <= half; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const double clean = static_cast<double> (gain[i]) * gain[i] * power[i];
        const double harmo = std::norm (work[i]);
        const double snr = (rho * clean + (1.0 - rho) * harmo) / std::max (est[i] * strength, 1.0e-30);
        gain[i] = std::max (static_cast<float> (snr / (1.0 + snr)), floorGain);
    }
}


void protectHarmonics (std::vector<float>& gain, int size, double sr, float pitchHz, bool pitchVoiced, const DenoiseSettings& s)
{
    // 倍音の位置（5kHz まで）の前後 1 区画で、残す割合をゆるめる（中心ほど強く）
    if (! pitchVoiced || pitchHz <= 0)
        return;
    const double binHz = sr / size;
    const int half = size / 2;
    for (int h = 1; h * pitchHz < 5000.0; ++h)
    {
        const double centre = h * pitchHz / binHz;
        for (int k = static_cast<int> (std::floor (centre)) - 1; k <= static_cast<int> (std::ceil (centre)) + 1; ++k)
        {
            if (k < 0 || k > half)
                continue;
            const double w = std::max (0.0, 1.0 - std::abs (k - centre));
            if (w <= 0)
                continue;
            auto& g = gain[static_cast<size_t> (k)];
            g = static_cast<float> (std::pow (static_cast<double> (g), 1.0 - s.protect * w));
        }
    }
}
} // namespace



void SpectralGain::prepare (int numBins, double hopSeconds, double binHzIn)
{
    bins = numBins;
    binHz = binHzIn;
    // 判定指向型の重み。もとの論文の 0.98 を、区切りの間隔 8ms のときの値とみなして、間隔に合わせる
    ddAlpha = std::pow (0.98, hopSeconds / 0.008);
    // Cohen の声がない確率: ξ をならす重み（論文の 0.7 を、間隔 16ms のときの値とみなす）
    zetaAlpha = std::pow (0.7, hopSeconds / 0.016);
    const auto b = static_cast<size_t> (bins);
    prevClean.assign (b, 0.0f);
    xiNow.assign (b, 0.0f);
    vNow.assign (b, 0.0f);
    zeta.assign (b, 0.0f);
    absence.assign (b, 0.5f);
    work.assign (b + 1, 0.0);
    // バーク（聴覚の帯域）の番号（Zwicker・Terhardt の近似）
    bark.assign (b, 0);
    numBark = 1;
    for (int k = 0; k < bins; ++k)
    {
        const double f = k * binHz;
        const double z = 13.0 * std::atan (0.00076 * f) + 3.5 * std::atan ((f / 7500.0) * (f / 7500.0));
        bark[static_cast<size_t> (k)] = std::min (24, static_cast<int> (z));
        numBark = std::max (numBark, bark[static_cast<size_t> (k)] + 1);
    }
    barkEnergy.assign (static_cast<size_t> (numBark), 0.0);
    barkThreshold.assign (static_cast<size_t> (numBark), 0.0);
    barkCount.assign (static_cast<size_t> (numBark), 0);
    for (int k = 0; k < bins; ++k)
        ++barkCount[static_cast<size_t> (bark[static_cast<size_t> (k)])];
    // 広がり方（Schroeder の式）を前もって計算する
    spreading.assign (static_cast<size_t> (numBark * numBark), 0.0);
    for (int i = 0; i < numBark; ++i)
        for (int j = 0; j < numBark; ++j)
        {
            const double dz = i - j + 0.474;
            const double db = 15.81 + 7.5 * dz - 17.5 * std::sqrt (1.0 + dz * dz);
            spreading[static_cast<size_t> (i * numBark + j)] = std::pow (10.0, db / 10.0);
        }
}

void SpectralGain::reset()
{
    std::fill (prevClean.begin(), prevClean.end(), 0.0f);
    std::fill (zeta.begin(), zeta.end(), 0.0f);
    std::fill (absence.begin(), absence.end(), 0.5f);
}

namespace
{
// 過剰減算の量（Berouti 1979）: SNR が低いほど多めに引く。−5dB で 4.75、20dB で 1
double beroutiAlpha (double snrDb)
{
    if (snrDb < -5.0)
        return 4.75;
    if (snrDb > 20.0)
        return 1.0;
    return 4.0 - 3.0 / 20.0 * snrDb;
}
} // namespace

void SpectralGain::compute (const float* power, const float* noise, Rule rule, const DenoiseSettings& s, float* gain)
{
    const double strength = std::pow (10.0, s.strengthDb / 10.0);
    const auto floorGain = static_cast<float> (std::pow (10.0, -s.maxReductionDb / 20.0));

    // 過剰減算と帯域ごとの減算: フレーム全体（帯域ごと）の SNR から、引く量を決める
    double frameAlpha = 1.0;
    double bandAlpha[4] = { 1, 1, 1, 1 };
    const double bandEdges[4] = { 1000.0, 2000.0, 4000.0, 1.0e9 };
    auto bandOf = [&] (int k) { const double f = k * binHz; return f < bandEdges[0] ? 0 : (f < bandEdges[1] ? 1 : (f < bandEdges[2] ? 2 : 3)); };
    if (rule == berouti || rule == multiband)
    {
        double sp = 0, sn = 0, bp[4] = {}, bn[4] = {};
        for (int k = 0; k < bins; ++k)
        {
            sp += power[k];
            sn += noise[k] * strength;
            bp[bandOf (k)] += power[k];
            bn[bandOf (k)] += noise[k] * strength;
        }
        frameAlpha = beroutiAlpha (10.0 * std::log10 ((sp + 1.0e-20) / (sn + 1.0e-20)));
        for (int b = 0; b < 4; ++b)
            bandAlpha[b] = beroutiAlpha (10.0 * std::log10 ((bp[b] + 1.0e-20) / (bn[b] + 1.0e-20)));
    }

    // 聴覚のマスキング（Virag 1999）: 声の見積もりから、聞こえなくなる大きさ（マスキングのしきい値）を求め、
    // しきい値が高い（ノイズが隠れやすい）区画ほど引く量を減らす
    if (rule == masking)
    {
        std::fill (barkEnergy.begin(), barkEnergy.end(), 0.0);
        double logSum = 0, sum = 0;
        for (int k = 0; k < bins; ++k)
        {
            const double clean = std::max (power[k] - noise[k] * strength, power[k] * 0.01);
            barkEnergy[static_cast<size_t> (bark[static_cast<size_t> (k)])] += clean;
            logSum += std::log (clean + 1.0e-20);
            sum += clean;
        }
        // 音の調子らしさ（スペクトルの平らさから。平らなら雑音らしい）
        const double gm = std::exp (logSum / bins), am = sum / bins + 1.0e-20;
        const double sfmDb = 10.0 * std::log10 (gm / am + 1.0e-20);
        const double tonality = std::min (1.0, sfmDb / -60.0);
        for (int i = 0; i < numBark; ++i)
        {
            double c = 0;
            for (int j = 0; j < numBark; ++j)
                c += barkEnergy[static_cast<size_t> (j)] * spreading[static_cast<size_t> (i * numBark + j)];
            const double offset = tonality * (14.5 + i) + (1.0 - tonality) * 5.5;
            barkThreshold[static_cast<size_t> (i)] = c * std::pow (10.0, -offset / 10.0) / std::max (1, barkCount[static_cast<size_t> (i)]);
        }
        // 引く量は、しきい値とノイズの比で決める: ノイズがしきい値より下（声に隠れて聞こえない）なら 1、
        // しきい値がノイズより 30dB 以上低い（ノイズがよく聞こえる）なら 6。間は dB で直線
        // （論文はフレームの中の最大と最小で割り振るが、それだとノイズだけのところでも引く量がばらつき、ピロピロが増えた）
        for (int k = 0; k < bins; ++k)
        {
            const double t = barkThreshold[static_cast<size_t> (bark[static_cast<size_t> (k)])] + 1.0e-20;
            const double ratioDb = std::clamp (10.0 * std::log10 (t / std::max (noise[k] * strength, 1.0e-20)), -30.0, 0.0);
            work[static_cast<size_t> (k)] = 6.0 - 5.0 * (ratioDb + 30.0) / 30.0;
        }
    }

    for (int k = 0; k < bins; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const double gamma = std::max (power[k] / std::max (noise[k] * strength, 1.0e-30), 1.0e-6);
        double g, xi;
        if (rule == subtraction || rule == berouti || rule == multiband || rule == masking)
        {
            // スペクトル減算（パワー）: 残す割合 = √(1 − 引く量 × ノイズ / 今の音)
            double alpha = 1.0;
            if (rule == berouti)
                alpha = frameAlpha;
            else if (rule == multiband)
            {
                // Kamath・Loizou 2002: 帯域ごとの SNR の引く量に、帯域の重み（1kHz まで 1、上の端の 2kHz は 1.5、その間 2.5）をかける
                const double f = k * binHz, top = binHz * (bins - 1);
                const double delta = f <= 1000.0 ? 1.0 : (f > top - 2000.0 ? 1.5 : 2.5);
                alpha = bandAlpha[bandOf (k)] * delta;
            }
            else if (rule == masking)
                alpha = work[i];
            g = std::sqrt (std::max (1.0 - alpha / gamma, 0.0));
            xi = std::max (gamma - 1.0, 0.0);
        }
        else
        {
            // 判定指向型: 声の SNR を、直前のフレームの結果と、今のフレームの結果の重み付きの平均で見積もる
            xi = std::max (ddAlpha * prevClean[i] + (1.0 - ddAlpha) * std::max (gamma - 1.0, 0.0), 1.0e-6);
            const double v = xi / (1.0 + xi) * gamma;
            if (rule == mmseStsa)
            {
                // 振幅の平均二乗誤差を最小にする（Ephraim・Malah 1984）
                g = std::sqrt (kPi) / 2.0 * std::sqrt (v) / gamma
                    * ((1.0 + v) * besselI0e (v / 2.0) + v * besselI1e (v / 2.0));
            }
            else if (rule == logMmse)
            {
                // 対数振幅の平均二乗誤差を最小にする（Ephraim・Malah 1985）
                g = xi / (1.0 + xi) * std::exp (0.5 * expIntE1 (v));
            }
            else if (rule == tsnr)
            {
                // 2 段階（Plapous ら 2006）: 判定指向型のウィーナーで一度下げた結果から、今のフレームの SNR を求め直す。
                // 判定指向型の「一拍遅れ」（声の出だしで、前のフレームに引きずられる）を取り戻す
                const double gDd = xi / (1.0 + xi);
                const double xiTsnr = gDd * gDd * gamma;
                g = xiTsnr / (1.0 + xiTsnr);
                xi = std::max (xiTsnr, 1.0e-6);
            }
            else
            {
                g = xi / (1.0 + xi);
            }
            g = std::min (g, 1.0);
            prevClean[i] = static_cast<float> (g * g * gamma);
        }
        xiNow[i] = static_cast<float> (xi);
        vNow[i] = static_cast<float> (xi / (1.0 + xi) * gamma);
        gain[k] = std::max (static_cast<float> (g), floorGain);
    }
}

void SpectralGain::applyPresence (float* gain, const DenoiseSettings& s)
{
    // 声がある確率 p = 1 / (1 + q / (1 − q) × (1 + ξ) e^(−v))。q は声がない確率（前もって）
    //   半々で固定: q = 0.5
    //   自動（Cohen・Berdugo 2001）: ξ を時間でならし、近く（3 区画）、広く（31 区画）、フレーム全体の 3 つの平均が
    //   −10dB〜−5dB のどこにあるかで、声のありそうさを 0〜1 にし、その積から q を決める
    const double floorGain = std::pow (10.0, -s.maxReductionDb / 20.0);
    if (s.presence == Presence::cohen)
    {
        for (int k = 0; k < bins; ++k)
            zeta[static_cast<size_t> (k)] = static_cast<float> (zetaAlpha * zeta[static_cast<size_t> (k)] + (1.0 - zetaAlpha) * xiNow[static_cast<size_t> (k)]);
        work[0] = 0;
        for (int k = 0; k < bins; ++k)
            work[static_cast<size_t> (k + 1)] = work[static_cast<size_t> (k)] + zeta[static_cast<size_t> (k)];
        auto average = [&] (int k, int half)
        {
            const int lo = std::max (0, k - half), hi = std::min (bins - 1, k + half);
            return (work[static_cast<size_t> (hi + 1)] - work[static_cast<size_t> (lo)]) / (hi - lo + 1);
        };
        static constexpr double zetaMin = 0.1, zetaMax = 0.316; // −10dB、−5dB
        auto likelihood = [] (double z)
        {
            if (z <= zetaMin) return 0.0;
            if (z >= zetaMax) return 1.0;
            return std::log (z / zetaMin) / std::log (zetaMax / zetaMin);
        };
        const double pFrame = likelihood (work[static_cast<size_t> (bins)] / bins);
        for (int k = 0; k < bins; ++k)
        {
            const double q = 1.0 - likelihood (average (k, 1)) * likelihood (average (k, 15)) * pFrame;
            absence[static_cast<size_t> (k)] = static_cast<float> (std::min (q, 0.95));
        }
    }
    for (int k = 0; k < bins; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const double q = s.presence == Presence::cohen ? absence[i] : 0.5;
        const double ratio = q / (1.0 - q);
        const double p = 1.0 / (1.0 + ratio * (1.0 + xiNow[i]) * std::exp (-std::min (static_cast<double> (vNow[i]), 700.0)));
        const double g = std::pow (std::max (static_cast<double> (gain[k]), floorGain), p) * std::pow (floorGain, 1.0 - p);
        gain[k] = static_cast<float> (std::max (g, floorGain));
    }
}



void GainSmoother::prepare (double sampleRate, int fftSize, int hopSize)
{
    sr = sampleRate;
    size = fftSize;
    hopSeconds = hopSize / sampleRate;
    bins = size / 2 + 1;
    fft.prepare (size);
    cep.assign (static_cast<size_t> (size), {});
    prevDb.assign (static_cast<size_t> (bins), 0.0f);
    cepSmooth.assign (static_cast<size_t> (size / 2 + 1), 0.0);
    beta.assign (static_cast<size_t> (size / 2 + 1), 0.0);
    work.assign (static_cast<size_t> (bins + 1), 0.0);
    reset();
}


void GainSmoother::reset()
{
    std::fill (prevDb.begin(), prevDb.end(), 0.0f);
    std::fill (cepSmooth.begin(), cepSmooth.end(), 0.0);
    std::fill (beta.begin(), beta.end(), 0.0);
    first = true;
}


void GainSmoother::apply (float* gain, const DenoiseSettings& s)
{
    const auto floorGain = static_cast<float> (std::pow (10.0, -s.maxReductionDb / 20.0));
    switch (s.smoothing)
    {
        case Smoothing::time: // 時間: 残す割合（dB）の変化を、上がるときはアタック、下がるときはリリースでならす
        {
            const auto attack = static_cast<float> (1.0 - std::exp (-hopSeconds / (std::max (0.1, s.attackMs) * 0.001)));
            const auto release = static_cast<float> (1.0 - std::exp (-hopSeconds / (std::max (1.0, s.releaseMs) * 0.001)));
            for (int k = 0; k < bins; ++k)
            {
                const auto i = static_cast<size_t> (k);
                const float target = 20.0f * std::log10 (std::max (gain[k], 1.0e-6f));
                if (first)
                    prevDb[i] = target;
                prevDb[i] += (target - prevDb[i]) * (target > prevDb[i] ? attack : release);
                gain[k] = std::pow (10.0f, prevDb[i] / 20.0f);
            }
            first = false;
            break;
        }
        case Smoothing::frequency: // 周波数: 前後「ならす幅」の半分ずつの区画で、残す割合を平均する
        {
            const int half = std::max (0, static_cast<int> (std::lround (s.smoothWidthHz * 0.5 / (sr / size))));
            work[0] = 0;
            for (int k = 0; k < bins; ++k)
                work[static_cast<size_t> (k + 1)] = work[static_cast<size_t> (k)] + gain[k];
            for (int k = 0; k < bins; ++k)
            {
                const int lo = std::max (0, k - half), hi = std::min (bins - 1, k + half);
                gain[k] = static_cast<float> ((work[static_cast<size_t> (hi + 1)] - work[static_cast<size_t> (lo)]) / (hi - lo + 1));
            }
            break;
        }
        case Smoothing::cepstrum: // ケプストラム（Breithaupt ら 2007）
        {
            // 残す割合の対数を、ケプストラム（ケフレンシー＝時間の軸）に変える
            for (int k = 0; k < bins; ++k)
            {
                const double v = std::log (std::max (gain[k], 1.0e-6f));
                cep[static_cast<size_t> (k)] = { v, 0.0 };
                if (k > 0 && k < size / 2)
                    cep[static_cast<size_t> (size - k)] = { v, 0.0 };
            }
            fft.inverse (cep.data());

            // ケフレンシーごとに、時間方向にならす強さを決める:
            //   包絡（0.5ms まで）→ ならさない（大まかな形は、すぐに追う）
            //   声の高さの山（70〜500Hz の周期のあたりで一番大きい山の前後）→ 弱く（倍音の並びを崩さない）
            //   それ以外 → 強く（ピロピロの元になる、細かくばらばらな揺れ）
            // 強さは 16ms 間隔のときの値で 0 / 0.2 / 0.9 とし、今の間隔に合わせる。論文の値（包絡 0.25ms 相当、強く 0.97）では、
            // 声の削れと語頭の欠けが大きかった（スペクトル減算で 0.8dB / 5.6dB → この値で 0.35dB / 3.3dB。ピロピロの指標の尖度比は 1.5 → 1.9）
            const int quarter = size / 2;
            const int envEnd = std::max (1, static_cast<int> (sr * 0.0005));
            const int pitchLo = static_cast<int> (sr / 500.0), pitchHi = std::min (quarter, static_cast<int> (sr / 70.0));
            int pitchQ = -1;
            double pitchVal = 0.2; // これより小さい山は、声の高さとみなさない
            for (int q = pitchLo; q <= pitchHi; ++q)
                if (cep[static_cast<size_t> (q)].real() > pitchVal)
                {
                    pitchVal = cep[static_cast<size_t> (q)].real();
                    pitchQ = q;
                }
            const double scale = hopSeconds / 0.016;
            const double betaPitch = std::pow (0.2, scale), betaConst = std::pow (0.9, scale);
            const double betaSmooth = std::pow (0.8, scale); // 強さそのものも、急に変わらないようにならす
            for (int q = 0; q <= quarter; ++q)
            {
                double b = betaConst;
                if (q <= envEnd)
                    b = 0.0;
                else if (pitchQ > 0 && std::abs (q - pitchQ) <= 2)
                    b = betaPitch;
                auto& bs = beta[static_cast<size_t> (q)];
                bs = first ? b : betaSmooth * bs + (1.0 - betaSmooth) * b;
                auto& cs = cepSmooth[static_cast<size_t> (q)];
                const double c = cep[static_cast<size_t> (q)].real();
                cs = first ? c : bs * cs + (1.0 - bs) * c;
            }
            first = false;

            // 周波数に戻す（ケプストラムは左右対称なので、ならした片側を写す）
            for (int q = 0; q <= quarter; ++q)
            {
                cep[static_cast<size_t> (q)] = { cepSmooth[static_cast<size_t> (q)], 0.0 };
                if (q > 0 && q < quarter)
                    cep[static_cast<size_t> (size - q)] = { cepSmooth[static_cast<size_t> (q)], 0.0 };
            }
            fft.forward (cep.data());
            for (int k = 0; k < bins; ++k)
                gain[k] = static_cast<float> (std::exp (cep[static_cast<size_t> (k)].real()));
            break;
        }
        default:
            return;
    }
    for (int k = 0; k < bins; ++k)
        gain[k] = std::clamp (gain[k], floorGain, 1.0f);
}



void FftDenoiser::prepare (double sampleRate, int newSize)
{
    sr = sampleRate;
    size = newSize;
    hop = size / 2;
    fft.prepare (size);
    window = periodicHann (size, true);
    ring.assign (static_cast<size_t> (size), 0.0f);
    acc.assign (static_cast<size_t> (size), 0.0f);
    outFifo.assign (static_cast<size_t> (hop), 0.0f);
    power.assign (static_cast<size_t> (size / 2 + 1), 0.0f);
    gain.assign (static_cast<size_t> (size / 2 + 1), 1.0f);
    spec.assign (static_cast<size_t> (size), {});
    work.assign (static_cast<size_t> (size), {});
    noiseTotal.assign (static_cast<size_t> (size / 2 + 1), 0.0f);
    windowEnergy = 0;
    for (double w : window)
        windowEnergy += w * w;
    noise.prepare (size / 2 + 1, sampleRate / hop);
    noise.setSettleSamples (sampleRate, size);
    reverb.prepare (size / 2 + 1, sampleRate / hop);
    rule.prepare (size / 2 + 1, hop / sampleRate, sampleRate / size);
    smoother.prepare (sampleRate, size, hop);
    calibrate (*this, noise, sampleRate, sampleRate / hop, true);
    reset();
}


void FftDenoiser::reset()
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    std::fill (acc.begin(), acc.end(), 0.0f);
    std::fill (outFifo.begin(), outFifo.end(), 0.0f);
    inPos = outPos = 0;
    noise.reset();
    reverb.reset();
    rule.reset();
    smoother.reset();
    voiceSnrDb = -60;
}


void FftDenoiser::frame (bool synthesize, const DenoiseSettings* s)
{
    const int half = size / 2;
    for (int i = 0; i < size; ++i)
        spec[static_cast<size_t> (i)] = { ring[static_cast<size_t> ((inPos + i) % size)] * window[static_cast<size_t> (i)], 0.0 };
    fft.forward (spec.data());
    for (int k = 0; k <= half; ++k)
        power[static_cast<size_t> (k)] = static_cast<float> (std::norm (spec[static_cast<size_t> (k)]));
    noise.push (power.data());
    reverb.push (power.data());
    if (! synthesize)
        return;

    // 残ったノイズの音色をそろえるときは、いったん深く（40dB）下げてから、削る量の上限の大きさの「作ったノイズ」を足す
    DenoiseSettings deep = *s;
    if (s->comfort)
        deep.maxReductionDb = 40.0;
    const float* est = noiseFor (*s);
    rule.compute (power.data(), est, SpectralGain::ruleFor (s->fftRule), deep, gain.data());
    if (s->fftRule == FftRule::wienerRegen)
        regenerateHarmonics (fft, size, spec, work, gain, power, noiseTotal.data(), deep);
    else if (s->fftRule == FftRule::wienerProtect)
        protectHarmonics (gain, size, sr, pitchHz, pitchVoiced, deep);
    if (s->presence != Presence::off)
        rule.applyPresence (gain.data(), deep);
    smoother.apply (gain.data(), deep);
    for (int k = 0; k <= half; ++k)
        spec[static_cast<size_t> (k)] *= gain[static_cast<size_t> (k)];
    if (s->comfort)
    {
        // コンフォートノイズ: 区画ごとに、残った音が「ノイズの見積もり × 上限の倍率の 2 乗」に足りない分を、
        // でたらめな位相の音で足す。残るノイズが、元のノイズと同じ音色で、ゆらがずに一定になる
        // 足した音は、窓をかけずに時間に戻るので、窓をかけた元の音と同じ大きさにするには「窓の大きさ ÷ 窓の 2 乗の和」倍にする
        const double floor2 = std::pow (10.0, -s->maxReductionDb / 10.0) * size / windowEnergy;
        const float* nz = noise.estimate (s->noiseMode);
        for (int k = 0; k <= half; ++k)
        {
            auto& v = spec[static_cast<size_t> (k)];
            const double target = floor2 * nz[k];
            const double missing = target - std::norm (v);
            if (missing <= 0)
                continue;
            comfortState ^= comfortState << 13;
            comfortState ^= comfortState >> 17;
            comfortState ^= comfortState << 5;
            const double phase = (k == 0 || k == half) ? 0.0 : 2.0 * kPi * (comfortState / 4294967296.0);
            v += std::polar (std::sqrt (missing), phase);
        }
    }
    for (int k = 1; k < half; ++k)
        spec[static_cast<size_t> (size - k)] = std::conj (spec[static_cast<size_t> (k)]);
    fft.inverse (spec.data());

    for (int i = 0; i < size; ++i)
        acc[static_cast<size_t> (i)] += static_cast<float> (spec[static_cast<size_t> (i)].real() * window[static_cast<size_t> (i)]);
    std::copy (acc.begin(), acc.begin() + hop, outFifo.begin());
    std::copy (acc.begin() + hop, acc.end(), acc.begin());
    std::fill (acc.begin() + (size - hop), acc.end(), 0.0f);
}


const float* FftDenoiser::noiseFor (const DenoiseSettings& s)
{
    // ノイズの見積もり（残響の抑制がオンなら、後部残響の見積もりを足す）。声のありそうさの判断用の SNR もここで求める
    const int bins = size / 2 + 1;
    const float* est = noise.estimate (s.noiseMode);
    const float* r = s.dereverb ? reverb.estimate (s.rt60) : nullptr;
    // 決め方の中で、ノイズの見積もりに「強さ」をかけるので、残響の分はその分を割っておく（残響を強さの分だけ多く見積もらない）
    const auto reverbScale = static_cast<float> (std::pow (10.0, -s.strengthDb / 10.0));
    const double binHz = sr / size;
    double sig = 0, nz = 0;
    for (int k = 0; k < bins; ++k)
    {
        const auto i = static_cast<size_t> (k);
        noiseTotal[i] = est[k] + (r != nullptr ? r[k] * reverbScale : 0.0f);
        if (k * binHz > 300 && k * binHz < 4000)
        {
            sig += power[i];
            nz += noiseTotal[i];
        }
    }
    voiceSnrDb = static_cast<float> (10.0 * std::log10 ((sig + 1.0e-20) / (nz + 1.0e-20)));
    return noiseTotal.data();
}


float FftDenoiser::noiseDensity (double hz, NoiseMode mode) const
{
    const int k = std::clamp (static_cast<int> (std::lround (hz / (sr / size))), 0, size / 2);
    return static_cast<float> (noise.estimate (mode)[k] / windowEnergy);
}




void FftDenoiser::process (float* data, int n, const DenoiseSettings& s)
{
    for (int j = 0; j < n; ++j)
    {
        noise.observe (data[j]);
        ring[static_cast<size_t> (inPos)] = data[j];
        inPos = (inPos + 1) % size;
        data[j] = outFifo[static_cast<size_t> (outPos)];
        if (++outPos == hop)
        {
            outPos = 0;
            frame (true, &s);
        }
    }
}


void FftDenoiser::analyzeOnly (const float* data, int n)
{
    for (int j = 0; j < n; ++j)
    {
        noise.observe (data[j]);
        ring[static_cast<size_t> (inPos)] = data[j];
        inPos = (inPos + 1) % size;
        if (++outPos == hop)
        {
            outPos = 0;
            frame (false, nullptr);
        }
    }
}



void LowLatencyDenoiser::prepare (double sampleRate, int newSize)
{
    sr = sampleRate;
    size = newSize;
    hop = size / 8;
    taps = size / 2;
    fft.prepare (size);
    // 分析の窓は、今に近い側を重くした非対称の形（3/4 で上がって 1/4 で下がる）。
    // 窓の重心が今に近づくので、声の出だしへの反応が早くなる（音は通さないので、窓の形は自由に選べる）
    window.assign (static_cast<size_t> (size), 0.0);
    {
        const int rise = size * 3 / 4, fall = size - rise;
        for (int i = 0; i < size; ++i)
            window[static_cast<size_t> (i)] = i < rise ? 0.5 - 0.5 * std::cos (kPi * i / rise)
                                                       : 0.5 + 0.5 * std::cos (kPi * (i - rise) / fall);
    }
    ring.assign (static_cast<size_t> (size), 0.0f);
    power.assign (static_cast<size_t> (size / 2 + 1), 0.0f);
    gain.assign (static_cast<size_t> (size / 2 + 1), 1.0f);
    hist.assign (static_cast<size_t> (taps * 2), 0.0f);
    hOld.assign (static_cast<size_t> (taps), 0.0f);
    hNew.assign (static_cast<size_t> (taps), 0.0f);
    hCur.assign (static_cast<size_t> (taps), 0.0f);
    spec.assign (static_cast<size_t> (size), {});
    cep.assign (static_cast<size_t> (size), {});
    work.assign (static_cast<size_t> (size), {});
    noiseTotal.assign (static_cast<size_t> (size / 2 + 1), 0.0f);
    windowEnergy = 0;
    for (double w : window)
        windowEnergy += w * w;
    noise.prepare (size / 2 + 1, sampleRate / hop);
    noise.setSettleSamples (sampleRate, size);
    reverb.prepare (size / 2 + 1, sampleRate / hop);
    rule.prepare (size / 2 + 1, hop / sampleRate, sampleRate / size);
    smoother.prepare (sampleRate, size, hop);
    synth.prepare (sampleRate);
    calibrate (*this, noise, sampleRate, sampleRate / hop, true);
    reset();
}


void LowLatencyDenoiser::reset()
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    std::fill (hist.begin(), hist.end(), 0.0f);
    std::fill (hOld.begin(), hOld.end(), 0.0f);
    std::fill (hNew.begin(), hNew.end(), 0.0f);
    std::fill (hCur.begin(), hCur.end(), 0.0f);
    hOld[0] = hNew[0] = hCur[0] = 1.0f; // 始めは素通し
    inPos = sinceHop = histPos = 0;
    fadePos = hop;
    noise.reset();
    reverb.reset();
    rule.reset();
    smoother.reset();
    synth.reset();
    voiceSnrDb = -60;
}


void LowLatencyDenoiser::analyze (const DenoiseSettings* s)
{
    const int half = size / 2;
    for (int i = 0; i < size; ++i)
        spec[static_cast<size_t> (i)] = { ring[static_cast<size_t> ((inPos + i) % size)] * window[static_cast<size_t> (i)], 0.0 };
    fft.forward (spec.data());
    for (int k = 0; k <= half; ++k)
        power[static_cast<size_t> (k)] = static_cast<float> (std::norm (spec[static_cast<size_t> (k)]));
    noise.push (power.data());
    reverb.push (power.data());
    if (s == nullptr)
        return;
    // 決め方は FFT の方式と同じ（倍音の再生と保護も）
    rule.compute (power.data(), noiseFor (*s), SpectralGain::ruleFor (s->fftRule), *s, gain.data());
    if (s->fftRule == FftRule::wienerRegen)
        regenerateHarmonics (fft, size, spec, work, gain, power, noiseTotal.data(), *s);
    else if (s->fftRule == FftRule::wienerProtect)
        protectHarmonics (gain, size, sr, pitchHz, pitchVoiced, *s);
    if (s->presence != Presence::off)
        rule.applyPresence (gain.data(), *s);
    smoother.apply (gain.data(), *s);
    if (bandMode)
    {
        // 区画の量を帯域にまとめ、次の分析までの間に、なめらかに移る
        float targets[BandSynth::kMaxBands];
        synth.binsToBands (gain.data(), power.data(), size / 2 + 1, sr / size,
                           static_cast<float> (std::pow (10.0, -s->maxReductionDb / 20.0)), targets);
        synth.setTargets (targets, hop);
        // 立ち上がりの保護に使うノイズ（FFT の大きさの 2 乗 → 1 サンプルあたりの 2 乗: 白色雑音の分散 σ² は、
        // 区画ごとに σ² × 窓の 2 乗の和、区画 1 つは帯域全体の 2 / size にあたる）
        synth.setNoise (noiseTotal.data(), size / 2 + 1, sr / size, 2.0 / (size * windowEnergy));
        synth.setProtection (s->onsetProtect);
    }
    else
    {
        designFilter();
    }
}


const float* LowLatencyDenoiser::noiseFor (const DenoiseSettings& s)
{
    const int bins = size / 2 + 1;
    const float* est = noise.estimate (s.noiseMode);
    const float* r = s.dereverb ? reverb.estimate (s.rt60) : nullptr;
    // 決め方の中で、ノイズの見積もりに「強さ」をかけるので、残響の分はその分を割っておく（残響を強さの分だけ多く見積もらない）
    const auto reverbScale = static_cast<float> (std::pow (10.0, -s.strengthDb / 10.0));
    const double binHz = sr / size;
    double sig = 0, nz = 0;
    for (int k = 0; k < bins; ++k)
    {
        const auto i = static_cast<size_t> (k);
        noiseTotal[i] = est[k] + (r != nullptr ? r[k] * reverbScale : 0.0f);
        if (k * binHz > 300 && k * binHz < 4000)
        {
            sig += power[i];
            nz += noiseTotal[i];
        }
    }
    voiceSnrDb = static_cast<float> (10.0 * std::log10 ((sig + 1.0e-20) / (nz + 1.0e-20)));
    return noiseTotal.data();
}


float LowLatencyDenoiser::noiseDensity (double hz, NoiseMode mode) const
{
    const int k = std::clamp (static_cast<int> (std::lround (hz / (sr / size))), 0, size / 2);
    return static_cast<float> (noise.estimate (mode)[k] / windowEnergy);
}


void LowLatencyDenoiser::designFilter()
{
    // 残す割合（大きさ）から、最小位相の FIR を作る（ケプストラムを折り返す方法）
    const int half = size / 2;
    for (int k = 0; k <= half; ++k)
    {
        // 隣の区画と軽くならす（ケプストラムが長くなって、折り返しで崩れないように）
        const auto lg = [&] (int b) { return std::log (gain[static_cast<size_t> (std::clamp (b, 0, half))]); };
        const double v = (lg (k - 1) + 2 * lg (k) + lg (k + 1)) * 0.25;
        cep[static_cast<size_t> (k)] = { v, 0.0 };
        if (k > 0 && k < half)
            cep[static_cast<size_t> (size - k)] = { v, 0.0 };
    }
    fft.inverse (cep.data());
    for (int i = 1; i < half; ++i)
    {
        cep[static_cast<size_t> (i)] = { cep[static_cast<size_t> (i)].real() * 2.0, 0.0 };
        cep[static_cast<size_t> (size - i)] = {};
    }
    cep[0] = { cep[0].real(), 0.0 };
    cep[static_cast<size_t> (half)] = { cep[static_cast<size_t> (half)].real(), 0.0 };
    fft.forward (cep.data());
    for (auto& c : cep)
        c = std::exp (c);
    fft.inverse (cep.data());

    hOld = hCur;
    const int taper = taps / 4;
    for (int i = 0; i < taps; ++i)
    {
        double w = 1.0;
        if (i >= taps - taper)
            w = 0.5 + 0.5 * std::cos (kPi * (i - (taps - taper) + 1) / (taper + 1));
        hNew[static_cast<size_t> (i)] = static_cast<float> (cep[static_cast<size_t> (i)].real() * w);
    }
    fadePos = 0;
}


void LowLatencyDenoiser::process (float* data, int n, const DenoiseSettings& s)
{
    if (s.bandSynthesis)
    {
        bandMode = true;
        for (int j = 0; j < n; ++j)
        {
            const float x = data[j];
            noise.observe (x);
            ring[static_cast<size_t> (inPos)] = x;
            inPos = (inPos + 1) % size;
            data[j] = synth.process (x);
            if (++sinceHop == hop)
            {
                sinceHop = 0;
                analyze (&s);
            }
        }
        return;
    }
    bandMode = false;
    for (int j = 0; j < n; ++j)
    {
        const float x = data[j];
        noise.observe (x);
        ring[static_cast<size_t> (inPos)] = x;
        inPos = (inPos + 1) % size;
        hist[static_cast<size_t> (histPos)] = x;
        hist[static_cast<size_t> (histPos + taps)] = x;

        // 前のフィルタから新しいフィルタへ、hop の間でなめらかに移る（係数を 16 サンプルごとに混ぜ直す。
        // 係数を混ぜるのは、2つのフィルタの出力を混ぜるのと同じ）
        if (fadePos < hop && fadePos % kFadeStep == 0)
        {
            const float w = static_cast<float> (fadePos + kFadeStep) / hop;
            for (int i = 0; i < taps; ++i)
                hCur[static_cast<size_t> (i)] = hOld[static_cast<size_t> (i)] + (hNew[static_cast<size_t> (i)] - hOld[static_cast<size_t> (i)]) * w;
        }
        if (fadePos < hop)
            ++fadePos;
        const float* h = hist.data() + histPos + taps;
        float y = 0;
        for (int i = 0; i < taps; ++i)
            y += hCur[static_cast<size_t> (i)] * h[-i];
        data[j] = y;
        histPos = (histPos + 1) % taps;

        if (++sinceHop == hop)
        {
            sinceHop = 0;
            analyze (&s);
        }
    }
}


void LowLatencyDenoiser::analyzeOnly (const float* data, int n)
{
    for (int j = 0; j < n; ++j)
    {
        noise.observe (data[j]);
        ring[static_cast<size_t> (inPos)] = data[j];
        inPos = (inPos + 1) % size;
        hist[static_cast<size_t> (histPos)] = data[j];
        hist[static_cast<size_t> (histPos + taps)] = data[j];
        histPos = (histPos + 1) % taps;
        if (++sinceHop == hop)
        {
            sinceHop = 0;
            analyze (nullptr);
        }
    }
}

} // namespace vcab
