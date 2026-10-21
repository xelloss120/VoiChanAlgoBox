#include "PitchView.h"

namespace
{
juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

const juce::Colour kVoicedColour { 0xff60c0ff };
const juce::Colour kUnvoicedColour { 0xff7e8ca6 };

// 周波数を音名にする（例: 220Hz → A3）
juce::String noteName (float hz)
{
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int midi = juce::roundToInt (69 + 12 * std::log2 (hz / 440.0f));
    return juce::String (names[(midi % 12 + 12) % 12]) + juce::String (midi / 12 - 1);
}
} // namespace

PitchView::PitchView (VoiChanAlgoBoxProcessor& p) : processor (p)
{
    setOpaque (true);
    setTooltip (utf8 ("ピッチ検出が見つけた声の高さ（直近 5 秒）。灰色の点は無声音です。"
                      "急に半分や 2 倍に飛ぶのは、1 オクターブ間違えたところです。"));
}

void PitchView::update()
{
    auto& m = processor.meters();
    Point pt;
    if (m.pitchActive.load())
    {
        pt.hz = m.pitchHz.load();
        pt.voiced = m.pitchVoiced.load();
    }
    history[static_cast<size_t> (head)] = pt;
    head = (head + 1) % static_cast<int> (history.size());
    repaint();
}

void PitchView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0f1724));
    const auto p = processor.effectiveParams();
    const float minHz = p.get (vcab::P::pitchMinHz);
    const float maxHz = juce::jmax (minHz * 1.5f, p.get (vcab::P::pitchMaxHz));

    auto area = getLocalBounds().toFloat().reduced (8, 6);
    auto title = area.removeFromTop (24);
    area.removeFromTop (10);    // 一番上の目盛りの文字が、見出しにかからないように
    area.removeFromBottom (10);
    area.removeFromLeft (48); // 目盛りの文字

    auto yFor = [&] (float hz)
    {
        const float t = std::log (hz / minHz) / std::log (maxHz / minHz);
        return area.getBottom() - juce::jlimit (0.0f, 1.0f, t) * area.getHeight();
    };

    // 目盛り（探す範囲の中にある、きりのよい周波数）
    g.setFont (juce::FontOptions (17.0f));
    for (float hz : { 50.0f, 100.0f, 200.0f, 300.0f, 500.0f, 700.0f, 1000.0f })
    {
        if (hz < minHz || hz > maxHz)
            continue;
        const float y = yFor (hz);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());
        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.drawText (juce::String (juce::roundToInt (hz)), juce::Rectangle<float> (area.getX() - 50, y - 10, 44, 20), juce::Justification::right);
    }

    // 線: 古いものが左、新しいものが右
    const auto n = static_cast<int> (history.size());
    const float dx = area.getWidth() / static_cast<float> (n - 1);
    juce::Path voicedPath;
    bool penDown = false;
    for (int k = 0; k < n; ++k)
    {
        const auto& pt = history[static_cast<size_t> ((head + k) % n)];
        const float x = area.getX() + k * dx;
        if (pt.hz <= 0)
        {
            penDown = false;
            continue;
        }
        const float y = yFor (pt.hz);
        if (pt.voiced)
        {
            if (penDown) voicedPath.lineTo (x, y); else voicedPath.startNewSubPath (x, y);
            penDown = true;
        }
        else
        {
            penDown = false;
            g.setColour (kUnvoicedColour.withAlpha (0.6f));
            g.fillEllipse (x - 2, y - 2, 4, 4);
        }
    }
    g.setColour (kVoicedColour);
    g.strokePath (voicedPath, juce::PathStrokeType (2.0f));

    // 今の値
    const auto& last = history[static_cast<size_t> ((head + n - 1) % n)];
    juce::String text = utf8 ("検出した声の高さ: ");
    if (last.hz <= 0)
        text << utf8 ("無音");
    else if (! last.voiced)
        text << utf8 ("無声音");
    else
        text << juce::String (juce::roundToInt (last.hz)) << " Hz (" << noteName (last.hz) << ")";
    g.setColour (last.voiced ? kVoicedColour : juce::Colours::white.withAlpha (0.7f));
    g.drawText (text, title, juce::Justification::centredLeft);
}
