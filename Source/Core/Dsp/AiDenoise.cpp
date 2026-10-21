#include "AiDenoise.h"

#include <rnnoise.h>

#if VCAB_HAS_DEEPFILTER
// Source/DeepFilter/src/lib.rs の入口
extern "C"
{
struct VcabDfn;
VcabDfn* vcab_dfn_create (float attenLimDb, int lowLatency);
int vcab_dfn_has_low_latency();
size_t vcab_dfn_frame_length (const VcabDfn*);
size_t vcab_dfn_delay (const VcabDfn*);
float vcab_dfn_process (VcabDfn*, const float* input, float* output);
void vcab_dfn_set_post_filter (VcabDfn*, float beta);
void vcab_dfn_reset (VcabDfn*);
void vcab_dfn_destroy (VcabDfn*);
}
#endif

#include "DenoiseDetail.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

namespace vcab
{

using namespace denoise_detail;



namespace
{
double besselI0 (double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 30; ++k)
    {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}
} // namespace


void StreamResampler::prepare (double inRate, double outRate)
{
    step = inRate / outRate;
    // 下げるときは、出力のナイキストより上を落とす（折り返しを防ぐ）
    const double cutoff = std::min (1.0, outRate / inRate) * 0.92;
    half = static_cast<int> (std::ceil (16.0 / cutoff));
    const int taps = half * 2;
    constexpr double beta = 8.0;
    const double i0beta = besselI0 (beta);
    table.assign (static_cast<size_t> ((kPhases + 1) * taps), 0.0f);
    for (int p = 0; p <= kPhases; ++p)
    {
        const double frac = static_cast<double> (p) / kPhases;
        for (int k = 0; k < taps; ++k)
        {
            const double d = k - half + 1 - frac; // 入力の位置 − 出力の位置
            const double x = cutoff * d;
            const double sinc = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (kPi * x) / (kPi * x);
            const double r = d / half;
            const double w = std::abs (r) < 1.0 ? besselI0 (beta * std::sqrt (1.0 - r * r)) / i0beta : 0.0;
            table[static_cast<size_t> (p * taps + k)] = static_cast<float> (cutoff * sinc * w);
        }
    }
    buf.assign (16384, 0.0f);
    reset();
}


void StreamResampler::reset()
{
    // 始めに half 個の 0（入力の -half〜-1 番目）を置く
    std::fill (buf.begin(), buf.end(), 0.0f);
    count = half;
    base = -half;
    t = 0;
}


void StreamResampler::process (const float* in, int n, std::vector<float>& out)
{
    const int taps = half * 2;
    for (int done = 0; done < n;)
    {
        const int space = static_cast<int> (buf.size()) - count;
        if (space <= 0)
            break; // 起こらない大きさにしてある
        const int len = std::min (space, n - done);
        std::copy (in + done, in + done + len, buf.begin() + count);
        count += len;
        done += len;

        for (;;)
        {
            const double ft = std::floor (t);
            const long long first = static_cast<long long> (ft) - half + 1; // 使う最初の入力の番号
            const int start = static_cast<int> (first - base);
            if (start + taps > count)
                break;
            const double fracPos = (t - ft) * kPhases;
            const int p = static_cast<int> (fracPos);
            const float a = static_cast<float> (fracPos - p);
            const float* h0 = table.data() + p * taps;
            const float* h1 = h0 + taps;
            const float* x = buf.data() + start;
            float y = 0;
            for (int k = 0; k < taps; ++k)
                y += x[k] * (h0[k] + (h1[k] - h0[k]) * a);
            out.push_back (y);
            t += step;
        }

        // 使い終わった入力を詰める
        const long long keepFrom = static_cast<long long> (std::floor (t)) - half + 1;
        const int drop = static_cast<int> (std::max (0LL, keepFrom - base));
        if (drop > 0)
        {
            std::copy (buf.begin() + drop, buf.begin() + count, buf.begin());
            count -= drop;
            base += drop;
        }
    }
}



namespace
{
constexpr int kAiFrame = 480;  // 1 回分（48kHz で 10ms）。RNNoise も DeepFilterNet も同じ
constexpr int kRnnDelay = 960; // RNNoise の中の遅れ（48kHz のサンプル数。1 回分先を見るので 2 回分。波形の相互相関で確かめた）
constexpr int kWebRtcDelay = 336; // WebRTC NS の中の遅れ（48kHz のサンプル数。FFT の重なり 96（16kHz）= 288 と、3 帯域フィルタの 48。波形の相互相関で確かめた）
} // namespace


bool AiDenoiser::available (Model m)
{
    if (m == Model::rnnoise || m == Model::webrtc)
        return true;
#if VCAB_HAS_DEEPFILTER
    if (m == Model::deepFilterLowLatency)
        return vcab_dfn_has_low_latency() != 0;
    return true;
#else
    return false;
#endif
}


AiDenoiser::~AiDenoiser()
{
    if (state == nullptr || model == Model::webrtc)
        return;
    if (model == Model::rnnoise)
        rnnoise_destroy (static_cast<DenoiseState*> (state));
#if VCAB_HAS_DEEPFILTER
    else
        vcab_dfn_destroy (static_cast<VcabDfn*> (state));
#endif
}


void AiDenoiser::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    if (state == nullptr)
    {
        if (model == Model::rnnoise)
        {
            state = rnnoise_create (nullptr);
            internalDelay = kRnnDelay;
        }
        else if (model == Model::webrtc)
        {
            webrtc = std::make_unique<WebRtcNs>();
            state = webrtc.get();
            internalDelay = kWebRtcDelay;
        }
#if VCAB_HAS_DEEPFILTER
        else if (auto* df = (model == Model::deepFilterLowLatency && vcab_dfn_has_low_latency() == 0)
                                ? nullptr
                                : vcab_dfn_create (100.0f, model == Model::deepFilterLowLatency ? 1 : 0)) // 削る量の上限は、こちらで元の音を混ぜて実現する（RNNoise とそろえる）
        {
            if (static_cast<int> (vcab_dfn_frame_length (df)) == kAiFrame)
            {
                state = df;
                internalDelay = static_cast<int> (vcab_dfn_delay (df));
            }
            else
            {
                vcab_dfn_destroy (df);
            }
        }
#endif
    }
    resample = std::abs (sr - 48000.0) > 0.5;
    if (resample)
    {
        up.prepare (sr, 48000.0);
        down.prepare (48000.0, sr);
        // 出力が足りなくならないように、先に置いておく量: 1 回分 + 変換で先を見る分
        prime = static_cast<int> (std::ceil ((kAiFrame + down.lookahead()) * sr / 48000.0)) + up.lookahead() + 2;
        latency = prime + static_cast<int> (std::lround (internalDelay * sr / 48000.0));
    }
    else
    {
        prime = kAiFrame;
        latency = prime + internalDelay;
    }
    const int block = std::max (maxBlockSize, 4096);
    // 帯域分割で下げるとき用: 入れた音の履歴（中の遅れ ＋ 窓）と、比べるための FFT（窓 960 を 1024 点で）
    synth.prepare (sr);
    bandFft.prepare (1024);
    inHist.assign (static_cast<size_t> (internalDelay + 2 * kAiFrame + 16), 0.0f);
    prevOut.assign (kAiFrame, 0.0f);
    bandWindow.assign (2 * kAiFrame, 0.0f);
    for (int i = 0; i < 2 * kAiFrame; ++i)
        bandWindow[static_cast<size_t> (i)] = static_cast<float> (0.5 - 0.5 * std::cos (2 * kPi * i / (2 * kAiFrame)));
    binRatio.assign (513, 1.0f);
    binPower.assign (513, 0.0f);
    bandSpecIn.assign (1024, {});
    bandSpecOut.assign (1024, {});
    in48.reserve (static_cast<size_t> (block * 2 + 64));
    scratch.reserve (kAiFrame * 2);
    frameIn.assign (kAiFrame, 0.0f);
    frameOut.assign (kAiFrame, 0.0f);
    outQueue.assign (static_cast<size_t> (prime + block * 2 + kAiFrame * 4), 0.0f);
    dryLine.assign (static_cast<size_t> (latency + 1), 0.0f);
    reset();
}


