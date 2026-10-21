// エディタのタブごとの処理: 並べ方（layout…Tab）、方式や設定に合わせたつまみの出し入れ（update…Visibility）、状態の文字（update…Status）
#include "PluginEditor.h"

#include "EditorStyle.h"

#include "../Core/Algorithms/FrequencyShifter.h"
#include "../Core/Choices.h"
#include "../Core/Dsp/DenoiseSettings.h"
#include "../Core/ParamRelevance.h"

using namespace editor;

void VoiChanAlgoBoxEditor::layoutTab (juce::Rectangle<int> c)
{
    switch (currentTab)
    {
        case tabCut: layoutCutTab (c); break;
        case tabNotch: layoutNotchTab (c); break;
        case tabRepair: layoutRepairTab (c); break;
        case tabDenoise: layoutDenoiseTab (c); break;
        case tabGate: layoutGateTab (c); break;
        case tabVoice: layoutVoiceTab (c); break;
        case tabBreath: layoutBreathTab (c); break;
        case tabEq: layoutEqTab (c); break;
        case tabDeEsser: layoutDeEsserTab (c); break;
        case tabComp: layoutCompTab (c); break;
        case tabFx: layoutFxTab (c); break;
        case tabOutput: layoutOutputTab (c); break;
        default: break;
    }
}

// カット
void VoiChanAlgoBoxEditor::layoutCutTab (juce::Rectangle<int> c)
{
    using vcab::P;
    juce::Rectangle<int> left, right;
    twoColumns (c, left, right);
    layoutBox (left, "ローカット", P::lowCutOn, { P::lowCutFreq, P::lowCutSlope });
    layoutBox (right, "ハイカット", P::highCutOn, { P::highCutFreq, P::highCutSlope });
}

// ノッチ
void VoiChanAlgoBoxEditor::layoutNotchTab (juce::Rectangle<int> c)
{
    using vcab::P;
    juce::Rectangle<int> left, right;
    twoColumns (c, left, right);
    const int h = std::max (layoutBox (left, "自動", P::notchAutoOn, { P::notchCount, P::notchSensitivity }),
                            layoutBox (right, "ハム", P::notchHumOn, { P::humFreq, P::humHarmonics }));
    left.removeFromTop (h + kGap);
    right.removeFromTop (h + kGap);
    layoutBox (left, "共通", std::nullopt, { P::notchDepth });
    notchStatusLabel.setBounds (right.removeFromTop (90));
}

// リペア
void VoiChanAlgoBoxEditor::layoutRepairTab (juce::Rectangle<int> c)
{
    using vcab::P;
    // 一瞬だけの音の処理を、1 つずつ枠に並べる（見出しの横でオン・オフ）
    // 3 列: 左に音割れ、パチパチ、吹かれ。真ん中に突発音とリップノイズ。右に息継ぎと、見つけた回数
    // （処理の順番は 音割れ → パチパチ → 吹かれ → 突発音 → リップノイズ → 息継ぎ）
    auto left = c.removeFromLeft ((c.getWidth() - 2 * kGap) / 3);
    c.removeFromLeft (kGap);
    auto right = c.removeFromLeft ((c.getWidth() - kGap) / 2);
    c.removeFromLeft (kGap);
    auto third = c;
    const int h1 = layoutBox (left, "音割れの修復", P::repairDeclipOn, { P::repairClipLevel });
    left.removeFromTop (h1 + kGap);
    const int h2 = layoutBox (left, "パチパチ（細かいクリックの連続）", P::repairCrackleOn, { P::repairCrackleThreshold });
    left.removeFromTop (h2 + kGap);
    const int h5 = layoutBox (left, "吹かれ・破裂音", P::repairPlosiveOn, { P::repairPlosiveFreq, P::repairPlosiveThreshold, P::repairPlosiveDepth });
    juce::ignoreUnused (h5);
    const int h3 = layoutBox (right, "突発音の抑制（キーボード、クリック）", P::repairTransientOn,
                              { P::repairTransientMode, P::repairTransientThreshold, P::repairTransientDepth });
    right.removeFromTop (h3 + kGap);
    layoutBox (right, "リップノイズ", P::repairClickOn, { P::repairClickMode, P::repairClickThreshold, P::repairClickDepth });
    const int h6 = layoutBox (third, "息継ぎ", P::repairBreathOn, { P::repairBreathMargin, P::repairBreathDepth });
    third.removeFromTop (h6 + kGap);
    repairStatusLabel.setBounds (third.removeFromTop (80).withTrimmedLeft (4));
}

