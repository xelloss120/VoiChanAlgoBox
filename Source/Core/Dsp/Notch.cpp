#include "Notch.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr double kAutoQ = 28.6;           // 自動のノッチの細さ（2kHz で幅 70Hz）
constexpr double kHumWidthHz = 4.0;       // ハムのノッチの幅
constexpr double kLowHz = 100, kHighHz = 16000;
constexpr int kFloorHalf = 16;            // 周りの平均をとる幅（区画）
constexpr int kFloorGap = 3;              // 山のすぐ隣は平均に含めない（山の裾で平均が上がらないように）
constexpr double kActivateSeconds = 1.0;  // これだけ居続けたらノッチをかける
constexpr double kReleaseSeconds = 2.0;   // 消えてから、これだけかけて外す
constexpr double kForgetSeconds = 0.3;    // ノッチをかける前の山は、これだけ見えなければ忘れる
constexpr double kAttackSeconds = 0.2;    // かけ始めるときに、これだけかけて深くする
constexpr double kMainsSeconds = 3.0;     // 電源の周波数の判断をならす時間
constexpr double kQuietDb = -96.0;        // これより小さい山（dBFS）は追いかけない
} // namespace

void AdaptiveNotch::prepare (double sampleRate)
{
    sr = sampleRate;
    // 区画の細かさをサンプルレートによらず約 12Hz にする（48kHz で 4096 点）
    fftSize = 1;
    while (fftSize < static_cast<int> (sr * 0.085))
        fftSize <<= 1;
    hop = fftSize / 4;
    hopSeconds = hop / sr;
    ring.assign (static_cast<size_t> (fftSize), 0.0f);
    window.resize (static_cast<size_t> (fftSize));
    for (int i = 0; i < fftSize; ++i)
        window[static_cast<size_t> (i)] = 0.5 - 0.5 * std::cos (2 * kPi * i / fftSize);
    magDb.assign (static_cast<size_t> (fftSize / 2 + 1), 0.0);
    prefix.assign (static_cast<size_t> (fftSize / 2 + 2), 0.0);
    spec.assign (static_cast<size_t> (fftSize), {});
    fft.prepare (fftSize);
    candidates.reserve (256);
    reset();
}

void AdaptiveNotch::reset()
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    ringPos = sinceHop = 0;
    for (auto& t : tracks)
        t = {};
    for (auto& f : humFilters)
        f.reset();
    score50 = score60 = 0;
    mainsHz = 50;
    humConfiguredHarmonics = -1;
    humConfiguredDepth = humConfiguredMains = -1;
}

int AdaptiveNotch::autoFrequencies (float* out) const
{
    int count = 0;
    for (const auto& t : tracks)
        if (t.used && t.fade > 0.01 && count < kMaxAuto)
            out[count++] = static_cast<float> (t.freq);
    return count;
}

