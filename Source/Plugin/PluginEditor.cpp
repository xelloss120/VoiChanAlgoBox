#include "PluginEditor.h"

#include "EditorStyle.h"

#include "../Core/Choices.h"
#include "../Core/Dsp/DenoiseSettings.h"
#include "../Core/Dsp/PitchDetector.h"

#include "../Core/Algorithms/FrequencyShifter.h"

using namespace editor;

namespace
{

// つまみに刻みを付ける（パラメータ自体は刻みなしでホストに伝えているため、画面の側でだけ付ける）
void withStep (juce::Slider& slider, vcab::P id)
{
    const auto& s = vcab::spec (id);
    if (s.step <= 0)
        return;
    auto range = slider.getNormalisableRange();
    range.interval = s.step;
    slider.setNormalisableRange (range);
}

// 枠の見出しの名前にマウスを乗せたときの説明。見出しにオン・オフがある枠は、そのスイッチの説明
juce::String boxDescription (const char* title, std::optional<vcab::P> toggle)
{
    using vcab::P;
    if (toggle)
        return utf8 (vcab::paramDescription (*toggle));
    const auto t = utf8 (title);
    auto is = [&] (const char* s) { return t == utf8 (s); };
    if (is ("デノイズ")) return utf8 (vcab::paramDescription (P::denoiseOn));
    if (is ("ゲート")) return utf8 (vcab::paramDescription (P::gateOn));
    if (is ("ブレス")) return utf8 (vcab::paramDescription (P::breathOn));
    if (is ("ディエッサー")) return utf8 (vcab::paramDescription (P::deessOn));
    if (is ("コンプ")) return utf8 (vcab::paramDescription (P::compOn));
    if (is ("出力ゲイン")) return utf8 (vcab::paramDescription (P::outputGain));
    if (is ("リミッター")) return utf8 ("出力が上限を超えないように、大きな音の瞬間だけ抑えて、音割れを防ぎます。");
    if (is ("ノイズ")) return utf8 ("ノイズの大きさの見積もり方と、学習（黙っている間のノイズを覚える）の設定です。");
    if (is ("共通")) return utf8 ("自動とハムの両方に効く設定です。");
    return {};
}

// タブの名前と、タブの横のオン・オフ（ないものは nullopt）
struct TabInfo { const char* name; std::optional<vcab::P> toggle; };
const TabInfo kTabs[] = {
    { "カット",       vcab::P::cutOn },
    { "ノッチ",       vcab::P::notchOn },
    { "リペア",         vcab::P::repairOn },
    { "デノイズ",     vcab::P::denoiseOn },
    { "ゲート",       vcab::P::gateOn },
    { "ボイチェン",   vcab::P::voiceOn },
    { "ブレス",         vcab::P::breathOn },
    { "EQ",           vcab::P::eqOn },
    { "ディエッサー", vcab::P::deessOn },
    { "コンプ",       vcab::P::compOn },
    { "リバーブ",       vcab::P::fxOn },
    { "出力",         vcab::P::outputOn },
};

// 見出し付きの選択欄（方式の一覧）。選択肢の番号は見出しのまとまりの順に並んでいるので、まとまりが変わるところに見出しを入れる
struct ChoiceSections
{
    const std::vector<const char*>* headings = nullptr;
    int (*sectionOf) (int index) = nullptr;
};

ChoiceSections choiceSections (vcab::P id)
{
    using vcab::P;
    if (id == P::algorithm)
        return { &vcab::algorithmSections(), [] (int i) { return vcab::algorithmInfo (i).section; } };
    if (id == P::denoiseMethod)
        return { &vcab::denoiseSections(), [] (int i) { return vcab::denoiseMethodInfo (i).section; } };
    if (id == P::pitchMethod || id == P::denoisePitchMethod)
        return { &vcab::pitchSections(), vcab::pitchSectionOf };
    return {};
}

} // namespace

// ---------------------------------------------------------------- Editor