// デノイズ
void VoiChanAlgoBoxEditor::layoutDenoiseTab (juce::Rectangle<int> c)
{
    using vcab::P;
    // 2 列: 左にデノイズの枠（方式、削る量の上限、方式のつまみ、倍音を使う方式では声の高さ。右上に削っている量）、
    // 右にノイズの見積もりと残響、その下に状態。方式や決め方で使わないつまみは、layoutBox が詰める（ParamRelevance）
    juce::Rectangle<int> left, right;
    twoColumns (c, left, right);
    const std::vector<P> ids { P::denoiseMethod, P::denoiseMaxReduction,
                               P::denoiseFftSize, P::denoiseSmoothing,                                       // FFT の方式
                               P::denoiseAttackMs, P::denoiseReleaseMs, P::denoiseStrength,                  // 強さは帯域分割式も
                               // ここから下は詳細だけ（普通と詳細を切り替えても、上の行がずれないように）
                               P::denoiseSmoothWidth,                                                         // ならし方が周波数のとき
                               P::denoiseRule, P::denoiseThreshold, P::denoiseRatio,                        // 帯域分割式の下げる量の決め方
                               P::denoiseFftRule,                                                             // FFT の方式の下げる量の決め方
                               P::denoisePresence, P::denoiseComfort, P::denoiseOnset,
                               P::denoiseDfnModel, P::denoiseDfnPostFilter,                                  // DeepFilterNet
                               P::denoisePitchMethod, P::denoiseCombDepth, P::denoiseProtect };              // 倍音を使う方式
    layoutBox (left, "デノイズ", std::nullopt, ids);
    denoiseAmountLabel.setBounds (left.reduced (10, 5).removeFromTop (kBoxTitle - 2).withTrimmedLeft (120));

    // 右: ノイズの見積もり（見積もり方、学習のボタン、見積もり方ごとのつまみ、保存）、残響（見出しの横でオン・オフ）、その下に状態
    const std::vector<P> noiseIds { P::denoiseNoiseMode, P::denoiseMinWindow, P::denoiseLevelThreshold, P::denoiseSppSnr, P::denoiseRnnThreshold, P::denoiseSaveLearned };
    const int h = layoutBox (right, "ノイズ", std::nullopt, noiseIds);
    learnButton.setBounds ({});
    if (h > 0)
    {
        // 枠の中、見積もり方の下に学習のボタンを差し込む（枠を1行分のばし、その下の行を1行下げる）
        boxes.back().bounds.setHeight (h + kRowHeight);
        auto b = right.reduced (8, 0).withTrimmedTop (kBoxTitle + kRowHeight + 5).withHeight (kRowHeight);
        learnButton.setBounds (b.reduced (0, 2));
        for (size_t i = 1; i < noiseIds.size(); ++i)
        {
            const auto id = noiseIds[i];
            auto& r = row (id);
            r.label->setBounds (r.label->getBounds().translated (0, kRowHeight));
            r.control()->setBounds (r.control()->getBounds().translated (0, kRowHeight));
        }
        right.removeFromTop (h + kRowHeight + kGap);
    }
    below (right, layoutBox (right, "残響の抑制", P::denoiseDereverbOn, { P::denoiseRt60 }));
    denoiseStatusLabel.setBounds (right.removeFromTop (100).withTrimmedLeft (4));
}

// ゲート
void VoiChanAlgoBoxEditor::layoutGateTab (juce::Rectangle<int> c)
{
    using vcab::P;
    juce::Rectangle<int> left, right;
    twoColumns (c, left, right);
    // 比率とニーは、動き方を切り替えても並びが動かないように、いつも並べる（ゲートのときは灰色）
    layoutBox (left, "ゲート", std::nullopt,
               { P::gateMode, P::gateThreshold, P::gateRange, P::gateRatio, P::gateKnee,
                 P::gateAttackMs, P::gateHoldMs, P::gateReleaseMs, P::gateHysteresis, P::gateFromDenoise });
}

