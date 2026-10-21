// パラメータの定義。プラグイン（JUCE の APVTS）と WAV 処理ツールの両方がこの表を使う。
// JUCE には依存しない。
#pragma once

#include <array>
#include <string>
#include <vector>

namespace vcab
{

// パラメータの番号（処理の順番。ホストのオートメーションの一覧もこの順番）。Params.cpp の表もこの順番に並べる。
enum class P : int
{
    // カット（ボイチェンの前のローカット・ハイカット）
    cutOn,
    lowCutOn,
    lowCutFreq,
    lowCutSlope,
    highCutOn,
    highCutFreq,
    highCutSlope,

    // ノッチ（細く高い持続音とハムを削る）
    notchOn,
    notchAutoOn,
    notchCount,
    notchSensitivity,
    notchHumOn,
    humFreq,
    humHarmonics,
    notchDepth,

    // リペア（一瞬だけの音を直す。音割れ → パチパチ → 吹かれ → 突発音 → リップノイズ → 息継ぎ）
    repairOn,
    repairDeclipOn,
    repairClipLevel,
    repairCrackleOn,
    repairCrackleThreshold,
    repairPlosiveOn,
    repairPlosiveFreq,
    repairPlosiveThreshold,
    repairPlosiveDepth,
    repairTransientOn,
    repairTransientMode,
    repairTransientThreshold,
    repairTransientDepth,
    repairClickOn,
    repairClickMode,
    repairClickThreshold,
    repairClickDepth,
    repairBreathOn,
    repairBreathMargin,
    repairBreathDepth,

    // デノイズ（持続するノイズを下げる）
    denoiseOn,
    denoiseMethod,
    denoiseMaxReduction,
    denoiseRule,
    denoiseThreshold,
    denoiseRatio,
    denoiseFftRule,
    denoiseFftSize,
    denoiseStrength,
    denoiseSmoothing,
    denoiseSmoothWidth,
    denoiseAttackMs,
    denoiseReleaseMs,
    denoisePresence,
    denoiseComfort,
    denoiseOnset,
    denoiseDfnModel,
    denoiseDfnPostFilter,
    denoisePitchMethod,
    denoiseCombDepth,
    denoiseProtect,
    denoiseNoiseMode,
    denoiseLevelThreshold,
    denoiseRnnThreshold,
    denoiseMinWindow,
    denoiseSppSnr,
    denoiseSaveLearned,
    denoiseDereverbOn,
    denoiseRt60,

    // ゲート
    gateOn,
    gateMode,
    gateThreshold,
    gateRange,
    gateRatio,
    gateKnee,
    gateAttackMs,
    gateHoldMs,
    gateReleaseMs,
    gateHysteresis,
    gateFromDenoise,

    // ボイチェン
    voiceOn,
    algorithm,
    pitch,
    formant,
    formantWarp,
    formantF1,
    formantF2,
    formantF3,
    intonation,
    pitchMethod,
    pitchMinHz,
    pitchMaxHz,
    dlWindowMs,
    wsolaWindowMs,
    wsolaSearchMs,
    psolaMarking,
    pvFftSize,
    pvPhaseLock,
    formantMethod,
    smMaxSines,
    smNoiseOn,
    fsReferenceHz,
    bsBands,
    bsFormantMethod,
    lpcOrder,
    lpcBandwidth,
    lpcInterpolation,
    lpcEmphasis,
    cvBands,
    mixedExcitation,
    glottalShape,
    worldF0Method,
    worldBreath,

    // ブレス（ボイチェンの後に息の音を足す）
    breathOn,
    breathAmount,
    breathToneHz,

    // EQ（ローカット・ハイカットと 4 つの帯域）
    eqOn,
    eqLowCutOn,
    eqLowCutFreq,
    eqLowCutSlope,
    eqHighCutOn,
    eqHighCutFreq,
    eqHighCutSlope,
    eqLowShelfOn,
    eqLowShelfFreq,
    eqLowShelfGain,
    eqLowShelfQ,
    eqPeak1On,
    eqPeak1Freq,
    eqPeak1Gain,
    eqPeak1Q,
    eqPeak2On,
    eqPeak2Freq,
    eqPeak2Gain,
    eqPeak2Q,
    eqHighShelfOn,
    eqHighShelfFreq,
    eqHighShelfGain,
    eqHighShelfQ,

