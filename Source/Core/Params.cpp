#include "Params.h"

#include "Choices.h"

#include <algorithm>
#include <cassert>

namespace vcab
{

namespace
{
const std::vector<const char*> kSlopeNames { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" };
// ピッチ検出の方式（PitchDetector::Method と同じ順番）
const std::vector<const char*> kPitchMethodNames { "自己相関", "YIN", "pYIN", "McLeod（MPM）", "RAPT", "ケプストラム法", "SWIPE", "SIFT", "DIO", "Harvest" };

std::vector<const char*> algorithmNames()
{
    std::vector<const char*> names;
    for (int i = 0; i < static_cast<int> (Algorithm::count); ++i)
        names.push_back (algorithmInfo (i).name);
    return names;
}

std::vector<const char*> denoiseMethodNames()
{
    std::vector<const char*> names;
    for (int i = 0; i < static_cast<int> (DenoiseMethod::count); ++i)
        names.push_back (denoiseMethodInfo (i).name);
    return names;
}

std::vector<ParamSpec> makeSpecs()
{
    using T = ParamType;

    std::vector<ParamSpec> s {
        // id                     key                    name                          type        min      max      default  step   skew    unit

        // カット（ボイチェンの前のローカット・ハイカット）
        { P::cutOn,               "cut_on",              "カット",                     T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::lowCutOn,            "lowcut_on",           "ローカット",                 T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::lowCutFreq,          "lowcut_freq",         "ローカット 周波数",          T::floating, 20,      1000,    80,      0.1f,  120,    "Hz", {} },
        { P::lowCutSlope,         "lowcut_slope",        "ローカット 傾き",            T::choice,   0,       3,       2,       1,     0,      "",   kSlopeNames },
        { P::highCutOn,           "highcut_on",          "ハイカット",                 T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::highCutFreq,         "highcut_freq",        "ハイカット 周波数",          T::floating, 1000,    20000,   10000,   1,     5000,   "Hz", {} },
        { P::highCutSlope,        "highcut_slope",       "ハイカット 傾き",            T::choice,   0,       3,       1,       1,     0,      "",   kSlopeNames },

        // ノッチ（細く高い持続音とハムを削る）
        { P::notchOn,             "notch_on",            "ノッチ",                     T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::notchAutoOn,         "notch_auto_on",       "自動",                       T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::notchCount,          "notch_count",         "自動 本数",                  T::floating, 1,       8,       4,       1,     0,      "本", {} },
        { P::notchSensitivity,    "notch_sensitivity",   "自動 感度",                  T::floating, 6,       30,      12,      1,     0,      "dB", {} },
        { P::notchHumOn,          "notch_hum_on",        "ハム",                       T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::humFreq,             "hum_freq",            "ハム 電源の周波数",          T::choice,   0,       2,       0,       1,     0,      "",   { "自動", "50Hz", "60Hz" } },
        { P::humHarmonics,        "hum_harmonics",       "ハム 倍音の数",              T::floating, 1,       20,      10,      1,     0,      "",   {} },
        { P::notchDepth,          "notch_depth",         "深さ",                       T::floating, 6,       40,      20,      1,     0,      "dB", {} },

        // リペア（一瞬だけの音を直す。音割れ → パチパチ → 吹かれ → 突発音 → リップノイズ → 息継ぎ）
        { P::repairOn,            "repair_on",           "リペア",                     T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::repairDeclipOn,      "repair_declip_on",    "音割れの修復",               T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::repairClipLevel,     "repair_clip_level",   "割れの大きさ",               T::floating, -12,     0,       -0.3f,   0.01f,  0,      "dBFS", {} },
        { P::repairCrackleOn,     "repair_crackle_on",   "パチパチ",                   T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::repairCrackleThreshold, "repair_crackle_threshold", "パチパチ しきい値", T::floating, 3,       20,      10,       0.1f,  0,      "倍", {} },
        { P::repairPlosiveOn,     "repair_plosive_on",   "吹かれ・破裂音",             T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::repairPlosiveFreq,   "repair_plosive_freq", "吹かれ 周波数",              T::floating, 60,      300,     120,     1,     0,      "Hz", {} },
        { P::repairPlosiveThreshold, "repair_plosive_threshold", "吹かれ しきい値",   T::floating, 0,       24,      6,       0.5f,  0,      "dB", {} },
        { P::repairPlosiveDepth,  "repair_plosive_depth", "吹かれ 下げる量",           T::floating, 0,       30,      18,      0.5f,  0,      "dB", {} },
        { P::repairTransientOn,  "repair_transient_on", "突発音の抑制",              T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::repairTransientMode, "repair_transient_mode", "突発音 直し方",          T::choice,   0,       1,       0,       1,     0,      "",   { "下げる", "補間でつなぎ直す" } },
        { P::repairTransientThreshold, "repair_transient_threshold", "突発音 しきい値", T::floating, 6,     36,      12,      0.5f,  0,      "dB", {} },
        { P::repairTransientDepth, "repair_transient_depth", "突発音 下げる量",       T::floating, 0,       30,      12,      0.5f,  0,      "dB", {} },
        { P::repairClickOn,       "repair_click_on",     "リップノイズ",               T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::repairClickMode,     "repair_click_mode",   "リップ 直し方",              T::choice,   0,       1,       0,       1,     0,      "",   { "下げる", "補間でつなぎ直す" } },
        { P::repairClickThreshold, "repair_click_threshold", "リップ しきい値",       T::floating, 6,       30,      10,      0.5f,  0,      "dB", {} },
        { P::repairClickDepth,    "repair_click_depth",  "リップ 下げる量",            T::floating, 0,       30,      12,      0.5f,  0,      "dB", {} },
        { P::repairBreathOn,      "repair_breath_on",    "息継ぎ",                     T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::repairBreathMargin,  "repair_breath_margin", "息継ぎ 声との差",           T::floating, 0,       30,      12,      0.5f,  0,      "dB", {} },
        { P::repairBreathDepth,   "repair_breath_depth", "息継ぎ 下げる量",            T::floating, 0,       30,      10,      0.5f,  0,      "dB", {} },

        // デノイズ（持続するノイズを下げる）
        { P::denoiseOn,           "denoise_on",          "デノイズ",                   T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::denoiseMethod,       "denoise_method",      "方式",                       T::choice,   0,       8,       3,       1,     0,      "",   denoiseMethodNames() },
        { P::denoiseMaxReduction, "denoise_max_reduction", "削る量の上限",             T::floating, 0,       30,      24,      0.5f,  0,      "dB", {} },
        { P::denoiseRule,         "denoise_rule",        "下げる量の決め方",           T::choice,   0,       2,       2,       1,     0,      "",   { "ゲート式", "エキスパンダー式", "SNR 式" } },
        { P::denoiseThreshold,    "denoise_threshold",   "しきい値",                   T::floating, 0,       20,      6,       0.5f,  0,      "dB", {} },
        { P::denoiseRatio,        "denoise_ratio",       "比率",                       T::floating, 1,       8,       3,       0.1f,  0,      ":1", {} },
        { P::denoiseFftRule,      "denoise_fft_rule",    "下げる量の決め方",           T::choice,   0,       5,       3,       1,     0,      "",   { "スペクトル減算", "ウィーナー", "MMSE-STSA", "log-MMSE", "ウィーナー＋倍音の再生", "ウィーナー＋倍音の保護", "過剰減算（Berouti）", "帯域ごとの減算（Kamath-Loizou）", "ウィーナー＋2 段階（TSNR）", "聴覚のマスキング（Virag）" } },
        { P::denoiseFftSize,      "denoise_fft_size",    "FFT の窓の長さ",             T::choice,   0,       2,       1,       1,     0,      "",   { "256", "512", "1024" } },
        { P::denoiseStrength,     "denoise_strength",    "強さ",                       T::floating, 0,       12,      6,       0.5f,  0,      "dB", {} },
        { P::denoiseSmoothing,    "denoise_smoothing",   "ならし方",                   T::choice,   0,       3,       1,       1,     0,      "",   { "なし", "時間", "周波数", "ケプストラム" } },
        { P::denoiseSmoothWidth,  "denoise_smooth_width", "ならす幅",                  T::floating, 50,      1000,    200,     10,    200,    "Hz", {} },
        { P::denoiseAttackMs,     "denoise_attack",      "アタック",                   T::floating, 0.5f,    50,      10,       0.1f,  5,      "ms", {} },
        { P::denoiseReleaseMs,    "denoise_release",     "リリース",                   T::floating, 10,      1000,    100,      1,     150,    "ms", {} },
        { P::denoisePresence,     "denoise_presence",    "声のありそうさで調整",       T::choice,   0,       2,       0,       1,     0,      "",   { "オフ", "半々で固定", "自動（Cohen）" } },
        { P::denoiseComfort,      "denoise_comfort",     "残ったノイズの音色をそろえる", T::toggle,  0,       1,       0,       1,     0,      "",   {} },
        { P::denoiseOnset,        "denoise_onset",       "立ち上がりの保護",           T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::denoiseDfnModel,     "denoise_dfn_model",   "モデル",                     T::choice,   0,       1,       0,       1,     0,      "",   { "標準（DeepFilterNet3）", "低遅延（DeepFilterNet3 LL）" } },
        { P::denoiseDfnPostFilter, "denoise_dfn_post_filter", "後段のフィルタ",        T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::denoisePitchMethod,  "denoise_pitch_method", "ピッチ検出の方式",          T::choice,   0,       9,       1,       1,     0,      "",   kPitchMethodNames },
        { P::denoiseCombDepth,    "denoise_comb_depth",  "くし形の深さ",               T::floating, 0,       24,      9,       0.5f,  0,      "dB", {} },
        { P::denoiseProtect,      "denoise_protect",     "倍音の保護",                 T::floating, 0,       100,     70,      1,     0,      "%",  {} },
        { P::denoiseNoiseMode,    "denoise_noise_mode",  "ノイズの見積もり",           T::choice,   0,       1,       1,       1,     0,      "",   { "自動（最小値）", "自動（音量の判定）", "自動（Gerkmann の声の推定）", "自動（RNNoise の声の判定）", "学習", "学習＋自動（音量の判定）", "学習＋自動（Gerkmann の声の推定）", "学習＋自動（RNNoise の声の判定）" } },
        { P::denoiseLevelThreshold, "denoise_level_threshold", "判定のしきい値",     T::floating, 0,       20,      6,       0.5f,  0,      "dB", {} },
        { P::denoiseRnnThreshold, "denoise_rnn_threshold", "声とみなす確率",          T::floating, 10,      90,      50,      1,     0,      "%",  {} },
        { P::denoiseMinWindow,    "denoise_min_window",  "最小値を探す長さ",           T::floating, 0.8f,    3.2f,    1.6f,    0.1f,  0,      "s",  {} },
        { P::denoiseSppSnr,       "denoise_spp_snr",     "声の SNR の仮定",            T::floating, 5,       25,      15,      0.5f,  0,      "dB", {} },
        { P::denoiseSaveLearned,  "denoise_save_learned", "学習した値を保存",          T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::denoiseDereverbOn,   "denoise_dereverb_on", "残響の抑制",                 T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::denoiseRt60,         "denoise_rt60",        "残響 長さ",                  T::floating, 0.1f,    2.0f,    0.5f,    0.01f, 0.5f,   "s",  {} },

        // ゲート
        { P::gateOn,              "gate_on",             "ゲート",                     T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::gateMode,            "gate_mode",           "動き方",                     T::choice,   0,       1,       0,       1,     0,      "",   { "ゲート", "エキスパンダー（下方）" } },
        { P::gateThreshold,       "gate_threshold",      "しきい値",                   T::floating, -90,     0,       -42,     0.1f,  0,      "dB", {} },
        { P::gateRange,           "gate_range",          "レンジ",                     T::floating, -80,     0,       -42,     0.1f,  0,      "dB", {} },
        { P::gateRatio,           "gate_ratio",          "比率",                       T::floating, 1,       10,      2,       0.1f,  0,      ":1", {} },
        { P::gateKnee,            "gate_knee",           "ニー",                       T::floating, 0,       24,      6,       0.1f,  0,      "dB", {} },
        { P::gateAttackMs,        "gate_attack",         "アタック",                   T::floating, 0.1f,    50,      1,       0.1f,  5,      "ms", {} },
        { P::gateHoldMs,          "gate_hold",           "ホールド",                   T::floating, 0,       500,     50,      1,     80,     "ms", {} },
        { P::gateReleaseMs,       "gate_release",        "リリース",                   T::floating, 10,      1000,    150,     1,     150,    "ms", {} },
        { P::gateHysteresis,      "gate_hysteresis",     "ヒステリシス",               T::floating, 0,       20,      6,       0.1f,  0,      "dB", {} },
        { P::gateFromDenoise,     "gate_from_denoise",   "デノイズの判断で開く",       T::toggle,   0,       1,       0,       1,     0,      "",   {} },

        // ボイチェン
        { P::voiceOn,             "voice_on",            "ボイチェン",                 T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::algorithm,           "algorithm",           "ボイチェンの方式",           T::choice,   0,       17,      2,       1,     0,      "",   algorithmNames() },
        { P::pitch,               "pitch",               "ピッチ",                     T::floating, -24,     24,      12,      0.1f,  0,      "st", {} },
        { P::formant,             "formant",             "フォルマント",               T::floating, -24,     24,      6,       0.1f,  0,      "st", {} },
        { P::formantWarp,         "formant_warp",        "動かし方",                   T::choice,   0,       1,       0,       1,     0,      "",   { "一律", "低い側を残す", "帯域ごと", "フォルマントごと" } },
        { P::formantF1,           "formant_f1",          "F1 の差",                    T::floating, -12,     12,      0,       0.1f,  0,      "st", {} },
        { P::formantF2,           "formant_f2",          "F2 の差",                    T::floating, -12,     12,      0,       0.1f,  0,      "st", {} },
        { P::formantF3,           "formant_f3",          "F3 の差",                    T::floating, -12,     12,      0,       0.1f,  0,      "st", {} },
        { P::intonation,          "intonation",          "抑揚",                       T::floating, 0,       100,     0,       1,     0,      "%",  {} },
        { P::pitchMethod,         "pitch_method",        "ピッチ検出の方式",           T::choice,   0,       9,       1,       1,     0,      "",   kPitchMethodNames },
        { P::pitchMinHz,          "pitch_min",           "探す範囲の下限",             T::floating, 40,      200,     80,      1,     0,      "Hz", {} },
        { P::pitchMaxHz,          "pitch_max",           "探す範囲の上限",             T::floating, 150,     1000,    400,     1,     400,    "Hz", {} },
        { P::dlWindowMs,          "dl_window",           "窓の長さ",                   T::floating, 10,      100,     30,      0.1f,  30,     "ms", {} },
        { P::wsolaWindowMs,       "wsola_window",        "窓の長さ",                   T::floating, 10,      100,     40,      0.1f,  30,     "ms", {} },
        { P::wsolaSearchMs,       "wsola_search",        "探す範囲",                   T::floating, 1,       20,      10,      0.1f,  0,      "ms", {} },
        { P::psolaMarking,        "psola_marking",       "印付けの方式",               T::choice,   0,       1,       0,       1,     0,      "",   { "波形の重なり", "声門閉鎖点" } },
        { P::pvFftSize,           "pv_fft_size",         "FFT の窓の長さ",             T::choice,   0,       2,       1,       1,     0,      "",   { "512", "1024", "2048" } },
        { P::pvPhaseLock,         "pv_phase_lock",       "位相ロック",                 T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::formantMethod,       "formant_method",      "フォルマント処理",           T::choice,   0,       3,       3,       1,     0,      "",   { "なし", "ケプストラム", "LPC 包絡", "True Envelope" } },
        { P::smMaxSines,          "sm_max_sines",        "正弦波の数の上限",           T::floating, 10,      200,     80,      1,     0,      "",   {} },
        { P::smNoiseOn,           "sm_noise_on",         "雑音成分",                   T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::fsReferenceHz,       "fs_reference",        "基準の周波数",               T::floating, 40,      500,     120,     1,     0,      "Hz", {} },
        { P::bsBands,             "bs_bands",            "帯域の数",                   T::floating, 8,       100,     48,      1,     0,      "",   {} },
        { P::bsFormantMethod,     "bs_formant_method",   "フォルマント処理",           T::choice,   0,       4,       1,       1,     0,      "",   { "なし", "帯域の大きさ", "ケプストラム", "LPC 包絡", "True Envelope" } },
        { P::lpcOrder,            "lpc_order",           "LPC の次数",                 T::floating, 8,       60,      28,      1,     0,      "",   {} },
        { P::lpcBandwidth,        "lpc_bandwidth",       "帯域拡張",                   T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::lpcInterpolation,    "lpc_interp",          "係数補間",                   T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::lpcEmphasis,         "lpc_emphasis",        "高域強調",                   T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::cvBands,             "cv_bands",            "帯域の数",                   T::floating, 4,       48,      24,      1,     0,      "",   {} },
        { P::mixedExcitation,     "mixed_excitation",    "混合励振",                   T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::glottalShape,        "glottal_shape",       "声帯の波形",                 T::choice,   0,       2,       0,       1,     0,      "",   { "パルス列", "Rosenberg", "LF" } },
        { P::worldF0Method,       "world_f0_method",     "ピッチ検出の方式",           T::choice,   0,       1,       0,       1,     0,      "",   { "DIO", "Harvest" } },
        { P::worldBreath,         "world_breath",        "かすれ",                     T::floating, 0,       100,     0,       1,     0,      "%",  {} },

        // ブレス（ボイチェンの後に息の音を足す）
        { P::breathOn,            "breath_on",           "ブレス",                     T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::breathAmount,        "breath_amount",       "量",                         T::floating, 0,       100,     30,      1,     0,      "%",  {} },
        { P::breathToneHz,        "breath_tone",         "明るさ",                     T::floating, 300,     6000,    1500,    1,     1500,   "Hz", {} },

        // EQ（ローカット・ハイカットと 4 つの帯域）
        { P::eqOn,                "eq_on",               "EQ",                         T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::eqLowCutOn,          "eq_lowcut_on",        "ローカット",                 T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::eqLowCutFreq,        "eq_lowcut_freq",      "ローカット 周波数",          T::floating, 20,      1000,    120,      0.1f,  120,    "Hz", {} },
        { P::eqLowCutSlope,       "eq_lowcut_slope",     "ローカット 傾き",            T::choice,   0,       3,       2,       1,     0,      "",   kSlopeNames },
        { P::eqHighCutOn,         "eq_highcut_on",       "ハイカット",                 T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::eqHighCutFreq,       "eq_highcut_freq",     "ハイカット 周波数",          T::floating, 1000,    20000,   12000,   1,     5000,   "Hz", {} },
        { P::eqHighCutSlope,      "eq_highcut_slope",    "ハイカット 傾き",            T::choice,   0,       3,       1,       1,     0,      "",   kSlopeNames },
        { P::eqLowShelfOn,        "eq_ls_on",            "低域シェルフ",               T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::eqLowShelfFreq,      "eq_ls_freq",          "低域シェルフ 周波数",        T::floating, 20,      2000,    200,     0.1f,  200,    "Hz", {} },
        { P::eqLowShelfGain,      "eq_ls_gain",          "低域シェルフ 増減",          T::floating, -18,     18,      0,       0.1f,  0,      "dB", {} },
        { P::eqLowShelfQ,         "eq_ls_q",             "低域シェルフ Q",             T::floating, 0.3f,    3,       0.7f,    0.01f, 0.8f,   "",   {} },
        { P::eqPeak1On,           "eq_p1_on",            "中域ピーク1",                T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::eqPeak1Freq,         "eq_p1_freq",          "中域ピーク1 周波数",         T::floating, 50,      10000,   350,     0.1f,  700,    "Hz", {} },
        { P::eqPeak1Gain,         "eq_p1_gain",          "中域ピーク1 増減",           T::floating, -18,     18,      -3,       0.1f,  0,      "dB", {} },
        { P::eqPeak1Q,            "eq_p1_q",             "中域ピーク1 Q",              T::floating, 0.3f,    10,      1,       0.01f, 1.5f,   "",   {} },
        { P::eqPeak2On,           "eq_p2_on",            "中域ピーク2",                T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::eqPeak2Freq,         "eq_p2_freq",          "中域ピーク2 周波数",         T::floating, 200,     16000,   3200,    1,     2500,   "Hz", {} },
        { P::eqPeak2Gain,         "eq_p2_gain",          "中域ピーク2 増減",           T::floating, -18,     18,      3,       0.1f,  0,      "dB", {} },
        { P::eqPeak2Q,            "eq_p2_q",             "中域ピーク2 Q",              T::floating, 0.3f,    10,      1,       0.01f, 1.5f,   "",   {} },
        { P::eqHighShelfOn,       "eq_hs_on",            "高域シェルフ",               T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::eqHighShelfFreq,     "eq_hs_freq",          "高域シェルフ 周波数",        T::floating, 1000,    18000,   10000,    1,     5000,   "Hz", {} },
        { P::eqHighShelfGain,     "eq_hs_gain",          "高域シェルフ 増減",          T::floating, -18,     18,      -2,       0.1f,  0,      "dB", {} },
        { P::eqHighShelfQ,        "eq_hs_q",             "高域シェルフ Q",             T::floating, 0.3f,    3,       0.7f,    0.01f, 0.8f,   "",   {} },

        // ディエッサー
        { P::deessOn,             "deess_on",            "ディエッサー",               T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::deessFreq,           "deess_freq",          "周波数",                     T::floating, 2000,    12000,   6000,    1,     5000,   "Hz", {} },
        { P::deessThreshold,      "deess_threshold",     "しきい値",                   T::floating, -60,     0,       -30,     0.1f,  0,      "dB", {} },
        { P::deessMaxReduction,   "deess_max_reduction", "最大の削減量",               T::floating, 0,       24,      6,      0.1f,  0,      "dB", {} },

        // コンプ
        { P::compOn,              "comp_on",             "コンプ",                     T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::compThreshold,       "comp_threshold",      "しきい値",                   T::floating, -60,     0,       -24,     0.1f,  0,      "dB", {} },
        { P::compRatio,           "comp_ratio",          "比率",                       T::floating, 1,       20,      2,       0.1f,  3,      ":1", {} },
        { P::compAttackMs,        "comp_attack",         "アタック",                   T::floating, 0.1f,    100,     10,      0.1f,  10,     "ms", {} },
        { P::compReleaseMs,       "comp_release",        "リリース",                   T::floating, 10,      1000,    120,     1,     150,    "ms", {} },
        { P::compKnee,            "comp_knee",           "ニー",                       T::floating, 0,       24,      6,       0.1f,  0,      "dB", {} },

        // リバーブ（リバーブとエコー。処理はエコー → リバーブ）
        { P::fxOn,                "fx_on",               "リバーブ",                   T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::reverbOn,            "reverb_on",           "リバーブ",                   T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::reverbSize,          "reverb_size",         "部屋の大きさ",               T::floating, 0,       100,     50,      1,     0,      "%",  {} },
        { P::reverbDamping,       "reverb_damping",      "高い音の減衰",               T::floating, 0,       100,     50,      1,     0,      "%",  {} },
        { P::reverbPreDelayMs,    "reverb_predelay",     "プリディレイ",               T::floating, 0,       100,     20,      1,     0,      "ms", {} },
        { P::reverbMix,           "reverb_mix",          "混ぜる量",                   T::floating, 0,       100,     20,      1,     0,      "%",  {} },
        { P::echoOn,              "echo_on",             "エコー",                     T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::echoTimeMs,          "echo_time",           "時間",                       T::floating, 20,      1000,    250,     1,     250,    "ms", {} },
        { P::echoFeedback,        "echo_feedback",       "繰り返し",                   T::floating, 0,       90,      30,      1,     0,      "%",  {} },
        { P::echoDampingHz,       "echo_damping",        "高い音の減衰",               T::floating, 1000,    16000,   5000,    1,     5000,   "Hz", {} },
        { P::echoMix,             "echo_mix",            "混ぜる量",                   T::floating, 0,       100,     25,      1,     0,      "%",  {} },

        // 出力（出力ゲインとリミッター）
        { P::outputOn,           "output_on",          "出力",                       T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::outputGain,          "output_gain",         "出力ゲイン",                 T::floating, -12,     24,      6,       0.1f,  0,      "dB", {} },
        { P::limiterCeiling,      "limiter_ceiling",     "上限",                       T::floating, -24,     0,       -1,      0.1f,  0,      "dB", {} },
        { P::limiterReleaseMs,    "limiter_release",     "リリース",                   T::floating, 10,      500,     100,     1,     100,    "ms", {} },

        // 画面と入力（音の処理の設定ではないもの）
        { P::uiMode,              "ui_mode",             "表示",                       T::choice,   0,       2,       1,       1,     0,      "",   { "簡単", "普通", "詳細" } },
        { P::inputChannel,        "input_channel",       "入力チャンネル",             T::choice,   0,       3,       0,       1,     0,      "",   { "自動", "1（左）", "2（右）", "1+2 の平均" } },
        { P::monitorMode,         "monitor_mode",        "モニター",                   T::choice,   0,       7,       6,       1,     0,      "",   { "スペクトル", "入力スペクトログラム", "出力スペクトログラム", "スペクトログラム両方",
                                                                                                                                                   "スペクトル & 入力スペクトログラム", "スペクトル & 出力スペクトログラム", "全部", "非表示" } },
        { P::showNoiseEstimate,   "show_noise_estimate", "ノイズの見積もりを表示",     T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::showDynamicsLive,    "show_dynamics_live",  "ダイナミクスの点とメーター", T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::voiceGraph,          "voice_graph",         "グラフ",                     T::choice,   0,       3,       0,       1,     0,      "",   { "声の高さ", "フォルマントの動く量", "両方", "非表示" } },
    };

    // choice の範囲は選択肢の数から決める
    for (auto& p : s)
        if (p.type == T::choice)
            p.maxValue = static_cast<float> (p.choices.size() - 1);

    // 番号（P の順）で並べ直す。上の並びと P の並びが食い違うと、別の設定の値を読んでしまう（assert はリリースの
    // ビルドでは効かないので、並べ替えで防ぐ）
    std::stable_sort (s.begin(), s.end(), [] (const ParamSpec& a, const ParamSpec& b) { return a.id < b.id; });
    for (size_t i = 0; i < s.size(); ++i)
        assert (static_cast<size_t> (s[i].id) == i);

    assert (s.size() == static_cast<size_t> (kNumParams));
    return s;
}
} // namespace

const std::vector<ParamSpec>& paramSpecs()
{
    static const std::vector<ParamSpec> specs = makeSpecs();
    return specs;
}

const ParamSpec& spec (P id)
{
    return paramSpecs()[static_cast<size_t> (id)];
}

int findParamByKey (const std::string& key)
{
    const auto& specs = paramSpecs();
    for (size_t i = 0; i < specs.size(); ++i)
        if (key == specs[i].key)
            return static_cast<int> (i);
    return -1;
}

const char* paramDescription (P id)
{
    static const char* const slope = "切り方の急さ。数字が大きいほど、切る周波数の手前でばっさり切れます。";
    static const char* const eqFreq = "この帯域の中心（シェルフでは境目）の周波数。スペクトルの上の点を左右にドラッグしても変えられます。";
    static const char* const eqGain = "この帯域を持ち上げる（+）か、下げる（-）量。スペクトルの上の点を上下にドラッグしても変えられます。";
    static const char* const eqQ = "影響する範囲の幅。大きいほど狭い範囲だけ、小さいほど広い範囲がなだらかに変わります。スペクトルの上の点にマウスを乗せてホイールを回しても変えられます。";

    switch (id)
    {
        case P::voiceOn:             return "声の変換をオン・オフします。オフにすると変換しない声になり、変換の前後を聞き比べられます。";
        case P::algorithm:           return "声を変える仕組みを選びます。方式ごとに、音の変わり方と遅延が違います（括弧の中は、初期設定での遅延の目安）。";
        case P::pitch:               return "声の高さを半音単位で変えます。+12 で1オクターブ上（周波数が2倍）。括弧の中は周波数の変化率です。";
        case P::formant:             return "声色（口や喉の大きさの感じ）を変えます。上げると小柄な人の声、下げると大柄な人の声に近づきます。ピッチとは別に動かせます（動かせない方式もあります）。";
        case P::dlWindowMs:          return "早回しする断片の長さ。短いとザラついた音に、長いとうなりやエコーのような感じが出やすくなります。遅延はこの半分です。";
        case P::wsolaWindowMs:       return "早回しする断片の長さ。短いとザラついた音に、長いとエコーのような感じが出やすくなります。遅延はこの半分です。";
        case P::wsolaSearchMs:       return "断片をつなぐときに、波形がよく重なる位置を探す範囲。声の周期（男性で 5〜10ms）より長めにします。広いほど重くなります。";
        case P::pitchMethod:         return "声の高さ（周期）を見つける方法。";
        case P::pitchMinHz:          return "声の高さを探す範囲の下限。下げるほど低い声まで追えますが、調べる窓が長くなり（下限の周期の2つ分）、遅延が増えます。";
        case P::pitchMaxHz:          return "声の高さを探す範囲の上限。範囲を広げるほど、1オクターブ上や下に間違えやすくなります。";
        case P::pvFftSize:           return "一度に周波数成分へ分ける長さ。長いほど声が滑らかになりますが、遅延（この 3/4。1024 で約 16ms）が増え、子音がぼやけます。";
        case P::pvPhaseLock:         return "倍音の山とその周りを、形を崩さずにまとめて動かします。オフにすると、こもった響き（位相の乱れ）が出やすくなります。";
        case P::formantMethod:       return "声色の形（スペクトル包絡）の求め方。";
        case P::smMaxSines:          return "鳴らし直す正弦波の数の上限（大きい順に選ぶ）。減らすと高い倍音から消えて、こもった電子音になります。増やすと重くなります。";
        case P::smNoiseOn:           return "正弦波で表せない成分（息やサ行）を、雑音として鳴らします。オフにすると息やサ行が消え、「水っぽい」音になります。";
        case P::lpcOrder:            return "口と喉のフィルタの係数の数（48kHz で 20〜40 が目安）。少ないと声がこもり、多すぎると元の声の高さが残って聞こえます。";
        case P::lpcBandwidth:        return "フィルタの山（フォルマント）を少しなだらかにします（ラグ窓）。オフにすると「ビーン」という金属っぽさが出やすくなります。";
        case P::lpcInterpolation:    return "5ms ごとに求め直すフィルタの係数を、急に切り替えずに少しずつつなぎます。オフにすると、切り替えのたびの小さな段差がザラつきとして聞こえることがあります。";
        case P::lpcEmphasis:         return "分析の前に高い音を持ち上げ、声に戻した後で下げます。声は高い音ほど弱いので、そのままだと低いフォルマントに係数を取られて、高いフォルマントがぼやけます。";
        case P::cvBands:             return "声を分ける帯域の数。少ないとロボットのような声に、多いと元の声に近づきます（細かい帯域ほど、声の変化への追従は少し遅れます）。";
        case P::fsReferenceHz:       return "ずらす量の基準。全部の周波数を「基準 ×（倍率 - 1）」Hz ずらします。自分の声の高さに合わせると、一番低い倍音が正しく上がります。";
        case P::bsBands:             return "声を分ける帯域の数。多いほど倍音の並びが崩れにくく、ピッチシフトに近づきますが、計算が重くなります。";
        case P::bsFormantMethod:     return "声色の形（包絡）の求め方。";
        case P::worldBreath:         return "声のかすれ（息の混ざり具合）を増やします。WORLD が分けた「非周期性（声の中の雑音の割合）」を、この割合だけ雑音の側に寄せます。";
        case P::worldF0Method:       return "WORLD が声の高さを求める方法。";
        case P::mixedExcitation:     return "作り直す音源を、5 つの帯域ごとにパルス列と雑音を混ぜて作り、自然な息混じりにします。オフにするとブザーのような響きになります。";
        case P::formantWarp:         return "フォルマントの動かし方。";
        case P::intonation:          return "抑揚（声の高さの上下の幅）を強調します。最近 2 秒の平均からのずれを、100% で 2 倍にします。ピッチ検出を使う方式だけで効きます。";
        case P::formantF1:
        case P::formantF2:
        case P::formantF3:           return "動かし方が「帯域ごと」「フォルマントごと」のとき、この目印を、フォルマントのつまみより何半音多く動かすか。グラフの点でも変えられます。";
        case P::glottalShape:        return "ボコーダで作り直す声帯の音の、1 周期の形。";
        case P::psolaMarking:        return "声の 1 周期ごとに付ける印（切り出しの中心）の決め方。";
        case P::breathOn:            return "声に息の音を足して、息っぽい（ささやき気味の）声にします。どのボイチェンの方式にも効きます。ボイチェンの後、EQ の前にかかります。";
        case P::breathAmount:        return "足す息の量。声と同じ母音の「ささやき声」を作り、その高い側を足します。息の大きさは、声の大きさに合わせて自動で動きます。";
        case P::breathToneHz:        return "息を足し始める周波数。上げるほど、明るく細い息に、下げるほど、太くこもった息になります。";

        case P::cutOn:               return "ボイチェンの前に、いらない低い音と高い音を切ります。ローカットとハイカットを、まとめてオン・オフします。";
        case P::lowCutOn:            return "低い音（電源のハム、机やマイクの振動、息の吹かれ）を切ります。ピッチを上げると低い雑音も上がって聞こえやすくなるので、変換の前に切っておきます。";
        case P::lowCutFreq:          return "これより低い音を切ります。";
        case P::lowCutSlope:         return slope;
        case P::highCutOn:           return "高い音（マイクの高域のノイズ）を切ります。切った位置は変換で動くので、最終的な帯域は EQ で整えます。";
        case P::highCutFreq:         return "これより高い音を切ります。";
        case P::highCutSlope:        return slope;
        case P::notchOn:             return "細く高い持続音（コイル鳴き、ファンのうなり、電源のハム）を、その周波数だけ細く削ります。遅延は増えません。";
        case P::notchAutoOn:         return "周波数の分からない細い持続音を、自動で探して追いかけ、削ります。1 秒以上続いた音だけが対象で、声の倍音は除きます。";
        case P::notchCount:          return "同時に追いかけて削る音の数。周りより突き出た量の大きい順に選びます。";
        case P::notchSensitivity:    return "周りより何 dB 突き出た山を対象にするか。小さくすると弱い音も拾いますが、声の倍音などを誤って削りやすくなります。";
        case P::notchHumOn:          return "電源の 50 / 60Hz とその倍音を、細いノッチ（幅 4Hz）で削ります。男声の低い倍音もわずかに削るので、ハムがあるときだけ使います。";
        case P::humFreq:             return "電源の周波数。自動は、50Hz と 60Hz の倍音の並びのどちらが目立つかで決めます（東日本は 50Hz、西日本は 60Hz）。";
        case P::humHarmonics:        return "何倍音まで削るか。ハムは倍音が多いので、ブーンという音が残るときは増やします。";
        case P::notchDepth:          return "1 本のノッチで削る量（自動とハムで共通）。深すぎると、穴があいたような不自然な音になります。";

        case P::repairOn:            return "キーボード、吹かれ、リップノイズ、音割れなど、一瞬だけの音を直します（ずっと続くノイズは、デノイズで下げます）。";
        case P::repairDeclipOn:      return "波形の山が平らに切れたところ（音割れ）を見つけ、なめらかな山を作り直します。2ms 先を見るので、その分遅延が増えます。";
        case P::repairClipLevel:     return "この大きさ以上の値が 2 サンプル以上続いたら、割れとみなします。マイクや機器で割れたなら 0dBFS の近くにします。";
        case P::repairPlosiveOn:     return "息やパ行で出る「ボフッ」という低い音の塊を、その瞬間だけ下げます。低い側が、声の帯域より急に大きくなったときに働きます。";
        case P::repairPlosiveFreq:   return "この周波数より下を、吹かれの音として扱います。男声の低い声（100Hz 前後）まで下げないように、低めにしておくのがおすすめです。";
        case P::repairPlosiveThreshold: return "低い側の音量が、声の帯域より何 dB 大きくなったら、吹かれとみなすか。小さくすると弱い吹かれも拾いますが、低い声を誤って下げやすくなります。";
        case P::repairPlosiveDepth:  return "吹かれを見つけたときに、低い側を下げる量。";
        case P::repairClickOn:       return "口の中のペチャッという音（リップノイズ）を、高い側だけ短く下げます。声の低い側はそのままです。2ms 先を見る分、遅れます。";
        case P::repairClickThreshold: return "周りより何 dB 急に大きくなった音を、リップノイズとみなすか。小さくすると弱い音も拾いますが、サ行やタ行などの子音を誤って下げやすくなります。";
        case P::repairClickDepth:    return "リップノイズを見つけたときに、高い側を下げる量。";
        case P::repairTransientMode:
        case P::repairClickMode:     return "見つけた音の直し方。";
        case P::repairCrackleOn:     return "接触不良などで出る、細かいクリックの連続（パチパチ）を消します。飛び抜けた 1〜8 サンプルの点を作り直します。遅延は約 1.2ms。";
        case P::repairCrackleThreshold: return "予測の外れが、周りの何倍を超えたら点とみなすか。小さくすると弱いパチパチも拾いますが、声の鋭いところ（声帯が閉じる瞬間など）を誤って直しやすくなります。";
        case P::repairBreathOn:      return "言葉の合間の息継ぎ（スゥッという音）を下げます。倍音がなく、声より小さい雑音が 40ms 以上続くと息継ぎとみなします。5ms 遅れます。";
        case P::repairBreathMargin:  return "最近の声の大きさより、何 dB 以上小さい音を息継ぎとみなすか。小さくすると大きめの息継ぎも下げますが、小さな声のハ行やささやき声を誤って下げやすくなります。";
        case P::repairBreathDepth:   return "息継ぎを下げる量。消し切ると不自然な無音になるので、少し残すのが定番です。";
        case P::denoiseOn:           return "黙っている間も続く、サーというノイズ（空調、パソコンのファン、マイクの雑音）を下げます。ノッチの後、ゲートの前にかけます。";
        case P::denoiseMethod:       return "ノイズを下げる仕組み。";
        case P::denoiseRule:         return "帯域ごとの、下げる量の決め方。";
        case P::denoiseFftSize:      return "FFT で一度に見る長さ。長いほど細かく分けられますが、FFT では遅れが増え（512 で約 11ms）、FFT＋FIR・FFT＋帯域分割では反応が遅くなります。";
        case P::denoiseNoiseMode:    return "ノイズの大きさの見積もり方。";
        case P::denoiseRnnThreshold: return "RNNoise が出す声の確率が、これを超えたら声とみなします。下げると声を削りにくく、ノイズの変化には追いつきにくくなります。";
        case P::denoiseLevelThreshold: return "全体の音量が、ノイズの見積もりよりこれだけ大きければ声とみなします。ノイズがこれ以上大きくなると追いつけないので、そのときは上げてください。";
        case P::denoiseMinWindow: return "この長さの中で一番小さい音をノイズとみなします。長くすると話し続けても声を削りにくく、ノイズの変化には追いつきにくくなります。";
        case P::denoiseSppSnr: return "声があるとき、ノイズよりこれだけ大きいと仮定して、声の確率を出します。下げると声を削りにくく、ノイズの変化には追いつきにくくなります。";
        case P::denoiseMaxReduction: return "ノイズを下げる量の上限。完全に消そうとすると、声が水っぽく、ロボットのようになるので、ノイズを少し残します。";
        case P::denoiseStrength:     return "ノイズの見積もりを、何 dB 大きく見るか。大きくするとノイズがよく消えますが、小さな声や語尾も削れやすくなります。";
        case P::denoiseThreshold:    return "帯域の大きさが、ノイズよりこれだけ大きければ声とみなします。";
        case P::denoiseRatio:        return "しきい値より下で、1dB 小さくなるごとに何 dB 下げるか（2:1 なら 1dB ごとに、さらに 1dB 下げる）。";
        case P::denoiseAttackMs:     return "声が出て、下げるのをやめるまでの時間。短いほど語頭が欠けにくくなります。";
        case P::denoisePitchMethod:  return "倍音を使う方式で、声の高さを求める方法。";
        case P::denoiseCombDepth:    return "有声音のとき、倍音と倍音の間で、さらに下げる量（4kHz まで）。深くすると、声の高さの誤りで声そのものが削れやすくなります。";
        case P::denoiseProtect:      return "検出した倍音の位置で、下げる量をどれだけゆるめるか（100% で下げない）。違いを聞くときは、窓の長さを 1024 にします。";
        case P::denoiseSmoothing:    return "下げる量のならし方（ピロピロ音を減らす）。";
        case P::denoisePresence:     return "下げる量を、「声がある確率」で調整します。";
        case P::denoiseSaveLearned:  return "学習で覚えたノイズを設定と一緒に保存し、次に開いたときも使います（EXE は閉じたとき、DAW はプロジェクトに）。";
        case P::repairTransientOn:  return "キーボードやクリック、机を叩く音などの、短く鋭い音だけを下げます（声の出だしは対象外）。5ms 先を見る分、遅れます。";
        case P::repairTransientThreshold: return "周りより何 dB 急に大きくなった音を、突発音とみなすか。小さくすると弱いクリックも拾いますが、サ行やタ行などの子音を誤って下げやすくなります。";
        case P::repairTransientDepth: return "突発音を見つけたときに下げる量。";
        case P::denoiseDereverbOn:   return "部屋の響き（声の後に残る反射音）を、ノイズと一緒に下げます。ノイズを自分で見積もる方式（AI 以外）で使えます。";
        case P::denoiseRt60:         return "部屋の響きの長さ（音が 60dB 小さくなるまでの時間）。普通の部屋で 0.3〜0.6 秒。長くすると強く下げますが、声の語尾も削れやすくなります。";
        case P::denoiseDfnModel:     return "DeepFilterNet のモデル。低遅延は先を見ないので、遅延が約 20ms 短くなります（約 20ms）。その分、品質は少し下がります。";
        case P::denoiseDfnPostFilter: return "DeepFilterNet の後段のフィルタ。声らしいところを少し強調し、ノイズの残りをさらに下げます。";
        case P::denoiseFftRule:      return "FFT で、区画ごとに残す割合の決め方。";
        case P::denoiseComfort:      return "いったん深く下げてから、元のノイズと同じ音色の「作ったノイズ」を上限の大きさで足し、残るノイズを一定にします。";
        case P::denoiseOnset:        return "FFT＋帯域分割で、話し始めの子音を守ります。ノイズより急に大きくなった帯域を、30ms の間開けます。";
        case P::showNoiseEstimate:   return "見積もっているノイズの大きさを、モニターのスペクトルに緑の点線で重ねます。AI の方式では表示しません。";
        case P::gateFromDenoise:     return "ゲートを、音量ではなく、デノイズが判断した「声のありそうさ」で開閉します。ノイズが大きく、しきい値を合わせにくいとき向けです。";
        case P::denoiseSmoothWidth:  return "周波数でならすときに、平均する幅。広いほどピロピロは減りますが、細かく下げられなくなります。";
        case P::denoiseReleaseMs:    return "声が止まって、下げきるまでの時間。長いほど語尾が自然に消えますが、ノイズが残る時間も長くなります。";

        case P::gateOn:              return "声が小さいとき（黙っているとき）に音を下げて、ノイズを目立たなくします。";
        case P::gateThreshold:       return "この音量を超えるとゲートが開きます。右のグラフで、しゃべったときだけ緑の点がしきい値（緑の線の折れ目）より右に来るように合わせます。";
        case P::gateAttackMs:        return "開くまでの時間。短いほど語頭が欠けにくくなります。";
        case P::gateHoldMs:          return "音量が下がっても、閉じ始めるまで待つ時間。言葉の途中の短い切れ目で閉じないようにします。";
        case P::gateReleaseMs:       return "閉じきるまでの時間。長いほど語尾が自然に消えます。";
        case P::gateHysteresis:      return "開くときと閉じるときのしきい値の差。しきい値の付近で、ゲートがパタパタと開閉しないようにします。";
        case P::gateMode:            return "しきい値より小さい音の下げ方。";
        case P::gateRatio:           return "エキスパンダーのとき、しきい値より下で下げる比率。2 なら、1dB 小さくなるごとに出力は 2dB 小さくなります（レンジまで）。";
        case P::gateKnee:            return "エキスパンダーのとき、しきい値の前後この幅で、下がり始めをなめらかにします（0 で角のある折れ線）。閉じた瞬間の下がり方の段差も小さくなります。";
        case P::gateRange:           return "閉じたときに音を下げる量。完全な無音にせず少しだけ残すと、開閉が自然に聞こえます。";

        case P::eqOn:                return "ボイチェンの後で、音色を整えます。ローカット、ハイカットと、4つの帯域（低域シェルフ、中域ピーク2つ、高域シェルフ）を、まとめてオン・オフします。";
        case P::eqLowCutOn:          return "変換の後に、低い音を切ります。変換で出る低い濁りや揺れを取り除きます。";
        case P::eqLowCutFreq:        return "これより低い音を切ります。";
        case P::eqLowCutSlope:       return slope;
        case P::eqHighCutOn:         return "変換の後に、高い音を切ります。変換で出るシャリシャリ感やきらつきが気になるときに下げます（目安は 12kHz）。";
        case P::eqHighCutFreq:       return "これより高い音を切ります。10〜16kHz には声の抜けの成分もあるので、下げすぎると暗い音になります。";
        case P::eqHighCutSlope:      return slope;
        case P::eqLowShelfOn:        return "ある周波数より下の低音を、まとめて持ち上げたり下げたりします。声の太さや、こもり具合が変わります。";
        case P::eqPeak1On:           return "決めた周波数の周りを、持ち上げたり下げたりします。声の芯や、鼻にかかった感じなどを整えます。";
        case P::eqPeak2On:           return "決めた周波数の周りを、持ち上げたり下げたりします。声の明るさや、刺さりなどを整えます。";
        case P::eqHighShelfOn:       return "ある周波数より上の高音を、まとめて持ち上げたり下げたりします。声の明るさや、抜けが変わります。";
        case P::eqLowShelfFreq:      return "この周波数より下の低音を、まとめて持ち上げたり下げたりします。声の太さや、こもり具合が変わります。";
        case P::eqPeak1Freq:
        case P::eqPeak2Freq:
        case P::eqHighShelfFreq:     return eqFreq;
        case P::eqLowShelfGain:
        case P::eqPeak1Gain:
        case P::eqPeak2Gain:
        case P::eqHighShelfGain:     return eqGain;
        case P::eqLowShelfQ:
        case P::eqPeak1Q:
        case P::eqPeak2Q:
        case P::eqHighShelfQ:        return eqQ;

        case P::deessOn:             return "サ行などの「シー」「スー」という鋭い音（歯擦音）が強く出た瞬間だけ、高い帯域を下げます。フォルマントを上げると刺さりやすくなるので、その対策です。";
        case P::deessFreq:           return "この周波数より上を見て、強すぎるときだけ下げます（スペクトルの色付けの範囲）。サ行の刺さる高さ（5〜8kHz）に合わせます。";
        case P::deessThreshold:      return "高い帯域の音量がこれを超えたときに下げ始めます。サ行のときだけ削減のメーターが動くように合わせます。";
        case P::deessMaxReduction:   return "一度に下げる量の上限。大きくしすぎると、サ行がこもって舌足らずに聞こえます。";

        case P::compOn:              return "大きな声を抑えて、声の大きさのばらつきを小さくします。初期値はゆるめ（2:1）です。強くかけると抑揚がなくなり、しゃべりにくくなります。";
        case P::compThreshold:       return "この音量を超えた分を抑えます。右のグラフの橙の線の折れ目です。";
        case P::compRatio:           return "しきい値を超えた分を、どれだけ縮めるか。2:1 なら、しきい値を 10dB 超えた音が 5dB 超えに縮みます。";
        case P::compAttackMs:        return "抑え始めるまでの時間。短いと語頭の勢いまで抑え、長いと語頭の勢いが残ります。";
        case P::compReleaseMs:       return "抑えた後に戻るまでの時間。短いと音量が揺れて聞こえ、長いと抑えた後しばらく小さいままになります。";
        case P::compKnee:            return "しきい値の付近での効き方のなめらかさ。大きいほど、しきい値の手前からゆるやかに効き始め、自然に聞こえます。";
        case P::fxOn:                return "エコーとリバーブを、まとめてオン・オフします。VRChat ではワールドの響きも付くので、かけすぎに注意します。";
        case P::echoOn:              return "遅らせた声を、繰り返しながら足します（やまびこ）。";
        case P::echoTimeMs:          return "元の声から、遅れた声が聞こえるまでの時間。短いと（50ms 以下）声が太くなったように、長いとはっきりしたやまびこに聞こえます。";
        case P::echoFeedback:        return "遅れた声を、もう一度遅れの入口に戻す割合。大きいほど、何度も繰り返します。";
        case P::echoDampingHz:       return "繰り返すたびに、この周波数より上を少しずつ削ります。下げるほど、繰り返すにつれてこもっていきます（テープエコー風）。";
        case P::echoMix:             return "遅れた声を足す量。";
        case P::reverbOn:            return "部屋の響き（残響）を足します。";
        case P::reverbSize:          return "響きの長さ。大きいほど、広い部屋のように長く残ります。";
        case P::reverbDamping:       return "響きの中の高い音を削る量。大きいほど、こもった柔らかい響きになります（カーテンや家具の多い部屋）。小さいと、明るく硬い響きになります（タイルの浴室）。";
        case P::reverbPreDelayMs:    return "元の声から、響きが始まるまでの間。少し空けると、声がはっきりしたまま広さの感じが出ます。";
        case P::reverbMix:           return "響きを足す量。";

        case P::outputGain:          return "最後の音量。上げすぎても、リミッターが上限で止めます。コンプで小さくなった分も、ここで持ち上げます。";
        case P::outputOn:           return "出力の段（出力ゲインとリミッター）をまとめてオン・オフします。オフにすると、音量を変えず、上限で抑えることもしません。";
        case P::limiterCeiling:      return "出力の上限。";
        case P::limiterReleaseMs:    return "抑えた後に、元の音量へ戻るまでの時間。短いと音量が揺れて聞こえ、長いと抑えた後しばらく音が小さくなります。";

        case P::monitorMode:         return "下のモニターの表示。スペクトルは今の周波数ごとの大きさ、スペクトログラムは時間の流れです。非表示で軽くなり、ウィンドウも縮みます。";
        case P::inputChannel:        return "マイクの入力チャンネル。自動は、1ch と 2ch の大きいほうを使います（もう片方が数秒続けて 6dB 以上大きいと切り替え）。";
        case P::showDynamicsLive:    return "右のグラフの、今の音量の点とメーターを動かします。オフにすると止めて、線だけを設定の変更時に描き直します（負荷が下がります）。";
        case P::uiMode:              return "画面に出す項目の量。隠れた機能はオフになります。";
        case P::voiceGraph:          return "ボイチェンのタブの右下に出すグラフ。方式が使わないグラフは出ません。非表示にすると、描画と更新が止まります。";

        case P::count:               break;
    }
    return "";
}

const char* choiceDescription (P id, int index)
{
    auto pick = [index] (std::initializer_list<const char*> list) -> const char*
    {
        if (index < 0 || index >= static_cast<int> (list.size()))
            return nullptr;
        return list.begin()[index];
    };
    switch (id)
    {
        case P::pitchMethod:
        case P::denoisePitchMethod: return pick ({ "一番シンプル。1 オクターブ低く間違えやすい。", "ピッチ検出の定番。間違いが少ない。", "YIN の候補から確率で選ぶ（10ms 先を見る）。", "YIN と並ぶ定番。間違いが少ない。", "候補から前後のつながりで選ぶ（10ms 先を見る）。", "倍音の等間隔な並びから周期を探す。", "精度が高いが窓が長く、遅延が増えやすい。", "LPC で声道の影響を除いてから探す、古典的な方法。", "WORLD の速い方法。遅延が約 60ms 増える。", "WORLD の正確な方法。遅延が約 80ms 増える。" });
        case P::formantMethod: return pick ({ "フォルマントは、ピッチと一緒に動く。", "なだらかで、山が低め。", "山が鋭い。", "倍音の頂点をなぞる包絡。" });
        case P::bsFormantMethod: return pick ({ "フォルマントは、ピッチと一緒に動く。", "遅れなく追従するが、形は帯域の数の分だけ粗い。", "なだらかで細かい形。変化には少し遅れる。", "山が鋭い。変化には少し遅れる。", "倍音の頂点をなぞる。変化には少し遅れる。" });
        case P::worldF0Method: return pick ({ "速い方法。", "有声・無声の判断が正確で、かすれが減ることがある。重く、遅延が約 40ms 増える。" });
        case P::formantWarp: return pick ({ "すべての周波数を、同じ倍率で動かす。", "150Hz 以下を動かさない。声が細くなりにくい。", "固定の目印ごとに差を決めて動かす（グラフの点）。", "声から追いかけた F1〜F3 ごとに動かす。" });
        case P::psolaMarking: return pick ({ "前の印の周りと、一番よく重なる位置に付ける。", "声帯が閉じる瞬間に付ける。遅延が少し増える。" });
        case P::glottalShape: return pick ({ "鋭い山だけ。ブザーのような響きになりやすい。", "声帯の空気の流れを真似た、丸い山。やわらかい響き。", "閉じる瞬間が鋭く、閉じた後の戻りも含む。パルス列に近く、太め。" });
        case P::repairClickMode:
        case P::repairTransientMode: return pick ({ "その瞬間の音量を下げる。試験では一番自然だった。", "前の音からの予測で作り直す。長い区間では声が薄くなる。" });
        case P::denoiseMethod: return index >= 0 && index < static_cast<int> (DenoiseMethod::count) ? denoiseMethodInfo (index).description : nullptr;
        case P::denoiseRule: return pick ({ "しきい値より小さい帯域を、一気に下げる。", "しきい値より下を、なだらかに下げる。ゲートより浅く、深くするには比率を上げる。", "ノイズの割合だけ下げる（一番自然）。" });
        case P::denoiseNoiseMode: return pick ({ "直近 1.6 秒の、一番静かな所を見る。話し続けると声を削ることがある。",
                                                 "全体の音量が小さい間だけ更新する。しきい値で調整する。",
                                                 "声がなさそうな区画だけ更新する（統計モデル）。",
                                                 "AI（RNNoise）が声なしと判定した間だけ更新する。声を削りにくい。",
                                                 "ボタンを押して 2 秒黙っている間のノイズを覚え、その値で下げる。",
                                                 "学習した値から始めて、その後は音量の判定で追う。",
                                                 "学習した値から始めて、その後は Gerkmann の推定で追う。",
                                                 "学習した値から始めて、その後は RNNoise の判定で追う。" });
        case P::denoiseSmoothing: return pick ({ "ならさない。", "変化をゆっくりにする。強いと語頭が欠ける。", "前後の区画で平均する。強いと倍音の間が残る。", "倍音の並びを残し、細かい揺れだけならす。" });
        case P::denoisePresence: return pick ({ "調整しない。", "声がない確率を、50% とみなす。", "確率を区画ごとに見積もる（本来の形）。" });
        case P::denoiseFftRule: return pick ({ "一番素朴。ピロピロした音（ミュージカルノイズ）が出る。", "判定指向型で、ピロピロしない。", "ウィーナーより控えめに下げ、声を傷めにくい。", "MMSE-STSA と同じく控えめ。大きさを対数で考えた形。", "ウィーナーの後、細った倍音を作り直す。", "検出した倍音の位置だけ、下げる量をゆるめる（窓 1024 向け）。", "SNR が低いフレームほど多めに引き、ピロピロを減らす。", "過剰減算を、4 つの帯域ごとに行う。", "ウィーナーの結果から SNR を求め直し、語頭の欠けを取り戻す。", "声に隠れて聞こえないノイズは残し、声を傷めない。ピロピロは残る。" });
        case P::gateMode: return pick ({ "レンジの分だけ一気に下げる（開くか閉じるか）。", "比率に応じて、なだらかに下げる。声の終わりや小さな声が自然に残る。" });
        case P::uiMode: return pick ({ "声の変わり方を大きく決める項目だけ。", "カット、ゲート、EQ、コンプなども出す。", "細かいつまみまで、すべて出す。" });
        default: return nullptr;
    }
}

int paramLevel (P id)
{
    switch (id)
    {
        // 強（簡単から）: 声の変わり方と、聞こえ方を大きく決めるもの。画面だけの設定も
        case P::voiceOn: case P::algorithm: case P::pitch: case P::formant:
        case P::denoiseOn: case P::denoiseMethod: case P::denoiseMaxReduction:
        case P::fxOn: case P::reverbOn: case P::reverbSize: case P::reverbMix:
        case P::echoOn: case P::echoTimeMs: case P::echoFeedback: case P::echoMix:
        case P::outputOn: case P::outputGain:
        case P::monitorMode: case P::inputChannel: case P::showDynamicsLive: case P::showNoiseEstimate: case P::uiMode:
            return 0;

        // 中（普通から）: 音色や強さを整えるもの
        case P::pitchMethod: case P::worldF0Method: case P::pitchMinHz: case P::pitchMaxHz:
        case P::pvFftSize: case P::dlWindowMs: case P::lpcOrder: case P::fsReferenceHz:
        case P::denoiseFftSize: case P::denoiseStrength:
        case P::denoiseNoiseMode: case P::denoiseLevelThreshold: case P::denoiseRnnThreshold: case P::denoiseMinWindow: case P::denoiseSppSnr: case P::denoiseSaveLearned: case P::denoiseSmoothing: case P::denoiseAttackMs: case P::denoiseReleaseMs:
        case P::cutOn: case P::lowCutOn: case P::lowCutFreq: case P::lowCutSlope: case P::highCutOn: case P::highCutFreq: case P::highCutSlope:
        case P::gateOn: case P::gateThreshold: case P::gateRange: case P::gateMode: case P::gateRatio:
        case P::deessOn: case P::deessFreq: case P::deessThreshold: case P::deessMaxReduction:
        case P::eqOn: case P::eqLowCutOn: case P::eqLowCutFreq: case P::eqLowCutSlope: case P::eqHighCutOn: case P::eqHighCutFreq: case P::eqHighCutSlope:
        case P::eqLowShelfOn: case P::eqLowShelfFreq: case P::eqLowShelfGain: case P::eqLowShelfQ:
        case P::eqPeak1On: case P::eqPeak1Freq: case P::eqPeak1Gain: case P::eqPeak1Q:
        case P::eqPeak2On: case P::eqPeak2Freq: case P::eqPeak2Gain: case P::eqPeak2Q:
        case P::eqHighShelfOn: case P::eqHighShelfFreq: case P::eqHighShelfGain: case P::eqHighShelfQ:
        case P::compOn: case P::compThreshold: case P::compRatio:
        case P::reverbDamping: case P::reverbPreDelayMs: case P::echoDampingHz:
        case P::limiterCeiling:
            return 1;

        // 弱（詳細だけ）: ノッチ、リペア、ブレス、デノイズの倍音と残響、方式ごとの細かいつまみ、時間の設定など
        default:
            return 2;
    }
}

bool isFeatureToggle (P id)
{
    switch (id)
    {
        case P::cutOn: case P::lowCutOn: case P::highCutOn:
        case P::notchOn: case P::notchAutoOn: case P::notchHumOn:
        case P::repairOn: case P::repairDeclipOn: case P::repairPlosiveOn: case P::repairClickOn: case P::repairCrackleOn:
        case P::repairBreathOn: case P::repairTransientOn:
        case P::denoiseOn: case P::denoiseDereverbOn: case P::gateOn: case P::voiceOn: case P::breathOn:
        case P::eqOn: case P::eqLowCutOn: case P::eqHighCutOn: case P::eqLowShelfOn: case P::eqPeak1On: case P::eqPeak2On: case P::eqHighShelfOn:
        case P::deessOn: case P::compOn: case P::fxOn: case P::echoOn: case P::reverbOn: case P::outputOn:
            return true;
        default:
            return false;
    }
}

bool isUiSetting (P id)
{
    switch (id)
    {
        case P::uiMode: case P::inputChannel: case P::monitorMode: case P::showNoiseEstimate: case P::showDynamicsLive: case P::voiceGraph:
            return true;
        default:
            return false;
    }
}

ParamValues applyDisplayMode (ParamValues p)
{
    const int mode = p.index (P::uiMode);
    for (const auto& s : paramSpecs())
        if (paramLevel (s.id) > mode)
            p.set (s.id, isFeatureToggle (s.id) ? 0.0f : s.defaultValue);
    return p;
}

ParamValues::ParamValues()
{
    for (const auto& p : paramSpecs())
        v[static_cast<size_t> (p.id)] = p.defaultValue;
}

} // namespace vcab
