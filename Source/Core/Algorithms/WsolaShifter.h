// WSOLA 式（ディレイライン式の改良版）
//
// ディレイライン式と同じく、2つの読み出し位置が早回しで読み出し、交互に戻りながらクロスフェードでつなぐ。
// 違いは「戻る位置」の決め方: 決まった位置に戻るのではなく、探す範囲の中で、もう片方の読み出し位置が
// 今出している波形と一番よく重なる位置（正規化した相互相関が最大の位置）を探して、そこに戻る。
// つなぎ目で波の山と谷が打ち消し合わなくなるので、ディレイライン式の「うなり」が減る。
// フォルマントはピッチと一緒に動く。ピッチ検出は不要。遅延は窓の長さの半分ほど。
#pragma once

#include "VoiceAlgorithm.h"

#include <array>

namespace vcab
{

class WsolaShifter : public VoiceAlgorithm
{
public:
    bool supportsFormant (const ParamValues&) const override { return false; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process (float* data, int n, const ParamValues& params) override;
    int latencySamples (const ParamValues& params) const override;

private:
    float readAt (double delaySamples) const noexcept;
    // 戻り先の遅延を探す。from〜to の範囲で、相手（otherDelay）と一番よく重なる遅延を返す
    double findBestDelay (double from, double to, double otherDelay) const noexcept;

    double sr = 48000;
    std::vector<float> buffer;
    int writePos = 0;
    std::array<double, 2> delay { 0, 0 }; // 2つの読み出し位置の遅延（サンプル数）
    double lastWindow = 0;
};

} // namespace vcab
