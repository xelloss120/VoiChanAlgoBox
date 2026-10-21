#pragma once

#include "Engine.h"
#include "LatencyProbe.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <functional>

// 画面に送る音（モニター用）。オーディオのスレッドが書き、画面のスレッドが読む。
class AnalysisFifo
{
public:
    explicit AnalysisFifo (int size = 1 << 15) : fifo (size), buffer (static_cast<size_t> (size)) {}

    void push (const float* data, int n) noexcept
    {
        const auto scope = fifo.write (n);
        if (scope.blockSize1 > 0) std::copy (data, data + scope.blockSize1, buffer.begin() + scope.startIndex1);
        if (scope.blockSize2 > 0) std::copy (data + scope.blockSize1, data + scope.blockSize1 + scope.blockSize2, buffer.begin() + scope.startIndex2);
    }

    // 溜まっている分を全部取り出す
    int pull (std::vector<float>& out)
    {
        const int ready = fifo.getNumReady();
        out.resize (static_cast<size_t> (ready));
        const auto scope = fifo.read (ready);
        if (scope.blockSize1 > 0) std::copy (buffer.begin() + scope.startIndex1, buffer.begin() + scope.startIndex1 + scope.blockSize1, out.begin());
        if (scope.blockSize2 > 0) std::copy (buffer.begin() + scope.startIndex2, buffer.begin() + scope.startIndex2 + scope.blockSize2, out.begin() + scope.blockSize1);
        return ready;
    }

private:
    juce::AbstractFifo fifo;
    std::vector<float> buffer;
};

class VoiChanAlgoBoxProcessor : public juce::AudioProcessor
{
public:
    VoiChanAlgoBoxProcessor();
    ~VoiChanAlgoBoxProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& state() { return apvts; }
    void resetAllParameters(); // すべての設定値を初期値に戻す
    vcab::ParamValues currentParams() const;
    // 処理に使う値（表示のモードで隠れた項目を、オフや初期値に置き換えたもの）。グラフもこれで描く
    vcab::ParamValues effectiveParams() const { return vcab::applyDisplayMode (currentParams()); }

    // 画面用
    vcab::EngineMeters& meters() { return engine.meters; }
    void requestNoiseLearning() { engine.requestNoiseLearning(); } // デノイズのノイズの学習（2 秒）を始める
    AnalysisFifo inputAnalysis, outputAnalysis;
    // 今の設定の方式が、フォルマントを分けて動かせるか（設定だけで決まるので、音が流れていなくても画面から求められる）
    bool supportsFormant (const vcab::ParamValues& p) const { return engine.algorithmSupportsFormant (p); }
    std::atomic<int> latencyVoice { 0 }, latencyDenoise { 0 }, latencyOutput { 0 }, latencyRepair { 0 };
    std::atomic<double> currentSampleRate { 48000 };
    std::atomic<int> usedInputChannel { 0 }; // 自動のときに使っている入力チャンネル（0 = 1ch、1 = 2ch）

    // EXE 版のオーディオの設定画面を開く（EXE 版の起動部分が設定する。VST3 では空のまま）
    std::function<void()> openAudioSettings;

    LatencyProbe latencyProbe; // 遅延の実測（画面が頼む）

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::AudioProcessorValueTreeState apvts;
    std::array<std::atomic<float>*, vcab::kNumParams> raw {};

    vcab::Engine engine;
    std::vector<float> mono;
    int reportedLatency = -1;

    // 入力チャンネルの自動選択（数秒ならした各チャンネルの音量）
    double autoLevel[2] { 0, 0 };
    int autoChannel = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiChanAlgoBoxProcessor)
};
