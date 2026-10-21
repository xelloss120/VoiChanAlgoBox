// 正弦波モデル（McAulay-Quatieri）と、雑音成分（SMS: 正弦波＋雑音モデル）
//
// 1. フェーズボコーダと同じく、窓の 1/4 ずつずらして FFT し、振幅スペクトルの山（倍音など）を見つける。
//    山の周波数は、前の窓からの位相の進みから精密に求める。
// 2. 前の窓の山と周波数が近いものをつないで「線」として追いかける。つながらなかった線は消え（音を絞る）、
//    新しい山からは線が生まれる（音を上げる）。
// 3. 各線を、周波数をピッチの倍率倍にした正弦波で鳴らし直す。窓と窓の間は、大きさと周波数を直線でつなぐ。
//    フォルマント処理を選んだときは、包絡から各正弦波の大きさを決め直して、フォルマントを別に動かす。
// 4. 雑音成分: スペクトルから山の部分を取り除いた残り（息やサ行の子音）の形を、ランダムな位相の雑音で鳴らす。
//    オフにすると正弦波だけになり、息やサ行が消えて「ヒューヒュー」「水っぽい」音になる。
//
// 遅延は窓の長さの 3/4（フェーズボコーダと同じ）。
#pragma once

#include "../Dsp/Fft.h"
#include "../Dsp/FormantWarp.h"
#include "../Dsp/SpectralEnvelope.h"
#include "VoiceAlgorithm.h"

#include <array>
#include <cstdint>

namespace vcab
{

class SinusoidalModel : public VoiceAlgorithm
{
public:
    bool supportsFormant (const ParamValues& p) const override;

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process (float* data, int n, const ParamValues& params) override;
    int latencySamples (const ParamValues& params) const override;

private:
    static constexpr int kMaxTracks = 256;

    struct Track
    {
        bool active = false;
        bool matched = false;
        double freq = 0, amp = 0;          // 前の窓での値（周波数はビンの単位、元のピッチ）
        double newFreq = 0, newAmp = 0;    // 今の窓での値
        double outAmpStart = 0, outAmpEnd = 0; // 鳴らす大きさ（フォルマントの補正込み）
        double phase = 0;                  // 鳴らしている正弦波の位相
    };

    void configure (int fftSize);
    void processFrame (const ParamValues& params);
    void synthesizeNoise (const ParamValues& params, int envMethod, double formantRatio, double ratio);
    Fft::Complex windowKernel (double delta) const noexcept; // ハン窓をかけた正弦波のスペクトルの形（中心から delta ビン）

    double sr = 48000;
    int size = 0, half = 0, hop = 0, latency = 0;

    std::vector<float> inFifo, outFifo;
    std::vector<double> outAccum, window;
    int rover = 0;

    Fft fft;
    SpectralEnvelope envelope;
    FormantWarp warp;
    std::vector<Fft::Complex> spec, residualSpec;
    std::vector<double> mag, phase, freq, lastPhase, env, residual, smoothed;
    std::vector<int> peaks;
    std::array<Track, kMaxTracks> tracks {};
    std::uint32_t noiseSeed = 12345;
};

} // namespace vcab
