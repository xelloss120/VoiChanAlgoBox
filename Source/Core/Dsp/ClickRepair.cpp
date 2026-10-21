#include "ClickRepair.h"

#include "Lpc.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
float coeffFor (double seconds, double sr)
{
    return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * sr)));
}

void ensure (std::vector<double>& v, size_t n)
{
    if (v.size() < n)
        v.resize (n); // prepare で十分な大きさを取っておくので、ふだんはここで確保しない
}
} // namespace

bool estimateAr (const float* x, int from, int to, int order, double* a, std::vector<double>& work)
{
    const int len = to - from;
    if (len <= order * 2)
        return false;
    ensure (work, static_cast<size_t> (len + 3 * (order + 1)));
    double* w = work.data();
    double* r = w + len;
    double* k = r + order + 1;
    double* tmp = k + order + 1;
    for (int i = 0; i < len; ++i)
        w[i] = x[from + i] * (0.5 - 0.5 * std::cos (2 * kPi * (i + 0.5) / len)); // ハン窓
    for (int lag = 0; lag <= order; ++lag)
    {
        double s = 0;
        for (int i = lag; i < len; ++i)
            s += w[i] * w[i - lag];
        r[lag] = s;
    }
    if (r[0] < 1.0e-14)
        return false;
    r[0] *= 1.0001; // ほんの少し白色雑音を足したことにして、計算を安定させる
    lpc::levinson (r, order, a, k, tmp);
    return true;
}

void lsarInterpolate (float* x, int start, int count, const double* a, int order, std::vector<double>& work)
{
    const int m = count;
    if (m <= 0)
        return;
    ensure (work, static_cast<size_t> (m * m + (m + order) + (order + 1) + m));
    double* R = work.data();             // m × m（下三角にコレスキー分解の結果を入れる）
    double* known = R + m * m;           // 区間を 0 としたときの予測の外れ（n = start .. start + m + order）
    double* rb = known + (m + order);    // 係数の自己相関
    double* rhs = rb + (order + 1);      // 右辺 → 解

    auto inGap = [&] (int idx) { return idx >= start && idx < start + m; };
    for (int i = 0; i < m + order; ++i)
    {
        const int n = start + i;
        double e = 0;
        for (int j = 0; j <= order; ++j)
            if (! inGap (n - j))
                e += a[j] * x[n - j];
        known[i] = e;
    }
    for (int d = 0; d <= order; ++d)
    {
        double s = 0;
        for (int j = 0; j + d <= order; ++j)
            s += a[j] * a[j + d];
        rb[d] = s;
    }
    for (int i = 0; i < m; ++i)
    {
        for (int l = 0; l < m; ++l)
        {
            const int d = std::abs (i - l);
            R[i * m + l] = d <= order ? rb[d] : 0.0;
        }
        R[i * m + i] += rb[0] * 1.0e-9;
        double s = 0;
        for (int j = 0; j <= order; ++j)
            s += a[j] * known[i + j];
        rhs[i] = -s;
    }
    // コレスキー分解 R = L Lᵀ（帯行列だが、区間は短いので素直に解く）
    for (int i = 0; i < m; ++i)
    {
        for (int l = 0; l <= i; ++l)
        {
            double s = R[i * m + l];
            for (int q = 0; q < l; ++q)
                s -= R[i * m + q] * R[l * m + q];
            if (i == l)
            {
                if (s <= 0)
                    return; // 解けない（そのままにする）
                R[i * m + i] = std::sqrt (s);
            }
            else
            {
                R[i * m + l] = s / R[l * m + l];
            }
        }
    }
    for (int i = 0; i < m; ++i) // L y = b
    {
        double s = rhs[i];
        for (int q = 0; q < i; ++q)
            s -= R[i * m + q] * rhs[q];
        rhs[i] = s / R[i * m + i];
    }
    for (int i = m - 1; i >= 0; --i) // Lᵀ x = y
    {
        double s = rhs[i];
        for (int q = i + 1; q < m; ++q)
            s -= R[q * m + i] * rhs[q];
        rhs[i] = s / R[i * m + i];
    }
    for (int i = 0; i < m; ++i)
        x[start + i] = static_cast<float> (rhs[i]);
}

// ================= クリックを補間でつなぎ直す =================

