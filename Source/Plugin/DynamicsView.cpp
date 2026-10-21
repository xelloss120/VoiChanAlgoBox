#include "DynamicsView.h"

#include "Dsp/Compressor.h"
#include "Dsp/Gate.h"

namespace
{
juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

constexpr float kMinDb = -60.0f;
constexpr float kMaxDb = 0.0f;
constexpr int kMeterThickness = 10;

const juce::Colour kGateColour { 0xff60c070 };
const juce::Colour kCompColour { 0xfff0a040 };
const juce::Colour kInputColour { 0xff4fa3e0 };
} // namespace

DynamicsView::DynamicsView (VoiChanAlgoBoxProcessor& p)
    : processor (p)
{
    setOpaque (true);
    setTooltip (utf8 ("横が入力、縦が出力の音量（-60〜0dB）。緑の線はゲート、橙の線はコンプとリミッターの特性で、点は今の音量です。"
                      "下辺と右辺は、入力と出力のメーターです。"));
}

void DynamicsView::update()
{
    auto& m = processor.meters();
    const auto p = processor.effectiveParams();

    // 点とメーターを止めているときは、線を描き直す必要があるとき（設定が変わったとき）だけ描き直す
    if (! p.on (vcab::P::showDynamicsLive))
    {
        if (p.v != drawnParams.v || wasLive)
        {
            drawnParams = p;
            wasLive = false;
            repaint();
        }
        return;
    }
    wasLive = true;

    // メーターは、上がるときはすぐ、下がるときはゆっくり
    auto fall = [] (float& shown, float now, float speed) { shown = now > shown ? now : shown - speed; };
    fall (inDb, m.inputPeakDb.load(), 1.5f);
    fall (outDb, m.outputPeakDb.load(), 1.5f);

    // 点は、ならした値をそのまま使う（線の上を動くように、入力と出力を同じ時点の値で計算する）
    gateInDb = m.gateLevelDb.load();
    gateOutDb = gateInDb + m.gateGainDb.load();
    gateOpen = m.gateOpen.load();

    compInDb = m.compLevelDb.load();
    // 出力ゲインとリミッターは、「出力」タブがオンのときだけかかる
    const bool outputOn = p.on (vcab::P::outputOn);
    float o = compInDb + m.compGrDb.load() + (outputOn ? p.get (vcab::P::outputGain) : 0.0f);
    if (outputOn)
        o = juce::jmin (o, p.get (vcab::P::limiterCeiling));
    compOutDb = o;

    repaint();
}

juce::Rectangle<float> DynamicsView::graphArea() const
{
    auto r = getLocalBounds().toFloat().reduced (4.0f);
    r.removeFromLeft (40.0f);                                  // 縦軸の目盛り
    r.removeFromRight (static_cast<float> (kMeterThickness) + 6.0f);  // 出力のメーター
    r.removeFromBottom (static_cast<float> (kMeterThickness) + 26.0f); // 入力のメーターと横軸の目盛り
    r.removeFromTop (24.0f);                                   // 凡例
    const float side = juce::jmin (r.getWidth(), r.getHeight());
    return r.withSizeKeepingCentre (side, side).withX (r.getX());
}

juce::Point<float> DynamicsView::toPoint (float in, float out, juce::Rectangle<float> a) const
{
    const float tx = juce::jlimit (0.0f, 1.0f, (in - kMinDb) / (kMaxDb - kMinDb));
    const float ty = juce::jlimit (0.0f, 1.0f, (out - kMinDb) / (kMaxDb - kMinDb));
    return { a.getX() + tx * a.getWidth(), a.getBottom() - ty * a.getHeight() };
}

