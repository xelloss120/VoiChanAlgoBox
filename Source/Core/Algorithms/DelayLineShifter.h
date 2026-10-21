// ディレイライン式（リアルタイムで動く早回し）
//
// 入ってくる音を短いバッファに書き込み続け、ピッチの倍率に合わせた速さで読み出す。
// 読み出し位置が書き込み位置に追いついたら戻る。戻るときのプツッという音を隠すため、
// 半周期ずらした2つの読み出し位置を、交互にクロスフェードする。
// 断片を早回ししているので、フォルマントもピッチと一緒に動く。
#pragma once

#include "VoiceAlgorithm.h"

namespace vcab
{

class DelayLineShifter : public VoiceAlgorithm
{
public:
    bool supportsFormant (const ParamValues&) const override { return false; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process (float* data, int n, const ParamValues& params) override;
    int latencySamples (const ParamValues& params) const override;

private:
    float readAt (double delaySamples) const noexcept;

    double sr = 48000;
    std::vector<float> buffer;
    int writePos = 0;
    double phase = 0; // 0〜1。読み出し位置1の遅延は phase × 窓の長さ
};

} // namespace vcab
