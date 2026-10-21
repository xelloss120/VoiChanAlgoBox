// Harvest を少しずつ計算する版（説明は HarvestStream.h）。
// 元: WORLD の harvest.cpp（Copyright 2012 Masanori Morise、修正 BSD ライセンス。THIRD_PARTY_LICENSES.md を参照）
#include "HarvestStream.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>

namespace vcab
{

namespace
{
constexpr double kTargetFs = 8000.0;     // 間引いた後のサンプルレートの目安（元と同じ）
constexpr double kChannelsInOctave = 40; // 1オクターブあたりの帯域の数（元と同じ）
constexpr int kOverlap = 3;              // 前後何フレームの候補を借りるか（元と同じ）
constexpr int kFixLead = 100;            // つなぐ処理で見る、過去のフレームの数
constexpr int kFixEvery = 5;             // つなぐ処理をやり直す間隔（フレーム）
constexpr int kDefaultFixTail = 20;  // 評価で、20 より長くしても元の Harvest との違いは減らなかった

int matlabRound (double x) { return x > 0 ? static_cast<int> (x + 0.5) : static_cast<int> (x - 0.5); }

double lowestBoundary (double floorHz) { return floorHz * 0.9 * std::pow (2.0, 1.0 / kChannelsInOctave); }

// ---- 3. つなぐ処理（元の関数をほぼそのまま移したもの） ----

double selectBestF0 (double reference, const double* candidates, int n, double allowedRange, double& bestError)
{
    double best = 0.0;
    bestError = allowedRange;
    for (int i = 0; i < n; ++i)
    {
        const double tmp = std::abs (reference - candidates[i]) / reference;
        if (tmp > bestError)
            continue;
        best = candidates[i];
        bestError = tmp;
    }
    return best;
}

int getBoundaryList (const double* f0, int len, int* boundary, std::vector<int>& vuv)
{
    vuv.resize (static_cast<size_t> (len));
    for (int i = 0; i < len; ++i)
        vuv[static_cast<size_t> (i)] = f0[i] > 0 ? 1 : 0;
    vuv[0] = vuv[static_cast<size_t> (len - 1)] = 0;
    int count = 0;
    for (int i = 1; i < len; ++i)
        if (vuv[static_cast<size_t> (i)] != vuv[static_cast<size_t> (i - 1)])
        {
            boundary[count] = i - count % 2;
            ++count;
        }
    return count;
}

void getMultiChannelF0 (const double* f0, int len, const int* boundary, int numBoundaries, std::vector<std::vector<double>>& out)
{
    for (int i = 0; i < numBoundaries / 2; ++i)
    {
        auto& c = out[static_cast<size_t> (i)];
        std::fill (c.begin(), c.begin() + len, 0.0);
        for (int j = boundary[i * 2]; j <= boundary[i * 2 + 1]; ++j)
            c[static_cast<size_t> (j)] = f0[j];
    }
}

int extendF0 (int len, int origin, int lastPoint, int shift, const double* const* candidates, int n,
              double allowedRange, double* extended)
{
    (void) len;
    constexpr int threshold = 4;
    double tmp = extended[origin];
    int shiftedOrigin = origin;
    const int distance = std::abs (lastPoint - origin);
    int count = 0;
    double dummy;
    for (int i = 0; i <= distance; ++i)
    {
        const int index = origin + shift * i;
        extended[index + shift] = selectBestF0 (tmp, candidates[index + shift], n, allowedRange, dummy);
        if (extended[index + shift] == 0.0)
        {
            ++count;
        }
        else
        {
            tmp = extended[index + shift];
            count = 0;
            shiftedOrigin = index + shift;
        }
        if (count == threshold)
            break;
    }
    return shiftedOrigin;
}

double searchScore (double f0, const double* candidates, const double* scores, int n)
{
    double score = 0.0;
    for (int i = 0; i < n; ++i)
        if (f0 == candidates[i] && score < scores[i])
            score = scores[i];
    return score;
}

int mergeF0Sub (const double* f1, int st1, int ed1, const double* f2, int st2, int ed2,
                const double* const* candidates, const double* const* scores, int n, double* merged)
{
    (void) f1;
    if (st1 <= st2 && ed1 >= ed2)
        return ed1;
    double score1 = 0.0, score2 = 0.0;
    for (int i = st2; i <= ed1; ++i)
    {
        score1 += searchScore (merged[i], candidates[i], scores[i], n);
        score2 += searchScore (f2[i], candidates[i], scores[i], n);
    }
    if (score1 > score2)
        for (int i = ed1; i <= ed2; ++i)
            merged[i] = f2[i];
    else
        for (int i = st2; i <= ed2; ++i)
            merged[i] = f2[i];
    return ed2;
}

void filteringF0 (double* x, int len, int st, int ed, double* y, std::vector<double>& tmp)
{
    // 2次のバターワース（1ms ごとの値に対して、遮断は約 20Hz）を、前から後ろからの両方向にかける
    static const double b[2] = { 0.0078202080334971724, 0.015640416066994345 };
    static const double a[2] = { 1.7347257688092754, -0.76600660094326412 };
    tmp.resize (static_cast<size_t> (len));
    for (int i = 0; i < st; ++i)
        x[i] = x[st];
    for (int i = ed + 1; i < len; ++i)
        x[i] = x[ed];
    double w[2] = { 0.0, 0.0 };
    for (int i = 0; i < len; ++i)
    {
        const double wt = x[i] + a[0] * w[0] + a[1] * w[1];
        tmp[static_cast<size_t> (len - i - 1)] = b[0] * wt + b[1] * w[0] + b[0] * w[1];
        w[1] = w[0];
        w[0] = wt;
    }
    w[0] = w[1] = 0.0;
    for (int i = 0; i < len; ++i)
    {
        const double wt = tmp[static_cast<size_t> (i)] + a[0] * w[0] + a[1] * w[1];
        y[len - i - 1] = b[0] * wt + b[1] * w[0] + b[0] * w[1];
        w[1] = w[0];
        w[0] = wt;
    }
}
} // namespace

// ---- 0 を横切る点の記録 ----

void HarvestStream::Events::feed (double value, double index, double rate)
{
    // 正から 0 以下に変わったら、横切った位置を直線で補間して求め、前の位置との間隔を記録する
    if (hasPrev && 0.0 < prevValue && value <= 0.0)
    {
        const double edge = index - prevValue / (value - prevValue);
        if (hasEdge)
        {
            loc[head] = (edge + lastEdge) * 0.5;
            hz[head] = rate / (edge - lastEdge);
            head = (head + 1) % kSize;
            count = std::min (count + 1, kSize);
        }
        lastEdge = edge;
        hasEdge = true;
    }
    prevValue = value;
    hasPrev = true;
}

double HarvestStream::Events::at (double t, double stale) const
{
    // 時刻 t の直前と直後の記録を、直線で補間する（元の interp1 と同じ）。片側しかなければ、その値を使う
    int after = -1, before = -1;
    for (int k = 0; k < count; ++k)
    {
        const int idx = (head - 1 - k + kSize) % kSize; // 新しい順
        if (loc[idx] > t)
        {
            after = idx;
        }
        else
        {
            before = idx;
            break;
        }
    }
    if (before >= 0 && after >= 0)
    {
        const double s = (t - loc[before]) / (loc[after] - loc[before]);
        return hz[before] + s * (hz[after] - hz[before]);
    }
    if (before >= 0)
        return t - loc[before] <= stale ? hz[before] : 0.0;
    if (after >= 0)
        return loc[after] - t <= stale ? hz[after] : 0.0;
    return 0.0;
}

// ---- 準備 ----

double HarvestStream::delayMsFor (double floorHz)
{
    // 候補を探すのに必要な先の音（フィルタの長さの半分 + 横切る点を 1.5 周期待つ）、前後のフレームを借りる分、
    // つなぐ処理で待つ分、やり直す間隔
    const double b = lowestBoundary (floorHz);
    const double rawMs = (2.0 / b + 1.5 / b) * 1000.0 + 1.0;
    return rawMs + kOverlap + 1 + kDefaultFixTail + kFixEvery + 1;
}

void HarvestStream::prepare (double rate)
{
    inputRate = rate;
    ratio = std::max (1, matlabRound (rate / kTargetFs));
    fs = rate / ratio;
    const double cutoff = 0.4 * fs;
    antiAlias[0].setCoeffs (BiquadCoeffs::lowPass (rate, cutoff, 0.5412));
    antiAlias[1].setCoeffs (BiquadCoeffs::lowPass (rate, cutoff, 1.3066));
    dcCoeff = std::exp (-2.0 * 3.14159265358979 * 20.0 / fs);

    int size = 1;
    while (size < static_cast<int> (fs))
        size <<= 1;
    ring.assign (static_cast<size_t> (size) * 3, 0.0); // 3つ並べて書き、どこからでも続けて読めるようにする
    ringMask = size - 1;
    configure();
}

void HarvestStream::setRange (double newFloor, double newCeil)
{
    if (newFloor == floorHz && newCeil == ceilHz && ! channels.empty())
        return;
    floorHz = newFloor;
    ceilHz = newCeil;
    configure();
}

void HarvestStream::configure()
{
    const double adjustedFloor = floorHz * 0.9;
    const double adjustedCeil = ceilHz * 1.1;
    const int numChannels = 1 + static_cast<int> (std::log (adjustedCeil / adjustedFloor) / std::log (2.0) * kChannelsInOctave);
    channels.assign (static_cast<size_t> (numChannels), {});
    rawLag = 0;
    for (int i = 0; i < numChannels; ++i)
    {
        auto& c = channels[static_cast<size_t> (i)];
        c.boundary = adjustedFloor * std::pow (2.0, (i + 1) / kChannelsInOctave);
        c.half = matlabRound (fs / c.boundary * 2.0);
        // ナットール窓 × 帯域の中心の余弦（左右対称なので、中心から片側だけ持つ）
        c.taps.resize (static_cast<size_t> (c.half) + 1);
        for (int k = 0; k <= c.half; ++k)
        {
            const double tmp = static_cast<double> (c.half + k) / (2.0 * c.half);
            const double nuttall = 0.355768 - 0.487396 * std::cos (2 * kPi * tmp) + 0.144232 * std::cos (4 * kPi * tmp)
                                 - 0.012604 * std::cos (6 * kPi * tmp);
            c.taps[static_cast<size_t> (k)] = nuttall * std::cos (2 * kPi * c.boundary * k / fs);
        }
        rawLag = std::max (rawLag, c.half + 1.5 * fs / c.boundary + 3.0);
    }

    maxOfficial = std::max (1, matlabRound (numChannels / 10.0));
    numCandidates = maxOfficial * (2 * kOverlap + 1);
    official.assign (static_cast<size_t> (kFrames * maxOfficial), 0.0);
    refinedF0.assign (static_cast<size_t> (kFrames * numCandidates), 0.0);
    refinedScore.assign (refinedF0.size(), 0.0);
    finalF0.assign (refinedF0.size(), 0.0);
    finalScore.assign (refinedF0.size(), 0.0);
    output.assign (kFrames, 0.0);
    refineHalfMax = static_cast<int> (1.5 * fs / floorHz + 1.0) + 2;
    win.assign (static_cast<size_t> (2 * refineHalfMax + 4), 0.0);
    dwin.assign (win.size(), 0.0);
    reset();
}

void HarvestStream::reset()
{
    for (auto& f : antiAlias)
        f.reset();
    phase = 0;
    dcPrevIn = dcPrevOut = 0;
    std::fill (ring.begin(), ring.end(), 0.0);
    written = 0;
    for (auto& c : channels)
    {
        c.next = 0;
        c.prevF = c.prevD = 0;
        c.hasPrevF = false;
        for (auto& e : c.events)
            e.clear();
    }
    rawNext = refineNext = finalNext = outNext = 0;
    std::fill (output.begin(), output.end(), 0.0);
}

double HarvestStream::f0AtFrame (std::int64_t frame) const noexcept
{
    if (frame < 0 || frame >= outNext || frame < outNext - kFrames)
        return 0.0;
    return output[static_cast<size_t> (frame & frameMask)];
}

// ---- 音を受け取る ----

void HarvestStream::push (const float* x, int n)
{
    const auto size = static_cast<std::int64_t> (ringMask + 1);
    for (int i = 0; i < n; ++i)
    {
        double v = x[i];
        if (ratio > 1)
            v = antiAlias[1].process (antiAlias[0].process (static_cast<float> (v)));
        if (++phase < ratio)
            continue;
        phase = 0;

        // 直流を取り除く（元は区間の平均を引く）
        const double out = v - dcPrevIn + dcCoeff * dcPrevOut;
        dcPrevIn = v;
        dcPrevOut = out;
        const auto slot = static_cast<size_t> (written & ringMask);
        ring[slot] = ring[slot + static_cast<size_t> (size)] = ring[slot + 2 * static_cast<size_t> (size)] = out;
        ++written;
    }

    filterChannels();

    // 1. 候補を探す → 2. 磨く（前後 3 フレームの候補がそろってから）→ 前後のフレームと比べて捨てる
    while (static_cast<double> (written) >= static_cast<double> (rawNext) * fs / 1000.0 + rawLag)
        computeOfficial (rawNext++);
    while (refineNext + kOverlap < rawNext)
        refine (refineNext++);
    while (finalNext + 1 < refineNext)
        removeUnreliable (finalNext++);

    // 3. つなぐ（kFixEvery フレームずつ決める）
    while (finalNext - fixTail - outNext >= kFixEvery)
        fixWindow (outNext + kFixEvery + fixTail);
}

void HarvestStream::filterChannels()
{
    const auto size = static_cast<std::int64_t> (ringMask + 1);
    const double* base = ring.data();
    for (auto& c : channels)
    {
        const int half = c.half;
        const double* taps = c.taps.data();
        // 出力の位置 m は、入力の m + 1 を中心にした畳み込み（元の遅れの補正と同じ）
        for (; c.next <= written - 2 - half; ++c.next)
        {
            const std::int64_t m = c.next;
            const double* p = base + ((m + 1) & ringMask) + size; // 前後 half まで続けて読める
            double s0 = taps[0] * p[0], s1 = 0, s2 = 0, s3 = 0;
            int k = 1;
            for (; k + 3 <= half; k += 4)
            {
                s0 += taps[k] * (p[-k] + p[k]);
                s1 += taps[k + 1] * (p[-k - 1] + p[k + 1]);
                s2 += taps[k + 2] * (p[-k - 2] + p[k + 2]);
                s3 += taps[k + 3] * (p[-k - 3] + p[k + 3]);
            }
            for (; k <= half; ++k)
                s0 += taps[k] * (p[-k] + p[k]);
            const double f = (s0 + s1) + (s2 + s3);

            const auto index = static_cast<double> (m);
            c.events[0].feed (f, index, fs);       // 正 → 負
            c.events[1].feed (-f, index, fs);      // 負 → 正
            if (c.hasPrevF)
            {
                const double d = f - c.prevF;      // 傾き。正 → 負が山、負 → 正が谷
                c.events[2].feed (d, index - 1, fs);
                c.events[3].feed (-d, index - 1, fs);
            }
            c.prevF = f;
            c.hasPrevF = true;
        }
    }
}

// ---- 1. 候補を探す ----

void HarvestStream::computeOfficial (std::int64_t k)
{
    const double t = static_cast<double> (k) * fs / 1000.0;
    const int numChannels = static_cast<int> (channels.size());
    double raw[512];
    int vuv[512];
    for (int c = 0; c < numChannels && c < 512; ++c)
    {
        const auto& ch = channels[static_cast<size_t> (c)];
        const double stale = 4.0 * fs / ch.boundary;
        double v = 0;
        for (const auto& e : ch.events)
            v += e.at (t, stale);
        v *= 0.25;
        if (v > ch.boundary * 1.1 || v < ch.boundary * 0.9 || v > ceilHz || v < floorHz)
            v = 0.0;
        raw[c] = v;
        vuv[c] = v > 0 ? 1 : 0;
    }
    vuv[0] = vuv[numChannels - 1] = 0;

    // 候補を持つ帯域が 10 以上続くところを、1つの候補（平均）にまとめる
    double* row = officialRow (k);
    int found = 0, st = 0;
    for (int c = 1; c < numChannels; ++c)
    {
        const int diff = vuv[c] - vuv[c - 1];
        if (diff == 1)
            st = c;
        if (diff == -1 && c - st >= 10 && found < maxOfficial)
        {
            double sum = 0;
            for (int j = st; j < c; ++j)
                sum += raw[j];
            row[found++] = sum / (c - st);
        }
    }
    for (int j = found; j < maxOfficial; ++j)
        row[j] = 0.0;
}

// ---- 2. 候補を磨く ----

void HarvestStream::refine (std::int64_t k)
{
    double* f0 = refinedRow (k);
    double* score = refinedScoreRow (k);
    const double t = static_cast<double> (k) / 1000.0;
    for (int block = 0; block <= 2 * kOverlap; ++block)
    {
        // 0: このフレーム、1〜3: 1〜3 前、4〜6: 1〜3 後のフレームの候補
        const std::int64_t from = block == 0 ? k : (block <= kOverlap ? k - block : k + (block - kOverlap));
        for (int j = 0; j < maxOfficial; ++j)
        {
            const int slot = j + maxOfficial * block;
            const double candidate = from >= 0 ? officialRow (from)[j] : 0.0;
            refineOne (t, candidate, f0[slot], score[slot]);
        }
    }
}

void HarvestStream::refineOne (double t, double f0, double& refined, double& score)
{
    refined = score = 0.0;
    if (f0 <= 0.0)
        return;

    // 3 周期分のブラックマン窓（と、その微分の窓）をかけ、倍音の位置の周波数成分から瞬時周波数を求める
    const int half = static_cast<int> (1.5 * fs / f0 + 1.0);
    const int len = 2 * half + 1;
    if (half > refineHalfMax)
        return;
    const double windowTime = len / fs;
    const int fftSize = 1 << (2 + static_cast<int> (std::log (static_cast<double> (len)) / std::log (2.0)));
    const int basic = matlabRound ((t - half / fs) * fs + 0.001);

    // 窓: 元の位置 (basic + i - 1) / fs - t を中心からのずれとして、ブラックマン窓
    const double delta = (basic - 1) / fs - t;
    for (int i = 0; i < len; ++i)
    {
        const double tmp = delta + i / fs;
        win[static_cast<size_t> (i)] = 0.42 + 0.5 * std::cos (2.0 * kPi * tmp / windowTime) + 0.08 * std::cos (4.0 * kPi * tmp / windowTime);
    }
    dwin[0] = -win[1] / 2.0;
    for (int i = 1; i < len - 1; ++i)
        dwin[static_cast<size_t> (i)] = -(win[static_cast<size_t> (i + 1)] - win[static_cast<size_t> (i - 1)]) / 2.0;
    dwin[static_cast<size_t> (len - 1)] = win[static_cast<size_t> (len - 2)] / 2.0;

    const int harmonics = std::min (static_cast<int> (fs / 2.0 / f0), 6);
    const std::int64_t first = basic - 1;
    const std::int64_t oldest = std::max<std::int64_t> (0, written - (ringMask + 1) + 1);

    double numerator = 0, denominator = 0, error = 0;
    for (int h = 0; h < harmonics; ++h)
    {
        // FFT の、倍音に一番近い番号の値だけを、直接計算する
        const int index = matlabRound (f0 * fftSize / fs * (h + 1));
        const std::complex<double> step = std::polar (1.0, -2.0 * kPi * index / fftSize);
        std::complex<double> rot (1.0, 0.0), mainSpec, diffSpec;
        for (int i = 0; i < len; ++i)
        {
            const std::int64_t pos = std::clamp<std::int64_t> (first + i, oldest, written - 1);
            const std::complex<double> z = rot * y (pos);
            mainSpec += win[static_cast<size_t> (i)] * z;
            diffSpec += dwin[static_cast<size_t> (i)] * z;
            rot *= step;
        }
        const double power = std::norm (mainSpec);
        const double numeratorI = mainSpec.real() * diffSpec.imag() - mainSpec.imag() * diffSpec.real();
        const double instantaneous = power == 0.0 ? 0.0 : index * fs / fftSize + numeratorI / power * fs / 2.0 / kPi;
        const double amplitude = std::sqrt (power);
        numerator += amplitude * instantaneous;
        denominator += amplitude * (h + 1.0);
        error += std::abs ((instantaneous / (h + 1.0) - f0) / f0);
    }
    refined = numerator / (denominator + 1.0e-50);
    score = 1.0 / (error / harmonics + 1.0e-50);
    if (refined < floorHz || refined > ceilHz || score < 2.5)
        refined = score = 0.0;
}

void HarvestStream::removeUnreliable (std::int64_t k)
{
    // 前後のフレームに、5% 以内の候補がないものは捨てる（比べるのは、捨てる前の値）
    const double* now = refinedRow (k);
    double* f0 = finalRow (k);
    double* score = finalScoreRow (k);
    std::copy (now, now + numCandidates, f0);
    std::copy (refinedScoreRow (k), refinedScoreRow (k) + numCandidates, score);
    if (k == 0)
        return;
    const double* prev = refinedRow (k - 1);
    const double* next = refinedRow (k + 1);
    for (int j = 0; j < numCandidates; ++j)
    {
        const double reference = now[j];
        if (reference == 0)
            continue;
        double e1, e2;
        selectBestF0 (reference, next, numCandidates, 1.0, e1);
        selectBestF0 (reference, prev, numCandidates, 1.0, e2);
        if (std::min (e1, e2) <= 0.05)
            continue;
        f0[j] = 0;
        score[j] = 0;
    }
}

// ---- 3. つなぐ ----

void HarvestStream::fixWindow (std::int64_t end)
{
    const std::int64_t start = std::max<std::int64_t> (0, end - (kFixLead + fixTail));
    const int len = static_cast<int> (end - start);
    const int n = numCandidates;
    if (len < 3)
    {
        for (; outNext < end - fixTail; ++outNext)
            output[static_cast<size_t> (outNext & frameMask)] = 0.0;
        return;
    }

    candRows.resize (static_cast<size_t> (len));
    scoreRows.resize (static_cast<size_t> (len));
    for (int i = 0; i < len; ++i)
    {
        candRows[static_cast<size_t> (i)] = finalRow (start + i);
        scoreRows[static_cast<size_t> (i)] = finalScoreRow (start + i);
    }
    const double* const* cands = candRows.data();
    const double* const* scores = scoreRows.data();
    work1.assign (static_cast<size_t> (len), 0.0);
    work2.assign (static_cast<size_t> (len), 0.0);
    work3.assign (static_cast<size_t> (len), 0.0);
    boundary.resize (static_cast<size_t> (len + 700));
    auto& vuv = vuvScratch;
    double* f1 = work1.data();
    double* f2 = work2.data();

    // 一番点数の高い候補
    for (int i = 0; i < len; ++i)
    {
        double best = 0.0;
        f1[i] = 0.0;
        for (int j = 0; j < n; ++j)
            if (scores[i][j] > best)
            {
                f1[i] = cands[i][j];
                best = scores[i][j];
            }
    }

    // Step 1: 急な変化を 0 にする
    for (int i = 0; i < len; ++i)
        f2[i] = 0.0;
    for (int i = 2; i < len; ++i)
    {
        if (f1[i] == 0.0)
            continue;
        const double reference = f1[i - 1] * 2 - f1[i - 2];
        f2[i] = std::abs ((f1[i] - reference) / reference) > 0.008 && std::abs (f1[i] - f1[i - 1]) / f1[i - 1] > 0.008 ? 0.0 : f1[i];
    }

    // Step 2: 短い（6 フレーム未満の）有声区間を取り除く
    std::copy (f2, f2 + len, f1);
    {
        const int nb = getBoundaryList (f2, len, boundary.data(), vuv);
        for (int i = 0; i < nb / 2; ++i)
        {
            if (boundary[static_cast<size_t> (i * 2 + 1)] - boundary[static_cast<size_t> (i * 2)] >= 6)
                continue;
            for (int j = boundary[static_cast<size_t> (i * 2)]; j <= boundary[static_cast<size_t> (i * 2 + 1)]; ++j)
                f1[j] = 0.0;
        }
    }

    // Step 3: 有声区間を、候補に沿って前後に伸ばし、重なったものは点数の高い方にまとめる
    std::copy (f1, f1 + len, f2);
    {
        int* bl = boundary.data();
        const int nb = getBoundaryList (f1, len, bl, vuv);
        const int numSections = nb / 2;
        if (static_cast<int> (sections.size()) < numSections)
            sections.resize (static_cast<size_t> (numSections));
        for (auto& s : sections)
            if (static_cast<int> (s.size()) < len + 700)
                s.resize (static_cast<size_t> (len + 700));
        getMultiChannelF0 (f1, len, bl, nb, sections);

        for (int i = 0; i < numSections; ++i)
        {
            double* ext = sections[static_cast<size_t> (i)].data();
            const int newEnd = extendF0 (len, bl[i * 2 + 1], std::min (len - 2, bl[i * 2 + 1] + 100), 1, cands, n, 0.18, ext);
            const int newStart = extendF0 (len, bl[i * 2], std::max (1, bl[i * 2] - 100), -1, cands, n, 0.18, ext);
            bl[i * 2 + 1] = newEnd;
            bl[i * 2] = newStart;
        }
        // 短すぎる区間（2200 / 平均の高さ フレーム以下）は使わない
        int count = 0;
        double mean = 0.0; // 元の実装と同じく、0 に戻さずに、前の区間の平均に足していく
        for (int i = 0; i < numSections; ++i)
        {
            const int st = bl[i * 2], ed = bl[i * 2 + 1];
            for (int j = st; j < ed; ++j)
                mean += sections[static_cast<size_t> (i)][static_cast<size_t> (j)];
            mean /= ed - st;
            if (2200.0 / mean < ed - st)
            {
                std::swap (sections[static_cast<size_t> (count)], sections[static_cast<size_t> (i)]);
                std::swap (bl[count * 2], bl[i * 2]);
                std::swap (bl[count * 2 + 1], bl[i * 2 + 1]);
                ++count;
            }
        }
        if (count != 0)
        {
            order.resize (static_cast<size_t> (count));
            for (int i = 0; i < count; ++i)
                order[static_cast<size_t> (i)] = i;
            for (int i = 1; i < count; ++i)
                for (int j = i - 1; j >= 0; --j)
                    if (bl[order[static_cast<size_t> (j)] * 2] > bl[order[static_cast<size_t> (i)] * 2])
                        std::swap (order[static_cast<size_t> (i)], order[static_cast<size_t> (j)]);
                    else
                        break;
            for (int i = 0; i < len; ++i)
                f2[i] = sections[0][static_cast<size_t> (i)];
            for (int i = 1; i < count; ++i)
            {
                const int o = order[static_cast<size_t> (i)];
                const double* sec = sections[static_cast<size_t> (o)].data();
                if (bl[o * 2] - bl[1] > 0)
                {
                    for (int j = bl[o * 2]; j <= bl[o * 2 + 1]; ++j)
                        f2[j] = sec[j];
                    bl[0] = bl[o * 2];
                    bl[1] = bl[o * 2 + 1];
                }
                else
                {
                    bl[1] = mergeF0Sub (f2, bl[0], bl[1], sec, bl[o * 2], bl[o * 2 + 1], cands, scores, n, f2);
                }
            }
        }
    }

    // Step 4: 短い（9 フレーム未満の）無声の隙間を、直線で埋める
    double* f3 = work3.data();
    std::copy (f2, f2 + len, f3);
    {
        const int nb = getBoundaryList (f2, len, boundary.data(), vuv);
        for (int i = 0; i < nb / 2 - 1; ++i)
        {
            const int distance = boundary[static_cast<size_t> ((i + 1) * 2)] - boundary[static_cast<size_t> (i * 2 + 1)] - 1;
            if (distance >= 9)
                continue;
            const double tmp0 = f2[boundary[static_cast<size_t> (i * 2 + 1)]] + 1;
            const double tmp1 = f2[boundary[static_cast<size_t> ((i + 1) * 2)]] - 1;
            const double coefficient = (tmp1 - tmp0) / (distance + 1.0);
            int c = 1;
            for (int j = boundary[static_cast<size_t> (i * 2 + 1)] + 1; j <= boundary[static_cast<size_t> ((i + 1) * 2)] - 1; ++j)
                f3[j] = tmp0 + coefficient * c++;
        }
    }

    // なめらかにする（有声区間ごとに、端の値を伸ばしてから両方向のフィルタ）
    constexpr int lag = 300;
    const int padded = len + lag * 2;
    smoothed.assign (static_cast<size_t> (len), 0.0);
    {
        std::vector<double>& contour = work1; // もう使わないので作業領域にする
        contour.assign (static_cast<size_t> (padded), 0.0);
        for (int i = 0; i < len; ++i)
            contour[static_cast<size_t> (lag + i)] = f3[i];
        if (static_cast<int> (boundary.size()) < padded)
            boundary.resize (static_cast<size_t> (padded));
        const int nb = getBoundaryList (contour.data(), padded, boundary.data(), vuv);
        const int numSections = nb / 2;
        if (static_cast<int> (sections.size()) < numSections)
            sections.resize (static_cast<size_t> (numSections));
        for (auto& s : sections)
            if (static_cast<int> (s.size()) < padded)
                s.resize (static_cast<size_t> (padded));
        getMultiChannelF0 (contour.data(), padded, boundary.data(), nb, sections);
        for (int i = 0; i < numSections; ++i)
        {
            const int st = boundary[static_cast<size_t> (i * 2)], ed = boundary[static_cast<size_t> (i * 2 + 1)];
            filteringF0 (sections[static_cast<size_t> (i)].data(), padded, st, ed, contour.data(), work2);
            for (int j = st; j <= ed; ++j)
                smoothed[static_cast<size_t> (j - lag)] = contour[static_cast<size_t> (j)];
        }
    }

    for (; outNext < end - fixTail; ++outNext)
        output[static_cast<size_t> (outNext & frameMask)] = smoothed[static_cast<size_t> (outNext - start)];
}

} // namespace vcab
