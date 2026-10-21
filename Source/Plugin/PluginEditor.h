#pragma once

#include "DynamicsView.h"
#include "LookAndFeel.h"
#include "FormantWarpView.h"
#include "MonitorView.h"
#include "PitchView.h"
#include "PluginProcessor.h"
#include "../Core/ParamRelevance.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>

// 1行分の操作（名前 + つまみ / スイッチ / 選択肢）
struct ParamRow
{
    vcab::P id;
    std::unique_ptr<juce::Label> label;
    std::unique_ptr<juce::Slider> slider;
    std::unique_ptr<juce::ToggleButton> toggle;
    std::unique_ptr<juce::ComboBox> combo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;
    std::unique_ptr<juce::ParameterAttachment> orderedAttachment; // 並べ替えた選択欄（保存する番号と並びが違う）

    juce::Component* control() const
    {
        if (slider) return slider.get();
        if (toggle) return toggle.get();
        return combo.get();
    }
    bool inlineToggle = false; // 名前をスイッチの横に書く行（名前の欄は使わない）
    juce::String baseTooltip;  // 選択欄: 全体の説明（選んでいる項目の説明は、その後ろに足す）
    int tooltipIndex = -1;     // 選択欄: ツールチップを、どの項目に合わせて作ったか
    void setVisible (bool v) const { label->setVisible (v && ! inlineToggle); control()->setVisible (v); }
    void setEnabled (bool e) const { label->setEnabled (e); control()->setEnabled (e); }
};

