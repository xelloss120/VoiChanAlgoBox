#include "NoiseTracker.h"

#include "DenoiseDetail.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace vcab
{

using namespace denoise_detail;



void NoiseTracker::prepare (int numChannels, double frameRate)
{
    channels = numChannels;
    frameRateHz = frameRate;
    smoothCoeff = 1.0 - std::exp (-1.0 / (frameRate * kSmoothSeconds));
    subLength = subLengthFor (minWindow, frameRate);
    learnFrames = std::max (1, static_cast<int> (std::lround (2.0 * frameRate)));
    const auto c = static_cast<size_t> (channels);
    smooth.assign (c, 0.0f);
    subMin.assign (c, 0.0f);
    rawMin.assign (c, 0.0f);
    autoEstimate.assign (c, 0.0f);
    if (bias.size() != c)
        bias.assign (c, 1.0f);
    learned.assign (c, 0.0f);
    learnSum.assign (c, 0.0);
    mins.assign (kSubWindows, std::vector<float> (c, 0.0f));
    spp.assign (c, 0.0f);
    sppPresence.assign (c, 0.0f);
    // Gerkmann の声の推定で追う見積もりの重み（論文の 16ms 間隔の値 0.8 / 0.9 を、今の間隔に合わせる）
    sppNoiseCoeff = std::pow (0.8, 1.0 / (frameRate * 0.016));
    sppPresenceCoeff = std::pow (0.9, 1.0 / (frameRate * 0.016));
    learnedValid = false;
    learnTarget = learnCount = 0;
    // RNNoise の声の判定で更新を止める見積もり
    vadDelay = std::max (1, static_cast<int> (std::ceil (kVadDelaySeconds * frameRate)));
    vadQueue.assign (static_cast<size_t> (vadDelay), std::vector<float> (c, 0.0f));
    vadEst.assign (c, 0.0f);
    vadOut.assign (c, 0.0f);
    vadSlowCoeff = 1.0 - std::exp (-1.0 / (frameRate * 0.4));  // 声のない間は、約 0.4 秒でならして追う
    vadStuckFrames = static_cast<int> (std::lround (4.0 * frameRate));
    vadStuckCoeff = 1.0 - std::exp (-1.0 / (frameRate * 2.0));
    updateVadNeed();
    // 音量の判定で更新を止める見積もり
    levelEst.assign (c, 0.0f);
    levelOut.assign (c, 0.0f);
    levelHangFrames = std::max (1, static_cast<int> (std::lround (0.1 * frameRate))); // 「声あり」の後 0.1 秒は更新しない
    reset();
}


void NoiseTracker::setMode (NoiseMode m)
{
    if (m == mode)
        return;
    mode = m;
    if (startsFromLearned (mode) && learnedValid)
        seedFromLearned();
}


void NoiseTracker::setLevelThreshold (double db)
{
    if (db == levelThresholdDb)
        return;
    levelThresholdDb = db;
    levelThreshold = std::pow (10.0, db / 10.0);
}


void NoiseTracker::setSppSnr (double db)
{
    if (db == sppSnrDb)
        return;
    sppSnrDb = db;
    xiH1 = std::pow (10.0, db / 10.0);
}


void NoiseTracker::setMinWindow (double seconds)
{
    if (seconds == minWindow)
        return;
    minWindow = seconds;
    applyMinWindow();
}


void NoiseTracker::applyMinWindow()
{
    // 区切りの長さだけ変える（それまでの区切りの最小値は、入れ替わるまで使う）
    subLength = subLengthFor (minWindow, frameRateHz);
    if (biasByLength.empty())
        return;
    // 測った長さの間は、長さの対数で倍率の対数を補間する。測った長さちょうどなら、測った値そのもの
    constexpr size_t n = std::size (kWindowLengths);
    size_t j = 0;
    while (j + 2 < n && minWindow > kWindowLengths[j + 1])
        ++j;
    const double t = std::clamp (std::log (minWindow / kWindowLengths[j]) / std::log (kWindowLengths[j + 1] / kWindowLengths[j]), 0.0, 1.0);
    const auto& a = biasByLength[j];
    const auto& b = biasByLength[j + 1];
    for (size_t k = 0; k < bias.size(); ++k)
        bias[k] = t <= 0.0 ? a[k] : t >= 1.0 ? b[k]
                                           : static_cast<float> (std::exp ((1.0 - t) * std::log (a[k]) + t * std::log (b[k])));
}


void NoiseTracker::seedFromLearned()
{
    constexpr int kSeeded = 1000; // 更新の回数を多いことにして、学習した値から、ふだんの速さ（約 0.4 秒）で追う
    switch (noiseTracking (mode))
    {
        case NoiseTracking::gerkmann:
            std::copy (learned.begin(), learned.end(), spp.begin());
            std::fill (sppPresence.begin(), sppPresence.end(), 0.0f);
            sppFirst = false;
            break;
        case NoiseTracking::level:
            std::copy (learned.begin(), learned.end(), levelEst.begin());
            std::copy (learned.begin(), learned.end(), levelOut.begin());
            levelUpdates = kSeeded;
            break;
        case NoiseTracking::rnnoise:
            std::copy (learned.begin(), learned.end(), vadEst.begin());
            std::copy (learned.begin(), learned.end(), vadOut.begin());
            vadUpdates = kSeeded;
            vadSinceUpdate = 0;
            break;
        default:
            break;
    }
}


void NoiseTracker::updateVadNeed()
{
    // 遅らせたフレームの分析の窓（settleLength）の前 50ms から、今（判定が出ている所）までが、ずっと「声なし」なら使う
    vadNeedSamples = static_cast<int> (std::lround ((kVadDelaySeconds + 0.05) * sampleRateHz)) + settleLength;
}


void NoiseTracker::setSettleSamples (double sampleRate, int settleSamples)
{
    quietLength = std::max (1, static_cast<int> (std::lround (0.002 * sampleRate))); // 2ms
    settleLength = std::max (0, settleSamples);
    quietRun = validRun = 0;
    sampleRateHz = sampleRate;
    updateVadNeed();
}


void NoiseTracker::reset()
{
    first = true;
    sppFirst = true;
    subCount = subIndex = filled = 0;
    quietRun = validRun = 0;
    std::fill (subMin.begin(), subMin.end(), std::numeric_limits<float>::max());
    // 最初の見積もりができるまでは下げない（前の状態や、倍率を測ったときの白色雑音の値を残さない）
    std::fill (autoEstimate.begin(), autoEstimate.end(), 0.0f);
    std::fill (spp.begin(), spp.end(), 0.0f);
    levelUpdates = levelHold = 0;
    std::fill (levelEst.begin(), levelEst.end(), 0.0f);
    std::fill (levelOut.begin(), levelOut.end(), 0.0f);
    vadPos = vadFilled = vadUpdates = vadSinceUpdate = voiceQuiet = 0;
    std::fill (vadEst.begin(), vadEst.end(), 0.0f);
    std::fill (vadOut.begin(), vadOut.end(), 0.0f);
    if (startsFromLearned (mode) && learnedValid)
        seedFromLearned();
}


void NoiseTracker::clearLearned()
{
    learnedValid = false;
    learnTarget = learnCount = 0;
}


void NoiseTracker::startLearning()
{
    learnTarget = learnFrames;
    learnCount = 0;
    std::fill (learnSum.begin(), learnSum.end(), 0.0);
}


void NoiseTracker::beginCalibration (int warmupFrames)
{
    bias.assign (static_cast<size_t> (channels), 1.0f);
    calibFrames.clear();
    calibWarmup = warmupFrames;
    calibEnd = -1;
    reset();
}


void NoiseTracker::finishCalibration (bool uniform)
{
    // 測った大きさを、長さごとに push と同じ手順で最小値を追い直す
    const auto c = static_cast<size_t> (channels);
    const int frames = static_cast<int> (calibFrames.size());
    const int span = (calibEnd < 0 ? frames : calibEnd) - calibWarmup;
    biasByLength.assign (std::size (kWindowLengths), std::vector<float> (c, 1.0f));
    std::vector<float> sm (c), sub (c), winMin (c);
    std::vector<std::vector<float>> subs (kSubWindows, std::vector<float> (c));
    std::vector<double> sumP (c), sumM (c);
    for (size_t j = 0; j < std::size (kWindowLengths); ++j)
    {
        const int len = subLengthFor (kWindowLengths[j], frameRateHz);
        const int start = calibWarmup + static_cast<int> (std::lround (std::max (0.0, kWindowLengths[j] - kDefaultMinWindow) * frameRateHz));
        const int end = std::min (frames, start + span);
        std::fill (sub.begin(), sub.end(), std::numeric_limits<float>::max());
        std::fill (winMin.begin(), winMin.end(), std::numeric_limits<float>::max());
        std::fill (sumP.begin(), sumP.end(), 0.0);
        std::fill (sumM.begin(), sumM.end(), 0.0);
        int count = 0, index = 0, used = 0;
        for (int f = 0; f < end; ++f)
        {
            const auto& power = calibFrames[static_cast<size_t> (f)];
            for (size_t i = 0; i < c; ++i)
            {
                const float p = std::max (power[i], kTiny);
                sm[i] = f == 0 ? p : sm[i] + static_cast<float> (smoothCoeff) * (p - sm[i]);
                sub[i] = std::min (sub[i], sm[i]);
                if (f >= start)
                {
                    sumP[i] += power[i];
                    sumM[i] += std::min (sub[i], winMin[i]);
                }
            }
            if (++count >= len)
            {
                subs[static_cast<size_t> (index)] = sub;
                index = (index + 1) % kSubWindows;
                used = std::min (used + 1, kSubWindows);
                std::fill (sub.begin(), sub.end(), std::numeric_limits<float>::max());
                count = 0;
                std::fill (winMin.begin(), winMin.end(), std::numeric_limits<float>::max());
                for (int u = 0; u < used; ++u)
                    for (size_t i = 0; i < c; ++i)
                        winMin[i] = std::min (winMin[i], subs[static_cast<size_t> (u)][i]);
            }
        }
        auto& b = biasByLength[j];
        double allP = 0, allM = 0;
        for (size_t i = 0; i < c; ++i)
        {
            // 端の区画（直流とナイキスト）は、ほかと統計が違うので、そろえるときの平均に入れない
            if (i > 0 && i + 1 < c)
            {
                allP += sumP[i];
                allM += sumM[i];
            }
            b[i] = sumM[i] > 0 ? static_cast<float> (sumP[i] / sumM[i]) : 1.0f;
        }
        if (uniform && allM > 0)
            std::fill (b.begin(), b.end(), static_cast<float> (allP / allM));
    }
    calibFrames.clear();
    calibFrames.shrink_to_fit();
    calibWarmup = -1;
    applyMinWindow();
    reset();
}


void NoiseTracker::push (const float* power)
{
    if (validRun < settleLength)
        return; // 無音の後で、分析の窓がまだ実際の音で埋まっていない

    for (int k = 0; k < channels; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const float p = std::max (power[k], kTiny);
        smooth[i] = first ? p : smooth[i] + static_cast<float> (smoothCoeff) * (p - smooth[i]);
        subMin[i] = std::min (subMin[i], smooth[i]);

        float m = subMin[i];
        for (int u = 0; u < filled; ++u)
            m = std::min (m, mins[static_cast<size_t> (u)][i]);
        rawMin[i] = m;
        autoEstimate[i] = m * bias[i];
    }
    first = false;

    // Gerkmann の声の推定で追う見積もり（Gerkmann・Hendriks 2012）。声がある場合の SNR を xiH1（初期値は論文の 15dB）と仮定し、
    // 今の大きさが「ノイズだけ」と「声あり」のどちらでありそうかの確率 p で、ノイズの更新の量を決める
    {
        for (int k = 0; k < channels; ++k)
        {
            const auto i = static_cast<size_t> (k);
            const double p = std::max (static_cast<double> (power[k]), 1.0e-20);
            if (sppFirst)
            {
                spp[i] = static_cast<float> (p); // 学習した値から始めるときは、seedFromLearned で sppFirst を下ろしてある
                sppPresence[i] = 0;
                continue;
            }
            const double gamma = p / std::max (static_cast<double> (spp[i]), 1.0e-20);
            double pH1 = 1.0 / (1.0 + (1.0 + xiH1) * std::exp (-gamma * xiH1 / (1.0 + xiH1)));
            sppPresence[i] = static_cast<float> (sppPresenceCoeff * sppPresence[i] + (1.0 - sppPresenceCoeff) * pH1);
            if (sppPresence[i] > 0.99f)
                pH1 = std::min (pH1, 0.99); // 声がずっとありそうなときも、少しずつは更新する（止まったままにならないように）
            const double expected = (1.0 - pH1) * p + pH1 * spp[i];
            spp[i] = static_cast<float> (sppNoiseCoeff * spp[i] + (1.0 - sppNoiseCoeff) * expected);
        }
        sppFirst = false;
    }

    // 音量の判定で更新を止める見積もり: 全体の音量が、今の見積もりの合計の levelThreshold 倍より小さい間だけ更新する。
    // 「声あり」の後 0.1 秒は更新しない（語尾を、ノイズとして覚えないように）。「声あり」が長く続いても、見積もりは動かさない
    // （話し続けた声を、ノイズとして覚えないように）。しきい値を超えるほどノイズが大きくなったときは、しきい値を上げて合わせ直してもらう
    {
        const float* base = levelUpdates > 0 ? levelEst.data() : autoEstimate.data();
        double sumP = 0, sumN = 0;
        for (int k = 0; k < channels; ++k)
        {
            sumP += std::max (power[k], kTiny);
            sumN += base[k];
        }
        const bool voice = sumP > sumN * levelThreshold;
        if (voice)
            levelHold = levelHangFrames;
        else if (levelHold > 0)
            --levelHold;
        if (! voice && levelHold == 0)
        {
            const double a = std::max (1.0 / (levelUpdates + 1), vadSlowCoeff); // 最初は平均、その後は約 0.4 秒でならす
            for (int k = 0; k < channels; ++k)
                levelEst[static_cast<size_t> (k)] += static_cast<float> (a) * (std::max (power[k], kTiny) - levelEst[static_cast<size_t> (k)]);
            ++levelUpdates;
        }
        std::copy (levelUpdates > 0 ? levelEst.begin() : autoEstimate.begin(), levelUpdates > 0 ? levelEst.end() : autoEstimate.end(), levelOut.begin());
    }

    // RNNoise の声の判定で更新を止める見積もり: vadDelay フレーム前の大きさを、その前後がずっと「声なし」と確かめられたときだけ使う
    {
        auto& slot = vadQueue[static_cast<size_t> (vadPos)];
        if (vadFilled >= vadDelay && voiceQuiet >= vadNeedSamples)
        {
            // 最初は平均（早く落ち着くように）、その後は約 0.4 秒でならす
            const double a = std::max (1.0 / (vadUpdates + 1), vadSlowCoeff);
            for (int k = 0; k < channels; ++k)
                vadEst[static_cast<size_t> (k)] += static_cast<float> (a) * (slot[static_cast<size_t> (k)] - vadEst[static_cast<size_t> (k)]);
            ++vadUpdates;
            vadSinceUpdate = 0;
        }
        else if (vadUpdates > 0 && ++vadSinceUpdate > vadStuckFrames)
        {
            // 長く「声あり」が続くときは、最小値の見積もりの方が小さい区画だけ、そちらへゆっくり下げる（ノイズが小さくなったのに
            // 判定が止まったままのとき用）。上げる向きには寄せない（話し続けると最小値の見積もりは声で上がり、寄せると声を削るため）
            for (int k = 0; k < channels; ++k)
            {
                auto& v = vadEst[static_cast<size_t> (k)];
                v += static_cast<float> (vadStuckCoeff) * std::min (0.0f, autoEstimate[static_cast<size_t> (k)] - v);
            }
        }
        for (int k = 0; k < channels; ++k)
            slot[static_cast<size_t> (k)] = std::max (power[k], kTiny);
        vadPos = (vadPos + 1) % vadDelay;
        vadFilled = std::min (vadFilled + 1, vadDelay);
        // まだ一度も「声なし」を確かめられていない（いきなり話し始めた）ときは、最小値の見積もりを使う
        std::copy (vadUpdates > 0 ? vadEst.begin() : autoEstimate.begin(), vadUpdates > 0 ? vadEst.end() : autoEstimate.end(), vadOut.begin());
    }

    if (++subCount >= subLength)
    {
        mins[static_cast<size_t> (subIndex)] = subMin;
        subIndex = (subIndex + 1) % kSubWindows;
        filled = std::min (filled + 1, kSubWindows);
        std::fill (subMin.begin(), subMin.end(), std::numeric_limits<float>::max());
        subCount = 0;
    }

    if (calibWarmup >= 0)
        calibFrames.emplace_back (power, power + channels);

    if (learnTarget > 0)
    {
        for (int k = 0; k < channels; ++k)
            learnSum[static_cast<size_t> (k)] += std::max (power[k], kTiny);
        if (++learnCount >= learnTarget)
        {
            for (int k = 0; k < channels; ++k)
                learned[static_cast<size_t> (k)] = static_cast<float> (learnSum[static_cast<size_t> (k)] / learnCount);
            learnedValid = true;
            if (startsFromLearned (mode))
                seedFromLearned(); // 学習＋自動は、学習した値から始める
            learnTarget = learnCount = 0;
        }
    }
}


void NoiseTracker::importLearned (const float* values, int count)
{
    if (count != channels)
        return;
    std::copy (values, values + count, learned.begin());
    learnedValid = true;
    if (startsFromLearned (mode))
        seedFromLearned(); // 学習＋自動は、学習した値から始める
}



void LateReverb::prepare (int numChannels, double rate)
{
    channels = numChannels;
    frameRate = rate;
    delayFrames = std::max (1, static_cast<int> (std::lround (0.08 * frameRate))); // 80ms（直接音と初期反射の後）
    smoothCoeff = 1.0 - std::exp (-1.0 / (0.01 * frameRate));
    smooth.assign (static_cast<size_t> (channels), 0.0f);
    out.assign (static_cast<size_t> (channels), 0.0f);
    history.assign (static_cast<size_t> (delayFrames), std::vector<float> (static_cast<size_t> (channels), 0.0f));
    reset();
}


void LateReverb::reset()
{
    std::fill (smooth.begin(), smooth.end(), 0.0f);
    for (auto& h : history)
        std::fill (h.begin(), h.end(), 0.0f);
    pos = 0;
}


void LateReverb::push (const float* power)
{
    for (int k = 0; k < channels; ++k)
    {
        auto& v = smooth[static_cast<size_t> (k)];
        v += static_cast<float> (smoothCoeff) * (power[k] - v);
    }
    history[static_cast<size_t> (pos)] = smooth;
    pos = (pos + 1) % delayFrames;
}


const float* LateReverb::estimate (double rt60Seconds)
{
    // 残響は、60dB 下がるのに rt60 秒かかる指数の減衰とみなす。50ms 前の音の、その間に減衰した残り
    const double delta = 3.0 * std::log (10.0) / std::max (0.05, rt60Seconds);
    const auto factor = static_cast<float> (std::exp (-2.0 * delta * delayFrames / frameRate));
    const auto& old = history[static_cast<size_t> (pos)]; // 一番古い（delayFrames 前）
    for (int k = 0; k < channels; ++k)
        out[static_cast<size_t> (k)] = old[static_cast<size_t> (k)] * factor;
    return out.data();
}

} // namespace vcab
