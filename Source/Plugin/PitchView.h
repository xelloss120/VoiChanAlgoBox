// 検出した声の高さのグラフ（ピッチ検出を使う方式のとき、ボイチェンのタブに出す）
//
// 直近 5 秒の声の高さを、右から左へ流れる線で描く。縦軸は探す範囲（対数）。
// 有声音は色付きの線、無声音（息やサ行の子音）は灰色の点、無音は何も描かない。
// 検出がうまく追えているか、1オクターブ間違えていないかを、目で確かめるためのもの。
#pragma once

#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

class PitchView : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit PitchView (VoiChanAlgoBoxProcessor& p);

    void update(); // 画面のタイマー（30 回 / 秒）から呼ぶ
    void paint (juce::Graphics&) override;

private:
    struct Point
    {
        float hz = 0;
        bool voiced = false;
    };

    VoiChanAlgoBoxProcessor& processor;
    std::array<Point, 150> history {}; // 5 秒分
    int head = 0;
};
