// 方式の選択肢と、その性質の表。選択肢の番号（保存する値）はここの列挙の順番で、画面の並び（見出しのまとまり）と同じ。
// Params の選択肢の名前、遅延の目安、選択肢ごとの説明、処理の振り分け、画面でどのつまみを出すかは、この表から決める。
// JUCE には依存しない。
#pragma once

#include <vector>

namespace vcab
{

// ---- ボイチェンの方式 ----
enum class Algorithm
{
    // 時間領域
    delayLine, wsola, tdPsola,
    // 周波数領域
    phaseVocoder, sinusoidal,
    // 周波数シフト
    frequencyShift, bandShift,
    // 声色と声帯の音に分ける（声帯の音の高さを変える部品 × 声色の求め方）
    lpcDelayLine, lpcPsola, cepstrumDelayLine, cepstrumPsola, melCepstrumDelayLine, melCepstrumPsola,
    // ボコーダ（声帯の音を作り直す）
    channelVocoder, lpcVocoder, cepstrumVocoder, melCepstrumVocoder,
    // 分析合成
    world,
    count
};

// 方式の中心になる部品
enum class VoiceCore { delayLine, wsola, psola, phaseVocoder, sinusoidal, frequencyShift, bandShift, channelVocoder, sourceExciter, world };
// 声色の求め方（none は声色を分けない。それ以外は、LpcVoice が声色と声帯の音に分けて、中心の部品に声帯の音を渡す）
enum class Envelope { none, lpc, cepstrum, melCepstrum };

struct AlgorithmInfo
{
    const char* name;
    const char* latencyHint; // 初期設定のときの遅延の目安
    int section;             // 画面の見出しの番号（algorithmSections）
    VoiceCore core;
    Envelope envelope;

    bool usesPitchDetection() const noexcept
    {
        return core == VoiceCore::psola || core == VoiceCore::channelVocoder || core == VoiceCore::sourceExciter || core == VoiceCore::world;
    }
    bool isVocoder() const noexcept { return core == VoiceCore::channelVocoder || core == VoiceCore::sourceExciter; }
};

const AlgorithmInfo& algorithmInfo (Algorithm a);
const AlgorithmInfo& algorithmInfo (int index); // 範囲外は丸める
const std::vector<const char*>& algorithmSections();

// ---- デノイズの方式 ----
enum class DenoiseMethod
{
    // 帯域分割
    band, comb,
    // FFT
    fft, fftFir, fftBand, webrtc, webrtcBand,
    // AI
    rnnoise, rnnoiseBand, deepFilter, deepFilterBand,
    count
};

// 下げる量を決める部品
enum class DenoiseEngine
{
    band,        // 帯域分割式（BandDenoiser）
    comb,        // 帯域分割式 ＋ くし形フィルタ
    fft,         // FFT で分析して FFT で戻す（FftDenoiser）
    fftLowLatency, // FFT で分析し、今の音には FIR か帯域分割でかける（LowLatencyDenoiser）
    rnnoise, deepFilter, webrtc // ノイズを自分で見積もる方式（AiDenoiser の枠）
};

struct DenoiseMethodInfo
{
    const char* name;
    const char* latencyHint;
    const char* description; // 選んでいる方式の説明（ツールチップに足す）
    int section;             // 画面の見出しの番号（denoiseSections）
    DenoiseEngine engine;
    bool bandSynthesis;      // 決めた量を、帯域分割で今の音にかける（遅延 0）

    bool estimatesNoiseItself() const noexcept // AI と WebRTC NS（ノイズの見積もりや、つまみの多くを使わない）
    {
        return engine == DenoiseEngine::rnnoise || engine == DenoiseEngine::deepFilter || engine == DenoiseEngine::webrtc;
    }
    bool isBandSplit() const noexcept { return engine == DenoiseEngine::band || engine == DenoiseEngine::comb; }
    bool isFftFamily() const noexcept { return engine == DenoiseEngine::fft || engine == DenoiseEngine::fftLowLatency; }
};

const DenoiseMethodInfo& denoiseMethodInfo (DenoiseMethod m);
const DenoiseMethodInfo& denoiseMethodInfo (int index); // 範囲外は丸める
const std::vector<const char*>& denoiseSections();

// FFT の下げる量の決め方（denoise_fft_rule）
enum class FftRule { subtraction, wiener, mmseStsa, logMmse, wienerRegen, wienerProtect, berouti, multiband, tsnr, masking, count };

// ピッチ検出の方式（pitch_method。番号は PitchDetector::Method）の見出し
const std::vector<const char*>& pitchSections();
int pitchSectionOf (int method);

// デノイズの帯域分割式の決め方（denoise_rule）
enum class BandRule { gate, expander, snr };
// FFT の方式の、下げる量のならし方（denoise_smoothing）
enum class Smoothing { none, time, frequency, cepstrum };
// FFT の方式の、声のありそうさでの調整（denoise_presence）
enum class Presence { off, fixedHalf, cohen };
// ノイズの見積もり方（denoise_noise_mode）。自動の追い方 4 つ、学習（固定）、学習した値から始める自動 3 つ
enum class NoiseMode { autoMinimum, autoLevel, autoGerkmann, autoRnnoise, learned, learnedLevel, learnedGerkmann, learnedRnnoise };
// 見積もりの追い方（NoiseMode から決まる）
enum class NoiseTracking { minimum, level, gerkmann, rnnoise, fixed };
inline NoiseTracking noiseTracking (NoiseMode m) noexcept
{
    switch (m)
    {
        case NoiseMode::autoLevel:
        case NoiseMode::learnedLevel:    return NoiseTracking::level;
        case NoiseMode::autoGerkmann:
        case NoiseMode::learnedGerkmann: return NoiseTracking::gerkmann;
        case NoiseMode::autoRnnoise:
        case NoiseMode::learnedRnnoise:  return NoiseTracking::rnnoise;
        case NoiseMode::learned:         return NoiseTracking::fixed;
        default:                         return NoiseTracking::minimum;
    }
}
inline bool usesLearning (NoiseMode m) noexcept { return static_cast<int> (m) >= static_cast<int> (NoiseMode::learned); } // 学習のボタンを使う
inline bool startsFromLearned (NoiseMode m) noexcept { return usesLearning (m) && m != NoiseMode::learned; }          // 学習した値から始める自動
// ゲートの動き方（gate_mode）
enum class GateMode { gate, expander };
// リペアの直し方（repair_transient_mode、repair_click_mode）
enum class RepairFix { lower, interpolate };

// 選択欄の値（番号）を列挙に。範囲外は last までに丸める
template <typename E>
E choiceAs (int index, E last) noexcept
{
    return static_cast<E> (index < 0 ? 0 : index > static_cast<int> (last) ? static_cast<int> (last) : index);
}

// ボイチェンのタブのグラフ（voice_graph）
enum class VoiceGraph { pitch, formant, both, hidden };

// 表示のモード（ui_mode）。項目ごとの音への影響の度合い（Params の paramLevel）が、この番号以下なら見せる
enum class UiMode { easy, normal, detail };

} // namespace vcab
