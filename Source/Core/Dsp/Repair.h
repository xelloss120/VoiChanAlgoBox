// リペア（一瞬だけの音を直す）。突発音の抑制（Transient.h）のほかに、次の 3 つ。どれもノッチの後、デノイズの前。
//
//   音割れの修復（Declipper）: 入力が大きすぎて、波形の山が平らに切れたところ（同じ符号で「割れの大きさ」以上の値が
//     2 サンプル以上続くところ）を見つけ、前後 2 サンプルずつから 3 次の式で山を作り直す。区間の終わりまで見るために、
//     最大 2ms 先まで見る（その分遅れる）。作り直した山は 0dBFS を超えることがある（後の出力のリミッターで止まる）。
//   吹かれ・破裂音（PlosiveSuppressor）: マイクへの息やパ行で出る「ボフッ」という低い音の塊を下げる。
//     4 次の Linkwitz-Riley で低い側と高い側に分け、低い側の音量が、声の帯域（300Hz〜3kHz）の音量より「しきい値」以上
//     大きくなった間だけ、低い側を下げる（下げ始め 1ms、戻り 60ms）。先は見ない（遅延 0）。
//   リップノイズ（MouthClickSuppressor）: 口の中のペチャッという 1〜2ms ほどの鋭い音。3kHz より上の音量が、周り（5ms で
//     ならしたもの）より急に大きくなり、2ms 以内にまた小さくなったら、高い側だけを 3ms ほど下げる（声の最中でも、
//     低い側の声はそのまま）。2ms 先まで見る。
//   息継ぎ（BreathSuppressor）: 言葉の合間の息継ぎ（「スゥッ」）を下げる。デノイズは、ずっと鳴っている
//     小さな音をノイズとみなすので、一時的で背景より大きい息継ぎは声として残す。そこで「息らしさ」で見分ける:
//       - 倍音がない（12kHz に落とした音の、80〜400Hz の周期での正規化した相関が小さい）
//       - 背景（ゆっくり上がる最小値）より 6dB 以上大きい
//       - 最近の声（有声音の大きさの最大。ゆっくり下がる）より「声との差」以上小さい
//       - サ行・シャ行ではない（4kHz より上が、300Hz〜4kHz より小さい。日本語の文末の「す」は無声になりやすい）
//       - その状態が 40ms 以上続く（ハ行の「h」など、母音の前の短い息の音を除く）
//     息継ぎと判断している間、音量をまとめて「下げる量」だけ下げる（下げ始め 20ms、戻り 3ms）。言葉の出だしで、
//     速い音量（2ms）が息継ぎの大きさより 10dB 上がったら、すぐに戻す。戻りが出だしに間に合うように 5ms 先まで見る。
//     息継ぎの最初の 60〜100ms ほどは、まだ下がりきらない（先をそれだけ見ると遅延が増えるため）。
#pragma once

#include "Biquad.h"

#include <vector>

namespace vcab
{

class Declipper
{
public:
    void prepare (double sampleRate);
    void reset();
    void process (float* data, int n, double clipLevelDb);
    int latencySamples() const noexcept { return lookahead; }
    int detections() const noexcept { return count; }

private:
    double sr = 48000;
    int lookahead = 96, maxRun = 92, count = 0;
    std::vector<float> line; // 遅らせる線（先を見る分 ＋ 前の 2 サンプル）
    int pos = 0;
    int skip = 0;            // 作り直した区間の残り（同じ区間を二度直さないため）
};

class PlosiveSuppressor
{
public:
    void prepare (double sampleRate);
    void reset();
    void process (float* data, int n, double cutoffHz, double thresholdDb, double depthDb);
    int detections() const noexcept { return count; }

private:
    void setCutoff (double hz);
    double sr = 48000, cutoff = -1;
    Biquad low[2], high[2], voiceBand[2];
    float lowEnv = 0, voiceEnv = 0, gainDb = 0;
    float envAttack = 0, envRelease = 0, closeCoeff = 0, openCoeff = 0;
    bool active = false;
    int count = 0;
};

class MouthClickSuppressor
{
public:
    void prepare (double sampleRate);
    void reset();
    void process (float* data, int n, double thresholdDb, double depthDb);
    int latencySamples() const noexcept { return lookahead; }
    int detections() const noexcept { return count; }

private:
    double sr = 48000;
    int lookahead = 96, pos = 0, hold = 0, holdLength = 144, count = 0;
    float fast = 0, slow = 0, gainDb = 0, prevScore = -100;
    float fastAttack = 0, fastRelease = 0, slowCoeff = 0, closeCoeff = 0, openCoeff = 0;
    Biquad detectHigh[2], low[2], high[2];
    std::vector<float> lowLine, highLine, fastRing;
    std::vector<unsigned char> onsetRing;
};

class BreathSuppressor
{
public:
    void prepare (double sampleRate);
    void reset();
    void process (float* data, int n, double marginDb, double depthDb);
    int latencySamples() const noexcept { return lookahead; }
    int detections() const noexcept { return count; }

private:
    void analyzeHop (double marginDb);

    static constexpr int kLevelHops = 4;     // 音量は直近 4 回分（20ms）で測る
    double sr = 48000;
    int lookahead = 240, hop = 240, sinceHop = 0, pos = 0, count = 0;
    int decim = 4, decimPhase = 0, minLag = 30, maxLag = 150, window = 360, minRun = 12;
    double hopSeconds = 0.005;
    std::vector<float> line, lowRing;          // 遅らせる線、12kHz に落とした音
    long long lowCount = 0;
    Biquad decimLow[2], mid[2], high[2];
    double eFull = 0, eMid = 0, eHigh = 0;     // この回の、帯域ごとのエネルギー
    double hopFull[kLevelHops] {}, hopMid[kLevelHops] {}, hopHigh[kLevelHops] {};
    int hopIndex = 0;
    double speechDb = -200, floorDb = 200, breathDb = -200;
    int run = 0;
    bool breath = false;
    float fast = 0, fastAttack = 0, fastRelease = 0, gainDb = 0, closeCoeff = 0, openCoeff = 0;
};

} // namespace vcab
