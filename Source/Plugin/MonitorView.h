// モニター: 入力と出力のスペクトル（重ねて表示）と、スペクトログラム（並べて表示）
// スペクトルには、EQ の特性の線（帯域の点をドラッグして操作できる）と、ディエッサーが見ている帯域の色付けを重ねる。
#pragma once

#include "PluginProcessor.h"
#include "Dsp/CutFilter.h"
#include "Dsp/Equalizer.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

class MonitorView : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit MonitorView (VoiChanAlgoBoxProcessor& p);

    // 画面のタイマーから呼ぶ。溜まった音を分析して描き直す。
    void update();

    void paint (juce::Graphics&) override;
    void resized() override;

    void setEqHandlesActive (bool active) { eqHandlesActive = active; }
    bool showsSpectrum() const { return ! spectrumArea.isEmpty(); } // スペクトルを描いているか（ノイズの見積もりの線を出せるか）

    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    static constexpr int fftOrder = 12;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int hopSize = 1024;
    static constexpr float minDb = -100.0f;
    static constexpr float maxDb = 0.0f;
    static constexpr float minHz = 50.0f;
    static constexpr float maxHz = 20000.0f;

    struct Channel
    {
        std::vector<float> history = std::vector<float> (fftSize, 0.0f); // 直近 fftSize サンプル（リングバッファ）
        int writePos = 0;
        int sinceLastFrame = 0;
        std::vector<float> spectrumDb = std::vector<float> (fftSize / 2 + 1, minDb); // なめらかにしたスペクトル
        // スペクトログラムの絵（CPU 側のメモリに置く）。列はリングバッファのように使い、
        // writeX が次に書き込む列（= いちばん古い列）
        juce::Image spectrogram;
        int writeX = 0;
    };

    void analyse (Channel& ch, const std::vector<float>& incoming);
    void computeFrame (Channel& ch, std::vector<float>& frameDb);
    void scrollSpectrogram (Channel& ch, const std::vector<float>& frameDb);

    float xForHz (float hz, juce::Rectangle<float> area) const;
    float yForDb (float db, juce::Rectangle<float> area) const;
    void drawSpectrum (juce::Graphics&, juce::Rectangle<int> area);
    void drawSpectrogram (juce::Graphics&, juce::Rectangle<int> area, const Channel& ch, const juce::String& title);
    void drawFrequencyGrid (juce::Graphics&, juce::Rectangle<float> area, bool labels);

    // ---- EQ の線と点 ----
    // 点の番号: 0 低域シェルフ、1 中域ピーク1、2 中域ピーク2、3 高域シェルフ、4 ローカット、5 ハイカット
    static constexpr int numHandles = 6;
    static constexpr float eqRangeDb = 18.0f; // EQ の線の縦軸は ±18dB（スペクトルの dB とは別の目盛り）
    float eqYForDb (float db, juce::Rectangle<float> area) const;
    float eqDbForY (float y, juce::Rectangle<float> area) const;
    float hzForX (float x, juce::Rectangle<float> area) const;
    juce::Point<float> handlePosition (int handle, const vcab::ParamValues& p, juce::Rectangle<float> area) const;
    int handleAt (juce::Point<float> pos) const;
    void setParam (vcab::P id, float value);
    void drawEq (juce::Graphics&, juce::Rectangle<float> area, const vcab::ParamValues& p);

    vcab::Equalizer eqForDrawing; // 画面に線を描くための EQ（音の処理のものとは別）
    vcab::CutFilter lowCutForDrawing { vcab::CutFilter::Kind::lowCut }, highCutForDrawing { vcab::CutFilter::Kind::highCut }; // カットの傾きの線を描くため
    double cutSampleRate = 0;
    double eqSampleRate = 0;
    bool eqHandlesActive = true;
    int hoverHandle = -1, dragHandle = -1;
    int lastMonitorMode = -1;

    VoiChanAlgoBoxProcessor& processor;

    juce::dsp::FFT fft { fftOrder };
    std::vector<float> window;
    std::vector<float> fftBuffer;
    std::vector<float> pulled;
    std::vector<float> frameScratch = std::vector<float> (fftSize / 2 + 1, minDb); // 1回分の分析結果
    std::array<juce::PixelARGB, 256> heatLut; // dB → 色の表

    Channel input, output;

    juce::Rectangle<int> spectrumArea, inSpecArea, outSpecArea;
};
