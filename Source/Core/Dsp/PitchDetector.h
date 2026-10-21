// ピッチ検出（声の高さと、有声音か無声音かの判定）
//
// 音を少しずつ受け取り、5ms ごとに直近の窓を調べる。窓の長さは、探す範囲の下限の周期2つ分
// （下限 60Hz なら約33ms）。声の高さを探すだけなら高い音は要らないので、低域通過フィルタをかけてから
// 約 12kHz に間引いて計算し、負荷を下げている。
//
// 方式（番号の順番は Method を参照）:
//   自己相関     波形を自分自身とずらして重ね、一番よく重なるずれ（周期）を探す。一番シンプルだが、
//                周期の2倍のずれでも同じくらい重なるので、1オクターブ下に間違えやすい。
//   YIN          ずらしたときの差の大きさを、それまでの平均で割って比べる（自己相関の改良版）。
//                しきい値を下回る最初の谷を選ぶので、オクターブの間違いが少ない。
//   McLeod(MPM)  正規化した自己相関の山のうち、一番高い山に近い高さの「最初の山」を選ぶ。
//                短い窓でも安定しやすい。
//   ケプストラム 対数の振幅スペクトルを、もう一度フーリエ変換する。倍音が等間隔に並ぶ周期が山になる。
//   SWIPE        候補の高さの倍音の位置に山を持つ「くし」と、スペクトルの重なり具合で選ぶ（PitchSwipe.cpp）。
//                窓が長い（下限の周期の 8 つ分）ので、ほかの方式より遅れて分かる。
//   RAPT         正規化した相互相関の山をいくつか候補に残し、前後のフレームとのつながり（動的計画法）で
//                一番自然な並びを選ぶ（PitchRapt.cpp）。少し先まで見てから決めるので、その分遅れる。
//   DIO          WORLD の速い方式。いくつかの帯域で、0 を横切る間隔などから求める（PitchWorld.cpp）。
//                20ms ごとに、前後に余裕を持たせた区間をまとめて分析するので、約 60ms 遅れる。
//   Harvest      WORLD の精度の高い方式（PitchWorld.cpp、HarvestStream.h）。先の音も使う作りなので、
//                約 80ms 遅れる。
//   SIFT         LPC の逆フィルタで低いフォルマントを取り除いてから、自己相関で探す（PitchSift.cpp）。
//   pYIN         YIN の谷を、しきい値を変えながら確率つきの候補として残し、隠れマルコフモデルで前後のつながりが
//                一番もっともらしい並びを選ぶ（PitchPyin.cpp）。少し先まで見てから決めるので、その分遅れる。
#pragma once







