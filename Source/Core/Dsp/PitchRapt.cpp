// ピッチ検出の RAPT（Talkin の方法。ESPS の get_f0 の考え方と、標準の重みの値に沿って作った）
//
// 1. フレームごとに、正規化した相互相関（7.5ms か、下限の周期の 0.75 倍の長い方の窓）を、探す範囲のすべての周期で計算し、山をいくつか
//    （最大 20 個）候補として残す。1つの方式で1つに決めず、「候補のどれか、または無声」とする。
// 2. 候補ごとに、そのフレームだけで見た「らしくなさ」（コスト）を付ける。相関が低いほど、周期が長いほど
//    （1オクターブ下の間違いを防ぐため）高い。無声のコストは、一番高い相関（声らしいほど無声らしくない）。
// 3. 隣のフレームの候補へ移るコストを付ける。高さが急に変わるほど高く、ちょうど 2 倍・半分への変化は少し安い。
//    有声と無声の切り替わりは、音量が変わるところ・声色が変わるところでは安く、変わらないところでは高い。
// 4. 動的計画法で、ここまでのコストの合計が一番小さい並びを探す。今のフレームで一番安い道筋をたどって、
//    kRaptLookahead フレーム前の状態を、そのフレームの結果にする（先のフレームを見てから決める）。
//
// get_f0 は 2 段階（荒く間引いた音で候補を探し、元の音で直す）だが、ここでは間引いた後（約 12kHz）の音で
// 直接すべての周期を計算する。有声と無声の切り替わりのコストに使う音量と声色の変化は、決めるフレームの
// 前後ではなく、直近の 2 つの区間で比べる（先の音を待たないため）。
#include "Lpc.h"
#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr double kCandThreshold = 0.3;   // 一番高い山のこの割合以上の山を、候補に残す
constexpr double kLagWeight = 0.3;       // 周期が長いほどコストを上げる重み
constexpr double kFreqWeight = 0.02;     // 高さの変化のコストの重み
constexpr double kTransCost = 0.005;     // 有声と無声の切り替わりの基本のコスト
constexpr double kTransAmp = 0.5;        // 音量の変化の重み
constexpr double kTransSpec = 0.5;       // 声色の変化の重み
constexpr double kVoiceBias = 0.0;       // 有声と判断しやすくする（負）・しにくくする（正）
constexpr double kDoubleCost = 0.35;     // ちょうど 2 倍・半分へ変わるときのコスト
constexpr int kMaxCands = 20;
constexpr int kLpcOrder = 10;

double parabolicOffset (double a, double b, double c)
{
    const double d = a - 2 * b + c;
    return std::abs (d) < 1.0e-12 ? 0.0 : std::clamp (0.5 * (a - c) / d, -0.5, 0.5);
}
} // namespace

void PitchDetector::raptCandidates (int& numCands, double* lags, double* values, double& best)
{
    // 正規化した相互相関。基準の窓 x[0..w) と、k ずらした窓 x[k..k+w) を比べる（窓の平均を引いてから）
    const int w = raptWindow (fs, curMinHz);
    const float* x = frame.data();
    double mean = 0;
    for (int j = 0; j < w; ++j)
        mean += x[j];
    mean /= w;

    double e0 = 0;
    for (int j = 0; j < w; ++j)
        e0 += (x[j] - mean) * (x[j] - mean);

    auto& phi = work;
    double ek = 0;
    for (int j = 0; j < w; ++j)
        ek += (x[j + minLag - 1] - mean) * (x[j + minLag - 1] - mean);
    for (int k = minLag - 1; k <= maxLag; ++k)
    {
        if (k > minLag - 1)
        {
            const double add = x[k + w - 1] - mean, sub = x[k - 1] - mean;
            ek = std::max (0.0, ek + add * add - sub * sub);
        }
        double r = 0;
        for (int j = 0; j < w; ++j)
            r += (x[j] - mean) * (x[j + k] - mean);
        phi[static_cast<size_t> (k)] = r / std::sqrt (e0 * ek + 1.0e-12);
    }

    // 山を拾う
    best = 0;
    for (int k = minLag; k < maxLag; ++k)
        best = std::max (best, phi[static_cast<size_t> (k)]);
    numCands = 0;
    if (best <= 0)
        return;
    struct Peak { double lag, value; };
    Peak peaks[256];
    int numPeaks = 0;
    for (int k = minLag; k < maxLag && numPeaks < 256; ++k)
    {
        const double a = phi[static_cast<size_t> (k - 1)], b = phi[static_cast<size_t> (k)], c = phi[static_cast<size_t> (k + 1)];
        if (b >= a && b > c && b >= kCandThreshold * best)
        {
            const double off = parabolicOffset (a, b, c);
            peaks[numPeaks++] = { k + off, std::min (1.0, b - 0.25 * (a - c) * off) };
        }
    }
    // 高い順に kMaxCands 個まで
    std::sort (peaks, peaks + numPeaks, [] (const Peak& p, const Peak& q) { return p.value > q.value; });
    numCands = std::min (numPeaks, kMaxCands);
    for (int i = 0; i < numCands; ++i)
    {
        lags[i] = peaks[i].lag;
        values[i] = peaks[i].value;
    }
}

