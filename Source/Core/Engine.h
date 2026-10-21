// 処理の全体。プラグインと WAV 処理ツールの両方がこれを使う。JUCE には依存しない。
//
// 処理の順番: カット → ノッチ → リペア → デノイズ → ゲート → ボイチェン → ブレス → EQ → ディエッサー → コンプ → リバーブ（エコーも） → 出力（出力ゲイン → リミッター）
#pragma once

#include "Algorithms/VoiceAlgorithm.h"
#include "Dsp/Breath.h"
#include "Dsp/Compressor.h"
#include "Dsp/CutFilter.h"
#include "Dsp/DeEsser.h"
#include "Dsp/Denoise.h"
#include "Dsp/ClickRepair.h"
#include "Dsp/FormantTracker.h"
#include "Dsp/Repair.h"
#include "Dsp/Transient.h"
#include "Dsp/Echo.h"
#include "Dsp/Equalizer.h"
#include "Dsp/Gate.h"
#include "Dsp/Limiter.h"
#include "Dsp/Notch.h"
#include "Dsp/Reverb.h"
#include "Params.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <memory>
#include <vector>

namespace vcab
{

// 遅延の内訳（サンプル数）
struct LatencyBreakdown
{
    int repair = 0;
    int denoise = 0;
    int voice = 0;
    int output = 0;  // 出力の段（リミッターの先読み）

    int total() const noexcept { return repair + denoise + voice + output; }
};

// 画面に出す値。オーディオ処理のスレッドが書き、画面のスレッドが読む。
struct EngineMeters
{
    std::atomic<float> inputPeakDb { -120 };   // マイクの生の音
    std::atomic<float> outputPeakDb { -120 };  // 最終出力

    std::atomic<float> gateLevelDb { -120 };   // ゲートが判断に使っている音量（200Hz〜4kHz）
    std::atomic<float> gateGainDb { 0 };       // ゲートが今下げている量
    std::atomic<bool> gateOpen { false };

    std::atomic<float> deessGrDb { 0 };        // ディエッサーが削っている量

    // ノッチ: 今かけている自動のノッチの周波数と、ハムの電源の周波数（オフなら 0）
    std::atomic<int> notchCount { 0 };
    std::atomic<float> notchHz[AdaptiveNotch::kMaxAuto] {};
    std::atomic<float> humMainsHz { 0 };

    // デノイズ: 削っている量（dB、正の値）、学習の進み具合（0〜1。学習していないときは -1）、今の方式で覚えた値があるか
    std::atomic<float> denoiseReductionDb { 0 };
    std::atomic<float> denoiseLearnProgress { -1 };
    std::atomic<bool> denoiseLearned { false };
    std::atomic<float> denoisePitchHz { 0 }; // 倍音を意識する方式で検出している声の高さ（無声音やほかの方式では 0）
    std::atomic<float> denoiseVoiceProbability { 0 }; // デノイズが判断した声のありそうさ（0〜1）
    std::atomic<int> repairTransients { 0 };          // リペア: 突発音を見つけた回数
    std::atomic<int> repairClips { 0 }, repairPlosives { 0 }, repairClicks { 0 }, repairCrackles { 0 }; // リペア: 音割れ、吹かれ、リップノイズ、パチパチの回数
    std::atomic<int> repairBreaths { 0 };             // リペア: 息継ぎを下げた回数
    // フォルマントの追いかけ（動かし方が「フォルマントごと」のときだけ formantTracking が true）
    std::atomic<bool> formantTracking { false }, formantValid { false };
    std::atomic<float> formantHz[3] { 500.0f, 1400.0f, 2700.0f };
    // ノイズの見積もりの表示（kNoiseCurvePoints 個の周波数での、白色雑音に直したときの 1 サンプルあたりの 2 乗、dB）
    static constexpr int kNoiseCurvePoints = 64;
    static float noiseCurveHz (int i) noexcept { return 40.0f * std::pow (500.0f, static_cast<float> (i) / (kNoiseCurvePoints - 1)); } // 40Hz〜20kHz
    std::atomic<float> noiseCurveDb[kNoiseCurvePoints] {};
    std::atomic<bool> noiseCurveValid { false };
    std::atomic<bool> noiseCurveWanted { false }; // 画面がスペクトルを出しているときだけ計算する（画面の側が立てる）

    std::atomic<float> compLevelDb { -120 };   // コンプに入る音量（判断用）
    std::atomic<float> compGrDb { 0 };         // コンプが下げている量

