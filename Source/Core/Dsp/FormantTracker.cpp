#include "FormantTracker.h"

#include "Lpc.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr double kRefHz[3] = { 500.0, 1500.0, 2500.0 };   // 目安の位置
constexpr double kMinHz[3] = { 180.0, 550.0, 1500.0 };    // それぞれの範囲
constexpr double kMaxHz[3] = { 1100.0, 3000.0, 4000.0 };
constexpr double kMaxBandwidth = 700.0;
constexpr int kMaxCandidates = 8;
} // namespace

void FormantTracker::prepare (double sampleRate)
{
    sr = sampleRate;
    decim = std::max (1, static_cast<int> (std::lround (sr / 12000.0)));
    fsLow = sr / decim;
    hop = std::max (1, static_cast<int> (std::lround (0.005 * fsLow)));
    window = static_cast<int> (std::lround (0.025 * fsLow));
    voicingWindow = static_cast<int> (std::lround (0.030 * fsLow));
    minLag = static_cast<int> (fsLow / 400.0);
    maxLag = static_cast<int> (std::ceil (fsLow / 80.0));
    int size = 1;
    while (size < voicingWindow + maxLag + window + 16)
        size <<= 1;
    ring.assign (static_cast<size_t> (size), 0.0f);
    const double cutoff = std::min (5000.0, 0.42 * fsLow);
    antiAlias[0].setCoeffs (BiquadCoeffs::lowPass (sr, cutoff, 0.5412));
    antiAlias[1].setCoeffs (BiquadCoeffs::lowPass (sr, cutoff, 1.3066));
    frame.assign (static_cast<size_t> (window), 0.0);
    hamming.resize (static_cast<size_t> (window));
    for (int i = 0; i < window; ++i)
        hamming[static_cast<size_t> (i)] = 0.54 - 0.46 * std::cos (2.0 * kPi * (i + 0.5) / window);
    autocorr.assign (kOrder + 1, 0.0);
    a.assign (kOrder + 1, 0.0);
    k.assign (kOrder + 1, 0.0);
    tmp.assign (kOrder + 1, 0.0);
    const double hopSeconds = hop / fsLow;
    smoothCoeff = 1.0 - std::exp (-hopSeconds / 0.010);
    returnCoeff = 1.0 - std::exp (-hopSeconds / 0.050);
    reset();
}

void FormantTracker::reset()
{
    antiAlias[0].reset();
    antiAlias[1].reset();
    std::fill (ring.begin(), ring.end(), 0.0f);
    count = 0;
    decimPhase = sinceHop = 0;
    rootsWarm = false;
    peakDb = -200;
    for (int i = 0; i < 3; ++i)
        prevHz[i] = current.hz[i] = FormantWarp::kBandHz[i];
    prevValid = false;
    current.valid = false;
    sinceValid = 1000000;
    lastVoicing = 0;
}

void FormantTracker::process (const float* data, int n)
{
    const long long mask = static_cast<long long> (ring.size()) - 1;
    for (int i = 0; i < n; ++i)
    {
        const float lo = antiAlias[1].process (antiAlias[0].process (data[i]));
        if (++decimPhase < decim)
            continue;
        decimPhase = 0;
        ring[static_cast<size_t> (count & mask)] = lo;
        ++count;
        if (++sinceHop >= hop)
        {
            sinceHop = 0;
            analyze();
        }
    }
}

bool FormantTracker::findRoots()
{
    // A(z) の根 = z^p + a1 z^(p-1) + ... + ap の根（デュラン・ケルナー法）
    using C = std::complex<double>;
    if (! rootsWarm)
        for (int i = 0; i < kOrder; ++i)
            roots[i] = std::polar (0.9, 2.0 * kPi * (i + 0.25) / kOrder);
    for (int iter = 0; iter < 80; ++iter)
    {
        double change = 0;
        for (int i = 0; i < kOrder; ++i)
        {
            C num = 1.0;
            for (int j = 1; j <= kOrder; ++j)
                num = num * roots[i] + a[static_cast<size_t> (j)];
            C den = 1.0;
            for (int j = 0; j < kOrder; ++j)
                if (j != i)
                    den *= roots[i] - roots[j];
            if (std::abs (den) < 1.0e-30)
                den = 1.0e-30;
            const C step = num / den;
            roots[i] -= step;
            change = std::max (change, std::abs (step));
        }
        if (change < 1.0e-9)
            break;
    }
    for (int i = 0; i < kOrder; ++i)
        if (! std::isfinite (roots[i].real()) || ! std::isfinite (roots[i].imag()) || std::abs (roots[i]) > 1.5)
        {
            rootsWarm = false; // 次は最初から
            return false;
        }
    rootsWarm = true;
    return true;
}

