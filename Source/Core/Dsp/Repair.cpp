#include "Repair.h"

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

// 4 点（t = x0..x3 で y0..y3）を通る 3 次の式の、t での値（ラグランジュ補間）
double cubicThrough (const double* x, const double* y, double t)
{
    double sum = 0;
    for (int i = 0; i < 4; ++i)
    {
        double term = y[i];
        for (int j = 0; j < 4; ++j)
            if (j != i)
                term *= (t - x[j]) / (x[i] - x[j]);
        sum += term;
    }
    return sum;
}
} // namespace

// ================= 音割れの修復 =================

void Declipper::prepare (double sampleRate)
{
    sr = sampleRate;
    maxRun = static_cast<int> (std::lround (0.002 * sr)); // これより長く平らな区間は、割れではなく直せないものとして触らない
    lookahead = maxRun + 3;
    line.assign (static_cast<size_t> (lookahead + 3), 0.0f);
    reset();
}

void Declipper::reset()
{
    std::fill (line.begin(), line.end(), 0.0f);
    pos = 0;
    skip = 0;
}

void Declipper::process (float* data, int n, double clipLevelDb)
{
    const auto level = static_cast<float> (std::pow (10.0, clipLevelDb / 20.0));
    const int size = static_cast<int> (line.size());
    // s(k): 今出す音（lookahead 前）から k 個あとの音
    auto at = [&] (int k) -> float& { return line[static_cast<size_t> (((pos - lookahead + k) % size + size) % size)]; };

    for (int i = 0; i < n; ++i)
    {
        line[static_cast<size_t> (pos)] = data[i];

        if (skip > 0)
        {
            --skip;
        }
        else
        {
            const float s0 = at (0);
            const bool clipped0 = std::abs (s0) >= level;
            // 割れの区間の始まり（前は割れていない、今と次が同じ符号で割れている）
            if (clipped0 && std::abs (at (-1)) < level && std::abs (at (1)) >= level && (s0 > 0) == (at (1) > 0))
            {
                int len = 1;
                while (len < maxRun && std::abs (at (len)) >= level && (at (len) > 0) == (s0 > 0))
                    ++len;
                // 区間の後ろの 2 サンプルが見えていて、そこが割れていなければ直す
                if (len < maxRun && len + 1 <= lookahead && std::abs (at (len)) < level)
                {
                    const double x[4] = { -2.0, -1.0, static_cast<double> (len), static_cast<double> (len + 1) };
                    const double y[4] = { at (-2), at (-1), at (len), at (len + 1) };
                    for (int k = 0; k < len; ++k)
                    {
                        const double v = cubicThrough (x, y, k);
                        // 作り直した値が、割れの大きさより小さくなるところは、元のまま（平らなまま）にする
                        if (std::abs (v) > std::abs (at (k)) && (v > 0) == (s0 > 0))
                            at (k) = static_cast<float> (v);
                    }
                    skip = len - 1;
                    ++count;
                }
            }
        }

        data[i] = at (0);
        pos = (pos + 1) % size;
    }
}

// ================= 吹かれ・破裂音 =================

void PlosiveSuppressor::prepare (double sampleRate)
{
    sr = sampleRate;
    voiceBand[0].setCoeffs (BiquadCoeffs::highPass (sr, 300.0, 0.7071));
    voiceBand[1].setCoeffs (BiquadCoeffs::lowPass (sr, 3000.0, 0.7071));
    envAttack = coeffFor (0.001, sr);
    envRelease = coeffFor (0.03, sr);
    closeCoeff = coeffFor (0.001, sr);
    openCoeff = coeffFor (0.06, sr);
    cutoff = -1;
    setCutoff (120.0);
    reset();
}

void PlosiveSuppressor::setCutoff (double hz)
{
    if (std::abs (hz - cutoff) < 0.5)
        return;
    cutoff = hz;
    for (auto& b : low)
        b.setCoeffs (BiquadCoeffs::lowPass (sr, hz, 0.7071));
    for (auto& b : high)
        b.setCoeffs (BiquadCoeffs::highPass (sr, hz, 0.7071));
}

