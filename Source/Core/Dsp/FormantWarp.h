// フォルマント（声色の形）を周波数方向に動かすときの、動かし方
//
//   一律           すべての周波数を、フォルマントの倍率だけ動かす（今までの動かし方）。
//   低い側を残す   1kHz より上は一律に倍率だけ動かし、そこから下は低いほど動かさない（150Hz 以下は動かさない）。
//                  一律に動かすと、声色の要（1〜3kHz）を動かすのと一緒に、声の高さのすぐ上の低い帯域まで動いて、
//                  声が細く・軽くなりすぎることがある。その低い帯域を残す。
//                  倍率 r に対して、元の周波数 f の山は f × r^s(f) に動く。s(f) は 150Hz で 0、1kHz で 1 になる
//                  なめらかな曲線（周波数の対数の上で、余弦の形でつなぐ）。
//   帯域ごと       F1・F2・F3 が来やすい帯域の目印（500 / 1400 / 2700Hz）ごとに、つまみの値からの差
//                  （F1 / F2 / F3 の差、半音）を決め、目印の間を周波数の対数の上で余弦の形でつなぐ。一番下の目印より下は
//                  F1 の差、一番上より上は F3 の差。目印は固定なので、母音によってフォルマントが帯域をまたぐと、
//                  別のフォルマントの差で動く（「お」の F2 は約 850Hz で、F1 の帯域に入る）。
//   フォルマントごと 帯域ごとと同じつなぎ方で、目印を、入力の声から追いかけた F1・F2・F3 の位置に置く
//                  （FormantTrack。追いかけられないとき（無声音や無音）は、追いかけの側が帯域の目印に戻す）。
//
// 各方式は「動かした後の周波数 f に、元のどの周波数の包絡を持ってくるか」（inverse）か、
// 「元の周波数 f の山がどこへ動くか」（forward）を使う。
#pragma once

