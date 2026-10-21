// 帯域ごとの周波数シフト
//
// 1. ヒルベルト変換の近似で、音を 90 度ずれた2本（I と Q）にする。
// 2. I と Q の両方を、同じバンドパスで帯域（60Hz〜16kHz を対数で等分）に分ける。同じフィルタを通すので、
//    帯域ごとにも I と Q は 90 度ずれたまま。
// 3. 帯域ごとに「中心周波数 × (倍率 - 1)」Hz だけずらして（I cos + Q sin）、足し戻す。
//    周波数に比例した量をずらすので、全体としては倍率をかける（ピッチシフト）に近づく。ただし帯域の中では
//    同じ Hz だけずれるので、倍音の並びが少し崩れる。帯域が細かいほど崩れは小さい。
// 4. フォルマント処理: 帯域をずらすと、声色の形（包絡）も一緒にずれる。ずらした先で「ずれた包絡」を外し、
//    「フォルマントの倍率で動かした包絡」をかけ直す。包絡の求め方は5つから選べる。
//      なし           フォルマントはピッチと一緒に動く
//      帯域の大きさ   帯域ごとの大きさを、そのまま包絡とみなす（遅れがない）
//      ケプストラム / LPC 包絡 / True Envelope   フェーズボコーダと同じ部品。FFT は分析だけに使い、音は通さないので
//                     遅延は増えないが、包絡の変化が少し遅れてついてくる
//
// FFT を音の通り道に使わないので、遅延はフィルタの分（数 ms）だけ。
#pragma once

#include "../Dsp/Biquad.h"
#include "../Dsp/Fft.h"
#include "../Dsp/FormantWarp.h"
#include "../Dsp/Hilbert.h"
#include "../Dsp/SpectralEnvelope.h"
#include "VoiceAlgorithm.h"

#include <array>
#include <complex>

namespace vcab
{

class BandShifter : public VoiceAlgorithm
{
public:
    enum EnvelopeMethod { envNone = 0, envBands, envCepstrum, envLpc, envTrueEnvelope };

    bool supportsFormant (const ParamValues& p) const override { return p.index (P::bsFormantMethod) != envNone; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process (float* data, int n, const ParamValues& params) override;
    int latencySamples (const ParamValues& params) const override;

    static constexpr int kMaxBands = 100;

private:
    struct Band
    {
        Biquad filterI[2], filterQ[2];  // 2段重ねのバンドパス（I と Q に同じもの）
        double centre = 0;              // 中心周波数（Hz）
        std::complex<double> rotor { 1, 0 }, step { 1, 0 }; // ずらすための回転（1サンプルごとに step をかける）
        double energy = 0;              // 帯域の大きさ（2乗の平均）
        double gain = 1, gainTarget = 1;
    };

    void configure (int count);
    void updateGains (const ParamValues& params, int method, double ratio, double formantRatio);
    double bandEnvelopeAt (double hz) const; // 帯域の大きさを、周波数の位置で読む（対数で補間）
    double fftEnvelopeAt (double hz) const;  // FFT で求めた包絡を、周波数の位置で読む
    void analyzeFft (int method);

    double sr = 48000;
    HilbertPair hilbert;
    std::array<Band, kMaxBands> bands {};
    int numBands = 0;
    double bandRatio = 1.1, makeup = 1;
    int sinceGains = 0;

    // ケプストラム / LPC 包絡 / True Envelope のための FFT（分析だけ）
    static constexpr int kFftSize = 1024, kFftHop = 256;
    Fft fft;
    SpectralEnvelope envelope;
    FormantWarp warp;
    std::vector<float> fftInput;
    int fftPos = 0, sinceFft = 0;
    std::vector<Fft::Complex> spec;
    std::vector<double> mag, env, window;
    bool envReady = false;
};

} // namespace vcab