void ClickInterpolator::prepare (double sampleRate, const Shape& s)
{
    sr = sampleRate;
    shape = s;
    auto samples = [this] (double ms) { return std::max (1, static_cast<int> (std::lround (ms * 0.001 * sr))); };
    window = samples (shape.windowMs);
    pre = samples (0.3);
    post = samples (0.3);
    maxGap = samples (shape.maxGapMs);
    contextBefore = std::max (kOrder * 6, samples (5.0));
    latency = window + pre + 2; // 補間する区間の始まり（立ち上がりの pre 前）が、まだ出ていないように
    size = 1;
    while (size < contextBefore + latency + maxGap + kOrder + 16)
        size <<= 1;
    for (auto& b : detectHigh)
        b.setCoeffs (BiquadCoeffs::highPass (sr, shape.detectHz, 0.7071));
    fastAttack = coeffFor (shape.fastAttackMs * 0.001, sr);
    fastRelease = coeffFor (shape.fastReleaseMs * 0.001, sr);
    slowAttack = coeffFor (shape.slowAttackMs * 0.001, sr);
    slowRelease = coeffFor (shape.slowReleaseMs * 0.001, sr);
    ring.assign (static_cast<size_t> (size), 0.0f);
    fastRing.assign (static_cast<size_t> (size), 0.0f);
    highRing.assign (static_cast<size_t> (size), 0.0f);
    onsetRing.assign (static_cast<size_t> (size), 0);
    segment.assign (static_cast<size_t> (contextBefore + maxGap + 2 * kOrder + 8), 0.0f);
    ar.assign (kOrder + 1, 0.0);
    work.assign (static_cast<size_t> (maxGap * maxGap + 4 * (maxGap + kOrder) + contextBefore + 8 * (kOrder + 1)), 0.0);
    reset();
}

void ClickInterpolator::reset()
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    std::fill (fastRing.begin(), fastRing.end(), 0.0f);
    std::fill (highRing.begin(), highRing.end(), 0.0f);
    std::fill (onsetRing.begin(), onsetRing.end(), static_cast<unsigned char> (0));
    for (auto& b : detectHigh)
        b.reset();
    fast = slow = 0;
    prevScore = -100;
    sampleIndex = 0;
    lastRepairedEnd = -1000000;
}

void ClickInterpolator::process (float* data, int n, double thresholdDb)
{
    const long long mask = size - 1;
    auto at = [&] (long long idx) -> float& { return ring[static_cast<size_t> (idx & mask)]; };
    for (int i = 0; i < n; ++i)
    {
        const long long idx = sampleIndex;
        const float x = data[i];
        at (idx) = x;

        const float d = detectHigh[1].process (detectHigh[0].process (x));
        const float p = d * d;
        fast += (p > fast ? fastAttack : fastRelease) * (p - fast);
        slow += (p > slow ? slowAttack : slowRelease) * (p - slow);
        const float score = 10.0f * std::log10 ((fast + 1.0e-12f) / (slow + 1.0e-10f));
        const bool onset = score > thresholdDb && prevScore <= thresholdDb && fast > 1.0e-7f;
        prevScore = score;
        fastRing[static_cast<size_t> (idx & mask)] = fast;
        highRing[static_cast<size_t> (idx & mask)] = p;
        onsetRing[static_cast<size_t> (idx & mask)] = onset ? 1 : 0;

        // window 前に立ち上がった音が、今までに山から 6dB 以上下がっていれば、クリックとして補間する
        const long long o = idx - window;
        if (o >= contextBefore + pre && onsetRing[static_cast<size_t> (o & mask)] != 0)
        {
            float peak = 0;
            long long peakAt = o;
            for (long long k = o; k <= idx; ++k)
                if (fastRing[static_cast<size_t> (k & mask)] > peak)
                {
                    peak = fastRing[static_cast<size_t> (k & mask)];
                    peakAt = k;
                }
            if (fast < peak * 0.25f)
            {
                // 区間は、高域通過した音が「周り × しきい値の半分」を超えているサンプルだけ（前後に 4 サンプル）
                const float limit = slow * std::pow (10.0f, static_cast<float> (thresholdDb) * 0.05f);
                long long first = -1, last = -1;
                for (long long k = o - pre; k <= idx; ++k)
                    if (highRing[static_cast<size_t> (k & mask)] > limit)
                    {
                        if (first < 0)
                            first = k;
                        last = k;
                    }
                if (first < 0)
                    first = last = o;
                const long long start = first - 4;
                const long long end = std::min (last + 5, idx - kOrder + 1); // 後ろに LPC の次数の分の音を残す
                const int len = static_cast<int> (end - start);
                if (start > lastRepairedEnd && len > 0 && len <= maxGap)
                {
                    // 前の音（contextBefore）、区間、後ろの音（kOrder）を一列に並べて補間する
                    const long long from = start - contextBefore;
                    const int total = contextBefore + len + kOrder;
                    for (int k = 0; k < total; ++k)
                        segment[static_cast<size_t> (k)] = at (from + k);
                    if (estimateAr (segment.data(), 0, contextBefore, kOrder, ar.data(), work))
                    {
                        lsarInterpolate (segment.data(), contextBefore, len, ar.data(), kOrder, work);
                        for (int k = 0; k < len; ++k)
                            at (start + k) = segment[static_cast<size_t> (contextBefore + k)];
                        lastRepairedEnd = end;
                        ++count;
                    }
                }
            }
        }

        data[i] = idx >= latency ? at (idx - latency) : 0.0f;
        ++sampleIndex;
    }
}

