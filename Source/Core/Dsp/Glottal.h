// ボコーダの音源の「声帯の波形」。LPC ボコーダ、ケプストラムボコーダ、メルケプボコーダ、
// チャンネルボコーダで共有する。
//
// パルス列: 今までの音源（MixedExcitation.h の PulseTrain）。1 周期に 1 回の鋭い山だけで、倍音の位相がそろうので、
//           ブザーのような響きになりやすい。
// Rosenberg: 声門を流れる空気の量を、開くとき（周期の 40%）は余弦の半分で上がり、閉じるとき（16%）は余弦の 1/4 で
//           下がる形で表す（Rosenberg 1971 の C 型）。音源には、その微分（口から出る音に近い）を使う。閉じた瞬間に段差がある。
// LF:       Liljencrants-Fant のモデル（1985）。開いている間は「指数関数 × 正弦波」、閉じた後は、すぐには止まらずに指数関数で
//           戻る（戻りの時間 Ta）。形は Fant の Rd（1995）で決める（Rd = 1.0。ふつうの張りの声）。
//
// どれも、空気の量の微分で、1 周期の平均は 0（直流を含めない）。
// 声色の包絡（LPC、ケプストラム）は、元の声から求めるので、声帯の音の「高い倍音ほど小さくなる傾き」をもう含んでいる。
// そのまま Rosenberg や LF の波形を通すと、傾きを 2 回かけることになり、こもった音になる。そこで、倍音の大きさの
// 全体の傾き（2〜127 番目の倍音のオクターブごとのパワーに合わせた直線）を、2 番目の倍音から上だけ平らにそろえる。
// 1 番目と 2 番目の倍音の大きさの差（声の張りでよく変わるところ）と、位相（波形の非対称さ）は残る
// （1 番目まで傾きをそろえると、1 番目が 2 番目より 5〜9dB 小さくなり、音が細く、大きさも 2〜6dB 下がった）。
// 傾きをそろえた後の波形の尖り（1 周期の 最大 ÷ 実効値、倍音 100 個）は、パルス列 14.1、Rosenberg 10.8、LF 13.9。
// LF は閉じる瞬間がパルスに近い鋭さなので、違いは主に 1 番目の倍音の強さ。Rosenberg は山が丸い。
//
// 折り返しの雑音を出さないように、倍音の数を変えた表を、あらかじめ作っておく（倍音の数を 2^(1/4) 倍ずつ、1〜1024 個）。
// 声の高さに合わせて、ナイキスト周波数を超えない一番多い表を使う。大きさは、2 番目から上の倍音の平均のパワーが、
// パルス列（パワー 1 を同じ数の倍音に等しく分けたもの）と同じになるようにそろえる（声色の包絡の大きさがそのまま合う）。
#pragma once

