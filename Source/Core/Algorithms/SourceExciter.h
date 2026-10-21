// 声帯の音を作り直す部品（LPC ボコーダ、ケプストラムボコーダで使う）
//
// LPC やケプストラムで声色を取り除いた残差の代わりに、作った音源を渡す（残差は使わない）。
//   有声音: ピッチ検出で求めた声の高さ × ピッチの倍率の、パルス列（倍音が同じ大きさに並ぶ音）。
//           「声帯の波形」で Rosenberg か LF を選ぶと、その波形（Glottal.h）
//   無声音: 白色雑音
// 切り替えは約 10ms でなめらかにつなぐ。混合励振がオンのときは、帯域ごとに混ぜる（MixedExcitation.h）。
// パワーはいつも 1 で、大きさは枠の側（LPC の予測の誤差、ケプストラムの包絡）で決める。残差の大きさに合わせると、声色のフィルタの求め方が遅れる変わり目（母音から
// 子音へなど）で残差が大きくなり、それを響きの強いフィルタに通して、音が大きく持ち上がってしまうため。
// ピッチ検出は結果が遅れて分かるので、その分が遅延になる（枠の側は、その時刻のフィルタを使う）。
#pragma once

#include "../Dsp/Glottal.h"
#include "../Dsp/Intonation.h"
#include "../Dsp/MixedExcitation.h"
#include "VoiceAlgorithm.h"

#include <cstdint>

namespace vcab
{

class SourceExciter : public VoiceAlgorithm
{
public:
    bool supportsFormant (const ParamValues&) const override { return true; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process (float* data, int n, const ParamValues& params) override;
    int latencySamples (const ParamValues& params) const override;
    const PitchEstimate* pitchEstimate() const override { return &detector.latest(); }
    void setDetectionSource (const float* source) override { detectionSource = source; }
    bool makesOwnSource() const override { return true; }

private:
    double sr = 48000;
    PitchDetector detector;
    const float* detectionSource = nullptr;

    GlottalSource pulses;            // パルス列、または声帯の波形（Glottal.h）
    IntonationEmphasis intonation;
    MixedExcitation mixed;
    int hop = 240, sinceHop = 0;
    double voicedMix = 0;
    double lastHz = 100;             // 最後に有声音だったときの声の高さ（無声音の間も、パルス列の高さを保つ）
    std::uint32_t seed = 12345;
};

} // namespace vcab