// ボイチェン
void VoiChanAlgoBoxEditor::layoutVoiceTab (juce::Rectangle<int> c)
{
    using vcab::P;
    // 左側に、方式 → ピッチとフォルマントの大きなつまみ → 方式ごとのつまみ
    juce::Rectangle<int> left, right;
    twoColumns (c, left, right);
    layoutAlgorithm = processor.currentParams().index (P::algorithm);
    const auto& algo = vcab::algorithmInfo (layoutAlgorithm);
    {
        auto line = left.removeFromTop (32);
        auto& algoRow = row (P::algorithm);
        algoRow.label->setBounds (line.removeFromLeft (150));
        algoRow.combo->setBounds (line.reduced (0, 1));
    }
    left.removeFromTop (8);
    auto knobs = left.removeFromTop (150);
    auto kl = knobs.removeFromLeft (knobs.getWidth() / 2);
    pitchLabel.setBounds (kl.removeFromTop (24));
    pitchKnob.setBounds (kl);
    formantLabel.setBounds (knobs.removeFromTop (24));
    formantKnob.setBounds (knobs);
    left.removeFromTop (8); // つまみの値の欄と、下の行の間をあける
    {
        // つまみの下の行: フォルマントの側に動かし方（フォルマントを分けられない方式では、代わりに案内を全幅で出す）
        auto under = left.removeFromTop (kRowHeight);
        // ピッチ検出を使う方式では、ピッチの側に抑揚の強調があるので、フォルマントの側の半分にだけ出す
        formantNote.setBounds (algo.usesPitchDetection() ? under.withTrimmedLeft (under.getWidth() / 2) : under);
        auto formantSide = under.withTrimmedLeft (under.getWidth() / 2);
        auto& warpRow = row (P::formantWarp);
        warpRow.label->setBounds (formantSide.removeFromLeft (80));
        warpRow.combo->setBounds (formantSide.reduced (0, 2));
        // ピッチの側に抑揚の強調（ピッチ検出を使う方式だけ）
        auto pitchSide = under.withWidth (under.getWidth() / 2).withTrimmedRight (12);
        auto& intoRow = row (P::intonation);
        intoRow.label->setBounds (pitchSide.removeFromLeft (50));
        intoRow.slider->setBounds (pitchSide.reduced (0, 2));
    }
    left.removeFromTop (8);
    // 方式ごとのつまみ。見えないものは updateVisibility で隠すので、方式ごとに同じ場所へ並べる
    // 方式によって並びが変わるところは、今の方式に合わせて置く（方式を変えたら updateVisibility が置き直す）
    layoutRows (left, { P::cvBands }, kRowHeight); // チャンネルボコーダ
    // ボコーダの混合励振と声帯の波形: ほかのつまみの下に置く（チャンネルボコーダは帯域の数の下、LPC ボコーダは改善のスイッチの下）
    // LPC の方式は、次数と改善のスイッチの 2 行が上にある
    const int lpcRows = algo.envelope == vcab::Envelope::lpc ? 2 : 0;
    layoutRows (left.withTrimmedTop ((algo.core == vcab::VoiceCore::channelVocoder ? 1 : lpcRows) * kRowHeight),
                { P::mixedExcitation, P::glottalShape }, kRowHeight);
    // TD-PSOLA の印付けの方式（LPC＋PSOLA は改善のスイッチの下）
    layoutRows (left.withTrimmedTop (lpcRows * kRowHeight), { P::psolaMarking }, kRowHeight);
    layoutRows (left, { P::fsReferenceHz }, kRowHeight); // 周波数シフト
    layoutRows (left, { P::bsBands, P::bsFormantMethod }, kRowHeight); // 帯域ごとの周波数シフト
    layoutRows (left, { P::worldBreath }, kRowHeight); // WORLD
    shiftLabel.setBounds (left.withHeight (kRowHeight).translated (0, kRowHeight).withTrimmedLeft (150));
    {
        auto lpcArea = left;
        if (algo.core != vcab::VoiceCore::delayLine)
            layoutRows (lpcArea.removeFromTop (kRowHeight), { P::lpcOrder }, kRowHeight);   // LPC＋PSOLA、LPC ボコーダ
        else
            layoutRows (lpcArea.removeFromTop (2 * kRowHeight), { P::dlWindowMs, P::lpcOrder }, kRowHeight); // LPC＋ディレイラインは、ディレイライン式の窓の下に次数

        // LPC の改善のスイッチ: 見出しの後に、3つを横に並べる
        auto line = lpcArea.removeFromTop (kRowHeight);
        lpcFixLabel.setBounds (line.removeFromLeft (150));
        const int w = line.getWidth() / 3;
        for (auto id : { P::lpcBandwidth, P::lpcInterpolation, P::lpcEmphasis })
        {
            row (id).label->setBounds ({});
            row (id).toggle->setBounds (line.removeFromLeft (w).reduced (0, 2));
        }
    }
    layoutRows (left, { P::wsolaWindowMs, P::wsolaSearchMs }, kRowHeight);
    layoutRows (left, { P::pvFftSize, P::pvPhaseLock, P::formantMethod }, kRowHeight);
    layoutRows (left, { P::pvFftSize, P::smNoiseOn, P::formantMethod }, kRowHeight); // 正弦波モデル: 位相ロックの場所に雑音成分

    // 右側に、ピッチ検出の設定と、グラフの切り替え、その下にグラフ。
    // 左に入りきらない方式ごとのつまみも、右の上に置く
    // 右側の行とグラフは、方式や表示のモードで項目が隠れても詰めずに、いつも同じ場所に置く
    {
        auto place = [&] (P id, int slot)
        {
            auto line = right.withTrimmedTop (slot * kRowHeight).withHeight (kRowHeight);
            auto& r = row (id);
            r.label->setBounds (line.removeFromLeft (150));
            r.control()->setBounds (line.reduced (0, 2));
        };
        place (P::smMaxSines, 0);
        place (P::worldF0Method, 0); // WORLD: ピッチ検出の方式の場所に
        place (P::pitchMethod, 0);
        place (P::pitchMinHz, 1);
        place (P::pitchMaxHz, 2);
        place (P::voiceGraph, 3);
        right.removeFromTop (4 * kRowHeight);
    }
    right.removeFromTop (8);
    right.removeFromBottom (48); // 下は「このタブを初期値に戻す」のボタン
    // 声の高さのグラフ（左）と、フォルマントの動く量のグラフ（右）を半分ずつ並べる。片方だけのときは、全幅で描く
    layoutGraphs = voiceGraphs (processor.currentParams());
    pitchOffNote.setBounds (right.withHeight (60)); // 2行に折り返す
    if (layoutGraphs == 3)
    {
        const int w = (right.getWidth() - 8) / 2;
        pitchView.setBounds (right.removeFromLeft (w));
        right.removeFromLeft (8);
        warpView.setBounds (right);
    }
    else
    {
        pitchView.setBounds (right);
        warpView.setBounds (right);
    }
}

