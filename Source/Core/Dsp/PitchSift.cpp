// ピッチ検出の SIFT（Markel の Simplified Inverse Filter Tracking。LPC を使って声の高さを求める方法）
//
// 1. 1kHz より上を落とす（声の高さを探すのに要るのは低い倍音だけ。高いフォルマントも消える）。
// 2. 4 次の LPC で、残った低いフォルマント（第1、第2）の形を求め、逆フィルタを通して取り除く。
//    残差は、倍音がほぼ同じ大きさに並んだ（平らなスペクトルの）音になる。
// 3. 残差の自己相関の山から周期を求める。フォルマントの響きが消えているので、山がはっきりする。
// 元の SIFT は 2kHz 前後に間引いて計算するが、ここでは間引いた後（約 12kHz）の音でそのまま計算する。
#include "Lpc.h"
#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr int kSiftOrder = 4;
constexpr double kSiftCutoffHz = 1000;
} // namespace

double PitchDetector::detectSift (float& clarity)
{
    const int n = frameLength;
    float* x = frame.data();

    // ---- 1. 低域通過（4 次のバターワース。窓ごとに頭から通す） ----
    Biquad lowPass[2];
    lowPass[0].setCoeffs (BiquadCoeffs::lowPass (fs, kSiftCutoffHz, 0.5412));
    lowPass[1].setCoeffs (BiquadCoeffs::lowPass (fs, kSiftCutoffHz, 1.3066));
    for (int i = 0; i < n; ++i)
        x[i] = lowPass[1].process (lowPass[0].process (x[i]));

    // ---- 2. 4 次の LPC と、逆フィルタ ----
    double r[kSiftOrder + 1];
    for (int lag = 0; lag <= kSiftOrder; ++lag)
    {
        double s = 0;
        for (int i = lag; i < n; ++i)
        {
            const double wa = 0.5 - 0.5 * std::cos (2 * kPi * (i + 0.5) / n);
            const double wb = 0.5 - 0.5 * std::cos (2 * kPi * (i - lag + 0.5) / n);
            s += x[i] * wa * x[i - lag] * wb;
        }
        r[lag] = s;
    }
    r[0] = r[0] * (1.0 + 1.0e-6) + 1.0e-12;
    double a[kSiftOrder + 1], k[kSiftOrder + 1], tmp[kSiftOrder + 1];
    lpc::levinson (r, kSiftOrder, a, k, tmp);
    // 後ろから書き換える（前の値を使うので）
    for (int i = n - 1; i >= kSiftOrder; --i)
    {
        double e = x[i];
        for (int j = 1; j <= kSiftOrder; ++j)
            e += a[j] * x[i - j];
        x[i] = static_cast<float> (e);
    }
    for (int i = 0; i < kSiftOrder; ++i)
        x[i] = 0.0f;

    // ---- 3. 残差の自己相関 ----
    const double highest = detectAutocorrelation (clarity);
    if (highest <= 0)
        return 0;
    // 一番高い山を選ぶだけだと、周期の 2 倍の山を選びやすい（1オクターブ下）。一番高い山の 0.9 倍以上ある山のうち、
    // 一番短い周期のものを選ぶ（自己相関の値は detectAutocorrelation が work に残している）
    const double top = clarity;
    for (int tau = minLag; tau < maxLag; ++tau)
    {
        const double v = work[static_cast<size_t> (tau)];
        if (v >= 0.9 * top && v >= work[static_cast<size_t> (tau - 1)] && v > work[static_cast<size_t> (tau + 1)])
        {
            const double before = work[static_cast<size_t> (tau - 1)], after = work[static_cast<size_t> (tau + 1)];
            const double d = before - 2 * v + after;
            const double off = std::abs (d) < 1.0e-12 ? 0.0 : std::clamp (0.5 * (before - after) / d, -0.5, 0.5);
            clarity = static_cast<float> (v);
            return tau + off;
        }
    }
    return highest;
}

} // namespace vcab
