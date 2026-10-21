// エディタ（PluginEditor.cpp と EditorTabs.cpp）で共通に使う、見た目の値と小さな道具
#pragma once

#include "LookAndFeel.h"

namespace editor
{

inline juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

inline const juce::Colour kBackground { 0xff17202e };
inline const juce::Colour kPanel { 0xff1e2a3c };
inline const juce::Colour kBox { 0xff273550 };
inline const juce::Colour kText { 0xffe2e8f2 };

inline constexpr float kFont = VCLookAndFeel::kMinFontSize; // 文字の大きさの下限（ツールチップと同じ）

// ウィンドウの大きさは1通りに固定
inline constexpr int kWidth = 1560;           // タブ 12 個の名前が折り返さずに並ぶ幅
inline constexpr int kHeaderHeight = 48;
inline constexpr int kTabBarHeight = 40;
inline constexpr int kContentHeight = 450;   // デノイズの枠（詳細で最大 13 行）が入る高さ
inline constexpr int kMonitorBarHeight = 38;  // モニターの上の、表示の切り替えの段
inline constexpr int kMonitorHeight = 300;
inline constexpr int kMargin = 8;
inline constexpr int kSideWidth = 330;        // 右側（入力チャンネルとダイナミクスのグラフ）
inline constexpr int kRowHeight = 30;
inline constexpr int kBoxTitle = 30;
inline constexpr int kValueBoxWidth = 92;     // つまみの値の欄の幅
inline constexpr int kGap = 10;             // 枠と枠の間

// 左右に半分ずつ分ける
inline void twoColumns (juce::Rectangle<int> area, juce::Rectangle<int>& left, juce::Rectangle<int>& right)
{
    const int w = (area.getWidth() - kGap) / 2;
    left = area.withWidth (w);
    right = area.withTrimmedLeft (w + kGap);
}

// 高さ h の枠の下へ進める（枠を出さなかったとき〈h = 0〉は詰める）
inline void below (juce::Rectangle<int>& area, int h)
{
    if (h > 0)
        area.removeFromTop (h + kGap);
}

} // namespace editor