#include "Fft.h"
#include "MixedExcitation.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vcab
{

class GlottalTables
{
public:
    static constexpr int kSize = 4096;     // 1 周期の表の長さ
    static constexpr int kMaxHarmonics = 1024;

    // 初めて呼んだときに作る（音の処理の前の prepare で呼んでおく）
    static const GlottalTables& get()
    {
        static const GlottalTables tables;
        return tables;
    }

    // shape: 1 = Rosenberg、2 = LF。dt: 1 サンプルあたりに進む周期の割合
    const float* table (int shape, double dt) const
    {
        const double allowed = 0.5 / std::max (dt, 1.0e-9); // ナイキスト周波数までに入る倍音の数
        int level = 0;
        while (level + 1 < static_cast<int> (limits.size()) && limits[static_cast<size_t> (level + 1)] < allowed)
            ++level;
        return tables[shape == 2 ? 1 : 0][static_cast<size_t> (level)].data();
    }

private:
    GlottalTables()
    {
        for (double h = 1; h <= kMaxHarmonics + 0.5; h *= std::pow (2.0, 0.25))
        {
            const int k = static_cast<int> (std::floor (h));
            if (limits.empty() || limits.back() != k)
                limits.push_back (k);
        }
        build (0, [] (double t) { return rosenberg (t); });
        const LfShape lf (1.0);
        build (1, [&lf] (double t) { return lf.value (t); });
    }

    // Rosenberg の C 型の微分（周期を 1 として）
    static double rosenberg (double t)
    {
        constexpr double tp = 0.40, tn = 0.16;
        if (t < tp)
            return kPi / (2 * tp) * std::sin (kPi * t / tp);
        if (t < tp + tn)
            return -kPi / (2 * tn) * std::sin (kPi * (t - tp) / (2 * tn));
        return 0.0;
    }

    // LF モデル（周期を 1、閉じる瞬間の負の山の大きさ Ee を 1 として）
    struct LfShape
    {
        explicit LfShape (double rd)
        {
            // Fant (1995) の Rd からの求め方
            const double rap = (-1.0 + 4.8 * rd) / 100.0;
            const double rkp = (22.4 + 11.8 * rd) / 100.0;
            const double rgp = 0.25 * rkp / (0.11 * rd / (0.5 + 1.2 * rkp) - rap);
            tp = 1.0 / (2.0 * rgp);
            te = tp * (1.0 + rkp);
            ta = rap;
            wg = kPi / tp;
            // ε Ta = 1 - exp(-ε (1 - Te))
            eps = 1.0 / ta;
            for (int i = 0; i < 50; ++i)
                eps = (1.0 - std::exp (-eps * (1.0 - te))) / ta;
            // 1 周期の空気の量の出入りが 0 になる α を、二分法で探す
            double lo = -50.0, hi = 200.0;
            const bool loPositive = netFlow (lo) > 0;
            for (int i = 0; i < 200; ++i)
            {
                const double mid = 0.5 * (lo + hi);
                ((netFlow (mid) > 0) == loPositive ? lo : hi) = mid;
            }
            alpha = 0.5 * (lo + hi);
            e0 = -1.0 / (std::exp (alpha * te) * std::sin (wg * te));
        }
        double netFlow (double a) const
        {
            const double amp = -1.0 / (std::exp (a * te) * std::sin (wg * te));
            const double open = amp * (std::exp (a * te) * (a * std::sin (wg * te) - wg * std::cos (wg * te)) + wg) / (a * a + wg * wg);
            const double rest = 1.0 - te, tail = std::exp (-eps * rest);
            const double ret = -1.0 / (eps * ta) * ((1.0 - tail) / eps - rest * tail);
            return open + ret;
        }
        double value (double t) const
        {
            if (t < te)
                return e0 * std::exp (alpha * t) * std::sin (wg * t);
            return -1.0 / (eps * ta) * (std::exp (-eps * (t - te)) - std::exp (-eps * (1.0 - te)));
        }
        double tp = 0.4, te = 0.6, ta = 0.04, wg = 1, eps = 25, alpha = 0, e0 = 1;
    };

    template <typename Shape>
    void build (int index, Shape shape)
    {
        // 細かく（表の 4 倍）並べて FFT し、倍音の大きさと位相を取り出す
        constexpr int fine = kSize * 4;
        Fft fft;
        fft.prepare (fine);
        std::vector<Fft::Complex> x (static_cast<size_t> (fine));
        for (int i = 0; i < fine; ++i)
            x[static_cast<size_t> (i)] = { shape ((i + 0.5) / fine), 0.0 };
        fft.forward (x.data());

        // 全体の傾き: オクターブごと（2〜3、4〜7、…、64〜127 番目）のパワーの平均に、対数の軸で直線を合わせる
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int count = 0;
        for (int lo = 2; lo < 128; lo *= 2)
        {
            double p = 0;
            for (int h = lo; h < 2 * lo; ++h)
                p += std::norm (x[static_cast<size_t> (h)]);
            const double lx = std::log (std::sqrt (lo * (2.0 * lo - 1))), ly = 0.5 * std::log (p / lo);
            sx += lx;
            sy += ly;
            sxx += lx * lx;
            sxy += lx * ly;
            ++count;
        }
        const double slope = (count * sxy - sx * sy) / (count * sxx - sx * sx); // 対数の振幅 / 対数の倍音の番号

        Fft inv;
        inv.prepare (kSize);
        std::vector<Fft::Complex> spec (static_cast<size_t> (kSize));
        for (int k : limits)
        {
            std::fill (spec.begin(), spec.end(), Fft::Complex {});
            double upper = 0; // 2 番目から上の倍音のパワーの和
            for (int h = 1; h <= k && h < kSize / 2; ++h)
            {
                const auto g = x[static_cast<size_t> (h)] * (h >= 2 ? std::pow (h / 2.0, -slope) : 1.0);
                spec[static_cast<size_t> (h)] = g;
                spec[static_cast<size_t> (kSize - h)] = std::conj (g);
                if (h >= 2 || k == 1)
                    upper += std::norm (g);
            }
            inv.inverse (spec.data());
            std::vector<float> t (static_cast<size_t> (kSize));
            // 倍音 1 つのパワーは 2|g|^2 / N^2（逆 FFT で 1/N をかけるため）。その平均を 1 / k にする
            const double perHarmonic = 2.0 * upper / (static_cast<double> (kSize) * kSize) / (k == 1 ? 1 : k - 1);
            const double scale = std::sqrt (1.0 / k / std::max (perHarmonic, 1.0e-30));
            for (int i = 0; i < kSize; ++i)
                t[static_cast<size_t> (i)] = static_cast<float> (spec[static_cast<size_t> (i)].real() * scale);
            tables[index].push_back (std::move (t));
        }
    }

    std::vector<int> limits;                    // 表ごとの倍音の数
    std::vector<std::vector<float>> tables[2];  // [Rosenberg, LF][表]
};

// 声帯の波形の音源（パワー 1）
class GlottalSource
{
public:
    enum Shape { pulseTrain = 0, rosenberg = 1, lf = 2 };

    void prepare() { GlottalTables::get(); }
    void reset()
    {
        pulses.reset();
        phase = 0;
    }

    double next (double dt, int shape)
    {
        if (shape == pulseTrain)
            return pulses.next (dt);
        phase += dt;
        if (phase >= 1.0)
            phase -= 1.0;
        const float* t = GlottalTables::get().table (shape, dt);
        const double p = phase * GlottalTables::kSize;
        const int i = std::min (GlottalTables::kSize - 1, static_cast<int> (p));
        const double f = p - i;
        return t[i] * (1.0 - f) + t[(i + 1) & (GlottalTables::kSize - 1)] * f;
    }

private:
    PulseTrain pulses;
    double phase = 0;
};

} // namespace vcab