// ================= パチパチ =================

void Decrackler::prepare (double sampleRate)
{
    sr = sampleRate;
    hop = std::max (64, static_cast<int> (std::lround (0.005 * sr)));
    latency = 2 * kOrder + kMaxRun + 2;
    size = 1;
    while (size < kHistory + latency + 16)
        size <<= 1;
    scaleCoeff = coeffFor (0.01, sr);
    ring.assign (static_cast<size_t> (size), 0.0f);
    forwardSpike.assign (static_cast<size_t> (size), 0);
    segment.assign (static_cast<size_t> (kHistory + 2 * kOrder + kMaxRun + 8), 0.0f);
    ar.assign (kOrder + 1, 0.0);
    work.assign (static_cast<size_t> (kHistory + 8 * (kOrder + 1) + kMaxRun * kMaxRun + 4 * (kMaxRun + kOrder)), 0.0);
    reset();
}

void Decrackler::reset()
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    std::fill (forwardSpike.begin(), forwardSpike.end(), static_cast<unsigned char> (0));
    sampleIndex = 0;
    sinceHop = 0;
    runStart = -1;
    runLength = 0;
    quiet = 0;
    scale = scaleBack = 0;
    haveAr = false;
}

void Decrackler::process (float* data, int n, double threshold)
{
    const long long mask = size - 1;
    auto at = [&] (long long idx) -> float& { return ring[static_cast<size_t> (idx & mask)]; };
    // 外れ値を抑えてならした |残差| の平均（ガウス分布なら標準偏差 ≈ 1.25 × 平均）
    auto track = [this] (double& sc, double ae) { if (sc <= 0) sc = ae; sc += scaleCoeff * (std::min (ae, 3.0 * sc + 1.0e-9) - sc); };
    for (int i = 0; i < n; ++i)
    {
        const long long idx = sampleIndex;
        at (idx) = data[i];

        // LPC の係数は、5ms ごとに直前の約 21ms から求め直す
        if (++sinceHop >= hop && idx >= kHistory)
        {
            sinceHop = 0;
            for (int k = 0; k < kHistory; ++k)
                segment[static_cast<size_t> (k)] = at (idx - kHistory + 1 + k);
            haveAr = estimateAr (segment.data(), 0, kHistory, kOrder, ar.data(), work);
        }

        if (haveAr && idx >= 2 * kOrder)
        {
            // 前向きの残差（今のサンプル）
            double ef = 0;
            for (int j = 0; j <= kOrder; ++j)
                ef += ar[static_cast<size_t> (j)] * at (idx - j);
            const double af = std::abs (ef);
            forwardSpike[static_cast<size_t> (idx & mask)] = af > threshold * 1.25 * scale && af > 1.0e-6 ? 1 : 0;
            track (scale, af);

            // 後ろ向きの残差（次数の分だけ前のサンプル。未来の次数個から当てる）
            const long long c = idx - kOrder;
            double eb = 0;
            for (int j = 0; j <= kOrder; ++j)
                eb += ar[static_cast<size_t> (j)] * at (c + j);
            const double ab = std::abs (eb);
            const bool backSpike = ab > threshold * 1.25 * scaleBack && ab > 1.0e-6;
            track (scaleBack, ab);
            const bool spike = backSpike && forwardSpike[static_cast<size_t> (c & mask)] != 0;

            if (spike)
            {
                if (runStart < 0)
                    runStart = c;
                runLength = static_cast<int> (c - runStart) + 1;
                quiet = 0;
            }
            else if (runStart >= 0)
            {
                // 点が終わってから LPC の次数の分だけ待つ（補間に、後ろの音が要るため）
                if (++quiet >= kOrder)
                {
                    if (runLength <= kMaxRun)
                    {
                        const long long start = runStart;
                        const long long from = start - kOrder;
                        const int total = kOrder + runLength + kOrder;
                        for (int k = 0; k < total; ++k)
                            segment[static_cast<size_t> (k)] = at (from + k);
                        lsarInterpolate (segment.data(), kOrder, runLength, ar.data(), kOrder, work);
                        for (int k = 0; k < runLength; ++k)
                            at (start + k) = segment[static_cast<size_t> (kOrder + k)];
                        ++count;
                    }
                    runStart = -1;
                    runLength = 0;
                    quiet = 0;
                }
            }
        }

        data[i] = idx >= latency ? at (idx - latency) : 0.0f;
        ++sampleIndex;
    }
}

} // namespace vcab
