// ピッチ検出の pYIN（Mauch と Dixon の probabilistic YIN。公開されている pYIN の実装の数値に沿って作った）
//
// 1. YIN と同じ正規化した差分関数を求める。YIN は1つのしきい値（0.15）で「最初の谷」を1つ選ぶが、pYIN は
//    しきい値を 0.01〜1.00 の 100 通りで試し、それぞれで選ばれた谷に、そのしきい値の起こりやすさ（ベータ分布、
//    平均 0.15）を足していく。1つのフレームに、確率つきの候補がいくつか残る。
// 2. 声の高さを 1 半音あたり 5 つの区画に分け、「区画 × 有声 / 無声」を状態とする隠れマルコフモデルで、
//    前後のつながりが一番もっともらしい並びを選ぶ（ビタビ）。隣のフレームへは ±1 半音まで、近いほど移りやすい。
//    有声と無声は、めったに切り替わらない（1%）。候補の確率の合計の半分を「有声らしさ」とする。
// 3. 今のフレームで一番もっともらしい道筋をたどって、kPyinLookahead フレーム前の状態を結果にする。
#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr int kBinsPerSemitone = 5;
constexpr int kTransitionHalfWidth = 5;  // 隣のフレームへ移れる区画の数（±1 半音）
constexpr double kSwitchProb = 0.01;     // 有声と無声が切り替わる確率
constexpr double kYinTrust = 0.5;        // 候補の確率のうち、有声とみなす割合
constexpr int kThresholds = 100;
constexpr double kBetaA = 2.0, kBetaB = 34.0 / 3.0; // ベータ分布（平均 0.15）
constexpr double kAbsoluteMinWeight = 0.01;           // しきい値を下回る谷がないとき、一番深い谷に足す割合

double logOf (double p) { return std::log (std::max (p, 1.0e-300)); }
} // namespace

void PitchDetector::configurePyin (double minHz, double maxHz)
{
    auto& s = pyinState;
    s.minHz = minHz;
    s.numBins = std::max (2, static_cast<int> (std::floor (12.0 * kBinsPerSemitone * std::log2 (maxHz / minHz))) + 1);
    const int states = 2 * s.numBins;

    // しきい値の起こりやすさ（ベータ分布の密度を、100 個の点で取って合計 1 にする）
    s.thresholdPrior.assign (kThresholds, 0.0);
    double sum = 0;
    for (int i = 0; i < kThresholds; ++i)
    {
        const double x = (i + 1) / static_cast<double> (kThresholds);
        const double v = x < 1.0 ? std::pow (x, kBetaA - 1) * std::pow (1.0 - x, kBetaB - 1) : 0.0;
        s.thresholdPrior[static_cast<size_t> (i)] = v;
        sum += v;
    }
    for (auto& v : s.thresholdPrior)
        v /= sum;

    // 隣の区画へ移る確率（三角形の重み。合計 1）
    s.transition.assign (2 * kTransitionHalfWidth + 1, 0.0);
    double tsum = 0;
    for (int d = -kTransitionHalfWidth; d <= kTransitionHalfWidth; ++d)
    {
        const double w = kTransitionHalfWidth + 1 - std::abs (d);
        s.transition[static_cast<size_t> (d + kTransitionHalfWidth)] = w;
        tsum += w;
    }
    for (auto& v : s.transition)
        v = std::log (v / tsum);

    s.score.assign (static_cast<size_t> (states), 0.0);
    s.next.assign (static_cast<size_t> (states), 0.0);
    s.logObs.assign (static_cast<size_t> (states), 0.0);
    s.back.assign (static_cast<size_t> (kPyinHistory * states), 0);
    s.candidates.assign (static_cast<size_t> (kPyinHistory), {});
    for (auto& c : s.candidates)
        c.reserve (32); // 音の処理の途中で、メモリを取り直さないように
    s.count = 0;
}