void PlosiveSuppressor::reset()
{
    for (auto* set : { low, high, voiceBand })
        for (int k = 0; k < 2; ++k)
            set[k].reset();
    lowEnv = voiceEnv = 0;
    gainDb = 0;
    active = false;
}

void PlosiveSuppressor::process (float* data, int n, double cutoffHz, double thresholdDb, double depthDb)
{
    setCutoff (cutoffHz);
    for (int i = 0; i < n; ++i)
    {
        const float x = data[i];
        const float lo = low[1].process (low[0].process (x));
        const float hi = high[1].process (high[0].process (x));
        const float v = voiceBand[1].process (voiceBand[0].process (x));
        const float pl = lo * lo, pv = v * v;
        lowEnv += (pl > lowEnv ? envAttack : envRelease) * (pl - lowEnv);
        voiceEnv += (pv > voiceEnv ? envAttack : envRelease) * (pv - voiceEnv);

        // 低い側が声の帯域より「しきい値」以上大きく、-50dBFS より大きいとき
        const float ratioDb = 10.0f * std::log10 ((lowEnv + 1.0e-12f) / (voiceEnv + 1.0e-12f));
        const bool now = ratioDb > thresholdDb && lowEnv > 1.0e-5f;
        if (now && ! active)
            ++count;
        active = now;

        const float target = active ? static_cast<float> (-depthDb) : 0.0f;
        gainDb += (target - gainDb) * (target < gainDb ? closeCoeff : openCoeff);
        data[i] = lo * std::pow (10.0f, gainDb / 20.0f) + hi;
    }
}

// ================= リップノイズ =================

void MouthClickSuppressor::prepare (double sampleRate)
{
    sr = sampleRate;
    lookahead = static_cast<int> (std::lround (0.002 * sr));
    holdLength = static_cast<int> (std::lround (0.003 * sr)) + lookahead;
    for (auto& b : detectHigh)
        b.setCoeffs (BiquadCoeffs::highPass (sr, 3000.0, 0.7071));
    for (auto& b : low)
        b.setCoeffs (BiquadCoeffs::lowPass (sr, 3000.0, 0.7071));
    for (auto& b : high)
        b.setCoeffs (BiquadCoeffs::highPass (sr, 3000.0, 0.7071));
    fastAttack = coeffFor (0.00005, sr);
    fastRelease = coeffFor (0.0005, sr);
    slowCoeff = coeffFor (0.005, sr);
    closeCoeff = coeffFor (0.0002, sr);
    openCoeff = coeffFor (0.003, sr);
    lowLine.assign (static_cast<size_t> (lookahead + 1), 0.0f);
    highLine.assign (static_cast<size_t> (lookahead + 1), 0.0f);
    fastRing.assign (static_cast<size_t> (lookahead + 1), 0.0f);
    onsetRing.assign (static_cast<size_t> (lookahead + 1), 0);
    reset();
}

void MouthClickSuppressor::reset()
{
    for (auto* set : { detectHigh, low, high })
        for (int k = 0; k < 2; ++k)
            set[k].reset();
    std::fill (lowLine.begin(), lowLine.end(), 0.0f);
    std::fill (highLine.begin(), highLine.end(), 0.0f);
    std::fill (fastRing.begin(), fastRing.end(), 0.0f);
    std::fill (onsetRing.begin(), onsetRing.end(), static_cast<unsigned char> (0));
    fast = slow = 0;
    gainDb = 0;
    prevScore = -100;
    hold = 0;
    pos = 0;
}