VoiChanAlgoBoxEditor::VoiChanAlgoBoxEditor (VoiChanAlgoBoxProcessor& p)
    : AudioProcessorEditor (&p), processor (p), dynamics (p), monitor (p)
{
    setLookAndFeel (&lookAndFeel);
    tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 400);

    using vcab::P;

    // すべてのパラメータの行を作る（どのタブに出すかは showTab で決める）
    for (const auto& s : vcab::paramSpecs())
        if (s.id != P::pitch && s.id != P::formant)
            addRow (s.id);

    // ピッチとフォルマントは大きなつまみ
    auto setupKnob = [this] (juce::Slider& knob, juce::Label& label, const char* name, vcab::P id,
                             std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& attachment)
    {
        knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 130, 28);
        knob.setTooltip (utf8 (vcab::paramDescription (id)));
        label.setTooltip (utf8 (vcab::paramDescription (id)));
        addChildComponent (knob);
        label.setText (utf8 (name), juce::dontSendNotification);
        label.setFont (juce::FontOptions (kFont, juce::Font::bold));
        label.setJustificationType (juce::Justification::centred);
        addChildComponent (label);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.state(), vcab::spec (id).key, knob);
        withStep (knob, id);
    };
    setupKnob (pitchKnob, pitchLabel, "ピッチ", P::pitch, pitchAttachment);
    setupKnob (formantKnob, formantLabel, "フォルマント", P::formant, formantAttachment);

    pitchOffNote.setFont (juce::FontOptions (kFont));
    pitchOffNote.setColour (juce::Label::textColourId, kText.withAlpha (0.6f));
    pitchOffNote.setMinimumHorizontalScale (1.0f); // 横に縮めず、2行に折り返す（文字の大きさの下限を守るため）
    pitchOffNote.setJustificationType (juce::Justification::topLeft);
    addChildComponent (pitchOffNote);

    shiftLabel.setFont (juce::FontOptions (kFont));
    shiftLabel.setColour (juce::Label::textColourId, kText);
    addChildComponent (shiftLabel);

    lpcFixLabel.setText (utf8 ("LPC の改善"), juce::dontSendNotification);
    lpcFixLabel.setFont (juce::FontOptions (kFont));
    lpcFixLabel.setColour (juce::Label::textColourId, kText);
    lpcFixLabel.setTooltip (utf8 ("LPC の方式の音質を上げる、定番の3つの対策です。それぞれオン・オフして聞き比べられます。"));
    addChildComponent (lpcFixLabel);

    formantNote.setText (utf8 ("この方式では、フォルマントはピッチと一緒に動きます"), juce::dontSendNotification);
    formantNote.setFont (juce::FontOptions (kFont));
    formantNote.setColour (juce::Label::textColourId, juce::Colour (0xffd0b060));
    formantNote.setJustificationType (juce::Justification::centred);
    addChildComponent (formantNote);

    // ディエッサーとコンプの、削っている量
    for (auto* l : { &deessMeterLabel, &compMeterLabel, &notchStatusLabel, &denoiseStatusLabel, &repairStatusLabel })
    {
        l->setFont (juce::FontOptions (kFont));
        l->setColour (juce::Label::textColourId, kText);
        addChildComponent (*l);
    }
    deessMeterLabel.setTooltip (utf8 ("ディエッサーが、今、高い帯域をどれだけ下げているか。サ行のときだけ動くのが理想です。"));
    notchStatusLabel.setTooltip (utf8 ("自動で追いかけて削っている周波数（スペクトルにも ▼ の印で出ます）と、ハムで削っている電源の周波数です。"));
    notchStatusLabel.setMinimumHorizontalScale (1.0f);
    notchStatusLabel.setJustificationType (juce::Justification::topLeft);
    denoiseStatusLabel.setTooltip (utf8 ("ノイズの学習の状態と、声のありそうさ（ゲートに使うとき）、検出した声の高さ（倍音を使う方式）です。"));
    denoiseAmountLabel.setTooltip (utf8 ("デノイズが入力に比べて音量をどれだけ下げているか（約 0.3 秒でならした値）。黙っているときに大きく、しゃべるときに小さいのが理想です。"));
    denoiseAmountLabel.setJustificationType (juce::Justification::centredRight);
    denoiseAmountLabel.setMinimumHorizontalScale (1.0f);
    addChildComponent (denoiseAmountLabel);
    denoiseStatusLabel.setMinimumHorizontalScale (1.0f);
    repairStatusLabel.setTooltip (utf8 ("リペアの各処理が、一瞬だけの音を見つけて直した回数です（起動してから）。"));
    repairStatusLabel.setMinimumHorizontalScale (1.0f);
    repairStatusLabel.setJustificationType (juce::Justification::topLeft);
    denoiseStatusLabel.setJustificationType (juce::Justification::topLeft);
    learnButton.setButtonText (utf8 ("ノイズを学習（2 秒）"));
    learnButton.setTooltip (utf8 ("押したら 2 秒間、黙っていてください。その間のノイズを覚えて、以後はその値で下げます（見積もりが「学習」のとき）。"));
    learnButton.onClick = [this] { processor.requestNoiseLearning(); };
    addChildComponent (learnButton);
    compMeterLabel.setTooltip (utf8 ("コンプが、今、音量をどれだけ下げているか。普通にしゃべって 3〜6dB 程度が、自然に聞こえる目安です。"));

    // 処理のタブは rebuildTabs で並べる（表示のモードで隠すタブがあるため）
    tabBar.addChangeListener (this);
    addAndMakeVisible (tabBar);

    resetButton.setButtonText (utf8 ("初期値に戻す"));
    resetButton.setTooltip (utf8 ("すべての設定を初期値に戻します（入力チャンネルは戻しません）。押すと確認が出ます。"));
    resetButton.onClick = [this]
    {
        auto options = juce::MessageBoxOptions()
                           .withIconType (juce::MessageBoxIconType::QuestionIcon)
                           .withTitle (utf8 ("初期値に戻す"))
                           .withMessage (utf8 ("すべての設定を初期値に戻します。よろしいですか？\n（入力チャンネルとオーディオデバイスの設定は変わりません）"))
                           .withButton (utf8 ("戻す"))
                           .withButton (utf8 ("やめる"))
                           .withAssociatedComponent (this);

        juce::Component::SafePointer<VoiChanAlgoBoxEditor> safeThis (this);
        juce::AlertWindow::showAsync (options, [safeThis] (int result)
        {
            if (result == 1 && safeThis != nullptr)
                safeThis->processor.resetAllParameters();
        });
    };
    addAndMakeVisible (resetButton);

    // EXE 版だけ、右上にオーディオの設定画面を開くボタン（「オプション」の中と同じ画面）
    audioSettingsButton.setButtonText (utf8 ("オーディオ設定"));
    audioSettingsButton.setTooltip (utf8 ("オーディオの設定画面（入出力の機器、サンプルレート、バッファの大きさ）を開きます。左上の「オプション」の中と同じ画面です。"));
    audioSettingsButton.onClick = [this]
    {
        if (processor.openAudioSettings)
            processor.openAudioSettings();
    };
    if (processor.openAudioSettings)
        addAndMakeVisible (audioSettingsButton);

    resetTabButton.setButtonText (utf8 ("このタブを初期値に戻す"));
    resetTabButton.setTooltip (utf8 ("今開いているタブの設定だけを初期値に戻します（タブの横のオン・オフも含む）。押すと確認が出ます。"));
    resetTabButton.onClick = [this]
    {
        const int tab = currentTab;
        auto options = juce::MessageBoxOptions()
                           .withIconType (juce::MessageBoxIconType::QuestionIcon)
                           .withTitle (utf8 ("このタブを初期値に戻す"))
                           .withMessage (utf8 ("「") + utf8 (kTabs[tab].name) + utf8 ("」の設定を初期値に戻します。よろしいですか？"))
                           .withButton (utf8 ("戻す"))
                           .withButton (utf8 ("やめる"))
                           .withAssociatedComponent (this);

        juce::Component::SafePointer<VoiChanAlgoBoxEditor> safeThis (this);
        juce::AlertWindow::showAsync (options, [safeThis, tab] (int result)
        {
            if (result == 1 && safeThis != nullptr)
                safeThis->resetTab (tab);
        });
    };
    addAndMakeVisible (resetTabButton);


    latencyLabel.setTooltip (utf8 ("マイクに入った音が、出力されるまでの遅れ。合計と、その内訳です。遅れが大きいほど、自分の声をモニターしながらしゃべりにくくなります。"));
    latencyLabel.setFont (juce::FontOptions (kFont));
    latencyLabel.setColour (juce::Label::textColourId, kText);
    addAndMakeVisible (latencyLabel);

    addAndMakeVisible (dynamics);
    addChildComponent (pitchView);
    addChildComponent (warpView);
    addAndMakeVisible (monitor);

    // 入力チャンネルとモニターの表示の切り替えは、タブに関係なく常に出す
    row (P::inputChannel).setVisible (true);
    row (P::monitorMode).setVisible (true);
    row (P::uiMode).setVisible (true);
    row (P::showNoiseEstimate).setVisible (true);
    // ダイナミクスの点とメーターのスイッチは、右上の入力チャンネルの枠の中に、名前付きのスイッチだけを置く
    row (P::showDynamicsLive).toggle->setVisible (true);
    row (P::showDynamicsLive).label->setVisible (false);

    updateWindowSize();

    rebuildTabs();
    startTimerHz (30);
}