void AiDenoiser::reset()
{
    if (state != nullptr)
    {
        if (model == Model::rnnoise)
            rnnoise_init (static_cast<DenoiseState*> (state), nullptr);
        else if (model == Model::webrtc)
            webrtc->reset();
#if VCAB_HAS_DEEPFILTER
        else
            vcab_dfn_reset (static_cast<VcabDfn*> (state));
#endif
    }
    if (resample)
    {
        up.reset();
        down.reset();
    }
    framePos = 0;
    std::fill (outQueue.begin(), outQueue.end(), 0.0f);
    queueRead = 0;
    queueWrite = static_cast<size_t> (prime);
    queueCount = static_cast<size_t> (prime);
    std::fill (dryLine.begin(), dryLine.end(), 0.0f);
    dryPos = 0;
    synth.reset();
    std::fill (inHist.begin(), inHist.end(), 0.0f);
    std::fill (prevOut.begin(), prevOut.end(), 0.0f);
    inHistPos = 0;
}


void AiDenoiser::updateBandGains()
{
    // AI が出した直近 2 回分（960）と、それに対応する入れた音（中の遅れの分だけ前）を、同じ窓で FFT して比べる
    const int hist = static_cast<int> (inHist.size());
    const int window = 2 * kAiFrame;
    for (int i = 0; i < 1024; ++i)
    {
        bandSpecIn[static_cast<size_t> (i)] = {};
        bandSpecOut[static_cast<size_t> (i)] = {};
    }
    for (int i = 0; i < window; ++i)
    {
        // 入れた音: 一番新しいものから internalDelay 前が、出した音の一番新しいものに対応する
        const int back = internalDelay + (window - 1 - i);
        const float in = inHist[static_cast<size_t> (((inHistPos - 1 - back) % hist + hist) % hist)];
        const float out = i < kAiFrame ? prevOut[static_cast<size_t> (i)] : frameOut[static_cast<size_t> (i - kAiFrame)];
        bandSpecIn[static_cast<size_t> (i)] = { in * bandWindow[static_cast<size_t> (i)], 0.0 };
        bandSpecOut[static_cast<size_t> (i)] = { out * bandWindow[static_cast<size_t> (i)], 0.0 };
    }
    bandFft.forward (bandSpecIn.data());
    bandFft.forward (bandSpecOut.data());
    for (int k = 0; k <= 512; ++k)
    {
        const double pin = std::norm (bandSpecIn[static_cast<size_t> (k)]);
        const double pout = std::norm (bandSpecOut[static_cast<size_t> (k)]);
        binPower[static_cast<size_t> (k)] = static_cast<float> (pin);
        binRatio[static_cast<size_t> (k)] = static_cast<float> (std::sqrt (pout / std::max (pin, 1.0e-20)));
    }
    float targets[BandSynth::kMaxBands];
    synth.binsToBands (binRatio.data(), binPower.data(), 513, 48000.0 / 1024, floorGain, targets);
    synth.setTargets (targets, static_cast<int> (std::lround (kAiFrame * sr / 48000.0)));
    std::copy (frameOut.begin(), frameOut.end(), prevOut.begin());
}