void MouthClickSuppressor::process (float* data, int n, double thresholdDb, double depthDb)
{
    const int size = lookahead + 1;
    for (int i = 0; i < n; ++i)
    {
        const float x = data[i];
        const float d = detectHigh[1].process (detectHigh[0].process (x));
        const float p = d * d;
        fast += (p > fast ? fastAttack : fastRelease) * (p - fast);
        slow += slowCoeff * (p - slow);
        const float score = 10.0f * std::log10 ((fast + 1.0e-12f) / (slow + 1.0e-10f));
        const bool onset = score > thresholdDb && prevScore <= thresholdDb && fast > 1.0e-8f;
        prevScore = score;

        lowLine[static_cast<size_t> (pos)] = low[1].process (low[0].process (x));
        highLine[static_cast<size_t> (pos)] = high[1].process (high[0].process (x));
        fastRing[static_cast<size_t> (pos)] = fast;
        onsetRing[static_cast<size_t> (pos)] = onset ? 1 : 0;

        // lookahead 前に立ち上がった音が、今までに 6dB 以上小さくなっていれば、短い音（リップノイズ）とみなす
        const int oldest = (pos + 1) % size;
        if (onsetRing[static_cast<size_t> (oldest)] != 0)
        {
            float peak = 0;
            for (float f : fastRing)
                peak = std::max (peak, f);
            if (fast < peak * 0.25f)
            {
                hold = holdLength;
                ++count;
            }
        }

        const float target = hold > 0 ? static_cast<float> (-depthDb) : 0.0f;
        if (hold > 0)
            --hold;
        gainDb += (target - gainDb) * (target < gainDb ? closeCoeff : openCoeff);
        // 低い側（声）はそのまま、高い側だけを下げる
        data[i] = lowLine[static_cast<size_t> (oldest)] + highLine[static_cast<size_t> (oldest)] * std::pow (10.0f, gainDb / 20.0f);
        pos = oldest;
    }
}

// ================= 息継ぎ =================

void BreathSuppressor::prepare (double sampleRate)
{
    sr = sampleRate;
    hop = std::max (1, static_cast<int> (std::lround (0.005 * sr)));
    hopSeconds = hop / sr;
    lookahead = hop; // 5ms 先まで見る
    line.assign (static_cast<size_t> (lookahead + 1), 0.0f);
    // 倍音らしさは 12kHz 前後に落として測る（3kHz の低域通過のあと間引く）
    decim = std::max (1, static_cast<int> (std::lround (sr / 12000.0)));
    const double low = sr / decim;
    minLag = static_cast<int> (low / 400.0);
    maxLag = static_cast<int> (std::ceil (low / 80.0));
    window = static_cast<int> (std::lround (0.030 * low));
    lowRing.assign (1u << 12, 0.0f);
    decimLow[0].setCoeffs (BiquadCoeffs::lowPass (sr, 3000.0, 0.5412));
    decimLow[1].setCoeffs (BiquadCoeffs::lowPass (sr, 3000.0, 1.3066));
    mid[0].setCoeffs (BiquadCoeffs::highPass (sr, 300.0, 0.7071));
    mid[1].setCoeffs (BiquadCoeffs::lowPass (sr, 4000.0, 0.7071));
    high[0].setCoeffs (BiquadCoeffs::highPass (sr, 4000.0, 0.5412));
    high[1].setCoeffs (BiquadCoeffs::highPass (sr, 4000.0, 1.3066));
    minRun = std::max (1, static_cast<int> (std::lround (0.040 / hopSeconds)));
    fastAttack = coeffFor (0.002, sr);
    fastRelease = coeffFor (0.020, sr);
    closeCoeff = coeffFor (0.020, sr);
    openCoeff = coeffFor (0.003, sr);
    reset();
}

void BreathSuppressor::reset()
{
    std::fill (line.begin(), line.end(), 0.0f);
    std::fill (lowRing.begin(), lowRing.end(), 0.0f);
    for (auto* set : { decimLow, mid, high })
        for (int k = 0; k < 2; ++k)
            set[k].reset();
    pos = sinceHop = decimPhase = hopIndex = run = 0;
    lowCount = 0;
    eFull = eMid = eHigh = 0;
    for (int k = 0; k < kLevelHops; ++k)
        hopFull[k] = hopMid[k] = hopHigh[k] = 0;
    speechDb = breathDb = -200;
    floorDb = 200;
    breath = false;
    fast = gainDb = 0;
}

