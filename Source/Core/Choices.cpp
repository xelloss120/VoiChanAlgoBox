#include "Choices.h"

#include <algorithm>
#include <iterator>

namespace vcab
{

namespace
{
using C = VoiceCore;
using E = Envelope;

// 遅延の目安: ディレイライン系は窓の長さ 30ms の半分、WSOLA は 40ms の半分、PSOLA 系は探す範囲の下限 80Hz の周期の 2 倍、
// フェーズボコーダと正弦波モデルは FFT 1024 の 3/4、チャンネルボコーダは帯域のフィルタの遅れ、
// 声帯の音を作り直すボコーダは LPC の窓の半分 + 係数を求め直す間隔の半分、WORLD は 4 フレーム + 後ろの余裕 + 合成器
const AlgorithmInfo kAlgorithms[] = {
    { "ディレイライン式",             "約15ms", 0, C::delayLine,      E::none },
    { "WSOLA 式",                     "約20ms", 0, C::wsola,          E::none },
    { "TD-PSOLA",                     "約25ms", 0, C::psola,          E::none },
    { "フェーズボコーダ",             "約16ms", 1, C::phaseVocoder,   E::none },
    { "正弦波モデル",                 "約16ms", 1, C::sinusoidal,     E::none },
    { "周波数シフト",                 "約0ms",  2, C::frequencyShift, E::none },
    { "帯域ごとの周波数シフト",       "約1ms",  2, C::bandShift,      E::none },
    { "LPC＋ディレイライン",          "約15ms", 3, C::delayLine,      E::lpc },
    { "LPC＋PSOLA",                   "約25ms", 3, C::psola,          E::lpc },
    { "ケプストラム＋ディレイライン", "約15ms", 3, C::delayLine,      E::cepstrum },
    { "ケプストラム＋PSOLA",          "約25ms", 3, C::psola,          E::cepstrum },
    { "メルケプ＋ディレイライン",     "約15ms", 3, C::delayLine,      E::melCepstrum },
    { "メルケプ＋PSOLA",              "約25ms", 3, C::psola,          E::melCepstrum },
    { "チャンネルボコーダ",           "約5ms",  4, C::channelVocoder, E::none },
    { "LPC ボコーダ",                 "約16ms", 4, C::sourceExciter,  E::lpc },
    { "ケプストラムボコーダ",         "約16ms", 4, C::sourceExciter,  E::cepstrum },
    { "メルケプボコーダ",             "約16ms", 4, C::sourceExciter,  E::melCepstrum },
    { "WORLD",                        "約84ms", 5, C::world,          E::none },
};
static_assert (std::size (kAlgorithms) == static_cast<size_t> (Algorithm::count));

using D = DenoiseEngine;
const char* const kAiBand = "AI の量を帯域でかける。遅延 0、語頭は遅れ気味。";
const DenoiseMethodInfo kDenoiseMethods[] = {
    { "帯域分割式",                    "遅延 0ms",        "遅延 0。20 の帯域ごとに下げる。",              0, D::band,          false },
    { "くし形フィルタ＋帯域分割",      "遅延 0ms",        "さらに倍音の間も下げる。遅延 0。",            0, D::comb,          false },
    { "FFT",                           "窓 512 で約11ms", "細かく下げられるが、窓の分だけ遅れる。",      1, D::fft,           false },
    { "FFT＋FIR",                      "遅延 0ms",        "FFT で決めた量をフィルタでかける。遅延 0。",  1, D::fftLowLatency, false },
    { "FFT＋帯域分割",                 "遅延 0ms",        "FFT で決めた量を 32 帯域でかける。遅延 0。",  1, D::fftLowLatency, true },
    { "WebRTC NS",                     "約17ms",          "ビデオ通話のノイズ抑制（AI ではない）。約 17ms。", 1, D::webrtc,  false },
    { "WebRTC NS＋帯域分割",           "遅延 0ms",        "WebRTC の量を帯域でかける。遅延 0。",         1, D::webrtc,        true },
    { "RNNoise",                       "約30ms",          "AI が下げる量を決める。遅延は約 30ms。",      2, D::rnnoise,       false },
    { "RNNoise＋帯域分割",             "遅延 0ms",        kAiBand,                                        2, D::rnnoise,       true },
    { "DeepFilterNet",                 "約40ms",          "AI が細かく下げる。遅延は約 40ms。",          2, D::deepFilter,    false },
    { "DeepFilterNet＋帯域分割",       "遅延 0ms",        kAiBand,                                        2, D::deepFilter,    true },
};
static_assert (std::size (kDenoiseMethods) == static_cast<size_t> (DenoiseMethod::count));

template <typename T, size_t N>
const T& clampedAt (const T (&table)[N], int index)
{
    return table[static_cast<size_t> (std::clamp (index, 0, static_cast<int> (N) - 1))];
}
} // namespace

const AlgorithmInfo& algorithmInfo (Algorithm a) { return clampedAt (kAlgorithms, static_cast<int> (a)); }
const AlgorithmInfo& algorithmInfo (int index) { return clampedAt (kAlgorithms, index); }

const std::vector<const char*>& algorithmSections()
{
    static const std::vector<const char*> s { "時間領域（波形を並べ直す）", "周波数領域（周波数成分を動かす）", "周波数シフト（全体をずらす）",
                                              "声色と声帯の音に分ける（残差の高さを変える）", "ボコーダ（声帯の音を作り直す）", "分析合成" };
    return s;
}

const DenoiseMethodInfo& denoiseMethodInfo (DenoiseMethod m) { return clampedAt (kDenoiseMethods, static_cast<int> (m)); }
const DenoiseMethodInfo& denoiseMethodInfo (int index) { return clampedAt (kDenoiseMethods, index); }

const std::vector<const char*>& denoiseSections()
{
    static const std::vector<const char*> s { "帯域分割（遅延 0）", "FFT", "AI" };
    return s;
}

const std::vector<const char*>& pitchSections()
{
    static const std::vector<const char*> s { "波形の重なり（時間領域）", "スペクトル（周波数領域）", "LPC", "WORLD" };
    return s;
}

int pitchSectionOf (int method)
{
    // PitchDetector::Method の順番: 自己相関, YIN, pYIN, McLeod, RAPT | ケプストラム, SWIPE | SIFT | DIO, Harvest
    return method <= 4 ? 0 : method <= 6 ? 1 : method == 7 ? 2 : 3;
}

} // namespace vcab