VoiChanAlgoBoxEditor::~VoiChanAlgoBoxEditor()
{
    stopTimer();
    processor.meters().noiseCurveWanted.store (false); // 画面を閉じたら、ノイズの見積もりの線の計算も止める
    tabToggleAttachments.clear(); // ボタン（タブが持つ）より先に、つなぎを消す
    setLookAndFeel (nullptr);
}

ParamRow& VoiChanAlgoBoxEditor::addRow (vcab::P id)
{
    const auto& s = vcab::spec (id);
    auto r = std::make_unique<ParamRow>();
    r->id = id;

    // 組の名前（ローカット、低域シェルフなど）と重なる部分は省く
    auto text = utf8 (s.name);
    for (auto* prefix : { "ローカット ", "ハイカット ", "低域シェルフ ", "中域ピーク1 ", "中域ピーク2 ", "高域シェルフ ", "自動 ", "ハム ", "突発音 ", "残響 ", "吹かれ ", "リップ ", "パチパチ ", "息継ぎ " })
    {
        // 先頭の文字列そのものを比べる（startsWith は、日本語で先頭の 1 文字が同じだけでも一致とみなすことがあった:
        // 「残ったノイズ…」が「残響 」で始まるとみなされ、先頭の 3 文字が消えた）
        const auto p = utf8 (prefix);
        if (text.substring (0, p.length()) == p)
            text = text.substring (p.length());
    }

    const auto description = utf8 (vcab::paramDescription (id));

    r->label = std::make_unique<juce::Label> (juce::String(), text);
    r->label->setTooltip (description);
    r->label->setFont (juce::FontOptions (kFont));
    r->label->setColour (juce::Label::textColourId, kText);
    addChildComponent (*r->label);

    auto& apvts = processor.state();

    switch (s.type)
    {
        case vcab::ParamType::floating:
            r->slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
            r->slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, kValueBoxWidth, kRowHeight - 6);
            r->slider->setTooltip (description);
            addChildComponent (*r->slider);
            r->sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, s.key, *r->slider);
            withStep (*r->slider, id);
            break;

        case vcab::ParamType::toggle:
            // LPC の改善のスイッチは1行に横に並べるので、名前をスイッチの横に書く
            r->inlineToggle = id == vcab::P::denoiseComfort || id == vcab::P::denoiseSaveLearned || id == vcab::P::showNoiseEstimate
                              || id == vcab::P::denoiseDfnPostFilter || id == vcab::P::gateFromDenoise;
            r->toggle = std::make_unique<juce::ToggleButton> (id == vcab::P::lpcBandwidth || id == vcab::P::lpcInterpolation || id == vcab::P::lpcEmphasis
                                                                  || id == vcab::P::showDynamicsLive || r->inlineToggle
                                                                  ? text : utf8 ("オン"));
            r->toggle->setTooltip (description);
            addChildComponent (*r->toggle);
            r->buttonAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, s.key, *r->toggle);
            break;

        case vcab::ParamType::choice:
        {
            r->combo = std::make_unique<juce::ComboBox>();
            // 項目の文字。ボイチェンの方式とピッチ検出の方式には、遅延の目安（初期設定のとき）を添える
            auto itemText = [&] (int index)
            {
                auto text = utf8 (s.choices[static_cast<size_t> (index)]);
                if (id == vcab::P::algorithm)
                    text += utf8 ("（遅延 ") + utf8 (vcab::algorithmInfo (index).latencyHint) + utf8 ("）");
                if (id == vcab::P::denoiseMethod)
                    text += utf8 ("（") + utf8 (vcab::denoiseMethodInfo (index).latencyHint) + utf8 ("）");
                if (id == vcab::P::pitchMethod || id == vcab::P::denoisePitchMethod)
                {
                    vcab::ParamValues pv;
                    pv.set (vcab::P::pitchMethod, static_cast<float> (index));
                    const double ms = vcab::PitchDetector::analysisDelayFor (48000.0, pv) / 48.0;
                    text += utf8 ("（遅延 約") + juce::String (juce::roundToInt (ms)) + utf8 ("ms）");
                }
                return text;
            };
            if (const auto sections = choiceSections (id); sections.headings != nullptr)
            {
                // 見出しを付けて並べる。見出しは項目に数えないので、項目の番号（保存する番号 + 1）でつなぐ
                int section = -1;
                for (int index = 0; index < static_cast<int> (s.choices.size()); ++index)
                {
                    if (sections.sectionOf (index) != section)
                    {
                        section = sections.sectionOf (index);
                        r->combo->addSectionHeading (utf8 ((*sections.headings)[static_cast<size_t> (section)]));
                    }
                    r->combo->addItem (itemText (index), index + 1);
                }
                auto* combo = r->combo.get();
                auto* param = apvts.getParameter (s.key);
                r->orderedAttachment = std::make_unique<juce::ParameterAttachment> (
                    *param, [combo] (float value) { combo->setSelectedId (juce::roundToInt (value) + 1, juce::dontSendNotification); });
                auto* attachment = r->orderedAttachment.get();
                combo->onChange = [combo, attachment]
                {
                    if (combo->getSelectedId() > 0)
                        attachment->setValueAsCompleteGesture (static_cast<float> (combo->getSelectedId() - 1));
                };
                r->orderedAttachment->sendInitialUpdate();
            }
            else
            {
                for (int index = 0; index < static_cast<int> (s.choices.size()); ++index)
                    r->combo->addItem (itemText (index), index + 1);
                r->comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, s.key, *r->combo);
            }
            r->combo->setTooltip (description);
            r->baseTooltip = description;
            addChildComponent (*r->combo);
            break;
        }
    }

    rows.push_back (std::move (r));
    return *rows.back();
}