void AiDenoiser::setPostFilter (bool on)
{
    if (postFilter == (on ? 1 : 0))
        return;
    postFilter = on ? 1 : 0;
#if VCAB_HAS_DEEPFILTER
    if (state != nullptr && (model == Model::deepFilter || model == Model::deepFilterLowLatency))
        vcab_dfn_set_post_filter (static_cast<VcabDfn*> (state), on ? 0.02f : 0.0f);
#endif
}


void AiDenoiser::pushOutput (const float* x, int n)
{
    for (int i = 0; i < n && queueCount < outQueue.size(); ++i)
    {
        outQueue[queueWrite] = x[i];
        queueWrite = (queueWrite + 1) % outQueue.size();
        ++queueCount;
    }
}


void AiDenoiser::processFrame()
{
    // 帯域分割で下げるときのために、入れた音を残しておく（RNNoise は frameIn をその場で書き換えるので、その前に）
    if (bandMode)
    {
        for (float v : frameIn)
        {
            inHist[static_cast<size_t> (inHistPos)] = v;
            inHistPos = (inHistPos + 1) % static_cast<int> (inHist.size());
        }
    }
    if (state == nullptr)
    {
        // 使えないとき（DeepFilterNet を組み込んでいないビルドなど）は、中の遅れなしの素通し
        std::copy (frameIn.begin(), frameIn.end(), frameOut.begin());
    }
    else if (model == Model::rnnoise)
    {
        // RNNoise は 16 ビットの整数の大きさで扱う
        for (auto& v : frameIn)
            v *= 32768.0f;
        voiceProb = rnnoise_process_frame (static_cast<DenoiseState*> (state), frameOut.data(), frameIn.data());
        for (auto& o : frameOut)
            o *= 1.0f / 32768.0f;
    }
    else if (model == Model::webrtc)
    {
        // 声のありそうさは外に出していないので、出した音と入れた音の大きさの比から決める（-3dB 以上で 1、-9dB 以下で 0）
        const float ratio = webrtc->process (frameIn.data(), frameOut.data());
        const float db = 20.0f * std::log10 (ratio + 1.0e-9f);
        voiceProb = std::clamp ((db + 9.0f) / 6.0f, 0.0f, 1.0f);
    }
#if VCAB_HAS_DEEPFILTER
    else
    {
        // 局所的な SNR（dB）から、声のありそうさにする
        const float lsnr = vcab_dfn_process (static_cast<VcabDfn*> (state), frameIn.data(), frameOut.data());
        if (! std::isnan (lsnr))
            voiceProb = static_cast<float> (1.0 / (1.0 + std::exp (-(lsnr - 5.0) / 3.0)));
    }
#endif

    if (bandMode)
    {
        updateBandGains();
        return; // 音は AI の出力ではなく、今の音を帯域分割で下げたものを使う
    }

    if (resample)
    {
        scratch.clear();
        down.process (frameOut.data(), kAiFrame, scratch);
        pushOutput (scratch.data(), static_cast<int> (scratch.size()));
    }
    else
    {
        pushOutput (frameOut.data(), kAiFrame);
    }
}