class VoiChanAlgoBoxEditor : public juce::AudioProcessorEditor,
                        private juce::Timer,
                        private juce::ChangeListener
{
public:
    explicit VoiChanAlgoBoxEditor (VoiChanAlgoBoxProcessor&);
    ~VoiChanAlgoBoxEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // 処理のタブ（処理の順番どおり）
    enum Tab { tabCut, tabNotch, tabRepair, tabDenoise, tabGate, tabVoice, tabBreath, tabEq, tabDeEsser, tabComp, tabFx, tabOutput, numTabs };

    // 枠で囲んだ組（名前の見出しと、必要なら見出しの行の右端にオン・オフ）
    struct Box
    {
        juce::Rectangle<int> bounds;
        juce::String title;
    };

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    ParamRow& addRow (vcab::P id);
    ParamRow& row (vcab::P id);
    std::vector<vcab::P> idsForTab (int tab) const;
    // 組を枠で囲んで並べる。headerToggle があれば見出しの行の右端に置く。使った高さを返す
    int layoutBox (juce::Rectangle<int> area, const char* title, std::optional<vcab::P> headerToggle, const std::vector<vcab::P>& ids);
    void layoutRows (juce::Rectangle<int> area, std::initializer_list<vcab::P> ids, int rowHeight = 24);
    void showTab (int tab);
    void rebuildTabs();                 // 表示のモードで見せるタブだけを並べ直す
    bool shown (vcab::P id) const { return vcab::paramLevel (id) <= shownMode; }
    void resetTab (int tab); // そのタブの値だけを初期値に戻す
    void updateVisibility();
    // タブごとの処理（EditorTabs.cpp）
    void layoutTab (juce::Rectangle<int> area);
    void layoutCutTab (juce::Rectangle<int> area);
    void layoutNotchTab (juce::Rectangle<int> area);
    void layoutRepairTab (juce::Rectangle<int> area);
    void layoutDenoiseTab (juce::Rectangle<int> area);
    void layoutGateTab (juce::Rectangle<int> area);
    void layoutVoiceTab (juce::Rectangle<int> area);
    void layoutBreathTab (juce::Rectangle<int> area);
    void layoutEqTab (juce::Rectangle<int> area);
    void layoutDeEsserTab (juce::Rectangle<int> area);
    void layoutCompTab (juce::Rectangle<int> area);
    void layoutFxTab (juce::Rectangle<int> area);
    void layoutOutputTab (juce::Rectangle<int> area);
    void updateTabVisibility (const vcab::ParamValues& p);
    void updateDenoiseExtras (const vcab::ParamValues& p);
    void updateVoiceExtras (const vcab::ParamValues& p);
    vcab::Relevance relevanceOf (vcab::P id, const vcab::ParamValues& p) const;   // その項目が今の設定で意味を持つか
    std::vector<vcab::Relevance> tabRelevance (const vcab::ParamValues& p) const; // 今のタブの項目（idsForTab の順）ごとの relevanceOf
    void updateTabStatus();
    void updateDeEsserStatus();
    void updateNotchStatus();
    void updateDenoiseStatus();
    void updateRepairStatus();
    void updateCompStatus();
    void updateWindowSize(); // モニターを非表示にしたときは、その分ウィンドウを縮める
    void updateChoiceTooltips(); // 選択欄のツールチップに、選んでいる項目の説明を足す

    VoiChanAlgoBoxProcessor& processor;
    VCLookAndFeel lookAndFeel;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow; // マウスを乗せて 0.4 秒で説明を出す

    std::vector<std::unique_ptr<ParamRow>> rows;

    // ボイチェンのタブ: ピッチとフォルマントは大きなつまみ
    juce::Slider pitchKnob, formantKnob;
    juce::Label pitchLabel, formantLabel, formantNote;
    juce::Label lpcFixLabel; // 「LPC の改善」の見出し（3つのスイッチを横に並べる行）
    juce::Label shiftLabel;  // 周波数シフトの、ずらす量の表示
    juce::Label pitchOffNote; // ボイチェンのタブで、グラフを出さないときの案内
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchAttachment, formantAttachment;

    // ディエッサーとコンプのタブの、削っている量の表示
    juce::Label deessMeterLabel, compMeterLabel;
    juce::Label notchStatusLabel; // ノッチのタブ: 追いかけている周波数と、ハムの電源の周波数
    juce::Label denoiseStatusLabel; // デノイズのタブ: 学習の状態など
    juce::Label denoiseAmountLabel; // デノイズのタブ: 削っている量（デノイズの枠の右上）
    juce::Label repairStatusLabel;  // リペアのタブ: 見つけた回数
    juce::TextButton learnButton;   // デノイズのタブ: ノイズの学習（2 秒）
    float deessGrShown = 0, compGrShown = 0;

    juce::Label latencyLabel;
    // 遅延の実測を頼んだときの設定（変わって 0.3 秒落ち着いたら、測り直しを頼む）
    vcab::ParamValues latencyParams;
    double latencyRate = 0;
    juce::uint32 latencyChangedAt = 0;
    bool latencyRequested = false;
    int shownInputChannel = -1;   // 入力チャンネルの「自動」の項目に出している、使っているチャンネル
    juce::TextButton resetButton;
    juce::TextButton audioSettingsButton; // EXE 版だけ: オーディオの設定画面を開く
    juce::TextButton resetTabButton; // タブごとの「初期値に戻す」

    juce::TabbedButtonBar tabBar { juce::TabbedButtonBar::TabsAtTop };
    // タブの横のオン・オフ（ボタン自体はタブが持つ。つなぎは、タブより先に消えるように後に宣言する）
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> tabToggleAttachments;
    int currentTab = tabVoice;
    std::vector<int> shownTabs;         // タブの並び（見せているタブの番号。タブの欄の位置 → Tab）
    int shownMode = 1;                  // 並べたときの表示のモード
    int monitorMode = -1;
    int layoutAlgorithm = -1; // ボイチェンのタブのつまみを、どの方式に合わせて並べたか
    int layoutGraphs = -1;    // ボイチェンのタブのグラフを、どの組み合わせで並べたか（1 声の高さ + 2 フォルマント）
    int lastFormantWarp = -1; // フォルマントの動かし方（一律から変えたら、グラフを「両方」にする）
    int voiceGraphs (const vcab::ParamValues& p) const;

    DynamicsView dynamics;
    MonitorView monitor;
    PitchView pitchView { processor };
    FormantWarpView warpView { processor };

    std::vector<vcab::Relevance> layoutRelevance; // 並べたときの、今のタブの項目の出し入れ（変わったら並べ直す）
    std::vector<Box> boxes;
    std::vector<std::unique_ptr<juce::Label>> boxTitleLabels; // 枠の見出しの名前（マウスを乗せると機能の説明を出す。文字は paint で描く）
    juce::Rectangle<int> headerArea, contentArea, sidePanel, deessMeterArea, compMeterArea;
};