ParamRow& VoiChanAlgoBoxEditor::row (vcab::P id)
{
    for (auto& r : rows)
        if (r->id == id)
            return *r;
    jassertfalse;
    return *rows.front();
}

std::vector<vcab::P> VoiChanAlgoBoxEditor::idsForTab (int tab) const
{
    using vcab::P;
    switch (tab)
    {
        case tabCut:     return { P::lowCutOn, P::lowCutFreq, P::lowCutSlope, P::highCutOn, P::highCutFreq, P::highCutSlope };
        case tabNotch:   return { P::notchAutoOn, P::notchCount, P::notchSensitivity, P::notchHumOn, P::humFreq, P::humHarmonics, P::notchDepth };
        case tabRepair:  return { P::repairTransientOn, P::repairTransientThreshold, P::repairTransientDepth,
                                  P::repairDeclipOn, P::repairClipLevel, P::repairPlosiveOn, P::repairPlosiveFreq, P::repairPlosiveThreshold,
                                  P::repairPlosiveDepth, P::repairClickOn, P::repairClickThreshold, P::repairClickDepth,
                                  P::repairTransientMode, P::repairClickMode, P::repairCrackleOn, P::repairCrackleThreshold,
                                  P::repairBreathOn, P::repairBreathMargin, P::repairBreathDepth };
        case tabDenoise: return { P::denoiseMethod, P::denoiseRule, P::denoiseFftSize, P::denoiseNoiseMode, P::denoiseMinWindow, P::denoiseLevelThreshold, P::denoiseSppSnr, P::denoiseRnnThreshold, P::denoiseMaxReduction, P::denoiseStrength,
                                  P::denoiseThreshold, P::denoiseRatio, P::denoiseAttackMs, P::denoiseReleaseMs,
                                  P::denoisePitchMethod, P::denoiseCombDepth, P::denoiseProtect, P::denoiseSmoothing, P::denoiseSmoothWidth,
                                  P::denoisePresence, P::denoiseSaveLearned,
                                  P::denoiseDereverbOn, P::denoiseRt60, P::denoiseDfnModel, P::denoiseDfnPostFilter,
                                  P::denoiseFftRule, P::denoiseComfort, P::denoiseOnset };
        case tabGate:    return { P::gateMode, P::gateRatio, P::gateKnee, P::gateThreshold, P::gateAttackMs, P::gateHoldMs, P::gateReleaseMs, P::gateHysteresis, P::gateRange, P::gateFromDenoise };
        case tabVoice:   return { P::algorithm, P::dlWindowMs, P::wsolaWindowMs, P::wsolaSearchMs, P::pitchMethod, P::pitchMinHz, P::pitchMaxHz, P::pvFftSize, P::pvPhaseLock, P::formantMethod, P::smMaxSines, P::smNoiseOn, P::lpcOrder, P::lpcBandwidth, P::lpcInterpolation, P::lpcEmphasis, P::cvBands, P::fsReferenceHz, P::bsBands, P::bsFormantMethod, P::worldBreath, P::worldF0Method, P::mixedExcitation, P::glottalShape, P::formantWarp, P::intonation, P::psolaMarking };
        case tabBreath:  return { P::breathAmount, P::breathToneHz };
        case tabEq:      return { P::eqLowCutOn, P::eqLowCutFreq, P::eqLowCutSlope, P::eqHighCutOn, P::eqHighCutFreq, P::eqHighCutSlope,
                                  P::eqLowShelfOn, P::eqPeak1On, P::eqPeak2On, P::eqHighShelfOn,
                                  P::eqLowShelfFreq, P::eqLowShelfGain, P::eqLowShelfQ, P::eqPeak1Freq, P::eqPeak1Gain, P::eqPeak1Q,
                                  P::eqPeak2Freq, P::eqPeak2Gain, P::eqPeak2Q, P::eqHighShelfFreq, P::eqHighShelfGain, P::eqHighShelfQ };
        case tabDeEsser: return { P::deessFreq, P::deessThreshold, P::deessMaxReduction };
        case tabComp:    return { P::compThreshold, P::compRatio, P::compAttackMs, P::compReleaseMs, P::compKnee };
        case tabFx:      return { P::echoOn, P::echoTimeMs, P::echoFeedback, P::echoDampingHz, P::echoMix,
                                  P::reverbOn, P::reverbSize, P::reverbDamping, P::reverbPreDelayMs, P::reverbMix };
        case tabOutput:  return { P::outputGain, P::limiterCeiling, P::limiterReleaseMs };
        default:         return {};
    }
}

