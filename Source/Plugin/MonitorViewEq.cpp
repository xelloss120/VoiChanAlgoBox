// モニターの、EQ の線と点（ドラッグとホイールでの操作を含む）
#include "MonitorView.h"

namespace
{
juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

const juce::Colour kEqColour { 0xff70e0d0 }; // EQ: 水色

struct HandleParams
{
    vcab::P freq;
    std::optional<vcab::P> gain;
    std::optional<vcab::P> q;
    const char* name;
};

// 点の番号: 0 低域シェルフ、1 中域ピーク1、2 中域ピーク2、3 高域シェルフ、4 ローカット、5 ハイカット
const HandleParams kHandles[] = {
    { vcab::P::eqLowShelfFreq,  vcab::P::eqLowShelfGain,  vcab::P::eqLowShelfQ,  "LS" },
    { vcab::P::eqPeak1Freq,     vcab::P::eqPeak1Gain,     vcab::P::eqPeak1Q,     "P1" },
    { vcab::P::eqPeak2Freq,     vcab::P::eqPeak2Gain,     vcab::P::eqPeak2Q,     "P2" },
    { vcab::P::eqHighShelfFreq, vcab::P::eqHighShelfGain, vcab::P::eqHighShelfQ, "HS" },
    { vcab::P::eqLowCutFreq,    std::nullopt,             std::nullopt,          "LC" },
    { vcab::P::eqHighCutFreq,   std::nullopt,             std::nullopt,          "HC" },
};
} // namespace

float MonitorView::eqYForDb (float db, juce::Rectangle<float> area) const
{
    const float half = area.getHeight() * 0.5f - 14.0f;
    return area.getCentreY() - juce::jlimit (-eqRangeDb, eqRangeDb, db) / eqRangeDb * half;
}

float MonitorView::eqDbForY (float y, juce::Rectangle<float> area) const
{
    const float half = area.getHeight() * 0.5f - 14.0f;
    return juce::jlimit (-eqRangeDb, eqRangeDb, (area.getCentreY() - y) / half * eqRangeDb);
}

float MonitorView::hzForX (float x, juce::Rectangle<float> area) const
{
    const float t = juce::jlimit (0.0f, 1.0f, (x - area.getX()) / area.getWidth());
    return minHz * std::pow (maxHz / minHz, t);
}

juce::Point<float> MonitorView::handlePosition (int h, const vcab::ParamValues& p, juce::Rectangle<float> area) const
{
    const auto& hp = kHandles[h];
    const float x = xForHz (p.get (hp.freq), area);
    const float y = hp.gain ? eqYForDb (p.get (*hp.gain), area) : eqYForDb (0, area);
    return { x, y };
}

int MonitorView::handleAt (juce::Point<float> pos) const
{
    if (spectrumArea.isEmpty() || ! eqHandlesActive)
        return -1;
    const auto p = processor.effectiveParams();
    if (! p.on (vcab::P::eqOn))
        return -1;
    const auto area = spectrumArea.toFloat();
    for (int h = 0; h < numHandles; ++h)
        if (handlePosition (h, p, area).getDistanceFrom (pos) < 9.0f)
            return h;
    return -1;
}

void MonitorView::setParam (vcab::P id, float value)
{
    if (auto* param = processor.state().getParameter (vcab::spec (id).key))
        param->setValueNotifyingHost (param->convertTo0to1 (value));
}