void AdaptiveNotch::analyze (const Settings& s)
{
    const int half = fftSize / 2;
    const double binHz = sr / fftSize;

    // ---- 直近の音を FFT して、区画ごとの大きさ（dB） ----
    for (int i = 0; i < fftSize; ++i)
        spec[static_cast<size_t> (i)] = { ring[static_cast<size_t> ((ringPos + i) % fftSize)] * window[static_cast<size_t> (i)], 0.0 };
    fft.forward (spec.data());
    // 大きさは dBFS に直す（ハン窓をかけた正弦波は、振幅 A で区画の値が A × N / 4 になる）
    const double toFullScale = 4.0 / fftSize;
    for (int k = 0; k <= half; ++k)
        magDb[static_cast<size_t> (k)] = 20.0 * std::log10 (std::abs (spec[static_cast<size_t> (k)]) * toFullScale + 1.0e-12);
    prefix[0] = 0;
    for (int k = 0; k <= half; ++k)
        prefix[static_cast<size_t> (k + 1)] = prefix[static_cast<size_t> (k)] + magDb[static_cast<size_t> (k)];

    // 周りの平均（山のすぐ隣を除いた、前後 kFloorHalf 区画）
    auto floorAt = [&] (int k)
    {
        const int a0 = std::max (0, k - kFloorHalf), a1 = std::max (0, k - kFloorGap);
        const int b0 = std::min (half + 1, k + kFloorGap + 1), b1 = std::min (half + 1, k + kFloorHalf + 1);
        const double sum = (prefix[static_cast<size_t> (a1)] - prefix[static_cast<size_t> (a0)])
                         + (prefix[static_cast<size_t> (b1)] - prefix[static_cast<size_t> (b0)]);
        const int count = (a1 - a0) + (b1 - b0);
        return count > 0 ? sum / count : magDb[static_cast<size_t> (k)];
    };

    // ---- 1. 突き出た山を探す ----
    candidates.clear();
    const int kLow = std::max (3, static_cast<int> (kLowHz / binHz));
    const int kHigh = std::min (half - 3, static_cast<int> (std::min (kHighHz, sr * 0.45) / binHz));
    for (int k = kLow; k <= kHigh; ++k)
    {
        const double v = magDb[static_cast<size_t> (k)];
        if (v < kQuietDb)
            continue; // 聞こえないほど小さい山は追いかけない
        bool isMax = true;
        for (int d = 1; d <= 2 && isMax; ++d)
            isMax = v >= magDb[static_cast<size_t> (k - d)] && v > magDb[static_cast<size_t> (k + d)];
        if (! isMax)
            continue;
        const double prominence = v - floorAt (k);
        if (prominence < s.sensitivityDb)
            continue;
        // 周波数を、区画の間まで細かく求める（3 点を通る放物線）
        const double a = magDb[static_cast<size_t> (k - 1)], c = magDb[static_cast<size_t> (k + 1)];
        const double den = a - 2 * v + c;
        const double off = std::abs (den) < 1.0e-12 ? 0.0 : std::clamp (0.5 * (a - c) / den, -0.5, 0.5);
        if (candidates.size() < 255)
            candidates.push_back ({ (k + off) * binHz, prominence, false });
    }

    // ---- 2. 声の倍音（等間隔に並んだ山）を見分ける ----
    // 5kHz より下の山について、基音の候補（山の周波数 ÷ 1〜6 のうち 70〜500Hz のもの）ごとに、その整数倍の近く
    // （基音の 8% 以内）にある山を数える。一番多い基音で 4 本以上なら、そのフレームは声が出ているとみなし、その山を対象外にする
    double bestF0 = 0;
    int bestCount = 0;
    for (const auto& c : candidates)
    {
        if (c.freq > 5000)
            break;
        for (int div = 1; div <= 6; ++div)
        {
            const double f0 = c.freq / div;
            if (f0 < 70 || f0 > 500)
                continue;
            int count = 0;
            for (const auto& o : candidates)
            {
                if (o.freq > 5000)
                    break;
                const double ratio = o.freq / f0;
                const double nearest = std::round (ratio);
                if (nearest >= 1 && std::abs (ratio - nearest) < 0.08) // 基音の 8% 以内のずれ
                    ++count;
            }
            if (count > bestCount)
            {
                bestCount = count;
                bestF0 = f0;
            }
        }
    }
    const bool voiced = bestCount >= 4;
    if (voiced)
        for (auto& c : candidates)
        {
            const double ratio = c.freq / bestF0;
            const double nearest = std::round (ratio);
            if (c.freq <= 5000 && nearest >= 1 && std::abs (ratio - nearest) < 0.08)
                c.harmonic = true;
        }

    updateTracks (candidates, voiced, s);

    // ---- ハムの電源の周波数（50Hz と 60Hz の倍音の並びの、突き出た量の合計を比べる） ----
    auto humScore = [&] (double base)
    {
        double sum = 0;
        for (int h = 1; h <= 6; ++h)
        {
            const int k = static_cast<int> (std::lround (base * h / binHz));
            if (k < 3 || k > half - 3)
                continue;
            double best = magDb[static_cast<size_t> (k)];
            for (int d = -1; d <= 1; ++d)
                best = std::max (best, magDb[static_cast<size_t> (k + d)]);
            sum += std::max (0.0, best - floorAt (k));
        }
        return sum;
    };
    const double a = 1.0 - std::exp (-hopSeconds / kMainsSeconds);
    score50 += a * (humScore (50) - score50);
    score60 += a * (humScore (60) - score60);
}

