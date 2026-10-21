// フェーズボコーダ（ビン移動式）
//
// 1. 窓の長さ N の 1/4 ずつずらしながら FFT で周波数成分（ビン）に分ける。
// 2. 前の窓からの位相の進みから、各ビンの本当の周波数を求める（ビンの中心からのずれも分かる）。
// 3. 各ビンを「元の位置 × ピッチの倍率」に移し、周波数も倍率倍にして、位相を進め直す。
// 4. 逆 FFT して、窓をかけて重ね足す。
//
// 位相ロック: 倍音の山とその周り（隣の山との谷まで）を、形を崩さずにまとめて移し、周りのビンの位相は山の位相に
//             そろえる（Laroche と Dolson の方法）。ビンごとにばらばらに位相を進めると、山の形が崩れて
//             こもった響き（位相の乱れ）になるのを減らす。
// フォルマント処理: 移す前のスペクトルから包絡（声色の形）を求め、移した後のスペクトルから「ピッチと一緒に動いた包絡」を
//             取り除いて、「フォルマントの倍率で動かした包絡」をかけ直す。なしのときは、フォルマントはピッチと一緒に動く。
//
// 遅延は窓の長さの 3/4（1024 なら 48kHz で約16ms）。
#pragma once

#include "../Dsp/Fft.h"
#include "../Dsp/FormantWarp.h"
#include "../Dsp/SpectralEnvelope.h"
#include "VoiceAlgorithm.h"

namespace vcab
{

class PhaseVocoder : public VoiceAlgorithm
{
public:
    bool supportsFormant (const ParamValues& p) const override;

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process (float* data, int n, const ParamValues& params) override;
    int latencySamples (const ParamValues& params) const override;

    static int fftSizeFor (const ParamValues& p);

private:
    void configure (int fftSize);
    void processFrame (const ParamValues& params);

    double sr = 48000;
    int size = 0, half = 0, hop = 0, latency = 0;

    std::vector<float> inFifo, outFifo;
    std::vector<double> outAccum, window;
    int rover = 0;

    Fft fft;
    SpectralEnvelope envelope;
    FormantWarp warp;
    std::vector<Fft::Complex> spec;
    std::vector<double> mag, phase, freq, lastPhase;      // 分析側
    std::vector<double> outMag, outPhase, outFreq, lastOutPhase; // 合成側
    std::vector<double> env;
    std::vector<int> peaks;
};

} // namespace vcab
