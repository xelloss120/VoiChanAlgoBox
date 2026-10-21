// FFT の方式（FftDenoiser: FFT で戻す）と、FFT＋FIR / FFT＋帯域分割（LowLatencyDenoiser: 今の音にフィルタでかける）
#pragma once

#include "BandDenoise.h"
#include "DenoiseSettings.h"
#include "Fft.h"
#include "NoiseTracker.h"

#include <cstdint>
#include <vector>

namespace vcab
{

// FFT で決めた、区画ごとの残す割合（スペクトル減算 / ウィーナー＋判定指向型）
class SpectralGain
{
public:
    void prepare (int bins, double hopSeconds, double binHz);
    void reset();
    enum Rule { subtraction = 0, wiener, mmseStsa, logMmse, berouti, multiband, tsnr, masking };
    // 設定の決め方（DenoiseSettings::fftRule）から。倍音の再生と保護は、ウィーナーの後に別の処理をかける
    static Rule ruleFor (FftRule fftRule) noexcept
    {
        switch (fftRule)
        {
            case FftRule::subtraction: return subtraction;
            case FftRule::mmseStsa:    return mmseStsa;
            case FftRule::logMmse:     return logMmse;
            case FftRule::berouti:     return berouti;
            case FftRule::multiband:   return multiband;
            case FftRule::tsnr:        return tsnr;
            case FftRule::masking:     return masking;
            default:                   return wiener;
        }
    }
    // power から gain を出す
    void compute (const float* power, const float* noise, Rule rule, const DenoiseSettings& s, float* gain);
    // 声のありそうさでの調整（compute の後に呼ぶ）: G^p × 下限^(1−p)
    void applyPresence (float* gain, const DenoiseSettings& s);

private:
    int bins = 0, numBark = 1;
    double ddAlpha = 0.98, zetaAlpha = 0.7, binHz = 93.75;
    std::vector<float> prevClean; // 直前のフレームの、声の SNR の見積もり（判定指向型）
    std::vector<float> xiNow, vNow; // 今のフレームの、前もっての SNR と v（声のありそうさに使う）
    std::vector<float> zeta, absence; // Cohen の声がない確率（ならした ξ と、区画ごとの q）
    std::vector<double> work;
    // 聴覚のマスキング（バークの帯域）
    std::vector<int> bark, barkCount;
    std::vector<double> barkEnergy, barkThreshold, spreading;
};

// 下げる量のならし方（区画ごとの残す割合を、その場で書き換える）
class GainSmoother
{
public:
    void prepare (double sampleRate, int fftSize, int hopSize);
    void reset();
    void apply (float* gain, const DenoiseSettings& s);

private:
    double sr = 48000, hopSeconds = 0.005;
    int size = 512, bins = 257;
    bool first = true;
    Fft fft;
    std::vector<Fft::Complex> cep;
    std::vector<float> prevDb;
    std::vector<double> cepSmooth, beta, work;
};

// FFT スペクトル減算 / FFT ウィーナー（＋倍音の再生 / 倍音の保護）
class FftDenoiser
{
public:
    void prepare (double sampleRate, int size);
    void setPitch (float hz, bool voiced) noexcept { pitchHz = hz; pitchVoiced = voiced; }
    void reset();
    void process (float* data, int n, const DenoiseSettings& s);
    void analyzeOnly (const float* data, int n);
    int latencySamples() const noexcept { return size; }
    NoiseTracker& tracker() noexcept { return noise; }
    const NoiseTracker& tracker() const noexcept { return noise; }
    float snrDb() const noexcept { return voiceSnrDb; }
    float noiseDensity (double hz, NoiseMode mode) const;

private:
    void frame (bool synthesize, const DenoiseSettings* s);
    const float* noiseFor (const DenoiseSettings& s);


    double sr = 48000;
    int size = 512, hop = 256, inPos = 0, outPos = 0;
    float pitchHz = 0;
    bool pitchVoiced = false;
    uint32_t comfortState = 0x2468ace1u; // コンフォートノイズの位相（毎回同じ並び）
    Fft fft;
    std::vector<double> window;
    std::vector<float> ring, acc, outFifo, power, gain, noiseTotal;
    std::vector<Fft::Complex> spec, work;
    float voiceSnrDb = -60;
    double windowEnergy = 1;
    NoiseTracker noise;
    LateReverb reverb;
    SpectralGain rule;
    GainSmoother smoother;
};

// FFT＋FIR / FFT＋帯域分割（下げる量は FFT で決め、今の音には最小位相の FIR か帯域分割でかける）
class LowLatencyDenoiser
{
public:
    void prepare (double sampleRate, int size);
    void reset();
    void process (float* data, int n, const DenoiseSettings& s);
    void analyzeOnly (const float* data, int n);
    NoiseTracker& tracker() noexcept { return noise; }
    const NoiseTracker& tracker() const noexcept { return noise; }
    float snrDb() const noexcept { return voiceSnrDb; }
    float noiseDensity (double hz, NoiseMode mode) const;
    void setPitch (float hz, bool voiced) noexcept { pitchHz = hz; pitchVoiced = voiced; }

private:
    void analyze (const DenoiseSettings* s);
    void designFilter();
    const float* noiseFor (const DenoiseSettings& s);

    int size = 512, hop = 128, taps = 128, inPos = 0, sinceHop = 0, histPos = 0, fadePos = 0;
    Fft fft;
    std::vector<double> window;
    static constexpr int kFadeStep = 16;
    std::vector<float> ring, power, gain, hist, hOld, hNew, hCur, noiseTotal;
    std::vector<Fft::Complex> spec, cep, work;
    float pitchHz = 0;
    bool pitchVoiced = false;
    BandSynth synth;     // FFT＋帯域分割のとき
    bool bandMode = false;
    double sr = 48000, windowEnergy = 1;
    float voiceSnrDb = -60;
    NoiseTracker noise;
    LateReverb reverb;
    SpectralGain rule;
    GainSmoother smoother;
};

} // namespace vcab
