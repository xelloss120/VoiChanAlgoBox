#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// 見た目の設定。画面のすべての文字の大きさの下限を、ツールチップと同じ kMinFontSize にする
// （JUCE の標準は小さくて読みにくいため）。EXE 版の外枠にも使う。
class VCLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static constexpr float kMinFontSize = 17.0f;

    VCLookAndFeel();

    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    // 選択欄のメニューの項目は、選択欄と同じ高さになり、文字が高さに合わせて縮められる。
    // 低い選択欄でも文字が下限を下回らないように、項目の高さに下限を設ける
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth, int& idealHeight) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;
    juce::Font getAlertWindowTitleFont() override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

    // タブ: 文字とチェックボックスの間に余白をとる
    juce::Font getTabButtonFont (juce::TabBarButton&, float height) override;
    int getTabButtonBestWidth (juce::TabBarButton&, int tabDepth) override;
    juce::Rectangle<int> getTabButtonExtraComponentBounds (const juce::TabBarButton&, juce::Rectangle<int>& textArea, juce::Component& extra) override;

private:
    juce::TextLayout layoutTooltip (const juce::String& text, juce::Colour colour) const;
};
