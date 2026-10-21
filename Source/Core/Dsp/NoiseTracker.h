// ノイズの見積もり（最小値 / 音量の判定 / Gerkmann の声の推定 / RNNoise の声の判定 / 学習、と学習した値から始める自動）と、後部残響の見積もり
#pragma once

#include "../Choices.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vcab
{

class NoiseTracker
{
public:
    void prepare (int channels, double frameRate);
    void reset();          // 自動の状態だけ消す（学習した値は残す）
    void clearLearned();

    // 完全な無音（デジタルの 0）を見分けるため、分析に入れたサンプルを 1 つずつ渡す。無音の後（と、起動・リセットの直後）は、
    // 分析の窓（settleSamples）が実際の音で埋まるまで、push を見積もりに使わない（無音をノイズの最小値と取り違えると、
    // 1.6 秒ほどノイズの見積もりが 0 になり、デノイズが効かなくなるため）。その間は、無音の前の見積もりを使い続ける
    void setSettleSamples (double sampleRate, int settleSamples);
    void observe (float x) noexcept
    {
        if (x > -kSilentLevel && x < kSilentLevel)
        {
            if (++quietRun >= quietLength)
            {
                validRun = 0;
                return;
            }
        }
        else
        {
            quietRun = 0;
        }
        if (validRun < settleLength)
            ++validRun;
    }
    void push (const float* power);  // 1 フレーム分の、区画ごとの大きさ（2乗）
    // 今使っている見積もり方（塊ごとに渡す）。「学習＋自動」に切り替えたときは、学習した値から始め直す
    void setMode (NoiseMode m);
    // 音量の判定のしきい値（全体の音量が、見積もりの何 dB 上なら「声あり」とみなすか）
    void setLevelThreshold (double db);
    // Gerkmann の声の推定で仮定する、声があるときの SNR（dB）
    void setSppSnr (double db);
    // 最小値を探す長さ（秒）。最小値を本来の大きさに戻す倍率は、kWindowLengths で測った値の間を補間する
    void setMinWindow (double seconds);
    // RNNoise の声の判定で更新を止める見積もりのため、「声なし」と確かめられた長さ（サンプル数、今から数えて）を、
    // 塊ごとに渡す（判定を動かしていないときは 0 のまま。その間、この見積もりは更新されない）
    void setVoiceQuiet (int samples) noexcept { voiceQuiet = samples; }
    void startLearning();
    bool learning() const noexcept { return learnTarget > 0; }
    float learnProgress() const noexcept { return learnTarget > 0 ? static_cast<float> (learnCount) / learnTarget : 0.0f; }
    bool hasLearned() const noexcept { return learnedValid; }

    // 見積もり（2乗）。学習は、覚えるまでは自動（最小値）の値。「学習＋自動」は、学習した値から始めた、それぞれの追いかけ
    const float* estimate (NoiseMode m) const noexcept
    {
        switch (noiseTracking (m))
        {
            case NoiseTracking::fixed:    return learnedValid ? learned.data() : autoEstimate.data();
            case NoiseTracking::level:    return levelOut.data();
            case NoiseTracking::gerkmann: return spp.data();
            case NoiseTracking::rnnoise:  return vadOut.data();
            default:                      return autoEstimate.data();
        }
    }
    // 学習した値の書き出しと読み込み（保存のため）
    const std::vector<float>& learnedValues() const noexcept { return learned; }
    void importLearned (const float* values, int count);
    // 白色雑音で倍率を測る（warmupFrames の後の、平均と最小値の比。uniform なら全区画で同じ値にする）。
    // 最小値を探す長さごとに測る。kDefaultMinWindow より長い長さは、窓が埋まるのを待つ分だけ後ろへずらして、同じ長さだけ測る
    // （markCalibrationEnd を呼んだ所までが、kDefaultMinWindow 以下の長さで測る範囲）
    void beginCalibration (int warmupFrames);
    void markCalibrationEnd() noexcept { if (calibWarmup >= 0 && calibEnd < 0) calibEnd = static_cast<int> (calibFrames.size()); }
    void finishCalibration (bool uniform);

    static constexpr double kDefaultMinWindow = 1.6;
    static constexpr double kWindowLengths[] = { 0.8, 1.0, 1.2, 1.6, 2.0, 2.4, 3.2 };

private:
    static constexpr int kSubWindows = 8;
    static int subLengthFor (double window, double frameRate)
    {
        return std::max (1, static_cast<int> (std::lround (window / kSubWindows * frameRate)));
    }
    void applyMinWindow();
    double minWindow = kDefaultMinWindow;
    std::vector<std::vector<float>> biasByLength; // kWindowLengths ごとの倍率
    std::vector<std::vector<float>> calibFrames;  // 倍率を測る間の、区画ごとの大きさ
    int calibEnd = -1;
    static constexpr float kSilentLevel = 1.0e-6f; // -120dBFS より小さいサンプルが quietLength 続いたら、無音とみなす
    int quietLength = 96, settleLength = 0, quietRun = 0, validRun = 0;
    int channels = 0, subLength = 1, subCount = 0, subIndex = 0, filled = 0;
    int learnTarget = 0, learnCount = 0, learnFrames = 1;
    double smoothCoeff = 0.1;
    bool first = true, learnedValid = false;
    std::vector<float> smooth, subMin, rawMin, autoEstimate, bias, learned;
    std::vector<double> learnSum;
    int calibWarmup = -1;
    std::vector<std::vector<float>> mins;
    NoiseMode mode = NoiseMode::autoMinimum;
    void seedFromLearned(); // 今の見積もり方（学習＋自動）の追いかけを、学習した値から始める
    // 音量の判定で更新を止める見積もり
    std::vector<float> levelEst, levelOut;
    int levelUpdates = 0, levelHold = 0, levelHangFrames = 1;
    double levelThresholdDb = 6.0, levelThreshold = 3.9810717055349722; // 6dB と、その倍率
    // Gerkmann の声の推定で追う見積もり
    std::vector<float> spp, sppPresence;
    bool sppFirst = true;
    double sppNoiseCoeff = 0.8, sppPresenceCoeff = 0.9;
    double sppSnrDb = 15.0, xiH1 = 31.622776601683793; // 声があるときの SNR の仮定（15dB）
    static constexpr double kVadDelaySeconds = 0.06; // 判定の遅れ（RNNoise の 1 回分 10ms ＋ サンプルレートの変換）より十分長く
    void updateVadNeed();
    // RNNoise の声の判定で更新を止める見積もり。分析した大きさを vadDelay フレーム遅らせ、その間に「声なし」が確かめられたものだけで更新する
    // （判定は 10ms ごとで、少し遅れて出るため。声の出だしを、ノイズとして覚えないように）
    std::vector<std::vector<float>> vadQueue;
    std::vector<float> vadEst, vadOut;
    int vadDelay = 1, vadPos = 0, vadFilled = 0, vadUpdates = 0, vadSinceUpdate = 0, vadStuckFrames = 1, voiceQuiet = 0;
    int vadNeedSamples = 0;
    double frameRateHz = 100, sampleRateHz = 48000, vadSlowCoeff = 0.01, vadStuckCoeff = 0.01;
};

// 残響の後ろの部分（後部残響）の見積もり。区画ごとの大きさを 80ms 遅らせ、残響の長さで決まる分だけ減衰させる
class LateReverb
{
public:
    void prepare (int channels, double frameRate);
    void reset();
    void push (const float* power);
    const float* estimate (double rt60Seconds);

private:
    int channels = 0, delayFrames = 1, pos = 0;
    double frameRate = 100, smoothCoeff = 0.5;
    std::vector<float> smooth, out;
    std::vector<std::vector<float>> history;
};

} // namespace vcab