void AiDenoiser::process (float* data, int n, const DenoiseSettings& s)
{
    floorGain = static_cast<float> (std::pow (10.0, -s.maxReductionDb / 20.0));
    if (webrtc != nullptr)
        webrtc->setMaxReduction (static_cast<float> (s.maxReductionDb));
    if (s.bandSynthesis != bandMode)
    {
        bandMode = s.bandSynthesis;
        reset();
    }
    in48.clear();
    if (resample)
        up.process (data, n, in48);
    else
        in48.assign (data, data + n);

    for (float v : in48)
    {
        frameIn[static_cast<size_t> (framePos)] = v;
        if (++framePos == kAiFrame)
        {
            framePos = 0;
            processFrame();
        }
    }

    if (bandMode)
    {
        // 遅延 0: 今の音に、帯域ごとの量をかける
        for (int i = 0; i < n; ++i)
            data[i] = synth.process (data[i]);
        return;
    }

    // 削る量の上限: 同じだけ遅らせた元の音を混ぜる（ノイズだけのところは、元の音 × 上限の倍率になる）
    const auto keep = static_cast<float> (std::pow (10.0, -s.maxReductionDb / 20.0));
    const int lineSize = static_cast<int> (dryLine.size());
    for (int i = 0; i < n; ++i)
    {
        float y = 0;
        if (queueCount > 0)
        {
            y = outQueue[queueRead];
            queueRead = (queueRead + 1) % outQueue.size();
            --queueCount;
        }
        dryLine[static_cast<size_t> (dryPos)] = data[i];
        const float dry = dryLine[static_cast<size_t> ((dryPos + 1) % lineSize)]; // latency サンプル前
        dryPos = (dryPos + 1) % lineSize;
        data[i] = y + (dry - y) * keep;
    }
}



VoiceDetector::~VoiceDetector()
{
    stopWorker.store (true);
    wake.notify_one();
    if (worker.joinable())
        worker.join();
    if (state != nullptr)
        rnnoise_destroy (static_cast<DenoiseState*> (state));
}


void VoiceDetector::prepare (double sampleRate, int maxBlockSize)
{
    // 作業スレッドを止めてから作り直す
    if (worker.joinable())
    {
        stopWorker.store (true);
        wake.notify_one();
        worker.join();
    }
    stopWorker.store (false);
    if (state == nullptr)
        state = rnnoise_create (nullptr);
    ratio = sampleRate / 48000.0;
    resample = std::abs (sampleRate - 48000.0) > 0.5;
    if (resample)
        up.prepare (sampleRate, 48000.0);
    in48.reserve (static_cast<size_t> (std::max (maxBlockSize, 4096) * 2 + 64));
    ring.assign (kRing, 0.0f);
    frameIn.assign (kAiFrame, 0.0f);
    frameOut.assign (kAiFrame, 0.0f);
    written.store (0);
    judged.store (0);
    lastVoice.store (0);
    restartAt.store (0);
    epoch.store (0);
    seenEpoch.store (-1);
    reset();
    worker = std::thread ([this] { workerLoop(); });
}


