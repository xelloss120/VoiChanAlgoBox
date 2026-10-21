// デノイズの設定（Engine が設定値から作り、各方式に渡す）
#pragma once

#include "../Choices.h"
#include "../Params.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

struct DenoiseSettings
{
    DenoiseEngine engine = DenoiseEngine::band; // 下げる量を決める部品
    bool bandSynthesis = false; // 決めた量を、帯域分割で今の音にかける（FFT の低遅延の部品と、ノイズを自分で見積もる方式）
    FftRule fftRule = FftRule::logMmse; // FFT の方式の下げる量の決め方
    BandRule rule = BandRule::snr; // 帯域分割式の決め方
    int fftSizeIndex = 1;    // 0 = 256, 1 = 512, 2 = 1024（48kHz のとき。ほかのサンプルレートでは時間が同じになるように）
    NoiseMode noiseMode = NoiseMode::autoLevel; // ノイズの見積もり方
    double levelThresholdDb = 6; // 音量の判定: 全体の音量がノイズの見積もりよりこれだけ大きければ「声あり」
    float rnnThreshold = 0.5f;   // RNNoise の声の判定: 声の確率がこれを超えたら「声あり」
    double minWindowSeconds = 1.6; // 最小値: 一番小さい音を探す長さ
    double sppSnrDb = 15;          // Gerkmann の声の推定: 声があるときの SNR の仮定
    double maxReductionDb = 24;
    double strengthDb = 6;   // ノイズの見積もりに上乗せする量（SNR 式と FFT の方式）
    double thresholdDb = 6;  // ゲート式・エキスパンダー式: ノイズより何 dB 大きければ声とみなすか
    double ratio = 3;        // エキスパンダー式の比率
    double attackMs = 10, releaseMs = 100; // 下げる量の動きの速さ（帯域分割式と、FFT の方式で時間でならすとき）
    int pitchMethod = 1;     // 倍音を意識する方式のピッチ検出（PitchDetector::Method）
    double combDepthDb = 9;  // くし形フィルタの深さ（倍音と倍音のちょうど間で下げる量）
    double protect = 0.7;    // 倍音の保護の強さ（0〜1）。倍音の位置の残す割合を、残す割合の (1 − protect) 乗にする
    Smoothing smoothing = Smoothing::time; // 下げる量のならし方（FFT の方式）
    double smoothWidthHz = 200; // 周波数でならす幅
    Presence presence = Presence::off; // 声のありそうさでの調整（FFT の方式）
    bool comfort = false;    // 残ったノイズの音色をそろえる（コンフォートノイズ。FFT で戻す方式）
    bool dereverb = false;   // 残響の抑制（ノイズを自分で見積もる方式）
    double rt60 = 0.5;       // 残響の長さ（秒。60dB 下がるまで）
    bool dfnPostFilter = false, dfnLowLatency = false; // DeepFilterNet の後段のフィルタと、低遅延のモデル
    bool onsetProtect = true; // FFT＋帯域分割で、立ち上がりの保護（FFT の反応の遅れの間、急に大きくなった帯域を開ける）
    const ParamValues* params = nullptr; // ピッチ検出の探す範囲（ボイチェンのタブの値）

    // 設定値から作る（処理と、画面でどのつまみを出すかの判断の両方で使う）
    static DenoiseSettings fromParams (const ParamValues& p)
    {
        DenoiseSettings s;
        const auto& method = denoiseMethodInfo (p.index (P::denoiseMethod));
        s.engine = method.engine;
        s.bandSynthesis = method.bandSynthesis;
        s.fftRule = static_cast<FftRule> (std::clamp (p.index (P::denoiseFftRule), 0, static_cast<int> (FftRule::count) - 1));
        s.rule = choiceAs (p.index (P::denoiseRule), BandRule::snr);
        s.fftSizeIndex = p.index (P::denoiseFftSize);
        s.noiseMode = choiceAs (p.index (P::denoiseNoiseMode), NoiseMode::learnedRnnoise);
        s.levelThresholdDb = p.get (P::denoiseLevelThreshold);
        s.rnnThreshold = p.get (P::denoiseRnnThreshold) * 0.01f;
        s.minWindowSeconds = std::round (p.get (P::denoiseMinWindow) * 10.0) / 10.0; // 0.1 秒刻み（1.6 秒は、測った値そのものを使うように）
        s.sppSnrDb = p.get (P::denoiseSppSnr);
        s.maxReductionDb = p.get (P::denoiseMaxReduction);
        s.strengthDb = p.get (P::denoiseStrength);
        s.thresholdDb = p.get (P::denoiseThreshold);
        s.ratio = p.get (P::denoiseRatio);
        s.attackMs = p.get (P::denoiseAttackMs);
        s.releaseMs = p.get (P::denoiseReleaseMs);
        s.pitchMethod = p.index (P::denoisePitchMethod);
        s.combDepthDb = p.get (P::denoiseCombDepth);
        s.protect = p.get (P::denoiseProtect) * 0.01;
        s.smoothing = choiceAs (p.index (P::denoiseSmoothing), Smoothing::cepstrum);
        s.smoothWidthHz = p.get (P::denoiseSmoothWidth);
        s.presence = choiceAs (p.index (P::denoisePresence), Presence::cohen);
        s.comfort = p.on (P::denoiseComfort);
        s.dereverb = p.on (P::denoiseDereverbOn);
        s.rt60 = p.get (P::denoiseRt60);
        s.dfnLowLatency = p.index (P::denoiseDfnModel) == 1;
        s.dfnPostFilter = p.on (P::denoiseDfnPostFilter);
        s.onsetProtect = p.on (P::denoiseOnset);
        s.params = &p;
        return s;
    }

    bool isFftFamily() const noexcept { return engine == DenoiseEngine::fft || engine == DenoiseEngine::fftLowLatency; }
    bool estimatesNoiseItself() const noexcept
    {
        return engine == DenoiseEngine::rnnoise || engine == DenoiseEngine::deepFilter || engine == DenoiseEngine::webrtc;
    }
    // 倍音を意識する方式（くし形フィルタと、FFT の倍音の保護）は、声の高さを求める
    bool needsPitch() const noexcept { return usesPitch (engine, fftRule); }
    static bool usesPitch (DenoiseEngine e, FftRule r) noexcept
    {
        return e == DenoiseEngine::comb || ((e == DenoiseEngine::fft || e == DenoiseEngine::fftLowLatency) && r == FftRule::wienerProtect);
    }
};

} // namespace vcab