    // ディエッサー
    deessOn,
    deessFreq,
    deessThreshold,
    deessMaxReduction,

    // コンプ
    compOn,
    compThreshold,
    compRatio,
    compAttackMs,
    compReleaseMs,
    compKnee,

    // リバーブ（リバーブとエコー。処理はエコー → リバーブ）
    fxOn,
    reverbOn,
    reverbSize,
    reverbDamping,
    reverbPreDelayMs,
    reverbMix,
    echoOn,
    echoTimeMs,
    echoFeedback,
    echoDampingHz,
    echoMix,

    // 出力（出力ゲインとリミッター）
    outputOn,
    outputGain,
    limiterCeiling,
    limiterReleaseMs,

    // 画面と入力（音の処理の設定ではないもの）
    uiMode,
    inputChannel,
    monitorMode,
    showNoiseEstimate,
    showDynamicsLive,
    voiceGraph,

    count
};

constexpr int kNumParams = static_cast<int> (P::count);

enum class ParamType
{
    floating,
    toggle,
    choice
};

struct ParamSpec
{
    P id;
    const char* key;   // 保存やツールの引数で使う名前（英数字）
    const char* name;  // 画面に出す名前（日本語、UTF-8）
    ParamType type;
    float minValue;
    float maxValue;
    float defaultValue;
    float step;        // 0 なら連続
    float skewCentre;  // 0 なら直線。値を入れると、その値がつまみの中央に来る
    const char* unit;
    std::vector<const char*> choices; // choice のときの選択肢
};

struct ParamValues;
const std::vector<ParamSpec>& paramSpecs();
const char* paramDescription (P id); // 画面でマウスを乗せたときに出す説明（UTF-8）
const char* choiceDescription (P id, int index); // 選択肢ごとの説明（選んでいる項目の分をツールチップに足す）。ないときは nullptr
const ParamSpec& spec (P id);

// 表示のモード（簡単 0 / 普通 1 / 詳細 2）。項目ごとの音への影響の度合い（0 強 / 1 中 / 2 弱）が、モードの番号以下なら見せる
int paramLevel (P id);
bool isFeatureToggle (P id); // 機能のオン・オフ（タブの横、枠の見出しの横のスイッチ）か
bool isUiSetting (P id);     // 画面と入力の設定（音の処理の設定ではない。オートメーションの対象にせず、遅延の実測でも無視する）
// 今のモードで隠れる項目を、機能のオン・オフはオフ、それ以外は初期値に置き換えた値（処理にはこれを使う）
ParamValues applyDisplayMode (ParamValues p);

// モニターの表示の選択肢の番号（Params.cpp の monitor_mode の選択肢と同じ順番）
enum MonitorMode
{
    monitorSpectrum,
    monitorInSpectrogram,
    monitorOutSpectrogram,
    monitorBothSpectrograms,
    monitorSpectrumAndIn,
    monitorSpectrumAndOut,
    monitorAll,
    monitorHidden
};
int findParamByKey (const std::string& key); // 見つからなければ -1

// 傾きの選択肢（dB/oct）
constexpr std::array<int, 4> kSlopeChoices { 6, 12, 24, 48 };

// 実行中の値の一式。オーディオ処理はブロックごとにこれを受け取る。
struct ParamValues
{
    std::array<float, kNumParams> v {};

    ParamValues();  // 初期値で埋める

    float get (P id) const noexcept { return v[static_cast<size_t> (id)]; }
    void set (P id, float value) noexcept { v[static_cast<size_t> (id)] = value; }
    bool on (P id) const noexcept { return get (id) >= 0.5f; }
    int index (P id) const noexcept { return static_cast<int> (get (id) + 0.5f); }
};

} // namespace vcab