#include "../Params.h"
#include "Biquad.h"
#include "Fft.h"
#include "HarvestStream.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace vcab
{

struct PitchEstimate
{
    float hz = 0;          // 声の高さ（無音のときは 0）
    bool voiced = false;   // 有声音か
    float clarity = 0;     // 確からしさ（0〜1。方式ごとに意味は少し違う）
    float levelDb = -120;  // 窓の中の音量
};

class PitchDetector
{
public:
    // 番号は pitch_method の値（画面の見出しのまとまりの順: 波形の重なり / スペクトル / LPC / WORLD）
    enum Method { autocorrelation = 0, yin, pyin, mcleod, rapt, cepstrum, swipe, sift, dio, harvest, numMethods };

    void prepare (double sampleRate);
    void reset();

    // 音を渡す。5ms ごとに検出をやり直し、latest() が新しくなる
    void push (const float* x, int n, const ParamValues& params);
    void setMethod (int m) noexcept { method = m; }

    const PitchEstimate& latest() const noexcept { return estimate; }

    // 検出をやり直した回数（新しい結果が出たかを知るため）
    int analysisCount() const noexcept { return count; }

    // 窓の中心が、今からどれだけ前か（元のサンプルレートのサンプル数）。結果はこの時点の声の高さ
    int analysisDelaySamples() const noexcept { return delaySamples; }

    // 窓の長さ（元のサンプルレートのサンプル数）。遅延の計算に使う
    static int windowSamples (double sampleRate, double minHz) { return static_cast<int> (2.0 * sampleRate / minHz); }

    // この設定のとき、窓の中心が今からどれだけ前になるか（元のサンプルレートのサンプル数。作る前に計算できる版）
    static int analysisDelayFor (double sampleRate, const ParamValues& params);

    static constexpr int kPyinLookahead = 2;   // pYIN が何フレーム先まで見てから決めるか（10ms。0〜16 で試して一番よかった）
    static constexpr int kRaptLookahead = 2;   // RAPT が何フレーム先まで見てから決めるか（10ms。評価では、長くしても良くならなかった）

private:
    void configure (double minHz, double maxHz, int forMethod);
    void configureSwipe (double minHz, double maxHz);
    static int swipeLongestWindow (double fsDecimated, double minHz);
    // RAPT の相関の窓。get_f0 の 7.5ms と、下限の周期の 0.75 倍の長い方（低い声で窓が1周期より短いと、
    // 周期でないずれでも波形がよく重なってしまい、間違えやすかったため）
    static int raptWindow (double fsDecimated, double minHz) { return static_cast<int> (std::lround (std::max (0.0075 * fsDecimated, 0.75 * fsDecimated / minHz))); }
    void analyze();
    void analyzeRapt (bool silent);
    void analyzeDio();
    static int dioDelayHops();
    static int dioSegmentHops();
    void analyzeHarvest();
    void configurePyin (double minHz, double maxHz);
    void analyzePyin (bool silent);

    // 戻り値は周期（間引いた後のサンプル数、小数）。0 なら見つからない
    double detectAutocorrelation (float& clarity);
    double detectYin (float& clarity);
    double detectMcLeod (float& clarity);
    double detectCepstrum (float& clarity);
    double detectSift (float& clarity);
    double detectSwipe (float& clarity);
    void raptCandidates (int& count, double* lags, double* values, double& best);

    double sr = 48000, fs = 12000; // 元と、間引いた後のサンプルレート
    int decimation = 4;
    int phase = 0;                 // 間引きの何番目か
    Biquad antiAlias[2];

    std::vector<float> ring;       // 間引いた後の音
    int ringPos = 0;
    int hop = 60, sinceLast = 0;

    static constexpr int kRingSize = 4096;  // 間引いた後の音をためる長さ（SWIPE の一番長い窓の2倍まで）
    double curMinHz = 0, curMaxHz = 0;
    int configuredMethod = -1;
    int minLag = 24, maxLag = 200, frameLength = 400;
    int delaySamples = 0;          // analysisDelaySamples() の値
    int method = yin;

    std::vector<float> frame;      // 調べる窓（古い順）
    std::vector<double> work;      // 方式ごとの途中の値
    Fft fft;
    std::vector<Fft::Complex> spectrum;

    // SWIPE の準備（設定が変わったときだけ作る）
    struct SwipeState
    {
        std::vector<double> candidates, freqs, kernels, weights, loudness, strength;
        std::vector<int> windows;
        std::vector<Fft> ffts;
        std::vector<std::vector<double>> hann;
        std::vector<std::complex<double>> buffer;
    } swipeState;
    bool swipeVoiced = false;

    // RAPT: 直近のフレームの候補と、そこまでの一番安い道筋（動的計画法）
    static constexpr int kRaptStates = 21;     // 候補 20 + 無声
    static constexpr int kRaptHistory = 16;    // 覚えておくフレームの数（先読みより多く）
    struct RaptFrame
    {
        int count = 0;                         // 有声の候補の数（状態 0 が無声、1〜count が有声）
        double lag[kRaptStates] {}, value[kRaptStates] {}, cost[kRaptStates] {};
        int back[kRaptStates] {};
    };
    RaptFrame raptFrames[kRaptHistory];
    int raptCount = 0;                         // 計算したフレームの数
    double raptPrevA[11] {};                   // 1つ前の区間の LPC 係数（声色の変化を測る）
    bool raptHasPrev = false;

    // pYIN: 候補と、ビタビの点数・たどる道筋
    static constexpr int kPyinHistory = 32;     // 覚えておくフレームの数（先読みより多く）
    struct PyinCandidate
    {
        int tau = 0;
        double prob = 0, hz = 0;
    };
    struct PyinState
    {
        double minHz = 80;
        int numBins = 0, count = 0;
        std::vector<double> thresholdPrior, transition, score, next, logObs;
        std::vector<int> back;                  // [フレーム × 状態]
        std::vector<std::vector<PyinCandidate>> candidates;
    } pyinState;

    // DIO: 20ms ごとに分析した 4 フレーム分の結果
    std::vector<double> dioSegment, dioTemporal, dioF0;
    double dioOut[4] {};
    std::int64_t hopCount = 0;

    // Harvest: 元のサンプルレートの音を渡す。時刻の起点は、作り直したときの位置
    HarvestStream harvestStream;
    std::int64_t inputCount = 0, harvestOrigin = 0;

    PitchEstimate estimate;
    int count = 0;
};

} // namespace vcab
