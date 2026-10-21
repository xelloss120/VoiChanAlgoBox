// ピッチ検出の SWIPE'（Camacho と Harris の方法。公開されている swipe.m の手順に沿って作った）
//
// 声の高さの候補（探す範囲を、1オクターブを 96 に分けて刻む）ごとに、その高さの倍音の位置で山、倍音の間で谷になる
// 「くし」の形の重みを作り、振幅スペクトルの平方根（耳の感じる大きさに近い）と重ね合わせて、一番よく合う候補を選ぶ。
//   - 周波数の軸は、耳の感じ方に近い ERB の目盛りで、0.1 ERB ごとに取る（低い音は細かく、高い音は粗く）
//   - くしの歯は、1倍音と、素数番目の倍音だけに付ける（2倍、3倍の位置にも山があるので、偶数番目まで使うと
//     1オクターブ下の候補が強くなりすぎる）
//   - 高い倍音ほど重みを小さくする（周波数の平方根で割る）
//   - 窓の長さは、候補の周期の 8 つ分が最適。長さの違う窓（2 のべき乗）で FFT し、候補ごとに、最適な長さに
//     近い2つの窓の結果を、近さに応じて混ぜる
// 一番長い窓は、探す範囲の下限の周期の 8 つ分（下限 80Hz なら約 85ms）になるので、ほかの方式より遅れて分かる。
#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr double kStepsPerOctave = 96;   // 候補の細かさ
constexpr double kErbStep = 0.1;         // 周波数の軸の細かさ（ERB）
constexpr double kVoicedStrength = 0.25; // これより強く合ったら有声音とみなす

double hzToErbs (double hz) { return 6.44 * (std::log2 (229.0 + hz) - 7.84); }
double erbsToHz (double erbs) { return std::pow (2.0, erbs / 6.44 + 7.84) - 229.0; }

double parabolicOffset (double a, double b, double c)
{
    const double d = a - 2 * b + c;
    return std::abs (d) < 1.0e-12 ? 0.0 : std::clamp (0.5 * (a - c) / d, -0.5, 0.5);
}

bool isPrime (int n)
{
    if (n < 2) return false;
    for (int d = 2; d * d <= n; ++d)
        if (n % d == 0) return false;
    return true;
}
} // namespace

int PitchDetector::swipeLongestWindow (double fsDecimated, double minHz)
{
    // 候補の周期の 8 つ分（2 のべき乗に丸める）。入力をためておく長さを超えないようにする
    const int log2w = static_cast<int> (std::lround (std::log2 (8.0 * fsDecimated / minHz)));
    return std::min (1 << std::max (log2w, 6), kRingSize / 2);
}

void PitchDetector::configureSwipe (double minHz, double maxHz)
{
    auto& s = swipeState;
    // 候補
    s.candidates.clear();
    for (double l = std::log2 (minHz); l <= std::log2 (maxHz) + 1.0e-9; l += 1.0 / kStepsPerOctave)
        s.candidates.push_back (std::pow (2.0, l));
    const int nc = static_cast<int> (s.candidates.size());

    // 周波数の軸（ERB の目盛りで、候補の下限の 1/4 からナイキスト周波数まで）
    s.freqs.clear();
    for (double e = hzToErbs (minHz / 4); e <= hzToErbs (fs / 2); e += kErbStep)
        s.freqs.push_back (erbsToHz (e));
    const int nf = static_cast<int> (s.freqs.size());

    // 窓の長さ（長い順）。一番短いのは上限の周期の 8 つ分
    const int longest = swipeLongestWindow (fs, minHz);
    const int shortest = std::max (64, 1 << static_cast<int> (std::lround (std::log2 (8.0 * fs / maxHz))));
    s.windows.clear();
    for (int w = longest; w >= std::min (shortest, longest); w /= 2)
        s.windows.push_back (w);
    const int nw = static_cast<int> (s.windows.size());

    // 候補ごとのくしの重み（全部の窓で共通）
    s.kernels.assign (static_cast<size_t> (nc * nf), 0.0);
    for (int c = 0; c < nc; ++c)
    {
        const double pc = s.candidates[static_cast<size_t> (c)];
        const int harmonics = static_cast<int> (s.freqs.back() / pc - 0.75);
        double* k = &s.kernels[static_cast<size_t> (c * nf)];
        for (int i = 0; i < nf; ++i)
        {
            const double q = s.freqs[static_cast<size_t> (i)] / pc;
            double v = 0;
            for (int h : { static_cast<int> (std::floor (q)), static_cast<int> (std::ceil (q)) })
            {
                if (h < 1 || h > harmonics || ! (h == 1 || isPrime (h)))
                    continue;
                const double a = std::abs (q - h);
                if (a < 0.25)
                    v += std::cos (2 * kPi * q);
                else if (a < 0.75)
                    v += std::cos (2 * kPi * q) / 2;
            }
            if (std::floor (q) == std::ceil (q) && v != 0) // q がちょうど整数のときは2回数えているので戻す
                v /= 2;
            k[i] = v * std::sqrt (1.0 / s.freqs[static_cast<size_t> (i)]);
        }
        double norm = 0;
        for (int i = 0; i < nf; ++i)
            if (k[i] > 0)
                norm += k[i] * k[i];
        norm = std::sqrt (norm);
        if (norm > 0)
            for (int i = 0; i < nf; ++i)
                k[i] /= norm;
    }

    // 候補ごとに、どの窓の結果をどれだけ混ぜるか（最適な長さの対数での近さ）
    s.weights.assign (static_cast<size_t> (nc * nw), 0.0);
    for (int c = 0; c < nc; ++c)
    {
        const double ideal = std::log2 (8.0 * fs / s.candidates[static_cast<size_t> (c)]);
        for (int w = 0; w < nw; ++w)
        {
            const double d = std::abs (ideal - std::log2 (static_cast<double> (s.windows[static_cast<size_t> (w)])));
            s.weights[static_cast<size_t> (c * nw + w)] = std::max (0.0, 1.0 - d);
        }
        // 一番長い窓より長いのが最適な候補（下限の近く）は、一番長い窓だけを使う
        double sum = 0;
        for (int w = 0; w < nw; ++w)
            sum += s.weights[static_cast<size_t> (c * nw + w)];
        if (sum <= 0)
            s.weights[static_cast<size_t> (c * nw)] = 1.0;
    }

    s.ffts.resize (static_cast<size_t> (nw));
    s.hann.resize (static_cast<size_t> (nw));
    for (int w = 0; w < nw; ++w)
    {
        const int len = s.windows[static_cast<size_t> (w)];
        s.ffts[static_cast<size_t> (w)].prepare (len);
        auto& h = s.hann[static_cast<size_t> (w)];
        h.resize (static_cast<size_t> (len));
        for (int i = 0; i < len; ++i)
            h[static_cast<size_t> (i)] = 0.5 - 0.5 * std::cos (2 * kPi * (i + 0.5) / len);
    }
    s.buffer.assign (static_cast<size_t> (longest), {});
    s.loudness.assign (static_cast<size_t> (nf), 0.0);
    s.strength.assign (static_cast<size_t> (nc), 0.0);
}