void VoiChanAlgoBoxEditor::resetTab (int tab)
{
    auto ids = idsForTab (tab);
    if (const auto toggleId = kTabs[tab].toggle)
        ids.push_back (*toggleId);
    if (tab == tabVoice)
    {
        ids.push_back (vcab::P::pitch);
        ids.push_back (vcab::P::formant);
        // F1 / F2 / F3 の差は、つまみの行ではなくグラフの点で変えるので、ここで足す
        for (auto id : { vcab::P::formantF1, vcab::P::formantF2, vcab::P::formantF3 })
            ids.push_back (id);
        ids.push_back (vcab::P::voiceGraph); // グラフの選択も戻す（初期値は声の高さ）
        // 方式も戻す（全体の「初期値に戻す」とそろえる）
    }

    for (auto id : ids)
        if (auto* param = processor.state().getParameter (vcab::spec (id).key))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->getDefaultValue());
            param->endChangeGesture();
        }
}

void VoiChanAlgoBoxEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const int index = tabBar.getCurrentTabIndex();
    if (index >= 0 && index < static_cast<int> (shownTabs.size()))
        showTab (shownTabs[static_cast<size_t> (index)]);
}

void VoiChanAlgoBoxEditor::rebuildTabs()
{
    shownMode = processor.currentParams().index (vcab::P::uiMode);

    // 処理のタブ（処理の順番どおり）。タブの横にオン・オフ。オン・オフが今のモードで隠れるタブは並べない
    tabToggleAttachments.clear(); // スイッチ（タブが持つ）より先に消す
    tabBar.clearTabs();
    shownTabs.clear();
    for (int t = 0; t < numTabs; ++t)
    {
        const auto toggleId = kTabs[t].toggle;
        if (toggleId && ! shown (*toggleId))
            continue;
        const int index = static_cast<int> (shownTabs.size());
        shownTabs.push_back (t);
        tabBar.addTab (utf8 (kTabs[t].name), kPanel, -1);
        if (toggleId)
        {
            auto* toggle = new juce::ToggleButton();
            toggle->setTooltip (utf8 (vcab::paramDescription (*toggleId)));
            toggle->setSize (26, 26);
            tabBar.getTabButton (index)->setExtraComponent (toggle, juce::TabBarButton::afterText); // タブが持つ
            tabToggleAttachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                processor.state(), vcab::spec (*toggleId).key, *toggle));
        }
    }

    // 今のタブが隠れたら、ボイチェンのタブへ（ボイチェンはどのモードでも出す）
    auto it = std::find (shownTabs.begin(), shownTabs.end(), currentTab);
    if (it == shownTabs.end())
        it = std::find (shownTabs.begin(), shownTabs.end(), static_cast<int> (tabVoice));
    const int index = static_cast<int> (it - shownTabs.begin());
    tabBar.setCurrentTabIndex (index, false);
    showTab (shownTabs[static_cast<size_t> (index)]);
}

void VoiChanAlgoBoxEditor::showTab (int tab)
{
    currentTab = tab;

    // いったん、タブの中身になる行を全部隠す（入力チャンネルとモニターの切り替えは除く）
    for (auto& r : rows)
        if (r->id != vcab::P::inputChannel && r->id != vcab::P::monitorMode && r->id != vcab::P::showDynamicsLive && r->id != vcab::P::showNoiseEstimate
            && r->id != vcab::P::uiMode)
            r->setVisible (false);
    for (auto* c : std::initializer_list<juce::Component*> { &pitchKnob, &formantKnob, &pitchLabel, &formantLabel, &formantNote, &deessMeterLabel, &compMeterLabel, &notchStatusLabel, &denoiseStatusLabel, &denoiseAmountLabel, &repairStatusLabel, &learnButton, &lpcFixLabel, &shiftLabel, &pitchOffNote })
        c->setVisible (false);

    for (auto id : idsForTab (tab))
        row (id).setVisible (true);

    if (tab == tabVoice)
        for (auto* c : std::initializer_list<juce::Component*> { &pitchKnob, &formantKnob, &pitchLabel, &formantLabel })
            c->setVisible (true);
    deessMeterLabel.setVisible (tab == tabDeEsser);
    notchStatusLabel.setVisible (tab == tabNotch);
    denoiseStatusLabel.setVisible (tab == tabDenoise);
    denoiseAmountLabel.setVisible (tab == tabDenoise);
    repairStatusLabel.setVisible (tab == tabRepair);
    learnButton.setVisible (tab == tabDenoise);
    pitchView.setVisible (false); // ボイチェンのタブでは updateVisibility が方式に合わせて出す
    warpView.setVisible (false);
    compMeterLabel.setVisible (tab == tabComp);

    updateVisibility();
    resized();
}

