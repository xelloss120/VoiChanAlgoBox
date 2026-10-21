#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }
}

juce::AudioProcessorValueTreeState::ParameterLayout VoiChanAlgoBoxProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;


    for (const auto& s : vcab::paramSpecs())
    {
        const juce::ParameterID id { s.key, 1 };
        const auto name = utf8 (s.name);
        const auto unit = utf8 (s.unit);

        switch (s.type)
        {
            case vcab::ParamType::floating:
            {
                // ホストには刻みのない「連続した値」として伝える。刻みを付けると、とびとびの値をとる
                // パラメータ（周波数なら 19,001 通り）として伝わり、ホストによっては扱いが重くなるおそれがあるため。
                // 刻みは VoiChanAlgoBox の画面のつまみの側でだけ付ける（PluginEditor の withStep）。
                juce::NormalisableRange<float> range (s.minValue, s.maxValue);
                if (s.skewCentre > 0)
                    range.setSkewForCentre (s.skewCentre);

                auto attributes = juce::AudioParameterFloatAttributes()
                                      .withLabel (unit)
                                      .withValueFromStringFunction ([] (const juce::String& text) { return text.getFloatValue(); });

                if (s.id == vcab::P::pitch || s.id == vcab::P::formant)
                {
                    // 半音と変化率を両方表示する（例: +12 (+100%)）
                    attributes = attributes.withStringFromValueFunction ([] (float v, int)
                    {
                        const auto st = std::abs (v - std::round (v)) < 0.05f ? juce::String (juce::roundToInt (v)) : juce::String (v, 1);
                        const int percent = juce::roundToInt ((std::pow (2.0f, v / 12.0f) - 1.0f) * 100.0f);
                        return (v > 0.04f ? "+" : "") + st + " (" + (percent > 0 ? "+" : "") + juce::String (percent) + "%)";
                    });
                }
                else
                {
                    const bool wholeSteps = s.step >= 1.0f;
                    attributes = attributes.withStringFromValueFunction ([unit, wholeSteps] (float v, int)
                    {
                        // 100 以上と、1 刻みのものは小数点なし（juce::String に桁数 0 を渡すと「桁数の指定なし」になるので、整数にしてから作る）
                        const auto number = wholeSteps || std::abs (v) >= 100.0f ? juce::String (juce::roundToInt (v)) : juce::String (v, 1);
                        // 「:1」（比率）は数字に続けて書く。ほかの単位は空白を入れる
                        return number + (unit.startsWith (":") ? unit : " " + unit);
                    });
                }

                layout.add (std::make_unique<juce::AudioParameterFloat> (id, name, range, s.defaultValue, attributes));
                break;
            }
            case vcab::ParamType::toggle:
            {
                auto attributes = juce::AudioParameterBoolAttributes().withAutomatable (! vcab::isUiSetting (s.id));
                layout.add (std::make_unique<juce::AudioParameterBool> (id, name, s.defaultValue >= 0.5f, attributes));
                break;
            }

            case vcab::ParamType::choice:
            {
                juce::StringArray items;
                for (auto* c : s.choices)
                    items.add (utf8 (c));

                // 選択肢が1つしかないと、JUCE の中で範囲が 0〜0 になり、0〜1 に換算するときに 0 で割って
                // 値が NaN になる。NaN は自分自身と等しくならないので、ホストによっては「値が変わった」と
                // 判断し続けて止まらなくなる（Element で、画面を開くと極端に重くなった原因）。
                // そのため、使わない2つ目の選択肢を裏で足す（画面の選択欄には出さない）。
                if (items.size() < 2)
                    items.add ("-");

                auto attributes = juce::AudioParameterChoiceAttributes().withAutomatable (! vcab::isUiSetting (s.id));

                layout.add (std::make_unique<juce::AudioParameterChoice> (id, name, items, static_cast<int> (s.defaultValue), attributes));
                break;
            }
        }
    }

    return layout;
}

VoiChanAlgoBoxProcessor::VoiChanAlgoBoxProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VoiChanAlgoBox", createLayout())
{
    for (const auto& s : vcab::paramSpecs())
        raw[static_cast<size_t> (s.id)] = apvts.getRawParameterValue (s.key);

}

bool VoiChanAlgoBoxProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    // 入力はモノラル（ステレオが来たら、入力チャンネルの設定で選ぶ）、出力はモノラルとステレオ
    const bool inOk = in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
    const bool outOk = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    return inOk && outOk;
}

vcab::ParamValues VoiChanAlgoBoxProcessor::currentParams() const
{
    vcab::ParamValues p;
    for (size_t i = 0; i < raw.size(); ++i)
        p.v[i] = raw[i]->load();
    return p;
}

void VoiChanAlgoBoxProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate.store (sampleRate);
    engine.prepare (sampleRate, samplesPerBlock);
    mono.assign (static_cast<size_t> (juce::jmax (samplesPerBlock, 1)), 0.0f);
    reportedLatency = -1;
}

void VoiChanAlgoBoxProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    if (n == 0)
        return;

    if (static_cast<int> (mono.size()) < n)
        mono.resize (static_cast<size_t> (n)); // 想定より大きな塊が来たとき（本来は起きない）

    const auto params = effectiveParams();

    // 入力チャンネルを選ぶ（マイクが 2ch 側に入っているオーディオインターフェースもあるため）
    const int numIn = getTotalNumInputChannels();
    int choice = params.index (vcab::P::inputChannel); // 0 = 自動、1 = 1ch、2 = 2ch、3 = 平均

    if (choice == 0 && numIn >= 2)
    {
        // 自動: 各チャンネルの音量を約2秒でならし、もう片方が 6dB（4倍の電力）以上大きくなったら切り替える
        const double coef = std::exp (-n / (2.0 * getSampleRate()));
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto* d = buffer.getReadPointer (ch);
            double sum = 0;
            for (int i = 0; i < n; ++i)
                sum += static_cast<double> (d[i]) * d[i];
            autoLevel[ch] = coef * autoLevel[ch] + (1 - coef) * (sum / n);
        }
        const int other = 1 - autoChannel;
        if (autoLevel[other] > autoLevel[autoChannel] * 4.0 && autoLevel[other] > 1.0e-10)
            autoChannel = other;
        usedInputChannel.store (autoChannel);
        choice = autoChannel + 1;
    }
    else if (choice == 0)
    {
        choice = 1;
    }

    if (numIn <= 0)
        std::fill (mono.begin(), mono.begin() + n, 0.0f);
    else if (numIn == 1 || choice == 1)
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + n, mono.begin());
    else if (choice == 2)
        std::copy (buffer.getReadPointer (1), buffer.getReadPointer (1) + n, mono.begin());
    else
    {
        const auto* l = buffer.getReadPointer (0);
        const auto* r = buffer.getReadPointer (1);
        for (int i = 0; i < n; ++i)
            mono[static_cast<size_t> (i)] = 0.5f * (l[i] + r[i]);
    }

    inputAnalysis.push (mono.data(), n);
    engine.setRealtime (! isNonRealtime()); // ホストが書き出し中なら、重い方式もその場で計算する
    engine.process (mono.data(), n, params);
    outputAnalysis.push (mono.data(), n);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom (ch, 0, mono.data(), n);

    // 画面用の値と、ホストへの遅延の報告
    const auto lat = engine.latency (params);
    latencyVoice.store (lat.voice);
    latencyDenoise.store (lat.denoise);
    latencyRepair.store (lat.repair);
    latencyOutput.store (lat.output);

    if (lat.total() != reportedLatency)
    {
        reportedLatency = lat.total();
        setLatencySamples (reportedLatency);
    }
}

juce::AudioProcessorEditor* VoiChanAlgoBoxProcessor::createEditor()
{
    return new VoiChanAlgoBoxEditor (*this);
}

void VoiChanAlgoBoxProcessor::resetAllParameters()
{
    // 入力チャンネルは環境（オーディオインターフェース）に合わせた設定なので、戻さない（グラフや表示のモードなど、画面の設定は戻す）
    for (auto* param : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
        {
            const auto id = ranged->getParameterID();
            if (id != vcab::spec (vcab::P::inputChannel).key)
                ranged->setValueNotifyingHost (ranged->getDefaultValue());
        }
}

void VoiChanAlgoBoxProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
    {
        // デノイズの学習した値（「学習した値を保存」がオンのとき）
        xml->removeAttribute ("denoiseLearned");
        if (currentParams().on (vcab::P::denoiseSaveLearned))
        {
            const auto learned = engine.exportLearnedNoise();
            juce::MemoryBlock block (learned.data(), learned.size() * sizeof (float));
            xml->setAttribute ("denoiseLearned", block.toBase64Encoding());
        }
        copyXmlToBinary (*xml, destData);
    }
}

void VoiChanAlgoBoxProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            juce::MemoryBlock block;
            if (xml->hasAttribute ("denoiseLearned") && block.fromBase64Encoding (xml->getStringAttribute ("denoiseLearned")))
            {
                std::vector<float> learned (block.getSize() / sizeof (float));
                std::memcpy (learned.data(), block.getData(), learned.size() * sizeof (float));
                engine.importLearnedNoise (std::move (learned));
            }
            xml->removeAttribute ("denoiseLearned"); // 設定値の木には入れない
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VoiChanAlgoBoxProcessor();
}