    // ピッチ検出（使う方式のときだけ pitchActive が true）
    std::atomic<bool> pitchActive { false };
    std::atomic<float> pitchHz { 0 };
    std::atomic<bool> pitchVoiced { false };
};

// 段がオンになった瞬間（と、オンのまま作り直したい設定が変わった瞬間）を知らせる。
// そのときに部品の状態を消してから使う（前にオンだったときの残りが出ないように）
class StageSwitch
{
public:
    // on: 今オンか。key: 変わったら作り直したい設定（方式など）
    bool startsNow (bool on, int key = 0) noexcept
    {
        const bool start = on && (! wasOn || key != lastKey);
        wasOn = on;
        lastKey = key;
        return start;
    }
    void forget() noexcept { wasOn = false; } // 次にオンのときは、始まったとみなす

private:
    bool wasOn = false;
    int lastKey = 0;
};

class Engine
{
public:
    Engine();

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    // モノラルの音をその場で書き換える
    void process (float* data, int n, const ParamValues& params);

    LatencyBreakdown latency (const ParamValues& params) const;
    bool algorithmSupportsFormant (const ParamValues& params) const;

    // デノイズのノイズの学習（2 秒）を始める。画面のスレッドから呼んでよい（次のブロックで始まる）
    void requestNoiseLearning() noexcept { learnRequested.store (true); }
    // 学習した値の書き出し（保存のため。画面のスレッドから呼ぶ）と、読み込み（次の prepare か process で反映する）
    std::vector<float> exportLearnedNoise() const { return denoiser.exportLearned(); }
    void importLearnedNoise (std::vector<float> data);

    // リアルタイムで動いているか（初期値は true。WAV 処理ツールやホストの書き出しでは false にする）
    void setRealtime (bool isRealtime);

    int numAlgorithms() const noexcept { return static_cast<int> (algorithms.size()); }
    double sampleRate() const noexcept { return sr; }

    EngineMeters meters;

private:
    VoiceAlgorithm& algorithmFor (const ParamValues& p) const;
    // 処理の段（process から順番に呼ぶ）
    void processCut (float* data, int n, const ParamValues& p);
    void processNotch (float* data, int n, const ParamValues& p);
    void processRepair (float* data, int n, const ParamValues& p);
    void processDenoise (float* data, int n, const ParamValues& p);
    void processGate (float* data, int n, const ParamValues& p);
    void processVoice (float* data, int n, const ParamValues& p);
    void processBreath (float* data, int n, const ParamValues& p);
    void processEq (float* data, int n, const ParamValues& p);
    void processDeEsser (float* data, int n, const ParamValues& p);
    void processCompressor (float* data, int n, const ParamValues& p);
    void processEffects (float* data, int n, const ParamValues& p);
    void processOutput (float* data, int n, const ParamValues& p);

    double sr = 48000;

    CutFilter lowCut { CutFilter::Kind::lowCut };
    CutFilter highCut { CutFilter::Kind::highCut };
    AdaptiveNotch notch;
    // リペア（順番: 音割れ → パチパチ → 吹かれ → 突発音 → リップノイズ）。突発音とリップノイズは、直し方（下げる / 補間）で使う部品が違う
    Declipper declipper;
    Decrackler decrackler;
    PlosiveSuppressor plosive;
    TransientSuppressor transient;
    ClickInterpolator transientInterp;
    MouthClickSuppressor mouthClick;
    ClickInterpolator mouthClickInterp;
    BreathSuppressor breathRepair;   // リペアの息継ぎ（ブレスの breath とは別）
    StageSwitch declipStage, crackleStage, plosiveStage, transientStage, clickStage, breathRepairStage;
    Denoiser denoiser;
    StageSwitch denoiseStage;
    std::atomic<bool> learnRequested { false };
    std::mutex pendingLearnedMutex;
    std::vector<float> pendingLearned;
    std::atomic<bool> pendingLearnedReady { false };
    int noiseCurveCounter = 0;
    void applyPendingLearned();
    Gate gate;

    std::vector<std::unique_ptr<VoiceAlgorithm>> algorithms;
    FormantTracker formantTracker;   // 動かし方が「フォルマントごと」のときに、ボイチェンに入る声から F1〜F3 を求める
    StageSwitch voiceStage, formantTrackingStage; // voiceStage の key は方式の番号

    Breath breath;
    StageSwitch breathStage;
    Equalizer eq;
    DeEsser deEsser;
    Compressor compressor;
    Echo echo;
    Reverb reverb;
    StageSwitch echoStage, reverbStage;
    Limiter limiter;
};

} // namespace vcab