void PitchDetector::analyzePyin (bool silent)
{
    auto& s = pyinState;
    const int nb = s.numBins, states = 2 * nb;
    auto& cands = s.candidates[static_cast<size_t> (s.count % kPyinHistory)];
    cands.clear();

    // ---- 1. 確率つきの候補 ----
    if (! silent)
    {
        float unused = 0;
        detectYin (unused); // 正規化した差分関数を work[0..maxLag] に作る（YIN が選んだ谷は使わない）
        const auto& d = work;
        int globalMin = minLag;
        for (int tau = minLag; tau < maxLag; ++tau)
            if (d[static_cast<size_t> (tau)] < d[static_cast<size_t> (globalMin)])
                globalMin = tau;

        auto addCandidate = [&] (int tau, double p)
        {
            for (auto& c : cands)
                if (c.tau == tau)
                {
                    c.prob += p;
                    return;
                }
            if (cands.size() < 32)
                cands.push_back ({ tau, p, 0.0 });
        };
        // しきい値を小さい順に試す。しきい値を下回る最初の谷（を、谷の底まで進めた位置）が、そのしきい値の候補
        for (int i = 0; i < kThresholds; ++i)
        {
            const double threshold = (i + 1) / static_cast<double> (kThresholds);
            int found = -1;
            for (int t = minLag; t < maxLag; ++t)
            {
                if (d[static_cast<size_t> (t)] < threshold)
                {
                    while (t + 1 < maxLag && d[static_cast<size_t> (t + 1)] < d[static_cast<size_t> (t)])
                        ++t;
                    found = t;
                    break;
                }
            }
            if (found >= 0)
                addCandidate (found, s.thresholdPrior[static_cast<size_t> (i)]);
            else
                addCandidate (globalMin, s.thresholdPrior[static_cast<size_t> (i)] * kAbsoluteMinWeight);
        }
        // 周期を、サンプルの間まで細かく求めて、周波数にする
        for (auto& c : cands)
        {
            const auto b = static_cast<size_t> (c.tau);
            const double a = d[b - 1], m = d[b], e = d[b + 1];
            const double den = a - 2 * m + e;
            const double off = std::abs (den) < 1.0e-12 ? 0.0 : std::clamp (0.5 * (a - e) / den, -0.5, 0.5);
            c.hz = fs / (c.tau + off);
        }
    }

    // ---- 2. 観測の確率: 有声の区画は候補の確率、無声の区画は残りを均等に ----
    std::vector<double>& obs = s.next; // 作業領域を借りる（下で上書きする前に使い切る）
    std::fill (obs.begin(), obs.end(), 0.0);
    double pitched = 0;
    for (const auto& c : cands)
    {
        const int bin = static_cast<int> (std::lround (12.0 * kBinsPerSemitone * std::log2 (c.hz / s.minHz)));
        if (bin < 0 || bin >= nb)
            continue;
        obs[static_cast<size_t> (bin)] += c.prob;
        pitched += c.prob;
    }
    const double reallyPitched = kYinTrust * pitched;
    for (int b = 0; b < nb; ++b)
    {
        if (pitched > 0)
            obs[static_cast<size_t> (b)] *= reallyPitched / pitched;
        obs[static_cast<size_t> (b + nb)] = (1.0 - reallyPitched) / nb;
    }
    auto& logObs = s.logObs;
    for (size_t i = 0; i < obs.size(); ++i)
        logObs[i] = logOf (obs[i]);

    // ---- ビタビ: 1つ前のフレームの点数から、一番もっともらしい移り方を選ぶ ----
    int* back = &s.back[static_cast<size_t> ((s.count % kPyinHistory) * states)];
    const double logStay = std::log (1.0 - kSwitchProb), logSwitch = std::log (kSwitchProb);
    if (s.count == 0)
    {
        for (int j = 0; j < states; ++j)
        {
            s.next[static_cast<size_t> (j)] = logObs[static_cast<size_t> (j)] - std::log (static_cast<double> (states));
            back[j] = j;
        }
    }
    else
    {
        for (int j = 0; j < states; ++j)
        {
            const bool voicedTo = j < nb;
            const int bin = voicedTo ? j : j - nb;
            double best = -1.0e300;
            int from = j;
            for (int dd = -kTransitionHalfWidth; dd <= kTransitionHalfWidth; ++dd)
            {
                const int b = bin + dd;
                if (b < 0 || b >= nb)
                    continue;
                const double t = s.transition[static_cast<size_t> (dd + kTransitionHalfWidth)];
                // 同じ有声・無声のまま
                const int same = voicedTo ? b : b + nb;
                const double v1 = s.score[static_cast<size_t> (same)] + t + logStay;
                if (v1 > best)
                {
                    best = v1;
                    from = same;
                }
                // 有声と無声が切り替わる
                const int other = voicedTo ? b + nb : b;
                const double v2 = s.score[static_cast<size_t> (other)] + t + logSwitch;
                if (v2 > best)
                {
                    best = v2;
                    from = other;
                }
            }
            s.next[static_cast<size_t> (j)] = best + logObs[static_cast<size_t> (j)];
            back[j] = from;
        }
    }
    // 点数が小さくなり続けないように、一番大きいものを 0 にそろえる
    const double top = *std::max_element (s.next.begin(), s.next.end());
    for (int j = 0; j < states; ++j)
        s.score[static_cast<size_t> (j)] = s.next[static_cast<size_t> (j)] - top;
    ++s.count;

    // ---- 3. 今一番もっともらしい道筋をたどって、kPyinLookahead フレーム前の状態を結果にする ----
    if (s.count <= kPyinLookahead)
    {
        estimate.hz = 0;
        estimate.voiced = false;
        estimate.clarity = 0;
        return;
    }
    int state = static_cast<int> (std::max_element (s.score.begin(), s.score.end()) - s.score.begin());
    for (int step = 0; step < kPyinLookahead; ++step)
        state = s.back[static_cast<size_t> (((s.count - 1 - step) % kPyinHistory) * states + state)];
    const auto& decided = s.candidates[static_cast<size_t> ((s.count - 1 - kPyinLookahead) % kPyinHistory)];
    if (state >= nb)
    {
        estimate.hz = 0;
        estimate.voiced = false;
        estimate.clarity = 0;
        return;
    }
    // 区画の中心に一番近い候補の周波数（1 区画以内になければ、区画の中心）
    const double centre = s.minHz * std::pow (2.0, state / (12.0 * kBinsPerSemitone));
    double hz = centre, prob = 0, bestDist = 1.0;
    for (const auto& c : decided)
    {
        const double dist = std::abs (12.0 * kBinsPerSemitone * std::log2 (c.hz / centre));
        if (dist <= bestDist)
        {
            bestDist = dist;
            hz = c.hz;
            prob = c.prob;
        }
    }
    estimate.hz = static_cast<float> (hz);
    estimate.voiced = true;
    estimate.clarity = static_cast<float> (std::min (1.0, prob));
}

} // namespace vcab
