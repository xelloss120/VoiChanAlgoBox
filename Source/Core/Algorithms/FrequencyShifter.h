// 周波数シフト
//
// 全部の周波数を、同じ Hz だけずらす（ピッチシフトは同じ「倍率」をかける）。
// 1. ヒルベルト変換の近似で、音を「同相成分 I」と「直交成分 Q」（90 度ずれた2本）に分ける。
// 2. I × cos(2πft) + Q × sin(2πft) とすると、片側の側波帯だけが残り、全部の周波数が f だけ上にずれる。
//
// 倍音は「基本周波数の整数倍」に並んでいるが、同じ Hz を足すと整数倍の関係が崩れ、金属的な声になる。
// ピッチシフトとの違いを体験するための方式。ずらす量は、ピッチのつまみと「基準の周波数」から
// 「基準の周波数 × (倍率 - 1)」Hz とする（基準の周波数の音は、ちょうどピッチのつまみどおりの高さになる）。
// 遅延はほぼ 0ms。
#pragma once

#include "../Dsp/Hilbert.h"
#include "VoiceAlgorithm.h"

namespace vcab
{

class FrequencyShifter : public VoiceAlgorithm
{
public:
    bool supportsFormant (const ParamValues&) const override { return false; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process (float* data, int n, const ParamValues& params) override;
    int latencySamples (const ParamValues&) const override { return 0; }

    static double shiftHz (const ParamValues& p);

private:
    double sr = 48000;
    HilbertPair hilbert;
    double phase = 0;
};

} // namespace vcab