// ブレス
void VoiChanAlgoBoxEditor::layoutBreathTab (juce::Rectangle<int> c)
{
    using vcab::P;
    juce::Rectangle<int> left, right;
    twoColumns (c, left, right);
    layoutBox (left, "ブレス", std::nullopt, { P::breathAmount, P::breathToneHz });
}

// EQ
void VoiChanAlgoBoxEditor::layoutEqTab (juce::Rectangle<int> c)
{
    using vcab::P;
    // 上の段: カットのタブと同じ並びで、ローカットとハイカット
    juce::Rectangle<int> left, right;
    twoColumns (c, left, right);
    const int h = layoutBox (left, "ローカット", P::eqLowCutOn, { P::eqLowCutFreq, P::eqLowCutSlope });
    layoutBox (right, "ハイカット", P::eqHighCutOn, { P::eqHighCutFreq, P::eqHighCutSlope });

    // 下の段: 4つの帯域を低い順に横に並べる
    auto bottom = c.withTrimmedTop (h > 0 ? h + kGap : 0);
    const int w = (bottom.getWidth() - kGap * 3) / 4;
    auto col = [&] { auto a = bottom.removeFromLeft (w); bottom.removeFromLeft (kGap); return a; };
    layoutBox (col(), "低域シェルフ", P::eqLowShelfOn, { P::eqLowShelfFreq, P::eqLowShelfGain, P::eqLowShelfQ });
    layoutBox (col(), "中域ピーク1", P::eqPeak1On, { P::eqPeak1Freq, P::eqPeak1Gain, P::eqPeak1Q });
    layoutBox (col(), "中域ピーク2", P::eqPeak2On, { P::eqPeak2Freq, P::eqPeak2Gain, P::eqPeak2Q });
    layoutBox (col(), "高域シェルフ", P::eqHighShelfOn, { P::eqHighShelfFreq, P::eqHighShelfGain, P::eqHighShelfQ });
}