void BreathSuppressor::analyzeHop (double marginDb)
{
    hopFull[hopIndex] = eFull;
    hopMid[hopIndex] = eMid;
    hopHigh[hopIndex] = eHigh;
    hopIndex = (hopIndex + 1) % kLevelHops;
    eFull = eMid = eHigh = 0;
    double full = 0, midE = 0, highE = 0;
    for (int k = 0; k < kLevelHops; ++k)
    {
        full += hopFull[k];
        midE += hopMid[k];
        highE += hopHigh[k];
    }
    const double levelDb = 10.0 * std::log10 (full / (kLevelHops * hop) + 1.0e-20);
    const double highRatioDb = 10.0 * std::log10 ((highE + 1.0e-20) / (midE + 1.0e-20));

    // 倍音らしさ: 直近 30ms の、周期 2.5〜12.5ms の正規化した相関の最大
    double voicing = 0;
    if (lowCount > window + maxLag)
    {
        const long long mask = static_cast<long long> (lowRing.size()) - 1;
        const long long end = lowCount;
        double e0 = 0;
        for (long long t = end - window; t < end; ++t)
            e0 += static_cast<double> (lowRing[static_cast<size_t> (t & mask)]) * lowRing[static_cast<size_t> (t & mask)];
        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            double c = 0, e1 = 0;
            for (long long t = end - window; t < end; ++t)
            {
                const double a = lowRing[static_cast<size_t> (t & mask)], b = lowRing[static_cast<size_t> ((t - lag) & mask)];
                c += a * b;
                e1 += b * b;
            }
            voicing = std::max (voicing, c / std::sqrt (e0 * e1 + 1.0e-30));
        }
    }

    // 背景はゆっくり上がる最小値（毎秒 3dB）。最近の声は、有声音の大きさの最大（毎秒 2dB で下がる）
    floorDb = std::min (levelDb, floorDb + 3.0 * hopSeconds);
    const bool voiced = voicing > 0.5 && levelDb > floorDb + 6.0;
    speechDb = voiced ? std::max (levelDb, speechDb) : speechDb - 2.0 * hopSeconds;

    const bool candidate = ! voiced && voicing < 0.4 && levelDb > floorDb + 6.0 && levelDb < speechDb - marginDb && highRatioDb < 0.0;
    run = candidate ? run + 1 : 0;
    if (! breath && run >= minRun)
    {
        breath = true;
        breathDb = levelDb;
        ++count;
    }
    else if (breath && ! candidate)
        breath = false;
    if (breath)
        breathDb = std::max (breathDb, levelDb);
}

void BreathSuppressor::process (float* data, int n, double marginDb, double depthDb)
{
    const int size = static_cast<int> (line.size());
    const long long mask = static_cast<long long> (lowRing.size()) - 1;
    for (int i = 0; i < n; ++i)
    {
        const float x = data[i];
        eFull += static_cast<double> (x) * x;
        const float m = mid[1].process (mid[0].process (x));
        eMid += static_cast<double> (m) * m;
        const float h = high[1].process (high[0].process (x));
        eHigh += static_cast<double> (h) * h;
        const float lo = decimLow[1].process (decimLow[0].process (x));
        if (++decimPhase >= decim)
        {
            decimPhase = 0;
            lowRing[static_cast<size_t> (lowCount & mask)] = lo;
            ++lowCount;
        }
        if (++sinceHop >= hop)
        {
            sinceHop = 0;
            analyzeHop (marginDb);
        }

        // 言葉の出だし: 速い音量が、息継ぎの大きさより 10dB 上がったら、すぐにやめる（雑音の一瞬の山は
        // 平均より 6dB ほど上に出るので、6dB では息継ぎの最中に何度もやめてしまった）
        const float p = x * x;
        fast += (p > fast ? fastAttack : fastRelease) * (p - fast);
        if (breath && 10.0 * std::log10 (fast + 1.0e-20) > breathDb + 10.0)
        {
            breath = false;
            run = 0;
        }

        line[static_cast<size_t> (pos)] = x;
        const float delayed = line[static_cast<size_t> ((pos + 1) % size)]; // lookahead サンプル前
        pos = (pos + 1) % size;
        const float target = breath ? static_cast<float> (-depthDb) : 0.0f;
        gainDb += (target - gainDb) * (target < gainDb ? closeCoeff : openCoeff);
        data[i] = delayed * std::pow (10.0f, gainDb / 20.0f);
    }
}

} // namespace vcab
