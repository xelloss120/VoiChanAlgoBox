// ダイナミクスのグラフ（RME TotalMix のダイナミクスの画面のような見た目）
//
// 横軸が入力、縦軸が出力（どちらも -60〜0dB）。
//  - ゲートの線（緑）: 横軸はゲートが判断に使っている音量（ボイチェンの前）、縦軸はゲートを通った後
//  - コンプとリミッターの線（橙）: 横軸はコンプに入る音量（ボイチェンと EQ の後）、縦軸は最終出力
// 今の音量を点で表し、それぞれ自分の線の上を動く。下辺に入力、右辺に出力のメーター。
#pragma once

#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

class DynamicsView : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit DynamicsView (VoiChanAlgoBoxProcessor& p);

    void update(); // 画面のタイマーから呼ぶ
    void paint (juce::Graphics&) override;

private:
    juce::Rectangle<float> graphArea() const;
    juce::Point<float> toPoint (float inDb, float outDb, juce::Rectangle<float> area) const;

    VoiChanAlgoBoxProcessor& processor;

    // 表示用にならした値
    float inDb = -120, outDb = -120;
    float gateInDb = -120, gateOutDb = -120;
    float compInDb = -120, compOutDb = -120;
    bool gateOpen = false;

    // 点とメーターを止めているとき、最後に線を描いたときの設定（変わったときだけ描き直す）
    vcab::ParamValues drawnParams;
    bool wasLive = true;
};