void FormantTracker::analyze()
{
    const long long mask = static_cast<long long> (ring.size()) - 1;
    if (count < voicingWindow + maxLag + 1)
        return;
    const double hopSeconds = hop / fsLow;

    // ---- 声の区間か（大きさと、倍音らしさ） ----
    double energy = 0;
    for (long long t = count - window; t < count; ++t)
        energy += static_cast<double> (ring[static_cast<size_t> (t & mask)]) * ring[static_cast<size_t> (t & mask)];
    const double levelDb = 10.0 * std::log10 (energy / window + 1.0e-20);
    peakDb = std::max (levelDb, peakDb - 3.0 * hopSeconds);

    double voicing = 0;
    {
        double e0 = 0;
        for (long long t = count - voicingWindow; t < count; ++t)
            e0 += static_cast<double> (ring[static_cast<size_t> (t & mask)]) * ring[static_cast<size_t> (t & mask)];
        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            double c = 0, e1 = 0;
            for (long long t = count - voicingWindow; t < count; ++t)
            {
                const double x0 = ring[static_cast<size_t> (t & mask)], x1 = ring[static_cast<size_t> ((t - lag) & mask)];
                c += x0 * x1;
                e1 += x1 * x1;
            }
            voicing = std::max (voicing, c / std::sqrt (e0 * e1 + 1.0e-30));
        }
    }
    lastVoicing = voicing;
    bool valid = voicing >= 0.5 && levelDb > peakDb - 40.0 && levelDb > -70.0;

    // ---- LPC の根から候補を出し、F1・F2・F3 を選ぶ ----
    double chosen[3] {};
    if (valid)
    {
        const double emphasis = std::exp (-2.0 * kPi * 50.0 / fsLow);
        for (int i = 0; i < window; ++i)
        {
            const long long t = count - window + i;
            const double x = ring[static_cast<size_t> (t & mask)] - emphasis * ring[static_cast<size_t> ((t - 1) & mask)];
            frame[static_cast<size_t> (i)] = x * hamming[static_cast<size_t> (i)];
        }
        for (int lag = 0; lag <= kOrder; ++lag)
        {
            double sum = 0;
            for (int i = lag; i < window; ++i)
                sum += frame[static_cast<size_t> (i)] * frame[static_cast<size_t> (i - lag)];
            autocorr[static_cast<size_t> (lag)] = sum;
        }
        autocorr[0] = autocorr[0] * (1.0 + 1.0e-9) + 1.0e-20;
        lpc::levinson (autocorr.data(), kOrder, a.data(), k.data(), tmp.data());

        double candHz[kMaxCandidates], candBw[kMaxCandidates];
        int numCand = 0;
        if (findRoots())
        {
            for (int i = 0; i < kOrder && numCand < kMaxCandidates; ++i)
            {
                if (roots[i].imag() <= 1.0e-6)
                    continue;
                const double hz = std::arg (roots[i]) * fsLow / (2.0 * kPi);
                const double bw = -std::log (std::abs (roots[i])) * fsLow / kPi;
                if (hz > 90.0 && bw < kMaxBandwidth && bw > 0)
                {
                    candHz[numCand] = hz;
                    candBw[numCand] = bw;
                    ++numCand;
                }
            }
            // 低い順に並べる
            for (int i = 1; i < numCand; ++i)
                for (int j = i; j > 0 && candHz[j] < candHz[j - 1]; --j)
                {
                    std::swap (candHz[j], candHz[j - 1]);
                    std::swap (candBw[j], candBw[j - 1]);
                }
        }
        const bool useContinuity = prevValid && sinceValid <= 10; // 50ms 以内に追いかけていたら、前の値に近いものを選ぶ
        double best = 1.0e30;
        for (int i = 0; i < numCand; ++i)
            for (int j = i + 1; j < numCand; ++j)
                for (int l = j + 1; l < numCand; ++l)
                {
                    const int idx[3] = { i, j, l };
                    double cost = 0;
                    bool ok = true;
                    for (int f = 0; f < 3 && ok; ++f)
                    {
                        const double hz = candHz[idx[f]];
                        ok = hz >= kMinHz[f] && hz <= kMaxHz[f];
                        cost += std::abs (std::log (hz / kRefHz[f]));
                        if (useContinuity)
                            cost += 2.0 * std::abs (std::log (hz / prevHz[f]));
                        cost += candBw[idx[f]] / 1000.0;
                    }
                    if (ok && cost < best)
                    {
                        best = cost;
                        for (int f = 0; f < 3; ++f)
                            chosen[f] = candHz[idx[f]];
                    }
                }
        valid = best < 1.0e29;
    }

    // ---- ならして、渡す ----
    if (valid)
    {
        for (int f = 0; f < 3; ++f)
            prevHz[f] = std::exp (std::log (prevHz[f]) + smoothCoeff * (std::log (chosen[f]) - std::log (prevHz[f])));
        prevValid = true;
        sinceValid = 0;
    }
    else
    {
        ++sinceValid;
        if (sinceValid * hopSeconds > 0.150)
        {
            for (int f = 0; f < 3; ++f)
                prevHz[f] = std::exp (std::log (prevHz[f]) + returnCoeff * (std::log (FormantWarp::kBandHz[f]) - std::log (prevHz[f])));
            prevValid = false;
        }
    }
    for (int f = 0; f < 3; ++f)
        current.hz[f] = prevHz[f];
    current.valid = valid;
}

} // namespace vcab