void PitchDetector::analyzeRapt (bool silent)
{
    auto& cur = raptFrames[raptCount % kRaptHistory];

    // ---- 1. 候補と、そのフレームだけで見たコスト ----
    int numCands = 0;
    double lags[kMaxCands], values[kMaxCands], best = 0;
    if (! silent)
        raptCandidates (numCands, lags, values, best);
    cur.count = numCands;
    cur.lag[0] = 0;
    cur.value[0] = 0;
    const double unvoicedLocal = kVoiceBias + best;
    double local[kRaptStates];
    local[0] = unvoicedLocal;
    for (int i = 0; i < numCands; ++i)
    {
        cur.lag[i + 1] = lags[i];
        cur.value[i + 1] = values[i];
        local[i + 1] = 1.0 - values[i] * (1.0 - kLagWeight * lags[i] / maxLag);
    }

    // ---- 有声と無声の切り替わりに使う、音量と声色の変化（直近の 2 つの 20ms の区間を比べる） ----
    const auto size = static_cast<int> (ring.size());
    const int seg = std::max (8, static_cast<int> (std::lround (0.02 * fs)));
    auto at = [&] (int back) { return ring[static_cast<size_t> ((ringPos - back + 2 * size) % size)]; };
    double eNow = 0, ePrev = 0;
    for (int i = 1; i <= seg; ++i)
    {
        eNow += at (i) * at (i);
        ePrev += at (i + seg) * at (i + seg);
    }
    const double rmsRatio = std::clamp (std::sqrt ((eNow + 1.0e-9) / (ePrev + 1.0e-9)), 0.01, 100.0);

    // 声色: 直近の区間の LPC と、1つ前のフレームで求めた LPC の、板倉の距離（同じなら 1、違うほど大きい）
    double buf[1024];
    const int len = std::min (seg, 1024);
    for (int i = 0; i < len; ++i) // 古い順に、ハン窓をかけて並べる
        buf[i] = at (len - i) * (0.5 - 0.5 * std::cos (2 * kPi * (i + 0.5) / len));
    double r[kLpcOrder + 1];
    for (int lag = 0; lag <= kLpcOrder; ++lag)
    {
        double s = 0;
        for (int i = lag; i < len; ++i)
            s += buf[i] * buf[i - lag];
        r[lag] = s;
    }
    r[0] = r[0] * (1.0 + 1.0e-6) + 1.0e-12;
    double a[kLpcOrder + 1], k[kLpcOrder + 1], tmp[kLpcOrder + 1];
    lpc::levinson (r, kLpcOrder, a, k, tmp);
    double stationarity = 1.0; // 1 = 声色が変わらない（切り替わりにくい）
    if (raptHasPrev)
    {
        auto quad = [&] (const double* c)
        {
            double q = 0;
            for (int i = 0; i <= kLpcOrder; ++i)
                for (int j = 0; j <= kLpcOrder; ++j)
                    q += c[i] * c[j] * r[std::abs (i - j)];
            return q;
        };
        const double d = std::max (1.0, quad (raptPrevA) / std::max (quad (a), 1.0e-30));
        stationarity = std::min (1.0, 0.2 / (d - 0.8));
    }
    std::copy (a, a + kLpcOrder + 1, raptPrevA);
    raptHasPrev = true;

    // ---- 2〜3. 1つ前のフレームから移るコストを足して、一番安いものを選ぶ ----
    const int stateCount = numCands + 1;
    if (raptCount == 0)
    {
        for (int s = 0; s < stateCount; ++s)
        {
            cur.cost[s] = local[s];
            cur.back[s] = 0;
        }
    }
    else
    {
        const auto& prev = raptFrames[(raptCount - 1) % kRaptHistory];
        const double toUnvoiced = kTransCost + kTransSpec * stationarity + kTransAmp * rmsRatio;
        const double toVoiced = kTransCost + kTransSpec * stationarity + kTransAmp / rmsRatio;
        for (int s = 0; s < stateCount; ++s)
        {
            double bestCost = 1.0e30;
            int bestFrom = 0;
            for (int p = 0; p <= prev.count; ++p)
            {
                double trans;
                if (s == 0 && p == 0)
                    trans = 0;
                else if (s == 0)
                    trans = toUnvoiced;
                else if (p == 0)
                    trans = toVoiced;
                else
                {
                    const double delta = std::abs (std::log (cur.lag[s] / prev.lag[p]));
                    trans = kFreqWeight * std::min (delta, kDoubleCost + std::abs (delta - std::log (2.0)));
                }
                const double c = prev.cost[p] + trans;
                if (c < bestCost)
                {
                    bestCost = c;
                    bestFrom = p;
                }
            }
            cur.cost[s] = bestCost + local[s];
            cur.back[s] = bestFrom;
        }
        // 合計が大きくなり続けないように、一番小さいものを 0 にそろえる
        const double lowest = *std::min_element (cur.cost, cur.cost + stateCount);
        for (int s = 0; s < stateCount; ++s)
            cur.cost[s] -= lowest;
    }
    ++raptCount;

    // ---- 4. 今一番安い道筋をたどって、kRaptLookahead フレーム前の状態を結果にする ----
    if (raptCount <= kRaptLookahead)
    {
        estimate.hz = 0;
        estimate.voiced = false;
        estimate.clarity = 0;
        return;
    }
    int state = static_cast<int> (std::min_element (cur.cost, cur.cost + stateCount) - cur.cost);
    for (int step = 0; step < kRaptLookahead; ++step)
        state = raptFrames[(raptCount - 1 - step) % kRaptHistory].back[state];
    const auto& decided = raptFrames[(raptCount - 1 - kRaptLookahead) % kRaptHistory];
    if (state == 0)
    {
        estimate.hz = 0;
        estimate.voiced = false;
        estimate.clarity = static_cast<float> (decided.count > 0 ? decided.value[1] : 0.0);
    }
    else
    {
        estimate.hz = static_cast<float> (fs / decided.lag[state]);
        estimate.voiced = true;
        estimate.clarity = static_cast<float> (decided.value[state]);
    }
}

} // namespace vcab