// ディエッサー
void VoiChanAlgoBoxEditor::layoutDeEsserTab (juce::Rectangle<int> c)
{
    using vcab::P;
    juce::Rectangle<int> left, right;
    twoColumns (c, left, right);
    const int h = layoutBox (left, "ディエッサー", std::nullopt, { P::deessFreq, P::deessThreshold, P::deessMaxReduction });
    left.removeFromTop (h + kGap);
    deessMeterLabel.setBounds (left.removeFromTop (26));
    deessMeterArea = left.removeFromTop (14);
}

// コンプ
void VoiChanAlgoBoxEditor::layoutCompTab (juce::Rectangle<int> c)
{
    using vcab::P;
    juce::Rectangle<int> left, right;
    twoColumns (c, left, right);
    const int h = layoutBox (left, "コンプ", std::nullopt, { P::compThreshold, P::compRatio, P::compAttackMs, P::compReleaseMs, P::compKnee });
    left.removeFromTop (h + kGap);
    compMeterLabel.setBounds (left.removeFromTop (26));
    compMeterArea = left.removeFromTop (14);
}

// リバーブ
void VoiChanAlgoBoxEditor::layoutFxTab (juce::Rectangle<int> c)
{
    using vcab::P;
    // 左にリバーブ、右にエコー（処理の順番はエコー → リバーブのまま）
    juce::Rectangle<int> left, right;
    twoColumns (c, left, right);
    layoutBox (left, "リバーブ", P::reverbOn, { P::reverbSize, P::reverbDamping, P::reverbPreDelayMs, P::reverbMix });
    layoutBox (right, "エコー", P::echoOn, { P::echoTimeMs, P::echoFeedback, P::echoDampingHz, P::echoMix });
}

// 出力
void VoiChanAlgoBoxEditor::layoutOutputTab (juce::Rectangle<int> c)
{
    using vcab::P;
    juce::Rectangle<int> left, right;
    twoColumns (c, left, right);
    const int h = layoutBox (left, "出力ゲイン", std::nullopt, { P::outputGain });
    below (left, h);
    layoutBox (left, "リミッター", std::nullopt, { P::limiterCeiling, P::limiterReleaseMs });
}

void VoiChanAlgoBoxEditor::updateTabVisibility (const vcab::ParamValues& p)
{
    // タブの項目は、今の設定で意味を持つものだけ出し、効かないものは灰色にする（ParamRelevance）
    const auto now = tabRelevance (p);
    const auto ids = idsForTab (currentTab);
    for (size_t i = 0; i < ids.size(); ++i)
    {
        row (ids[i]).setVisible (now[i] != vcab::Relevance::hidden);
        row (ids[i]).setEnabled (now[i] == vcab::Relevance::active);
    }
    // 出す項目の組み合わせ（ボイチェンのタブでは方式も。方式によって並びが変わる）が変わったら、並べ直す
    if (now != layoutRelevance || (currentTab == tabVoice && p.index (vcab::P::algorithm) != layoutAlgorithm))
        resized();

    if (currentTab == tabDenoise)
        updateDenoiseExtras (p);
    if (currentTab == tabVoice)
        updateVoiceExtras (p);
}

// デノイズのタブの、項目以外の部品（学習のボタン）
void VoiChanAlgoBoxEditor::updateDenoiseExtras (const vcab::ParamValues& p)
{
    const auto ds = vcab::DenoiseSettings::fromParams (p);
    const bool learns = vcab::usesLearning (ds.noiseMode);
    learnButton.setEnabled (! ds.estimatesNoiseItself() && learns && p.on (vcab::P::denoiseOn));
}