void AdaptiveNotch::updateTracks (const std::vector<Candidate>& cands, bool voiced, const Settings& s)
{
    const double binHz = sr / fftSize;
    std::array<bool, 16> matched {};

    // 今追いかけている山に、近い候補（±1.5 区画か ±1.5% の広いほう）を当てはめる
    for (const auto& c : cands)
    {
        int best = -1;
        double bestDist = 1.0e9;
        for (int i = 0; i < static_cast<int> (tracks.size()); ++i)
        {
            const auto& t = tracks[static_cast<size_t> (i)];
            if (! t.used)
                continue;
            const double dist = std::abs (c.freq - t.freq);
            if (dist < std::max (1.5 * binHz, 0.015 * t.freq) && dist < bestDist)
            {
                bestDist = dist;
                best = i;
            }
        }
        if (best >= 0)
        {
            auto& t = tracks[static_cast<size_t> (best)];
            // 声の倍音と重なった候補でも、もうノッチをかけている山なら追いかけ続ける
            if (c.harmonic && ! t.active)
                continue;
            t.freq += 0.3 * (c.freq - t.freq); // 山の動きに合わせて、ゆっくり動かす
            t.prominence += 0.3 * (c.prominence - t.prominence);
            t.presentSec += hopSeconds;
            t.missingSec = 0;
            matched[static_cast<size_t> (best)] = true;
        }
        else if (! voiced && ! c.harmonic)
        {
            // 新しい山（声が出ていないフレームでだけ拾う）
            for (auto& t : tracks)
                if (! t.used)
                {
                    t = {};
                    t.used = true;
                    t.freq = c.freq;
                    t.prominence = c.prominence;
                    t.presentSec = hopSeconds;
                    break;
                }
        }
    }

    // 見えなかった山: 声が出ているフレームでは数えない（声に隠れて見えないだけ）
    for (int i = 0; i < static_cast<int> (tracks.size()); ++i)
    {
        auto& t = tracks[static_cast<size_t> (i)];
        if (! t.used || matched[static_cast<size_t> (i)])
            continue;
        if (! voiced)
            t.missingSec += hopSeconds;
        if (! t.active && t.missingSec > kForgetSeconds)
            t = {};
    }

    // ノッチをかける山を選ぶ: 1 秒以上居続け、2 秒以上見えなくなっていないもののうち、突き出た量の大きい順に「本数」まで
    std::array<int, 16> order {};
    int numEligible = 0;
    for (int i = 0; i < static_cast<int> (tracks.size()); ++i)
    {
        const auto& t = tracks[static_cast<size_t> (i)];
        if (t.used && t.presentSec >= kActivateSeconds && t.missingSec < kReleaseSeconds)
            order[static_cast<size_t> (numEligible++)] = i;
    }
    std::sort (order.begin(), order.begin() + numEligible, [&] (int x, int y)
               { return tracks[static_cast<size_t> (x)].prominence > tracks[static_cast<size_t> (y)].prominence; });
    for (auto& t : tracks)
        t.active = false;
    for (int j = 0; j < std::min (numEligible, std::clamp (s.count, 1, kMaxAuto)); ++j)
        tracks[static_cast<size_t> (order[static_cast<size_t> (j)])].active = true;

    // 効き: かける山は 0.2 秒で深く、外す山（消えた・選ばれなくなった）は 2 秒で浅くする
    for (auto& t : tracks)
    {
        if (! t.used)
            continue;
        if (t.active && t.missingSec == 0)
            t.fade = std::min (1.0, t.fade + hopSeconds / kAttackSeconds);
        else
            t.fade = std::max (0.0, t.fade - hopSeconds / kReleaseSeconds);
        if (! t.active && t.fade <= 0 && t.missingSec >= kReleaseSeconds)
            t = {};
    }
}

void AdaptiveNotch::process (float* data, int n, const Settings& s)
{
    // ---- 探す（いつも。切り替えてすぐ使えるように） ----
    for (int i = 0; i < n; ++i)
    {
        ring[static_cast<size_t> (ringPos)] = data[i];
        ringPos = (ringPos + 1) % fftSize;
        if (++sinceHop >= hop)
        {
            sinceHop = 0;
            analyze (s);
        }
    }

    // ---- 自動のノッチ（係数は塊ごとに、今の周波数と効きで作り直す） ----
    if (s.autoOn)
        for (auto& t : tracks)
        {
            if (! t.used || t.fade <= 0.0)
                continue;
            const double freq = std::clamp (t.freq, kLowHz, std::min (kHighHz, sr * 0.45));
            const double gain = std::pow (10.0, -s.depthDb * t.fade / 20.0);
            t.filter.setCoeffs (BiquadCoeffs::notch (sr, freq, kAutoQ, gain));
            for (int i = 0; i < n; ++i)
                data[i] = t.filter.process (data[i]);
        }

    // ---- ハム ----
    humActive = s.humOn;
    if (s.humOn)
    {
        const double mains = s.mains == 1 ? 50.0 : (s.mains == 2 ? 60.0 : (score60 > score50 ? 60.0 : 50.0));
        mainsHz = mains;
        const int count = std::clamp (s.harmonics, 1, kMaxHum);
        if (mains != humConfiguredMains || count != humConfiguredHarmonics || s.depthDb != humConfiguredDepth)
        {
            const double gain = std::pow (10.0, -s.depthDb / 20.0);
            for (int h = 0; h < count; ++h)
            {
                const double f = mains * (h + 1);
                if (f < sr * 0.45)
                    humFilters[static_cast<size_t> (h)].setCoeffs (BiquadCoeffs::notch (sr, f, f / kHumWidthHz, gain));
            }
            humConfiguredMains = mains;
            humConfiguredHarmonics = count;
            humConfiguredDepth = s.depthDb;
        }
        for (int h = 0; h < count; ++h)
        {
            if (mains * (h + 1) >= sr * 0.45)
                break;
            auto& f = humFilters[static_cast<size_t> (h)];
            for (int i = 0; i < n; ++i)
                data[i] = f.process (data[i]);
        }
    }
}

} // namespace vcab