double PitchDetector::detectSwipe (float& clarity)
{
    auto& s = swipeState;
    const int nc = static_cast<int> (s.candidates.size());
    const int nf = static_cast<int> (s.freqs.size());
    const int nw = static_cast<int> (s.windows.size());
    if (nc < 3 || nw == 0)
        return 0;
    std::fill (s.strength.begin(), s.strength.end(), 0.0);

    for (int w = 0; w < nw; ++w)
    {
        // 窓の中心をそろえる（一番長い窓の中心 = 調べる区間の中心）
        const int len = s.windows[static_cast<size_t> (w)];
        const int start = (frameLength - len) / 2;
        const auto& h = s.hann[static_cast<size_t> (w)];
        for (int i = 0; i < len; ++i)
            s.buffer[static_cast<size_t> (i)] = { frame[static_cast<size_t> (start + i)] * h[static_cast<size_t> (i)], 0.0 };
        s.ffts[static_cast<size_t> (w)].forward (s.buffer.data());

        // 振幅スペクトルを周波数の軸の位置で読み（直線で補間）、平方根を取って、全体の大きさで割る
        double norm = 0;
        for (int i = 0; i < nf; ++i)
        {
            const double bin = s.freqs[static_cast<size_t> (i)] * len / fs;
            const int b0 = std::min (len / 2 - 1, static_cast<int> (bin));
            const double t = bin - b0;
            const double m = std::abs (s.buffer[static_cast<size_t> (b0)]) * (1 - t) + std::abs (s.buffer[static_cast<size_t> (b0 + 1)]) * t;
            s.loudness[static_cast<size_t> (i)] = std::sqrt (std::max (0.0, m));
            norm += s.loudness[static_cast<size_t> (i)] * s.loudness[static_cast<size_t> (i)];
        }
        norm = std::sqrt (norm);
        if (norm <= 0)
            continue;

        // 候補ごとに、くしの重みと重ね合わせる（この窓を使う候補だけ）
        for (int c = 0; c < nc; ++c)
        {
            const double weight = s.weights[static_cast<size_t> (c * nw + w)];
            if (weight <= 0)
                continue;
            const double* k = &s.kernels[static_cast<size_t> (c * nf)];
            double dot = 0;
            for (int i = 0; i < nf; ++i)
                dot += k[i] * s.loudness[static_cast<size_t> (i)];
            s.strength[static_cast<size_t> (c)] += weight * dot / norm;
        }
    }

    int best = 0;
    for (int c = 1; c < nc; ++c)
        if (s.strength[static_cast<size_t> (c)] > s.strength[static_cast<size_t> (best)])
            best = c;

    // 候補の間を、放物線で細かく求める（対数の周波数で）
    double logF = std::log2 (s.candidates[static_cast<size_t> (best)]);
    if (best > 0 && best < nc - 1)
        logF += parabolicOffset (s.strength[static_cast<size_t> (best - 1)], s.strength[static_cast<size_t> (best)],
                                       s.strength[static_cast<size_t> (best + 1)]) / kStepsPerOctave;
    const double hz = std::pow (2.0, logF);

    clarity = static_cast<float> (std::clamp (s.strength[static_cast<size_t> (best)], 0.0, 1.0));
    swipeVoiced = s.strength[static_cast<size_t> (best)] > kVoicedStrength;
    return fs / hz; // 周期（間引いた後のサンプル数）
}

} // namespace vcab