// ボイチェンのタブの、項目以外の部品（LPC の改善の見出し、ずらす量、フォルマントの大きなつまみと案内、グラフ）
void VoiChanAlgoBoxEditor::updateVoiceExtras (const vcab::ParamValues& p)
{
    using vcab::P;
    using vcab::Relevance;
    lpcFixLabel.setVisible (relevanceOf (P::lpcBandwidth, p) != Relevance::hidden);
    const bool shift = relevanceOf (P::fsReferenceHz, p) != Relevance::hidden;
    shiftLabel.setVisible (shift);
    if (shift)
    {
        const int hz = juce::roundToInt (vcab::FrequencyShifter::shiftHz (p));
        shiftLabel.setText (utf8 ("ずらす量: ") + (hz > 0 ? "+" : "") + juce::String (hz) + " Hz", juce::dontSendNotification);
    }

    // フォルマントを分けられない方式では、フォルマントのつまみを無効表示にして、案内を出す
    const bool formantOk = relevanceOf (P::formant, p) == Relevance::active;
    formantKnob.setEnabled (formantOk);
    formantLabel.setEnabled (formantOk);
    formantNote.setVisible (! formantOk);

    // グラフ（選んだもののうち、今の方式で出せるもの）
    const int graphs = voiceGraphs (p);
    if (graphs != layoutGraphs)
        resized();
    pitchView.setVisible ((graphs & 1) != 0);
    warpView.setVisible ((graphs & 2) != 0);
    const bool canChooseGraph = relevanceOf (P::voiceGraph, p) != Relevance::hidden;
    pitchOffNote.setVisible (graphs == 0 && shown (P::voiceGraph) && canChooseGraph); // 選んで隠したときだけ案内を出す
    if (graphs == 0)
        pitchOffNote.setText (p.index (P::voiceGraph) == static_cast<int> (vcab::VoiceGraph::hidden) ? utf8 ("グラフは非表示です（上の「グラフ」で切り替え）")
                                                           : utf8 ("この方式では、選んだグラフは出せません（上の「グラフ」で切り替え）"),
                              juce::dontSendNotification);
}

// ボイチェンのタブに出すグラフ（1 声の高さ + 2 フォルマントの動く量）。選んだもののうち、今の方式で出せるものだけ
int VoiChanAlgoBoxEditor::voiceGraphs (const vcab::ParamValues& p) const
{
    using vcab::VoiceGraph;
    if (shownMode == static_cast<int> (vcab::UiMode::easy))
        return 0; // 簡単のモードでは、グラフを出さない
    const bool pitchOk = vcab::algorithmInfo (p.index (vcab::P::algorithm)).usesPitchDetection();
    const bool warpOk = processor.supportsFormant (p);
    // 出せるグラフが 1 つだけの方式は、選択に関係なくそのグラフを出す（選択欄も出さない）。
    // ただしフォルマントのグラフは、動かし方を出すモード（詳細）だけ（普通では見る意味がない）
    if (pitchOk != warpOk)
        return pitchOk ? 1 : (shown (vcab::P::formantWarp) ? 2 : 0);
    if (! pitchOk)
        return 0;
    // 両方出せる方式: 選択欄が隠れる普通では、声の高さ（動かし方が詳細だけなので、フォルマントのグラフは要らない）
    const auto mode = shown (vcab::P::voiceGraph) ? static_cast<VoiceGraph> (p.index (vcab::P::voiceGraph)) : VoiceGraph::pitch;
    return (mode == VoiceGraph::pitch || mode == VoiceGraph::both ? 1 : 0) + (mode == VoiceGraph::formant || mode == VoiceGraph::both ? 2 : 0);
}

void VoiChanAlgoBoxEditor::updateTabStatus()
{
    switch (currentTab)
    {
        case tabDeEsser: updateDeEsserStatus(); break;
        case tabNotch: updateNotchStatus(); break;
        case tabDenoise: updateDenoiseStatus(); break;
        case tabRepair: updateRepairStatus(); break;
        case tabComp: updateCompStatus(); break;
        default: break;
    }
}

void VoiChanAlgoBoxEditor::updateDeEsserStatus()
{
    deessMeterLabel.setText (utf8 ("削っている量: ") + juce::String (std::abs (deessGrShown), 1) + " dB", juce::dontSendNotification);
    repaint (deessMeterArea);
}

