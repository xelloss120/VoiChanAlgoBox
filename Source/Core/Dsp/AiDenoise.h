// AI の方式（RNNoise / DeepFilterNet）と WebRTC NS、サンプルレートの変換
#pragma once

#include "BandDenoise.h"
#include "DenoiseSettings.h"
#include "Fft.h"
#include "WebRtcNs.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace vcab
{

// サンプルレートの変換（カイザー窓の sinc。途切れなく流す形）。
// 出力の m 番目は、入力の m × (入力のレート / 出力のレート) 番目の時刻の値（ずれなし）。
// ただし、先の入力を half 個使うので、出せるのはその分だけ後になる
class StreamResampler
{
public:
    void prepare (double inRate, double outRate);
    void reset();
    // 入力を n 個入れ、出せるだけ out に足す
    void process (const float* in, int n, std::vector<float>& out);
    int lookahead() const noexcept { return half; } // 入力のサンプル数

private:
    static constexpr int kPhases = 512;
    double step = 1, t = 0;
    int half = 16;
    long long base = 0;
    std::vector<float> table, buf;
    int count = 0;
};

// AI の方式（RNNoise / DeepFilterNet）。48kHz で 480 サンプルずつ処理する枠。WebRTC NS（AI ではない）も同じ単位なので、この枠で動かす
class AiDenoiser
{
public:
    enum class Model { rnnoise, deepFilter, deepFilterLowLatency, webrtc };

    explicit AiDenoiser (Model m) : model (m) {}
    ~AiDenoiser();
    AiDenoiser (const AiDenoiser&) = delete;
    AiDenoiser& operator= (const AiDenoiser&) = delete;

    static bool available (Model m); // このビルドで使えるか（DeepFilterNet は Rust がないと組み込まれない）

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void process (float* data, int n, const DenoiseSettings& s);
    int latencySamples() const noexcept { return latency; }
    float voiceProbability() const noexcept { return voiceProb; } // RNNoise の VAD / DeepFilterNet の局所的な SNR から
    void setPostFilter (bool on);

private:
    void pushOutput (const float* x, int n);
    void processFrame();

    Model model;
    double sr = 48000;
    bool resample = false;
    void* state = nullptr; // RNNoise の DenoiseState / DeepFilterNet のラッパー（ここでは中身を読み込まないため void*）
    std::unique_ptr<WebRtcNs> webrtc; // WebRTC NS（state は、使えることを示すためにこれを指す）
    int internalDelay = 0; // 中の遅れ（48kHz のサンプル数）
    StreamResampler up, down;
    std::vector<float> in48, scratch, frameIn, frameOut, outQueue, dryLine;
    int framePos = 0, prime = 0, latency = 0, dryPos = 0;
    size_t queueRead = 0, queueWrite = 0, queueCount = 0;
    float voiceProb = 0;
    int postFilter = -1; // 今の設定（-1 はまだ設定していない）
    // 「＋帯域分割」のとき（DenoiseSettings::bandSynthesis）: AI が出した音と、それに入れた音（中の遅れの分だけ前）の帯域ごとの比を、今の音にかける
    bool bandMode = false;
    BandSynth synth;
    Fft bandFft;
    std::vector<float> inHist, prevOut, bandWindow, binRatio, binPower;
    std::vector<Fft::Complex> bandSpecIn, bandSpecOut;
    int inHistPos = 0;
    float floorGain = 0.25f;
    void updateBandGains();
};

// RNNoise の声の判定（VAD）だけを使う部品。ノイズの見積もり「RNNoise の声の判定」で、声のない間だけ見積もりを更新するために使う。
// 音は変えない。「声なし」と確かめられた長さ（入力のサンプルレートで、今から数えて）を返す。
// RNNoise は 10ms 分（480 サンプル）ごとに 1 回、まとめて重い計算をするので、リアルタイムのときは作業スレッドで計算する
// （音の処理のスレッドは、48kHz にした入力をリングに書くだけ）。作業スレッドがまだ計算していない分は「声あり」かもしれないとみなし、
// その長さを差し引く（遅れても、声をノイズと取り違えない側に倒す）。リアルタイムでないとき（書き出し）は、その場で計算する（結果が毎回同じ）
class VoiceDetector
{
public:
    VoiceDetector() = default;
    ~VoiceDetector();
    VoiceDetector (const VoiceDetector&) = delete;
    VoiceDetector& operator= (const VoiceDetector&) = delete;

    void prepare (double sampleRate, int maxBlockSize);
    void setRealtime (bool isRealtime) noexcept { realtime.store (isRealtime); }
    void setThreshold (float probability) noexcept { threshold.store (probability); } // 声の確率がこれを超えたら「声あり」
    void reset();
    void process (const float* data, int n);
    int quietSamples() const noexcept;

private:
    std::atomic<float> threshold { 0.5f };      // これより大きければ「声あり」
    static constexpr int kRing = 96000;         // 48kHz で 2 秒
    static constexpr int kMaxBacklog = 1440;    // 作業スレッドの遅れがこれ（30ms）を超えたら、「声なし」を確かめられないとみなす
    bool processFrames();                       // 書かれた入力のうち、そろった 480 サンプルずつを判定する（判定したら true）
    void workerLoop();

    void* state = nullptr;
    double ratio = 1; // 入力のレート / 48kHz
    bool resample = false;
    StreamResampler up;
    std::vector<float> in48, ring, frameIn, frameOut;
    std::atomic<bool> realtime { true };
    // 位置は、48kHz にした入力の通し番号
    std::atomic<std::int64_t> written { 0 };    // 書いた位置（音の処理のスレッド）
    std::atomic<std::int64_t> judged { 0 };     // 判定し終えた位置（判定する側）
    std::atomic<std::int64_t> lastVoice { 0 };  // 最後に「声あり」だった 480 サンプルの終わりの位置（判定する側）
    std::atomic<std::int64_t> restartAt { 0 };  // リセットした位置
    std::atomic<int> epoch { 0 }, seenEpoch { -1 };
    std::mutex judgeMutex;                      // 判定する側が 2 つ同時に動かないように（リアルタイムの切り替えのとき）
    std::thread worker;
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::atomic<bool> stopWorker { false };
};

} // namespace vcab
