// 帯域分割式（と、帯域分割で下げる部品、くし形フィルタ）
#pragma once

#include "Biquad.h"
#include "DenoiseSettings.h"
#include "NoiseTracker.h"

#include <array>
#include <complex>
#include <vector>

namespace vcab
{

// 帯域分割で下げる部品（ほかのやり方で決めた量を、遅延 0 でかける）。作り:
//   帯域は 32 個で、境目は ERB（聴覚の帯域幅）の等間隔に 100Hz〜16kHz（前は帯域分割式と同じ 20 個）。
//   分け方は「低域通過の差」（帯域 k = LP_k − LP_{k−1}。全部 1 倍なら元の音とぴったり同じで、遅れも出ない）で、
//   低域通過は 2 次のバターワース（前は 4 次）。4 次は境目で位相が 180° 回るので、隣の帯域と逆向きになり、隣どうしの
//   倍率が違うと境目に深い谷ができた（1 倍と 0.25 倍で -11dB。声の区間が 1dB ほど細る原因）。2 次は 90° なので、
//   同じ倍率で境目は 0.59 倍（中間の 0.63 倍とほぼ同じ）。代わりに帯域の分かれ方はゆるやか（12dB/oct）。
//   （Linkwitz-Riley の 4 次＋オールパスで位相をそろえる形も試したが、オールパスの積み重ねで、全部 1 倍でも 100〜200Hz が
//   17〜22ms、500Hz が約 10ms 遅れたのでやめた）
//   立ち上がりの保護（setNoise と setProtection。FFT＋帯域分割のとき）: FFT で決める量は、分析の窓の分だけ反応が遅れ、
//   話し始めの子音を削りやすい。帯域ごとに、遅延 0 の帯域の音の「速い音量」（1ms で上がる）が、ノイズの見積もりより
//   10dB 以上大きく、かつ「ゆっくりの音量」（40ms）より 3dB 以上急に大きくなったら、30ms の間その帯域を開ける
//   （開ける量は、SNR が +9〜+15dB の間でなだらかに。その後 20ms で戻す）。
class BandSynth
{
public:
    static constexpr int kMaxBands = 32;
    void prepare (double sampleRate);
    void reset();
    void setTargets (const float* targets, int rampSamples); // 帯域ごとの倍率（numBands 個）
    // 立ち上がりの保護に使うノイズの見積もり（区画ごとの FFT の大きさの 2 乗と、それを 1 サンプルあたりの 2 乗に直す倍率）
    void setNoise (const float* binNoise, int bins, double binHz, double powerToVariance);
    void setProtection (bool on) noexcept { protect = on; }
    float process (float x) noexcept;
    int bands() const noexcept { return numBands; }
    // 区画ごとの残す割合（と大きさ）を、帯域ごとの倍率にまとめる
    void binsToBands (const float* binGain, const float* binPower, int bins, double binHz, float floorGain, float* out) const;

private:
    double sr = 48000;
    int numBands = 0, rampLeft = 0;
    std::array<Biquad, kMaxBands - 1> split {};   // 境目ごとの低域通過
    std::array<BiquadCoeffs, kMaxBands - 1> splitCoeffs {};
    // 帯域ごとの、区画の周波数での通り方 |LP_k − LP_{k−1}|²（帯域 × 区画。区画の数と間隔が変わったら作り直す）。
    // 2 次の低域通過は裾が広いので、帯域の名目の幅だけで数えると、帯域に入るノイズを少なく見積もってしまう
    const float* weights (int bins, double binHz) const;
    mutable std::vector<float> weightTable;
    mutable int weightBins = 0;
    mutable double weightHz = 0;
    std::array<double, kMaxBands> lo {}, hi {};
    std::array<float, kMaxBands> gain {}, step {};

    // 立ち上がりの保護
    bool protect = false, noiseValid = false;
    std::array<float, kMaxBands> noiseVar {}, fastEnv {}, slowEnv {}, opened {};
    std::array<int, kMaxBands> holdLeft {};
    float fastUp = 0, fastDown = 0, slowCoef = 0, openRelease = 0;
    int holdSamples = 0;
};

// 帯域分割式
class BandDenoiser
{
public:
    static constexpr int kMaxBands = 20;

    void prepare (double sampleRate);
    void reset();
    void process (float* data, int n, const DenoiseSettings& s);
    void analyzeOnly (const float* data, int n); // 学習のときに、選んでいなくても見積もりだけ進める
    NoiseTracker& tracker() noexcept { return noise; }
    const NoiseTracker& tracker() const noexcept { return noise; }
    float snrDb() const noexcept { return voiceSnrDb; } // 300Hz〜4kHz の、ノイズに対する音量（声のありそうさの判断に使う）
    // 表示用: hz でのノイズの見積もり（白色雑音に直したときの 1 サンプルあたりの 2 乗）
    float noiseDensity (double hz, NoiseMode mode) const;

private:
    void analyzeSample (float x) noexcept;
    void endBlock (const DenoiseSettings& s);

    static constexpr int kBlock = 32;
    double sr = 48000;
    int numBands = 0, blockPos = 0;
    std::array<std::array<Biquad, 2>, kMaxBands - 1> split {};   // 足し戻す側の低域通過（4次のバターワース）
    std::array<std::array<Biquad, 2>, kMaxBands> detect {};      // 音量を測る側のバンドパス
    std::array<double, kMaxBands> envCoeff {}, envAttack {};
    std::array<float, kMaxBands> env {}, gainDb {}, gainFrom {}, gainTo {};
    std::array<bool, kMaxBands> gateOpen {};
    std::array<double, kMaxBands> bandLo {}, bandHi {}, bandWidth {}; // bandWidth は測る側のフィルタの等価雑音帯域幅（Hz）
    std::array<double, kMaxBands> envScale {}; // 音量の追いかけ方（上がるとき速い）による偏り。表示のときに割る
    std::array<float, kMaxBands> noiseTotal {};
    float voiceSnrDb = -60;
    NoiseTracker noise;
    LateReverb reverb;
};

// くし形フィルタ（倍音の間を下げる）
class HarmonicComb
{
public:
    void prepare (double sampleRate);
    void reset();
    // hz が 0 のとき、または voiced が false のときは、なめらかに素通しへ戻す
    void process (float* data, int n, float hz, bool voiced, double depthDb);

private:
    static constexpr int kTaps = 4;
    double sr = 48000, period = 0, mix = 0, periodCoeff = 0.01, mixCoeff = 0.01;
    std::vector<float> ring;
    int pos = 0;
    Biquad lowPass[2], highPass[2]; // 4kHz で低い側と高い側に分ける（Linkwitz-Riley の 4 次。足すと大きさは元と同じ）
};

} // namespace vcab