void VoiChanAlgoBoxEditor::layoutRows (juce::Rectangle<int> area, std::initializer_list<vcab::P> ids, int rowHeight)
{
    for (auto id : ids)
    {
        if (! shown (id))
            continue; // 表示のモードで隠れる行は詰める
        auto& r = row (id);
        auto line = area.removeFromTop (rowHeight);
        r.label->setBounds (line.removeFromLeft (150));
        r.control()->setBounds (line.reduced (0, 2));
    }
}

int VoiChanAlgoBoxEditor::layoutBox (juce::Rectangle<int> area, const char* title, std::optional<vcab::P> headerToggle, const std::vector<vcab::P>& allIds)
{
    // 表示のモードで隠れる項目と、今の設定で使わない項目（ParamRelevance）は並べない。
    // 見出しのスイッチが隠れる枠と、項目がすべて隠れる枠は出さない（高さ 0 を返す）
    if (headerToggle && ! shown (*headerToggle))
        return 0;
    const auto p = processor.currentParams();
    std::vector<vcab::P> ids;
    for (auto id : allIds)
        if (shown (id) && relevanceOf (id, p) != vcab::Relevance::hidden)
            ids.push_back (id);
    if (! headerToggle && ids.empty())
        return 0;

    constexpr int pad = 5;
    const int height = kBoxTitle + static_cast<int> (ids.size()) * kRowHeight + pad * 2;

    auto box = area.removeFromTop (height);
    boxes.push_back ({ box, utf8 (title) });

    // 見出しの名前（の文字の範囲）にマウスを乗せたら、機能の説明を出す。文字は paint で描く
    {
        const auto index = boxes.size() - 1;
        if (index >= boxTitleLabels.size())
        {
            boxTitleLabels.push_back (std::make_unique<juce::Label>());
            addChildComponent (*boxTitleLabels.back());
        }
        auto& l = *boxTitleLabels[index];
        const int w = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (kFont, juce::Font::bold), utf8 (title)) + 8;
        l.setBounds (box.reduced (10, 5).removeFromTop (kBoxTitle - 2).withWidth (w));
        l.setTooltip (boxDescription (title, headerToggle));
        l.setVisible (true);
    }

    auto inner = box.reduced (8, pad);
    auto header = inner.removeFromTop (kBoxTitle);
    if (headerToggle)
    {
        auto& r = row (*headerToggle);
        r.label->setVisible (false); // 見出しの名前は枠の側で描く
        r.toggle->setTooltip ({});   // 説明は見出しの名前の側で出す
        r.toggle->setBounds (header.removeFromRight (72).reduced (0, 2));
    }

    // 狭い枠では、名前の欄を短くし、スライダーと値の欄を1本のバーにまとめる
    // （バー全体をドラッグして値を変え、値はバーの中に出す。短いスライダーは操作しにくいため）
    const bool narrow = inner.getWidth() < 260;
    const int labelWidth = narrow ? 62 : 120;
    for (auto id : ids)
    {
        auto& r = row (id);
        if (r.slider != nullptr)
        {
            r.slider->setSliderStyle (narrow ? juce::Slider::LinearBar : juce::Slider::LinearHorizontal);
            r.slider->setTextBoxStyle (narrow ? juce::Slider::TextBoxLeft : juce::Slider::TextBoxRight, false, kValueBoxWidth, kRowHeight - 6);
        }
        auto line = inner.removeFromTop (kRowHeight);
        if (r.inlineToggle)
        {
            r.control()->setBounds (line.withTrimmedLeft (4).reduced (0, 2));
            continue;
        }
        r.label->setBounds (line.removeFromLeft (labelWidth).withTrimmedLeft (4));
        r.control()->setBounds (line.reduced (0, 2));
    }
    return height;
}

void VoiChanAlgoBoxEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    g.setColour (kText);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("VoiChanAlgoBox", headerArea.reduced (12, 0).removeFromLeft (150), juce::Justification::centredLeft);

    // タブの中身の背景と、右側の背景
    g.setColour (kPanel);
    g.fillRoundedRectangle (contentArea.toFloat(), 6.0f);
    g.fillRoundedRectangle (sidePanel.toFloat(), 6.0f);

    // 組の枠と見出し
    for (const auto& b : boxes)
    {
        g.setColour (kBox);
        g.fillRoundedRectangle (b.bounds.toFloat(), 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawRoundedRectangle (b.bounds.toFloat().reduced (0.5f), 5.0f, 1.0f);
        g.setColour (kText);
        g.setFont (juce::FontOptions (kFont, juce::Font::bold));
        g.drawText (b.title, b.bounds.reduced (10, 5).removeFromTop (kBoxTitle - 2), juce::Justification::centredLeft);
    }

    // ディエッサーとコンプの、削っている量のバー（0〜-24dB）
    auto grBar = [&] (juce::Rectangle<int> area, float grDb, juce::Colour c)
    {
        if (area.isEmpty()) return;
        g.setColour (juce::Colour (0xff0f1724));
        g.fillRect (area);
        const float t = juce::jlimit (0.0f, 1.0f, -grDb / 24.0f);
        g.setColour (c);
        g.fillRect (area.toFloat().withWidth (static_cast<float> (area.getWidth()) * t));
    };
    if (currentTab == tabDeEsser) grBar (deessMeterArea, deessGrShown, juce::Colour (0xffd070c0));
    if (currentTab == tabComp) grBar (compMeterArea, compGrShown, juce::Colour (0xfff0a040));
}