void DynamicsView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e2a3c));

    const auto p = processor.effectiveParams();
    const auto a = graphArea();

    // 背景と目盛り
    g.setColour (juce::Colour (0xff15202f));
    g.fillRect (a);
    g.setFont (juce::FontOptions (17.0f));
    for (float db : { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f })
    {
        const auto pt = toPoint (db, db, a);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawHorizontalLine (static_cast<int> (pt.y), a.getX(), a.getRight());
        g.drawVerticalLine (static_cast<int> (pt.x), a.getY(), a.getBottom());
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.drawText (juce::String (static_cast<int> (db)), juce::Rectangle<float> (a.getX() - 40, pt.y - 10, 36, 20), juce::Justification::right);
        g.drawText (juce::String (static_cast<int> (db)), juce::Rectangle<float> (pt.x - 20, a.getBottom() + kMeterThickness + 4, 40, 20), juce::Justification::centred);
    }

    // 1:1 の線（何もしないときの線）
    g.setColour (juce::Colours::white.withAlpha (0.15f));
    g.drawLine ({ toPoint (kMinDb, kMinDb, a), toPoint (kMaxDb, kMaxDb, a) }, 1.0f);

    // ゲートの線
    if (p.on (vcab::P::gateOn))
    {
        const float thr = p.get (vcab::P::gateThreshold);
        const float range = p.get (vcab::P::gateRange);
        juce::Path path;
        if (vcab::choiceAs (p.index (vcab::P::gateMode), vcab::GateMode::expander) == vcab::GateMode::expander)
        {
            // エキスパンダー: しきい値より下は、比率に応じて下がる（下げる量はレンジまで）
            vcab::Gate::Settings gs;
            gs.thresholdDb = thr;
            gs.ratio = p.get (vcab::P::gateRatio);
            gs.kneeDb = p.get (vcab::P::gateKnee);
            for (int i = 0; i <= 120; ++i)
            {
                const float x = kMinDb + (thr - kMinDb) * static_cast<float> (i) / 120.0f;
                const auto pt = toPoint (x, x + juce::jmax (range, vcab::Gate::expanderGainDb (x, gs)), a);
                if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
            }
        }
        else
        {
            path.startNewSubPath (toPoint (kMinDb, kMinDb + range, a));
            path.lineTo (toPoint (thr, thr + range, a));
        }
        path.lineTo (toPoint (thr, thr, a));
        path.lineTo (toPoint (kMaxDb, kMaxDb, a));
        g.setColour (kGateColour.withAlpha (0.9f));
        g.strokePath (path, juce::PathStrokeType (1.6f));
    }

    // コンプとリミッターの線
    {
        vcab::Compressor::Settings cs;
        cs.thresholdDb = p.get (vcab::P::compThreshold);
        cs.ratio = p.get (vcab::P::compRatio);
        cs.kneeDb = p.get (vcab::P::compKnee);
        const bool compOn = p.on (vcab::P::compOn);
        const bool limit = p.on (vcab::P::outputOn); // 「出力」タブ（出力ゲインとリミッター）
        const float gain = limit ? p.get (vcab::P::outputGain) : 0.0f;
        const float ceiling = p.get (vcab::P::limiterCeiling);

        juce::Path path;
        for (int i = 0; i <= 120; ++i)
        {
            const float x = kMinDb + (kMaxDb - kMinDb) * static_cast<float> (i) / 120.0f;
            float y = (compOn ? vcab::Compressor::staticCurve (x, cs) : x) + gain;
            if (limit) y = juce::jmin (y, ceiling);
            const auto pt = toPoint (x, y, a);
            if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
        }
        g.setColour (kCompColour.withAlpha (0.9f));
        g.strokePath (path, juce::PathStrokeType (1.6f));
    }

    const bool live = p.on (vcab::P::showDynamicsLive);

    // 今の音量の点
    auto dot = [&] (float in, float out, juce::Colour c)
    {
        if (in < kMinDb - 1) return;
        const auto pt = toPoint (in, out, a);
        g.setColour (c);
        g.fillEllipse (juce::Rectangle<float> (9, 9).withCentre (pt));
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawEllipse (juce::Rectangle<float> (9, 9).withCentre (pt), 1.0f);
    };
    if (live && p.on (vcab::P::gateOn))
        dot (gateInDb, gateOutDb, gateOpen ? kGateColour.brighter (0.4f) : kGateColour.darker (0.6f));
    if (live)
        dot (compInDb, compOutDb, kCompColour.brighter (0.3f));

    // 下辺: 入力のメーター（横長）
    {
        auto m = juce::Rectangle<float> (a.getX(), a.getBottom() + 3, a.getWidth(), static_cast<float> (kMeterThickness - 2));
        g.setColour (juce::Colour (0xff0f1724));
        g.fillRect (m);
        const float t = live ? juce::jlimit (0.0f, 1.0f, (inDb - kMinDb) / (kMaxDb - kMinDb)) : 0.0f;
        g.setColour (inDb > -1.0f ? juce::Colours::red : kInputColour);
        g.fillRect (m.withWidth (m.getWidth() * t));
    }

    // 右辺: 出力のメーター（縦長）
    {
        auto m = juce::Rectangle<float> (a.getRight() + 4, a.getY(), static_cast<float> (kMeterThickness - 2), a.getHeight());
        g.setColour (juce::Colour (0xff0f1724));
        g.fillRect (m);
        const float t = live ? juce::jlimit (0.0f, 1.0f, (outDb - kMinDb) / (kMaxDb - kMinDb)) : 0.0f;
        g.setColour (outDb > -1.0f ? juce::Colours::red : kCompColour);
        g.fillRect (m.withTrimmedTop (m.getHeight() * (1.0f - t)));
    }

    // 凡例
    g.setFont (juce::FontOptions (17.0f));
    auto legend = juce::Rectangle<float> (a.getX(), a.getY() - 23, a.getWidth(), 20);
    g.setColour (kGateColour);
    g.drawText (utf8 ("ゲート"), legend.removeFromLeft (66), juce::Justification::left);
    g.setColour (kCompColour);
    g.drawText (utf8 ("コンプ・出力"), legend.removeFromLeft (118), juce::Justification::left);
}
