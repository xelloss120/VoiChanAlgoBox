// フォルマントの動かし方のグラフ（ボイチェンのタブ）
//
// 横軸が元の周波数（対数、100Hz〜8kHz）、縦軸がその周波数の山が動く量（半音）。灰色の点線は「動かさない」（0）、
// 薄い線は「一律」（どこもつまみの値）、色付きの線が今の動かし方。「低い側を残す」では、150Hz 以下で 0、
// そこから 1kHz に向けて立ち上がり、1kHz より上で一律の線に合流する。
// （元の周波数 → 動いた先の図にすると、違いが出る 1kHz より下が図の端に押し込められて、ほとんど見えなかったため）
// 「帯域ごと」では、F1・F2・F3 の目印に点を描き、点を上下にドラッグすると、その差（F1 / F2 / F3 の差）を変えられる
// （ホイールで 0.5 半音ずつ、ダブルクリックで 0。ボイチェンのタブの左側には、つまみを足す場所がないため）。
// 「フォルマントごと」では、点が追いかけた F1・F2・F3 の位置に動く（追いかけている間は塗りつぶし、追いかけられずに
// 目印に戻っている間は白抜き）。
#pragma once

#include "../Core/Dsp/FormantWarp.h"
#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

class FormantWarpView : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit FormantWarpView (VoiChanAlgoBoxProcessor& p) : processor (p)
    {
        setOpaque (true);
        setTooltip (juce::String (juce::CharPointer_UTF8 (
            "フォルマントの動かし方。横軸が周波数（100Hz〜8kHz）、縦軸がその周波数の山が動く量（半音）です。"
            "灰色の点線は動かさない場合（0）、薄い線は一律に動かした場合（どこもつまみの値）、色付きの線が今の動かし方です。"
            "動かし方が「帯域ごと」のときは、F1・F2・F3 の点を上下にドラッグして、つまみの値からの差を変えられます"
            "（ホイールで 0.5 半音ずつ、ダブルクリックで 0）。")));
    }

    // 画面のタイマーから呼ぶ。つまみか動かし方が変わったときだけ描き直す
    void update()
    {
        const auto p = processor.effectiveParams();
        const float f = p.get (vcab::P::formant);
        const int m = p.index (vcab::P::formantWarp);
        const float offs[3] = { p.get (vcab::P::formantF1), p.get (vcab::P::formantF2), p.get (vcab::P::formantF3) };
        bool changed = f != shownFormant || m != shownMode;
        for (int k = 0; k < 3; ++k)
            changed = changed || offs[k] != shownOffs[k];
        // フォルマントごと: 追いかけた位置（1% 以上動いたら描き直す）
        vcab::FormantTrack track;
        const auto& meters = processor.meters();
        const bool tracking = m == vcab::FormantWarp::tracked && meters.formantTracking.load();
        if (tracking)
        {
            for (int k = 0; k < 3; ++k)
            {
                track.hz[k] = meters.formantHz[k].load();
                changed = changed || std::abs (std::log (track.hz[k] / shownTrack[k])) > 0.01;
            }
            track.valid = meters.formantValid.load();
            changed = changed || track.valid != shownValid;
        }
        if (changed)
        {
            shownFormant = f;
            shownMode = m;
            std::copy (offs, offs + 3, shownOffs);
            std::copy (track.hz, track.hz + 3, shownTrack);
            shownValid = track.valid;
            warp.set (p, tracking ? &track : nullptr);
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        // 横軸: 元の周波数（対数、100Hz〜8kHz）、縦軸: 動く量（半音）。一律は横一直線、低い側を残すは低いところで 0 に下がる
        g.fillAll (juce::Colour (0xff0f1724));
        auto area = getLocalBounds().toFloat().reduced (8, 4);
        g.setFont (juce::FontOptions (17.0f)); // 文字の大きさの下限（画面のすべての文字で 17）
        g.setColour (juce::Colour (0xffe2e8f2));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("フォルマントの動く量")), area.removeFromTop (22), juce::Justification::centredLeft);
        g.setColour (juce::Colour (0xff8e9cb4));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("横: 周波数 / 縦: 半音")), area.removeFromTop (22), juce::Justification::centredLeft);

        const auto geo = geometry();
        const double st = 12.0 * std::log2 (warp.ratio());

        // 目盛り（横: 100 / 1k / 8k、縦: 0 と今の値）
        for (double hz : { 100.0, 1000.0, 8000.0 })
        {
            g.setColour (juce::Colour (0xff2a3a52));
            g.drawVerticalLine (static_cast<int> (geo.xFor (hz)), geo.area.getY(), geo.area.getBottom());
            g.setColour (juce::Colour (0xff8e9cb4));
            const auto label = hz >= 1000 ? juce::String (static_cast<int> (hz / 1000)) + "k" : juce::String (static_cast<int> (hz));
            g.drawText (label, juce::Rectangle<float> (geo.xFor (hz) - 20, geo.area.getBottom() + 1, 40, 19), juce::Justification::centred);
        }
        for (double semis : { 0.0, st })
        {
            g.setColour (juce::Colour (0xff8e9cb4));
            const auto label = (semis > 0 ? "+" : "") + juce::String (semis, std::abs (semis - std::round (semis)) < 0.05 ? 0 : 1);
            g.drawText (label, juce::Rectangle<float> (geo.area.getX() - 34, geo.yFor (semis) - 9, 31, 18), juce::Justification::centredRight);
            if (std::abs (st) < 0.5)
                break;
        }

        // 動かさない（0 の点線）、一律（薄い線）、今の動かし方（色付き）
        const float dash[] = { 4.0f, 4.0f };
        g.setColour (juce::Colour (0xff6e7e98));
        g.drawDashedLine (juce::Line<float> (geo.area.getX(), geo.yFor (0), geo.area.getRight(), geo.yFor (0)), dash, 2, 1.0f);
        auto curve = [&] (bool uniform)
        {
            juce::Path path;
            for (int i = 0; i <= 200; ++i)
            {
                const double hz = kLo * std::pow (kHi / kLo, i / 200.0);
                const double moved = uniform ? st : 12.0 * std::log2 (warp.forward (hz) / hz);
                const auto pt = juce::Point<float> (geo.xFor (hz), geo.yFor (moved));
                if (i == 0)
                    path.startNewSubPath (pt);
                else
                    path.lineTo (pt);
            }
            return path;
        };
        if (! warp.isUniform())
        {
            g.setColour (juce::Colour (0x5060c0ff));
            g.strokePath (curve (true), juce::PathStrokeType (1.5f));
        }
        g.setColour (juce::Colour (0xff60c0ff));
        g.strokePath (curve (false), juce::PathStrokeType (2.0f));

        // 帯域ごと: F1・F2・F3 の目印の点
        if (warp.usesAnchors())
        {
            for (int k = 0; k < 3; ++k)
            {
                const auto c = dotCentre (geo, k);
                g.setColour (k == dragging ? juce::Colour (0xffffffff) : juce::Colour (0xffffd060));
                if (warp.currentMode() == vcab::FormantWarp::tracked && ! shownValid)
                    g.drawEllipse (c.x - 5, c.y - 5, 10, 10, 2.0f); // 追いかけられずに、目印に戻っている
                else
                    g.fillEllipse (c.x - 5, c.y - 5, 10, 10);
                g.setColour (juce::Colour (0xffe2e8f2));
                g.drawText ("F" + juce::String (k + 1), juce::Rectangle<float> (c.x - 14, c.y - 24, 28, 18), juce::Justification::centred);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = hitDot (e.position);
        if (dragging >= 0)
        {
            if (auto* param = parameterFor (dragging))
                param->beginChangeGesture();
            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;
        const auto geo = geometry();
        const double st = 12.0 * std::log2 (warp.ratio());
        setOffset (dragging, std::round ((geo.semisAt (e.position.y) - st) * 10.0) / 10.0);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0)
            if (auto* param = parameterFor (dragging))
                param->endChangeGesture();
        dragging = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int k = hitDot (e.position);
        if (k >= 0)
            setOffset (k, 0.0, true);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const int k = hitDot (e.position);
        if (k < 0 || wheel.deltaY == 0)
            return;
        setOffset (k, warp.offset (k) + (wheel.deltaY > 0 ? 0.5 : -0.5), true);
    }

private:
    static constexpr double kLo = 100.0, kHi = 8000.0;

    struct Geometry
    {
        juce::Rectangle<float> area;
        double top = 2, bottom = -2;
        float xFor (double hz) const { return area.getX() + static_cast<float> (std::log (hz / kLo) / std::log (kHi / kLo)) * area.getWidth(); }
        float yFor (double semis) const { return area.getBottom() - static_cast<float> ((semis - bottom) / (top - bottom)) * area.getHeight(); }
        double semisAt (float y) const { return bottom + (area.getBottom() - y) / area.getHeight() * (top - bottom); }
    };

    Geometry geometry() const
    {
        Geometry geo;
        geo.area = getLocalBounds().toFloat().reduced (8, 4);
        geo.area.removeFromTop (52);   // 見出しの 2 行
        geo.area.removeFromLeft (34);  // 目盛りの文字
        geo.area.removeFromBottom (20); // 目盛りの文字
        const double st = 12.0 * std::log2 (warp.ratio());
        if (warp.usesAnchors())
        {
            // 点をドラッグしている間に縦の目盛りが変わらないように、差の範囲（±12 半音）が入る、決まった範囲にする
            geo.top = std::max (0.0, st + 12.0) + 1.0;
            geo.bottom = std::min (0.0, st - 12.0) - 1.0;
        }
        else
        {
            geo.top = std::max (0.0, st) + 2.0;
            geo.bottom = std::min (0.0, st) - 2.0;
        }
        return geo;
    }

    juce::Point<float> dotCentre (const Geometry& geo, int k) const
    {
        const double hz = warp.anchorHz (k);
        return { geo.xFor (hz), geo.yFor (12.0 * std::log2 (warp.forward (hz) / hz)) };
    }

    int hitDot (juce::Point<float> pos) const
    {
        if (! warp.usesAnchors())
            return -1;
        const auto geo = geometry();
        int best = -1;
        float bestDist = 14.0f;
        for (int k = 0; k < 3; ++k)
        {
            const float dist = dotCentre (geo, k).getDistanceFrom (pos);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = k;
            }
        }
        return best;
    }

    juce::RangedAudioParameter* parameterFor (int k) const
    {
        static const vcab::P ids[3] = { vcab::P::formantF1, vcab::P::formantF2, vcab::P::formantF3 };
        return processor.state().getParameter (vcab::spec (ids[k]).key);
    }

    void setOffset (int k, double semis, bool gesture = false)
    {
        if (auto* param = parameterFor (k))
        {
            if (gesture)
                param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (std::clamp (semis, -12.0, 12.0))));
            if (gesture)
                param->endChangeGesture();
        }
    }

    VoiChanAlgoBoxProcessor& processor;
    vcab::FormantWarp warp;
    float shownFormant = -1000;
    float shownOffs[3] { -1000, -1000, -1000 };
    int shownMode = -1;
    double shownTrack[3] { 500, 1400, 2700 };
    bool shownValid = false;
    int dragging = -1;
};
