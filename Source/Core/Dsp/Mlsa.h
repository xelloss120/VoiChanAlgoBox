// メルケプストラムと MLSA フィルタ（今井・住田・古市 1983）。メルケプストラムの方式（メルケプ＋ディレイライン、
// メルケプ＋PSOLA、メルケプボコーダ）で使う。日本の音声合成（SPTK、HTS）の定番。
//
// 周波数の曲げ方: 1 次の全域通過フィルタ z~^-1 = (z^-1 - α) / (1 - α z^-1) で z^-1 を置き換えると、周波数の軸が
//   ω~ = ω + 2 atan(α sin ω / (1 - α cos ω)) に曲がる。α を選ぶと、人の耳の感じ方（メル尺度）に近い曲がり方になり、
//   低い周波数ほど細かく、高い周波数ほど粗く、声色を表せる（48kHz で α ≒ 0.55）。
// メルケプストラム: 曲げた軸の上での、対数振幅スペクトルのケプストラム。log H(z) = Σ c~(m) z~^-m（m = 0〜M）。
//   ここでは、対数振幅スペクトルを曲げた軸の上で等間隔に読み直して、逆 FFT し、M 次までを残す（曲げた軸の上での
//   ケプストラムのなめらかにし方。SPTK の mcep の繰り返しの計算は使わない）。
// MLSA フィルタ: exp(F(z)) を、指数関数のパデ近似 R_L(F) = (1 + Σ A_l F^l) / (1 + Σ A_l (-F)^l) で作る。
//   F(z) = Σ b(m) Φ_m(z)、Φ_m(z) = (1 - α^2) z^-1 / (1 - α z^-1) × z~^-(m-1)。b はメルケプストラムから
//   b(M) = c~(M)、b(m) = c~(m) - α b(m+1) で求める。b(0) は大きさ（exp(b(0))）で、フィルタの外でかける。
//   F は必ず 1 サンプル遅れて効くので、フィードバックの形で作れる。近似の誤差を小さくするため、F を b(1) の部分と
//   残りの部分に分けて、2 段に重ねる（どちらも 5 次のパデ近似。係数は MLSA フィルタ用に調整された値）。
//   逆のフィルタ exp(-F) は、同じ形で F の符号を変えたもの。R_L(-F) = 1 / R_L(F) なので、係数を切り替えながら
//   使っても、通した音を元に戻せる（全域通過フィルタの列の中身は係数によらないため）。
#pragma once

#include "Biquad.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace vcab::mlsa
{

constexpr int kPade = 5;
// 5 次のパデ近似の係数（A_0〜A_5。MLSA フィルタ用）
constexpr double kPadeCoeffs[kPade + 1] = { 1.0, 0.4999391, 0.1107098, 0.01369984, 0.0009564853, 0.00003041721 };

// 周波数の曲げ方。ω（0〜π）を、曲げた軸の ω~ に（α を負にすると逆向き）
inline double warp (double omega, double alpha)
{
    return omega + 2.0 * std::atan2 (alpha * std::sin (omega), 1.0 - alpha * std::cos (omega));
}

// サンプルレートに合う α を、メル尺度（1127 ln(1 + f / 700)）に一番近くなるように選ぶ（48kHz で 0.55 前後）
inline double alphaFor (double sampleRate)
{
    const double nyquist = sampleRate * 0.5;
    const double melTop = std::log (1.0 + nyquist / 700.0);
    double best = 0, bestErr = 1.0e30;
    for (int step = 0; step <= 900; ++step)
    {
        const double a = step * 0.001;
        double err = 0;
        for (int i = 1; i < 256; ++i)
        {
            const double f = nyquist * i / 256.0;
            const double d = warp (kPi * f / nyquist, a) / kPi - std::log (1.0 + f / 700.0) / melTop;
            err += d * d;
        }
        if (err < bestErr)
        {
            bestErr = err;
            best = a;
        }
    }
    return best;
}

// メルケプストラム c~(0..M) から、MLSA フィルタの係数 b(0..M) へ
inline void toFilterCoeffs (const double* mc, int order, double alpha, double* b)
{
    b[order] = mc[order];
    for (int m = order - 1; m >= 0; --m)
        b[m] = mc[m] - alpha * b[m + 1];
}

// F(z) = Σ b(m) Φ_m(z) を 1 つ。状態（全域通過フィルタの列）は係数によらない
class Basis
{
public:
    void prepare (int maxOrder)
    {
        v.assign (static_cast<size_t> (maxOrder + 1), 0.0);
        reset();
    }
    void reset()
    {
        std::fill (v.begin(), v.end(), 0.0);
        inPrev = 0;
    }
    // 今のサンプルの出力（1 サンプル前までの入力から決まる）。b(from..to) を使う
    double output (const double* b, int from, int to, double alpha)
    {
        // v_1 = (1 - α^2) z^-1 / (1 - α z^-1) x、v_m = z~^-1 v_(m-1)
        double prevOld = v[1];
        v[1] = alpha * v[1] + (1.0 - alpha * alpha) * inPrev;
        double sum = from <= 1 ? b[1] * v[1] : 0.0;
        for (int m = 2; m <= to; ++m)
        {
            const double old = v[static_cast<size_t> (m)];
            v[static_cast<size_t> (m)] = -alpha * v[static_cast<size_t> (m - 1)] + prevOld + alpha * old;
            prevOld = old;
            if (m >= from)
                sum += b[m] * v[static_cast<size_t> (m)];
        }
        return sum;
    }
    void push (double x) { inPrev = x; }

private:
    std::vector<double> v;
    double inPrev = 0;
};

// exp(±F) のパデ近似の 1 段（F の一部 b(from..to) だけを使う）
class PadeStage
{
public:
    void prepare (int maxOrder)
    {
        for (auto& c : chain)
            c.prepare (maxOrder);
    }
    void reset()
    {
        for (auto& c : chain)
            c.reset();
    }
    double process (double x, const double* b, int from, int to, double alpha, double sign)
    {
        // s_l = (±F)^l u。u = x - Σ A_l (-1)^l s_l（フィードバック）、出力 = u + Σ A_l s_l
        double s[kPade + 1];
        double fb = 0, ff = 0;
        for (int l = 1; l <= kPade; ++l)
        {
            s[l] = sign * chain[static_cast<size_t> (l - 1)].output (b, from, to, alpha);
            fb += ((l & 1) ? -1.0 : 1.0) * kPadeCoeffs[l] * s[l];
            ff += kPadeCoeffs[l] * s[l];
        }
        const double u = x - fb;
        chain[0].push (u);
        for (int l = 2; l <= kPade; ++l)
            chain[static_cast<size_t> (l - 1)].push (s[l - 1]);
        return u + ff;
    }

private:
    std::array<Basis, kPade> chain;
};

// MLSA フィルタ。exp(F) か、その逆の exp(-F)。b(0) の大きさはかけない
class Filter
{
public:
    void prepare (int maxOrder)
    {
        first.prepare (1);
        rest.prepare (maxOrder);
    }
    void reset()
    {
        first.reset();
        rest.reset();
    }
    // b(1..order) を使う。inverse なら exp(-F)。逆は、合成と逆の順に 2 段を通す（係数が動いても元に戻るように）
    double process (double x, const double* b, int order, double alpha, bool inverse)
    {
        if (inverse)
            return first.process (rest.process (x, b, 2, order, alpha, -1.0), b, 1, 1, alpha, -1.0);
        return rest.process (first.process (x, b, 1, 1, alpha, 1.0), b, 2, order, alpha, 1.0);
    }

private:
    PadeStage first, rest;
};

} // namespace vcab::mlsa