void VoiChanAlgoBoxEditor::resized()
{
    using vcab::P;

    auto r = getLocalBounds();
    boxes.clear();
    for (auto& l : boxTitleLabels)
        l->setVisible (false);
    deessMeterArea = {};
    compMeterArea = {};

    // ---- 上の段 ----
    headerArea = r.removeFromTop (kHeaderHeight);
    {
        auto h = headerArea.reduced (12, 8);
        h.removeFromLeft (160);
        resetButton.setBounds (h.removeFromLeft (150));
        h.removeFromLeft (20);
        audioSettingsButton.setBounds (h.removeFromRight (170));
        h.removeFromRight (12);
        auto& ui = row (P::uiMode); // 表示のモード（簡単 / 普通 / 詳細）
        ui.combo->setBounds (h.removeFromRight (110));
        ui.label->setBounds (h.removeFromRight (48));
        h.removeFromRight (12);
        latencyLabel.setBounds (h);
    }

    // ---- 右側: 入力チャンネルとダイナミクスのグラフ ----
    auto middle = r.removeFromTop (kTabBarHeight + kContentHeight).reduced (kMargin, 0);
    sidePanel = middle.removeFromRight (kSideWidth);
    middle.removeFromRight (kMargin);
    {
        auto a = sidePanel.reduced (8, 6);
        auto& in = row (P::inputChannel);
        auto line = a.removeFromTop (30);
        in.label->setBounds (line.removeFromLeft (130));
        in.combo->setBounds (line.reduced (0, 1));
        row (P::showDynamicsLive).toggle->setBounds (a.removeFromTop (28).reduced (0, 2));
        a.removeFromTop (4);
        dynamics.setBounds (a);
    }

    // ---- タブと、その中身 ----
    tabBar.setBounds (middle.removeFromTop (kTabBarHeight));
    contentArea = middle;
    auto c = contentArea.reduced (10, 10);

    // タブごとの「初期値に戻す」は右下
    resetTabButton.setBounds (c.getRight() - 230, c.getBottom() - 32, 230, 32);
    resetTabButton.setVisible (true);

    layoutRelevance = tabRelevance (processor.currentParams()); // 並べたときの項目の出し入れ（変わったら並べ直す）
    layoutTab (c);

    // ---- モニター ----
    r.removeFromTop (kMargin);
    {
        auto bar = r.removeFromTop (kMonitorBarHeight).reduced (kMargin, 4);
        auto& mode = row (P::monitorMode);
        mode.label->setBounds (bar.removeFromLeft (100));
        mode.combo->setBounds (bar.removeFromLeft (240));
        bar.removeFromLeft (16);
        row (P::showNoiseEstimate).control()->setBounds (bar.removeFromLeft (280)); // デノイズのノイズの見積もりの線
    }
    monitor.setBounds (r.reduced (kMargin, 0).withTrimmedBottom (kMargin));
    monitor.resized();

    repaint();
}

vcab::Relevance VoiChanAlgoBoxEditor::relevanceOf (vcab::P id, const vcab::ParamValues& p) const
{
    return vcab::relevance (id, p, processor.supportsFormant (p));
}

std::vector<vcab::Relevance> VoiChanAlgoBoxEditor::tabRelevance (const vcab::ParamValues& p) const
{
    std::vector<vcab::Relevance> r;
    for (auto id : idsForTab (currentTab))
        r.push_back (relevanceOf (id, p));
    return r;
}

void VoiChanAlgoBoxEditor::updateWindowSize()
{
    monitorMode = processor.currentParams().index (vcab::P::monitorMode);
    const bool hidden = monitorMode == vcab::monitorHidden;

    // モニターの表示の切り替えの欄は残す（そこからまた表示に戻せるように）
    const int height = kHeaderHeight + kTabBarHeight + kContentHeight + kMargin + kMonitorBarHeight + (hidden ? 0 : kMonitorHeight) + kMargin;
    monitor.setVisible (! hidden);
    if (getWidth() != kWidth || getHeight() != height)
        setSize (kWidth, height);
    else
        resized();
}

void VoiChanAlgoBoxEditor::updateVisibility()
{
    using vcab::P;
    const auto p = processor.currentParams();

    updateTabVisibility (p);


    // 表示のモードで隠れる項目（上で出したものも含めて、最後に隠す）
    for (auto& r : rows)
        if (! shown (r->id))
            r->setVisible (false);
    if (! shown (P::denoiseNoiseMode))
        learnButton.setVisible (false);
    if (! shown (P::lpcBandwidth))
        lpcFixLabel.setVisible (false);
    if (! shown (P::fsReferenceHz))
        shiftLabel.setVisible (false);
}