void MonitorView::drawEq (juce::Graphics& g, juce::Rectangle<float> area, const vcab::ParamValues& p)
{
    if (! p.on (vcab::P::eqOn))
        return;

    // 0dB の線と目盛り（EQ の縦軸は ±18dB。スペクトルの dB とは別）
    g.setColour (kEqColour.withAlpha (0.2f));
    g.drawHorizontalLine (static_cast<int> (eqYForDb (0, area)), area.getX(), area.getRight());
    g.setFont (juce::FontOptions (17.0f));
    for (float db : { -12.0f, 12.0f })
    {
        g.setColour (kEqColour.withAlpha (0.35f));
        g.drawText ((db > 0 ? "+" : "") + juce::String (static_cast<int> (db)) + "dB",
                    juce::Rectangle<float> (area.getRight() - 62, eqYForDb (db, area) - 10, 58, 20), juce::Justification::right);
    }

    // 特性の線（画面用の EQ に今の値を設定して、周波数ごとの増減を計算する）
    const double sr = processor.currentSampleRate.load();
    if (std::abs (eqSampleRate - sr) > 1)
    {
        eqForDrawing.prepare (sr);
        eqSampleRate = sr;
    }
    eqForDrawing.setParams (p);

    juce::Path path;
    const int w = static_cast<int> (area.getWidth());
    for (int px = 0; px <= w; px += 2)
    {
        const float x = area.getX() + static_cast<float> (px);
        const float y = eqYForDb (static_cast<float> (eqForDrawing.magnitudeDbAt (hzForX (x, area))), area);
        if (px == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
    }
    g.setColour (kEqColour);
    g.strokePath (path, juce::PathStrokeType (2.0f));

    // 帯域の点
    for (int h = 0; h < numHandles; ++h)
    {
        bool active = true;
        if (h == 0) active = p.on (vcab::P::eqLowShelfOn);
        if (h == 1) active = p.on (vcab::P::eqPeak1On);
        if (h == 2) active = p.on (vcab::P::eqPeak2On);
        if (h == 3) active = p.on (vcab::P::eqHighShelfOn);
        if (h == 4) active = p.on (vcab::P::eqLowCutOn);
        if (h == 5) active = p.on (vcab::P::eqHighCutOn);

        const auto pos = handlePosition (h, p, area);
        const float r = (h == hoverHandle || h == dragHandle) ? 7.0f : 5.5f;
        const auto circle = juce::Rectangle<float> (r * 2, r * 2).withCentre (pos);
        g.setColour (active ? kEqColour : kEqColour.withAlpha (0.3f));
        g.fillEllipse (circle);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawEllipse (circle, 1.0f);
        g.setColour (kEqColour.withAlpha (active ? 0.9f : 0.4f));
        g.drawText (kHandles[h].name, juce::Rectangle<float> (pos.x - 22, pos.y - 30, 44, 20), juce::Justification::centred);
    }
}

void MonitorView::mouseMove (const juce::MouseEvent& e)
{
    const int h = handleAt (e.position);
    if (h == hoverHandle)
        return;

    hoverHandle = h;
    setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    if (h >= 0)
        setTooltip (h < 4 ? utf8 ("EQ の帯域。左右にドラッグで周波数、上下で増減、マウスのホイールで Q（幅）を変えます。")
                          : utf8 ("EQ のカット。左右にドラッグで周波数を変えます。"));
    else
        setTooltip ({});
    repaint();
}

void MonitorView::mouseDown (const juce::MouseEvent& e)
{
    dragHandle = handleAt (e.position);
    if (dragHandle < 0)
        return;

    const auto& hp = kHandles[dragHandle];
    for (auto id : { std::optional<vcab::P> (hp.freq), hp.gain })
        if (id)
            if (auto* param = processor.state().getParameter (vcab::spec (*id).key))
                param->beginChangeGesture();
}

void MonitorView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragHandle < 0 || spectrumArea.isEmpty())
        return;

    const auto area = spectrumArea.toFloat();
    const auto& hp = kHandles[dragHandle];
    const auto& fs = vcab::spec (hp.freq);
    setParam (hp.freq, juce::jlimit (fs.minValue, fs.maxValue, hzForX (e.position.x, area)));
    if (hp.gain)
        setParam (*hp.gain, eqDbForY (e.position.y, area));
    repaint();
}

void MonitorView::mouseUp (const juce::MouseEvent&)
{
    if (dragHandle < 0)
        return;

    const auto& hp = kHandles[dragHandle];
    for (auto id : { std::optional<vcab::P> (hp.freq), hp.gain })
        if (id)
            if (auto* param = processor.state().getParameter (vcab::spec (*id).key))
                param->endChangeGesture();
    dragHandle = -1;
}

void MonitorView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int h = handleAt (e.position);
    if (h < 0 || ! kHandles[h].q)
        return;

    const auto qId = *kHandles[h].q;
    const auto& qs = vcab::spec (qId);
    const float q = processor.currentParams().get (qId) * std::pow (1.15f, wheel.deltaY * 4.0f);
    if (auto* param = processor.state().getParameter (qs.key))
    {
        param->beginChangeGesture();
        setParam (qId, juce::jlimit (qs.minValue, qs.maxValue, q));
        param->endChangeGesture();
    }
    repaint();
}