#include "../Params.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vcab
{

// 追いかけたフォルマントの位置（FormantTracker が書き、各方式の FormantWarp が読む）
struct FormantTrack
{
    double hz[3] { 500.0, 1400.0, 2700.0 };
    bool valid = false; // 今の値が、追いかけた結果か（false なら帯域の目印）
};

class FormantWarp
{
public:
    enum Mode { uniform = 0, keepLow, bands, tracked };

    static constexpr double kLowHz = 150.0;   // これより下は動かさない
    static constexpr double kFullHz = 1000.0; // これより上は一律に動かす（つまみの値の分だけ動く基準）
    static constexpr double kBandHz[3] = { 500.0, 1400.0, 2700.0 }; // 帯域ごとの目印

    // 設定から倍率と動かし方を読む。変わったときだけ、表を作り直す
    void set (const ParamValues& p, const FormantTrack* track = nullptr)
    {
        const int m = std::clamp (p.index (P::formantWarp), 0, 3);
        const double ratio = std::pow (2.0, p.get (P::formant) / 12.0);
        if (m < bands)
        {
            set (ratio, m);
            return;
        }
        const double offs[3] = { p.get (P::formantF1), p.get (P::formantF2), p.get (P::formantF3) };
        set (ratio, m, offs, m == tracked && track != nullptr ? track->hz : kBandHz);
    }

    void set (double ratio, int newMode)
    {
        if (ratio == r && newMode == mode && (mode == uniform || ! table.empty()))
            return;
        r = ratio;
        mode = newMode;
        if (mode == keepLow)
            buildTable();
        else
            table.clear();
    }

    // 帯域ごと・フォルマントごと。offs は F1 / F2 / F3 の差（半音）、anchors は目印の周波数（低い順）
    void set (double ratio, int newMode, const double* offs, const double* anchors)
    {
        double a[3];
        for (int k = 0; k < 3; ++k)
            a[k] = std::max (anchors[k], k == 0 ? 100.0 : a[k - 1] * 1.15); // 目印が近づきすぎない（並びが逆にならない）ように
        bool same = ratio == r && newMode == mode && ! table.empty();
        for (int k = 0; k < 3 && same; ++k)
            same = offs[k] == d[k] && std::abs (a[k] - anchor[k]) < 1.0; // 1Hz 未満の動きでは作り直さない
        if (same)
            return;
        r = ratio;
        mode = newMode;
        semis = 12.0 * std::log2 (r);
        for (int k = 0; k < 3; ++k)
        {
            d[k] = offs[k];
            anchor[k] = a[k];
        }
        buildTable();
    }

    double ratio() const noexcept { return r; }
    // 倍率がほぼ 1 で、フォルマントごとの差もないか（tolerance は倍率の許容。方式ごとに今までの値を使う）
    bool isIdentity (double tolerance = 1.0e-6) const noexcept { return std::abs (r - 1.0) < tolerance && ! hasOffsets(); }
    bool isUniform() const noexcept { return mode == uniform; }
    bool usesAnchors() const noexcept { return mode >= bands; }
    int currentMode() const noexcept { return mode; }
    double anchorHz (int k) const noexcept { return anchor[k]; }
    double offset (int k) const noexcept { return d[k]; }

    // 設定だけから、一律でない動かし方が効いているか（FormantWarp を持たない部品が使う）
    static bool isCurved (const ParamValues& p)
    {
        const int m = p.index (P::formantWarp);
        if (m == uniform)
            return false;
        if (std::abs (p.get (P::formant)) > 1.0e-3)
            return true;
        return m >= bands && (p.get (P::formantF1) != 0.0f || p.get (P::formantF2) != 0.0f || p.get (P::formantF3) != 0.0f);
    }

    // 元の周波数 hz の山が、動いた先の周波数
    double forward (double hz) const
    {
        if (mode == uniform)
            return hz * r;
        if (mode >= bands)
            return hz * std::exp2 ((semis + offsetAt (hz)) / 12.0);
        return hz * std::pow (r, shape (hz));
    }

    // 動いた先の周波数 hz に来る、元の周波数
    double inverse (double hz) const
    {
        if (mode == uniform)
            return hz / r;
        if (hz <= 0)
            return 0;
        // 表（step Hz ごとの forward の値。単調に増えるようにしてある）を二分探索して、直線で補間する
        const auto it = std::lower_bound (table.begin(), table.end(), hz);
        if (it == table.begin())
            return mode >= bands ? hz * std::exp2 (-(semis + d[0]) / 12.0) : hz; // 一番下（低い側を残すは、150Hz より下は動かさない）
        if (it == table.end())
            return mode >= bands ? hz * std::exp2 (-(semis + d[2]) / 12.0) : hz / r; // 表より上
        const auto i = static_cast<double> (it - table.begin());
        const double hi = *it, lo = *(it - 1);
        const double t = hi > lo ? (hz - lo) / (hi - lo) : 0.0;
        return (i - 1 + t) * step;
    }

    // 画面のグラフなどのため: 曲がり具合（0 = 動かさない、1 = 一律に倍率だけ）
    static double shape (double hz)
    {
        if (hz <= kLowHz)
            return 0.0;
        if (hz >= kFullHz)
            return 1.0;
        const double x = std::log (hz / kLowHz) / std::log (kFullHz / kLowHz);
        return 0.5 - 0.5 * std::cos (3.14159265358979 * x);
    }

    // 帯域ごと・フォルマントごと: 周波数 hz での、つまみの値からの差（半音）
    double offsetAt (double hz) const
    {
        if (hz <= anchor[0])
            return d[0];
        if (hz >= anchor[2])
            return d[2];
        const int k = hz < anchor[1] ? 0 : 1;
        const double x = std::log (hz / anchor[k]) / std::log (anchor[k + 1] / anchor[k]);
        return d[k] + (d[k + 1] - d[k]) * (0.5 - 0.5 * std::cos (3.14159265358979 * x));
    }

private:
    static constexpr double kTop = 30000.0; // 表の上限（Hz）

    bool hasOffsets() const noexcept { return mode >= bands && (d[0] != 0.0 || d[1] != 0.0 || d[2] != 0.0); }

    void buildTable()
    {
        // 表の細かさ: 低い側を残すは 2Hz（今までどおり）。帯域ごと・フォルマントごとは、フォルマントごとでは
        // 5ms ごとに作り直すので 4Hz にする（包絡の FFT の区画は 23Hz 以上なので、十分に細かい）
        step = mode >= bands ? 4.0 : 2.0;
        const int n = static_cast<int> (kTop / step) + 1;
        table.resize (static_cast<size_t> (n));
        double prev = 0;
        for (int i = 0; i < n; ++i)
        {
            // 倍率をとても小さくすると、曲がるところでわずかに逆戻りするので、前の値より小さくならないようにする
            const double v = std::max (prev + 1.0e-9, forward (i * step));
            table[static_cast<size_t> (i)] = v;
            prev = v;
        }
    }

    double r = 1.0, semis = 0.0, step = 2.0;
    int mode = uniform;
    double d[3] { 0, 0, 0 };
    double anchor[3] { kBandHz[0], kBandHz[1], kBandHz[2] };
    std::vector<double> table;
};

} // namespace vcab
