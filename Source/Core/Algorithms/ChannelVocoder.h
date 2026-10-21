// チャンネルボコーダ（簡易ボコーダ合成）
//
// 1. 声を、100Hz〜10kHz の帯域（対数で等間隔）に分け、帯域ごとの大きさ（包絡）を測る。これがフォルマントの形になる。
// 2. 音源を作る。有声音のときは、ピッチ検出で求めた声の高さ × ピッチの倍率の、のこぎり波（声帯の音の代わり）。
//    無声音（息やサ行の子音）のときは、白色雑音。切り替えは約 10ms でなめらかにつなぐ。
//    混合励振がオンのときは、パルス列と雑音を、5 つの帯域ごとに元の声の倍音らしさで混ぜる（MixedExcitation.h）。
// 3. 音源を同じように帯域に分け、帯域ごとに「声の大きさ ÷ 音源の大きさ」をかけて足し合わせる。
//    音源の偏り（のこぎり波は低い音ほど強い）を打ち消して、声の包絡だけが乗るようにするため。
// 4. フォルマントは、合成側の帯域の中心周波数をフォルマントの倍率だけずらして動かす（帯域の割り当てをずらす）。
//
// 帯域の数が少ないほどロボットのような声になり、多いほど元の声に近づく（ただし帯域が細いと包絡の変化に遅れる）。
#pragma once

#include "../Dsp/Biquad.h"
#include "../Dsp/FormantWarp.h"
#include "../Dsp/Glottal.h"
#include "../Dsp/Intonation.h"
#include "../Dsp/MixedExcitation.h"
#include "../Dsp/PitchDetector.h"
#include "VoiceAlgorithm.h"

#include <array>
#include <cstdint>

namespace vcab
{

class ChannelVocoder : public VoiceAlgorithm
{
public:
    bool supportsFormant (const ParamValues&) const override { return true; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process (float* data, int n, const ParamValues& params) override;
    int latencySamples (const ParamValues& params) const override;

    const PitchEstimate* pitchEstimate() const override { return &detector.latest(); }

    static constexpr int kMaxBands = 48;

private:
    struct Band
    {
        Biquad analysis[2], carrier[2]; // 2段重ねのバンドパス（声の分析用と、音源用 = 合成）
        double voiceEnv = 0, carrierEnv = 0;          // 包絡（2乗の平均）
        bool active = false;                          // 合成側の帯域がナイキスト周波数を超えたら使わない
    };

    void configure (int bands, double formantRatio);

    double sr = 48000;
    PitchDetector detector;
    std::array<Band, kMaxBands> bands {};
    int numBands = 0;
    double configuredFormant = 0;
    int configuredWarp = -1;
    double configuredOffs[3] { 0, 0, 0 };
    FormantWarp warp;
    IntonationEmphasis intonation;
    double makeup = 1; // 帯域の重なりが足りない分を補う倍率

    double sawPhase = 0, voicedMix = 0;
    GlottalSource pulses;            // パルス列、または声帯の波形
    MixedExcitation mixed;
    int hop = 240, sinceHop = 0;
    std::uint32_t noiseSeed = 1;
};

} // namespace vcab