void VoiChanAlgoBoxEditor::updateChoiceTooltips()
{
    for (auto& r : rows)
    {
        if (r->combo == nullptr)
            continue;
        const int index = r->combo->getSelectedId() - 1; // 項目の番号は「保存する番号 + 1」
        if (index == r->tooltipIndex)
            continue;
        r->tooltipIndex = index;
        auto text = r->baseTooltip;
        const auto& choices = vcab::spec (r->id).choices;
        if (const char* d = vcab::choiceDescription (r->id, index); d != nullptr && index < static_cast<int> (choices.size()))
            text << "\n" << utf8 ("「") << utf8 (choices[static_cast<size_t> (index)]) << utf8 ("」") << utf8 (d);
        r->combo->setTooltip (text);
        r->label->setTooltip (text);
    }
}

void VoiChanAlgoBoxEditor::timerCallback()
{
    updateChoiceTooltips();

    // フォルマントの動かし方を一律から別の設定に変えたら、フォルマントのグラフが出ていなければ「両方」にする
    // （動かし方はフォルマントのグラフで見るため。「フォルマントの動く量」か「両方」なら、そのまま）。
    // 一律に戻したら「声の高さ」に戻す。開いたときの値は、変えたとはみなさない
    {
        using vcab::VoiceGraph;
        const auto p = processor.currentParams();
        const int warp = p.index (vcab::P::formantWarp);
        const auto graph = static_cast<VoiceGraph> (p.index (vcab::P::voiceGraph));
        std::optional<VoiceGraph> newGraph;
        if (lastFormantWarp == 0 && warp != 0 && graph != VoiceGraph::both && graph != VoiceGraph::formant)
            newGraph = VoiceGraph::both;
        if (lastFormantWarp > 0 && warp == 0)
            newGraph = VoiceGraph::pitch;
        if (newGraph)
            if (auto* param = processor.state().getParameter (vcab::spec (vcab::P::voiceGraph).key))
                param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (*newGraph)));
        lastFormantWarp = warp;
    }
    // 入力チャンネルが自動のときは、今どちらを使っているかを、選択欄の「自動」の項目の文字に出す
    // （選択欄の文字は、ほかの所から選び直されると元に戻ることがあるので、毎回確かめる）
    {
        const int used = processor.usedInputChannel.load() + 1;
        const auto text = utf8 ("自動（") + juce::String (used) + utf8 ("ch を使用中）");
        auto* combo = row (vcab::P::inputChannel).combo.get();
        if (used != shownInputChannel)
        {
            shownInputChannel = used;
            combo->changeItemText (1, text);
        }
        // 選択欄は、表示の文字が項目の文字と一致しないと「何も選んでいない」とみなすので、設定値で判断して選び直す
        if (processor.currentParams().index (vcab::P::inputChannel) == 0 && combo->getText() != text)
            combo->setSelectedId (1, juce::dontSendNotification);
    }

    if (processor.currentParams().index (vcab::P::monitorMode) != monitorMode)
        updateWindowSize();
    if (processor.currentParams().index (vcab::P::uiMode) != shownMode)
        rebuildTabs();

    updateVisibility();

    const double sr = processor.currentSampleRate.load();
    auto ms = [sr] (int samples) { return juce::String (samples * 1000.0 / sr, 1); };
    const int v = processor.latencyVoice.load(), d = processor.latencyDenoise.load(), l = processor.latencyOutput.load(), rp = processor.latencyRepair.load();
    // 実測: 設定を変えて 0.3 秒落ち着いたら、裏で測り直す（画面だけの設定は除く）
    {
        auto p = processor.effectiveParams();
        for (const auto& s : vcab::paramSpecs())
            if (vcab::isUiSetting (s.id))
                p.set (s.id, 0.0f);
        const auto now = juce::Time::getMillisecondCounter();
        if (p.v != latencyParams.v || sr != latencyRate)
        {
            latencyParams = p;
            latencyRate = sr;
            latencyChangedAt = now;
            latencyRequested = false;
        }
        if (! latencyRequested && now - latencyChangedAt > 300)
        {
            processor.latencyProbe.request (latencyParams, sr);
            latencyRequested = true;
        }
    }
    const float measured = processor.latencyProbe.result();
    juce::String measuredText;
    if (! latencyRequested || measured == LatencyProbe::kMeasuring)
        measuredText = utf8 ("実測中…");
    else if (measured == LatencyProbe::kFailed)
        measuredText = utf8 ("実測できません");
    else
        measuredText = utf8 ("実測 ") + juce::String (measured, 1) + " ms";

    latencyLabel.setText (utf8 ("遅延 ") + ms (v + d + l + rp) + utf8 (" ms（ボイチェン ") + ms (v) + utf8 (" / リペア ") + ms (rp) + utf8 (" / デノイズ ") + ms (d)
                              + utf8 (" / 出力 ") + ms (l) + utf8 ("）　") + measuredText,
                          juce::dontSendNotification);

    // 削っている量（下げるときはすぐ、戻るときはゆっくり表示）
    auto follow = [] (float& shown, float now) { shown = now < shown ? now : juce::jmin (0.0f, shown + 0.8f); };
    follow (deessGrShown, processor.meters().deessGrDb.load());
    follow (compGrShown, processor.meters().compGrDb.load());
    updateTabStatus();

    dynamics.update();
    if (pitchView.isVisible())
        pitchView.update();
    if (warpView.isVisible())
        warpView.update();
    if (monitor.isVisible())
        monitor.update();
    processor.meters().noiseCurveWanted.store (monitor.isVisible() && monitor.showsSpectrum());
}
