#include "LookAndFeel.h"

namespace
{
juce::Font atLeastMinSize (juce::Font f)
{
    return f.getHeight() < VCLookAndFeel::kMinFontSize ? f.withHeight (VCLookAndFeel::kMinFontSize) : f;
}
} // namespace

VCLookAndFeel::VCLookAndFeel()
{
    // 日本語を表示できるフォント
    setDefaultSansSerifTypefaceName ("Yu Gothic UI");

    // 紺系の配色（選択欄、スライダー、メニュー、EXE 版の外枠など。JUCE の標準の暗い配色を青寄りにしたもの）
    setColourScheme ({ 0xff1e2a3c,     // ウィンドウの背景
                       0xff121b2a,     // 部品の背景（選択欄、値の欄）
                       0xff1e2a3c,     // メニューの背景
                       0xff4a5d7c,     // 枠線
                       0xffffffff,     // 文字
                       0xff4a90e0,     // 塗り（スライダーのつまみ、チェック）
                       0xffffffff,     // 選んだ項目の文字
                       0xff2e4f80,     // 選んだ項目の背景
                       0xffffffff }); // メニューの文字
}

juce::TextLayout VCLookAndFeel::layoutTooltip (const juce::String& text, juce::Colour colour) const
{
    constexpr float maxWidth = 460.0f;

    juce::AttributedString s;
    s.setWordWrap (juce::AttributedString::WordWrap::byChar);
    s.setJustification (juce::Justification::topLeft);
    s.setLineSpacing (2.0f);
    s.append (text, juce::FontOptions (kMinFontSize), colour);

    juce::TextLayout tl;
    tl.createLayoutWithBalancedLineLengths (s, maxWidth);
    return tl;
}

juce::Rectangle<int> VCLookAndFeel::getTooltipBounds (const juce::String& text, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    const auto tl = layoutTooltip (text, juce::Colours::black);
    const auto w = static_cast<int> (tl.getWidth() + 20.0f);
    const auto h = static_cast<int> (tl.getHeight() + 14.0f);

    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6,
                                 w, h)
        .constrainedWithin (parentArea);
}

void VCLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto bounds = juce::Rectangle<int> (width, height).toFloat();
    g.setColour (juce::Colour (0xff0f1724));
    g.fillRect (bounds);
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.drawRect (bounds, 1.0f);

    layoutTooltip (text, juce::Colour (0xfff0f0f0)).draw (g, bounds.reduced (10.0f, 7.0f));
}

juce::Font VCLookAndFeel::getLabelFont (juce::Label& l)          { return atLeastMinSize (LookAndFeel_V4::getLabelFont (l)); }
juce::Font VCLookAndFeel::getComboBoxFont (juce::ComboBox&)      { return juce::FontOptions (kMinFontSize); }
juce::Font VCLookAndFeel::getPopupMenuFont()                     { return juce::FontOptions (kMinFontSize); }

void VCLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth, int& idealHeight)
{
    // 項目の文字は「項目の高さ ÷ 1.3」までに縮められるので、上下の余白を含めて 17 が入る高さにする
    const int minHeight = juce::roundToInt (kMinFontSize * 1.3f) + 6;
    if (standardMenuItemHeight > 0)
        standardMenuItemHeight = juce::jmax (standardMenuItemHeight, minHeight);
    LookAndFeel_V4::getIdealPopupMenuItemSize (text, isSeparator, standardMenuItemHeight, idealWidth, idealHeight);
}
juce::Font VCLookAndFeel::getTextButtonFont (juce::TextButton&, int) { return juce::FontOptions (kMinFontSize); }
juce::Font VCLookAndFeel::getAlertWindowMessageFont()            { return juce::FontOptions (kMinFontSize); }
juce::Font VCLookAndFeel::getAlertWindowFont()                   { return juce::FontOptions (kMinFontSize); }
juce::Font VCLookAndFeel::getAlertWindowTitleFont()              { return juce::FontOptions (kMinFontSize + 3, juce::Font::bold); }
juce::Font VCLookAndFeel::getTabButtonFont (juce::TabBarButton&, float) { return juce::FontOptions (kMinFontSize); }

void VCLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down)
{
    // JUCE の LookAndFeel_V4 と同じ描き方で、文字の大きさだけ下限以上にする
    const float fontSize = kMinFontSize;
    const float tickWidth = juce::jmin (fontSize * 1.1f, static_cast<float> (button.getHeight()) - 2.0f);

    drawTickBox (g, button, 4.0f, (static_cast<float> (button.getHeight()) - tickWidth) * 0.5f, tickWidth, tickWidth,
                 button.getToggleState(), button.isEnabled(), highlighted, down);

    g.setColour (button.findColour (juce::ToggleButton::textColourId));
    g.setFont (juce::FontOptions (fontSize));
    if (! button.isEnabled())
        g.setOpacity (0.5f);

    g.drawFittedText (button.getButtonText(),
                      button.getLocalBounds().withTrimmedLeft (juce::roundToInt (tickWidth) + 10).withTrimmedRight (2),
                      juce::Justification::centredLeft, 10);
}

int VCLookAndFeel::getTabButtonBestWidth (juce::TabBarButton& button, int)
{
    // 文字の幅 + 左右の余白 + チェックボックスとその両側の余白
    int width = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (kMinFontSize), button.getButtonText().trim()) + 24;
    if (auto* extra = button.getExtraComponent())
        width += extra->getWidth() + 14;
    return width;
}

juce::Rectangle<int> VCLookAndFeel::getTabButtonExtraComponentBounds (const juce::TabBarButton&, juce::Rectangle<int>& textArea, juce::Component& extra)
{
    // 右端に 8px の余白、チェックボックス、文字との間に 6px の余白
    textArea.removeFromRight (8);
    auto r = textArea.removeFromRight (extra.getWidth()).withSizeKeepingCentre (extra.getWidth(), extra.getHeight());
    textArea.removeFromRight (6);
    return r;
}