void VoiChanAlgoBoxEditor::updateNotchStatus()
{
    auto& m = processor.meters();
    const auto pp = processor.currentParams();
    juce::String text;
    if (! pp.on (vcab::P::notchOn))
        text = utf8 ("ノッチはオフです");
    else
    {
        if (pp.on (vcab::P::notchAutoOn))
        {
            const int count = m.notchCount.load();
            text << utf8 ("自動で削っている周波数: ");
            if (count == 0)
                text << utf8 ("なし");
            for (int i = 0; i < count; ++i)
                text << (i > 0 ? utf8 ("、") : juce::String()) << juce::String (juce::roundToInt (m.notchHz[i].load())) << " Hz";
        }
        if (pp.on (vcab::P::notchHumOn))
        {
            const float mains = m.humMainsHz.load();
            text << (text.isEmpty() ? juce::String() : juce::String ("\n")) << utf8 ("ハム: ") << juce::String (juce::roundToInt (mains)) << " Hz"
                 << (pp.index (vcab::P::humFreq) == 0 ? utf8 ("（自動で判定）") : juce::String()) << utf8 ("とその倍音");
        }
    }
    notchStatusLabel.setText (text, juce::dontSendNotification);
}

void VoiChanAlgoBoxEditor::updateDenoiseStatus()
{
    auto& m = processor.meters();
    const auto pp = processor.currentParams();
    denoiseAmountLabel.setText (pp.on (vcab::P::denoiseOn) ? utf8 ("削っている量: ") + juce::String (m.denoiseReductionDb.load(), 1) + " dB"
                                                           : utf8 ("デノイズはオフです"),
                                juce::dontSendNotification);
    juce::String text;
    const float progress = m.denoiseLearnProgress.load();
    if (progress >= 0)
        text << juce::String ("\n") << utf8 ("学習中… ") << juce::String (juce::roundToInt (progress * 100)) << utf8 ("%（黙っていてください）");
    else if (pp.index (vcab::P::denoiseNoiseMode) == static_cast<int> (vcab::NoiseMode::learned))
        text << juce::String ("\n") << (m.denoiseLearned.load() ? utf8 ("学習した値で下げています") : utf8 ("まだ学習していません（自動（最小値）で下げています）"));
    else if (vcab::startsFromLearned (static_cast<vcab::NoiseMode> (pp.index (vcab::P::denoiseNoiseMode))))
        text << juce::String ("\n") << (m.denoiseLearned.load() ? utf8 ("学習した値から始めて、自動で追っています") : utf8 ("まだ学習していません（自動で追っています）"));
    if (pp.on (vcab::P::denoiseOn) && pp.on (vcab::P::gateFromDenoise))
        text << juce::String ("\n") << utf8 ("声のありそうさ: ") << juce::String (juce::roundToInt (m.denoiseVoiceProbability.load() * 100)) << "%";
    if (pp.on (vcab::P::denoiseOn) && vcab::DenoiseSettings::fromParams (pp).needsPitch())
    {
        const float hz = m.denoisePitchHz.load();
        text << juce::String ("\n") << utf8 ("検出した声の高さ: ") << (hz > 0 ? juce::String (juce::roundToInt (hz)) + " Hz" : utf8 ("無声（倍音の処理はかけていません）"));
    }
    denoiseStatusLabel.setText (text.trimStart(), juce::dontSendNotification);
}

void VoiChanAlgoBoxEditor::updateRepairStatus()
{
    const auto pp = processor.currentParams();
    juce::String text;
    if (! pp.on (vcab::P::repairOn))
        text = utf8 ("リペアはオフです");
    else
    {
        auto& m = processor.meters();
        text = utf8 ("見つけて直した回数") + juce::String ("\n") + utf8 ("音割れ ") + juce::String (m.repairClips.load())
             + utf8 (" / 吹かれ ") + juce::String (m.repairPlosives.load())
             + utf8 (" / 突発音 ") + juce::String (m.repairTransients.load())
             + utf8 (" / リップノイズ ") + juce::String (m.repairClicks.load())
             + utf8 (" / パチパチ ") + juce::String (m.repairCrackles.load())
             + utf8 (" / 息継ぎ ") + juce::String (m.repairBreaths.load());
    }
    repairStatusLabel.setText (text, juce::dontSendNotification);
}

void VoiChanAlgoBoxEditor::updateCompStatus()
{
    compMeterLabel.setText (utf8 ("下げている量: ") + juce::String (std::abs (compGrShown), 1) + " dB", juce::dontSendNotification);
    repaint (compMeterArea);
}