void VoiceDetector::reset()
{
    // 判定する側に、今の位置から始め直すように伝える（RNNoise の状態も消す）
    if (resample)
        up.reset();
    restartAt.store (written.load (std::memory_order_relaxed), std::memory_order_release);
    epoch.fetch_add (1, std::memory_order_acq_rel);
    if (! realtime.load())
    {
        std::lock_guard<std::mutex> lock (judgeMutex);
        processFrames();
    }
    wake.notify_one();
}


bool VoiceDetector::processFrames()
{
    if (state == nullptr)
        return false;
    const int e = epoch.load (std::memory_order_acquire);
    if (e != seenEpoch.load (std::memory_order_relaxed))
    {
        rnnoise_init (static_cast<DenoiseState*> (state), nullptr);
        const auto start = restartAt.load (std::memory_order_acquire);
        lastVoice.store (start, std::memory_order_relaxed);
        judged.store (start, std::memory_order_release);
        seenEpoch.store (e, std::memory_order_release);
    }
    auto pos = judged.load (std::memory_order_relaxed);
    const auto end = written.load (std::memory_order_acquire);
    if (end - pos > kRing / 2)
    {
        // 追いつけないほど遅れた（リングを上書きされる前に飛ばす。飛ばした所は「声あり」だったかもしれないとみなす）
        pos = end - kAiFrame;
        lastVoice.store (pos, std::memory_order_relaxed);
        judged.store (pos, std::memory_order_release);
    }
    bool did = false;
    while (pos + kAiFrame <= end)
    {
        for (int i = 0; i < kAiFrame; ++i)
            frameIn[static_cast<size_t> (i)] = ring[static_cast<size_t> ((pos + i) % kRing)] * 32768.0f; // RNNoise は 16 ビットの整数の大きさで扱う
        const float prob = rnnoise_process_frame (static_cast<DenoiseState*> (state), frameOut.data(), frameIn.data());
        pos += kAiFrame;
        if (prob > threshold.load (std::memory_order_relaxed))
            lastVoice.store (pos, std::memory_order_relaxed);
        judged.store (pos, std::memory_order_release); // lastVoice を書いた後に進める（読む側は judged を先に読む）
        did = true;
    }
    return did;
}


void VoiceDetector::workerLoop()
{
    while (! stopWorker.load())
    {
        bool did = false;
        if (realtime.load())
        {
            std::lock_guard<std::mutex> lock (judgeMutex);
            did = processFrames();
        }
        if (! did)
        {
            // 次の 480 サンプルがそろうまで待つ（音の処理のスレッドが起こす。取りこぼしても 20ms で見直す）
            std::unique_lock<std::mutex> lock (wakeMutex);
            wake.wait_for (lock, std::chrono::milliseconds (20));
        }
    }
}


int VoiceDetector::quietSamples() const noexcept
{
    if (seenEpoch.load (std::memory_order_acquire) != epoch.load (std::memory_order_acquire))
        return 0; // リセットを、判定する側がまだ受け取っていない
    const auto w = written.load (std::memory_order_relaxed);
    const auto j = judged.load (std::memory_order_acquire);
    const auto v = lastVoice.load (std::memory_order_relaxed);
    const auto backlog = w - j; // まだ判定していない分（「声あり」かもしれない）
    if (backlog > kMaxBacklog)
        return 0;
    const auto quiet = j - v - backlog;
    return quiet > 0 ? static_cast<int> (std::min (static_cast<double> (quiet) * ratio, 1.0e9)) : 0;
}


void VoiceDetector::process (const float* data, int n)
{
    if (state == nullptr)
        return;
    const float* x = data;
    int count = n;
    if (resample)
    {
        in48.clear();
        up.process (data, n, in48);
        x = in48.data();
        count = static_cast<int> (in48.size());
    }
    const auto w = written.load (std::memory_order_relaxed);
    for (int i = 0; i < count; ++i)
        ring[static_cast<size_t> ((w + i) % kRing)] = x[i];
    written.store (w + count, std::memory_order_release);
    if (realtime.load())
    {
        wake.notify_one();
    }
    else
    {
        std::lock_guard<std::mutex> lock (judgeMutex);
        processFrames();
    }
}

} // namespace vcab
