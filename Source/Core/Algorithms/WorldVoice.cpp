#include "WorldVoice.h"

#include "../Dsp/FormantWarp.h"
#include "../Dsp/HarvestStream.h"
#include "../Dsp/Intonation.h"

#include "world/cheaptrick.h"
#include "world/d4c.h"
#include "world/dio.h"
#include "world/stonemask.h"
#include "world/synthesisrealtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace vcab
{

namespace
{
constexpr double kFramePeriodMs = 5.0;
constexpr int kChunkFrames = 4;      // 一度に分析するフレームの数（20ms）
// 4フレームの前後に足す余裕。区間の端では分析が正しくないため。DIO は声の区間がある程度続かないと声と認めない
// （短い有声区間を取り除く）ので、区間全体を長くする必要がある。前の余裕は過去の音なので遅延にならず、
// 長くしてよい。遅延になるのは後ろの余裕だけ
constexpr int kLeadFrames = 24;      // 前の余裕（120ms。80ms でも検出できたが、余裕を持たせる）
constexpr int kTailFrames = 8;       // 後ろの余裕（40ms）
constexpr int kSlots = 64;           // 合成器に渡した分析結果の置き場の数（合成が終わるまで使われる）
constexpr int kSynthBuffer = 64;     // 合成器が一度に作るサンプル数

// Harvest を使うときの後ろの余裕。Harvest は先の音を使う作りなので、DIO より長く待つ
int harvestTailFor (double sr, double minHz)
{
    return static_cast<int> (std::ceil (HarvestStream::delayMsFor (minHz) * sr / 1000.0));
}
} // namespace

struct WorldVoice::Impl
{
    double sr = 48000;
    int fs = 48000;
    int frameSamples = 240;          // 1フレームのサンプル数（5ms）
    int chunkSamples = 960;
    int leadSamples = 5760, tailSamples = 1920;
    int fftSize = 2048;
    int specBins = 1025;

    // ---- 音の処理のスレッドと作業スレッドの間で受け渡すもの ----
    std::vector<float> input;                    // 入力の履歴（リング）。音の処理のスレッドが書く
    std::atomic<std::int64_t> written { 0 };     // 書き込んだ入力のサンプル数（絶対位置）
    std::vector<float> output;                   // 合成した音（リング）。分析する側が書く
    std::atomic<std::int64_t> outWrite { 0 };    // 合成し終えた位置（絶対位置）
    std::atomic<std::int64_t> restartAt { 0 };   // 方式を切り替えたときなどに、ここから始め直す
    std::atomic<int> epoch { 0 };                // 始め直しの回数（分析する側が気づくため）
    std::atomic<bool> realtime { false };

    // 声の高さの求め方（音の処理のスレッドが決め、変わったら始め直す。分析する側は始め直すときに読む）
    std::atomic<bool> useHarvest { false };
    std::atomic<int> harvestTail { 0 };
    std::atomic<double> rangeMin { 80 }, rangeMax { 400 };

    // 設定値の受け渡し（シーケンスロック: 書く側は奇数の間に書き、読む側は前後で番号が同じなら成功）
    std::atomic<std::uint32_t> paramSeq { 0 };
    ParamValues sharedParams;

    std::atomic<float> pitchHz { 0 };
    std::atomic<bool> pitchVoiced { false };
    PitchEstimate estimate;                      // 画面向け（atomic から写す）

    // ---- 分析する側だけが使うもの（作業スレッド、またはリアルタイムでないときは音の処理のスレッド） ----
    std::mutex chunkMutex;                       // 分析する側が2つ同時に動かないように
    int seenEpoch = -1;
    std::int64_t nextChunk = 0;                  // 次に分析する4フレームの先頭（絶対位置）
    std::int64_t origin = 0;                     // 合成器の時刻 0 が、入力の何サンプル目か
    std::vector<double> segment, temporal, f0All, refined, spScratch;

    struct Slot
    {
        std::vector<double> f0;
        std::vector<std::vector<double>> sp, ap;
        std::vector<double*> spPtr, apPtr;
    };
    std::vector<Slot> slots;
    int slotIndex = 0;
    WorldSynthesizer synth {};
    bool synthReady = false;
    CheapTrickOption cheapTrick {};
    FormantWarp warp;
    IntonationEmphasis intonation;
    D4COption d4c {};
    HarvestStream harvest;
    bool harvestActive = false;                  // 分析する側が、始め直したときに useHarvest を写したもの
    std::int64_t harvestFed = 0;                 // Harvest に渡した入力の位置（絶対位置）

    // ---- 作業スレッド ----
    std::thread worker;
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::atomic<bool> stopWorker { false };

    int tailFor (bool harvestOn) const { return harvestOn ? std::max (tailSamples, harvestTail.load()) : tailSamples; }
    int latency() const { return chunkSamples + tailFor (useHarvest.load()) + fftSize / 2 + kSynthBuffer * 2; }

    void writeParams (const ParamValues& p)
    {
        paramSeq.fetch_add (1, std::memory_order_acq_rel); // 奇数: 書いている途中
        sharedParams = p;
        paramSeq.fetch_add (1, std::memory_order_acq_rel); // 偶数: 書き終わり
    }

    ParamValues readParams()
    {
        for (;;)
        {
            const auto before = paramSeq.load (std::memory_order_acquire);
            if (before & 1u)
                continue;
            ParamValues copy = sharedParams;
            std::atomic_thread_fence (std::memory_order_acquire);
            if (paramSeq.load (std::memory_order_acquire) == before)
                return copy;
        }
    }

    bool processOneChunk(); // 分析できる4フレームがそろっていれば、1つ分析して合成する。したら true
    void analyzeChunk (const ParamValues& params);
    void restartIfNeeded();
    void workerLoop();
};

void WorldVoice::Impl::restartIfNeeded()
{
    const int e = epoch.load (std::memory_order_acquire);
    if (e == seenEpoch)
        return;
    seenEpoch = e;
    const auto start = restartAt.load (std::memory_order_acquire);
    nextChunk = start;
    origin = start;
    slotIndex = 0;
    if (synthReady)
        RefreshSynthesizer (&synth);
    harvestActive = useHarvest.load();
    if (harvestActive)
    {
        harvest.setRange (rangeMin.load(), rangeMax.load());
        harvest.reset();
    }
    harvestFed = start;
    intonation.reset();
    outWrite.store (start, std::memory_order_release);
}

bool WorldVoice::Impl::processOneChunk()
{
    if (! synthReady)
        return false;
    restartIfNeeded();
    if (written.load (std::memory_order_acquire) < nextChunk + chunkSamples + tailFor (harvestActive))
        return false;
    analyzeChunk (readParams());
    return true;
}

void WorldVoice::Impl::workerLoop()
{
    while (! stopWorker.load())
    {
        bool did = false;
        if (realtime.load())
        {
            std::lock_guard<std::mutex> lock (chunkMutex);
            did = processOneChunk();
        }
        if (! did)
        {
            // 次の4フレームがそろうまで待つ（音の処理のスレッドが起こす。取りこぼしても 2ms ほどで見直す）
            std::unique_lock<std::mutex> lock (wakeMutex);
            wake.wait_for (lock, std::chrono::milliseconds (2));
        }
    }
}

void WorldVoice::Impl::analyzeChunk (const ParamValues& params)
{
    const int segmentLength = static_cast<int> (segment.size());
    const auto inSize = static_cast<std::int64_t> (input.size());

    // 分析する区間: [nextChunk - lead, nextChunk + chunk + tail)。始め直す前の音は使わない
    const std::int64_t start = nextChunk - leadSamples;
    for (int i = 0; i < segmentLength; ++i)
    {
        const std::int64_t pos = start + i;
        segment[static_cast<size_t> (i)] = pos < origin ? 0.0 : input[static_cast<size_t> (pos % inSize)];
    }

    // 使う4フレーム（区間の先頭から lead の位置が、4フレームの先頭）
    auto& slot = slots[static_cast<size_t> (slotIndex)];
    double positions[kChunkFrames];
    if (harvestActive)
    {
        // ---- 声の高さ: Harvest（少しずつ計算する版に、まだ渡していない入力を渡し、決まったフレームを読む） ----
        const std::int64_t target = nextChunk + chunkSamples + tailFor (true);
        while (harvestFed < target)
        {
            const auto at = static_cast<size_t> (harvestFed % inSize);
            const auto n = static_cast<int> (std::min<std::int64_t> (target - harvestFed, inSize - static_cast<std::int64_t> (at)));
            harvest.push (input.data() + at, n);
            harvestFed += n;
        }
        for (int f = 0; f < kChunkFrames; ++f)
        {
            positions[f] = (kLeadFrames + f) * kFramePeriodMs / 1000.0;
            const auto pos = nextChunk + static_cast<std::int64_t> (f) * frameSamples - origin;
            const auto frame = static_cast<std::int64_t> (std::llround (static_cast<double> (pos) * 1000.0 / sr)); // Harvest は 1ms ごと
            slot.f0[static_cast<size_t> (f)] = harvest.f0AtFrame (frame);
        }
    }
    else
    {
        // ---- 声の高さ: DIO → StoneMask ----
        DioOption dio;
        InitializeDioOption (&dio);
        dio.frame_period = kFramePeriodMs;
        dio.f0_floor = params.get (P::pitchMinHz);
        dio.f0_ceil = std::max (dio.f0_floor * 1.5, static_cast<double> (params.get (P::pitchMaxHz)));
        dio.speed = std::max (1, fs / 12000); // 約 12kHz に間引いて計算する（速くするため）
        Dio (segment.data(), segmentLength, fs, &dio, temporal.data(), f0All.data());
        StoneMask (segment.data(), segmentLength, fs, temporal.data(), f0All.data(),
                   static_cast<int> (temporal.size()), refined.data());
        for (int f = 0; f < kChunkFrames; ++f)
        {
            const int idx = kLeadFrames + f;
            positions[f] = temporal[static_cast<size_t> (idx)];
            slot.f0[static_cast<size_t> (f)] = refined[static_cast<size_t> (idx)];
        }
    }

    // ---- 声色とかすれ: 必要な4フレームだけ計算する ----
    CheapTrick (segment.data(), segmentLength, fs, positions, slot.f0.data(), kChunkFrames, &cheapTrick, slot.spPtr.data());
    D4C (segment.data(), segmentLength, fs, positions, slot.f0.data(), kChunkFrames, fftSize, &d4c, slot.apPtr.data());

    // 画面に出す声の高さ（4フレームの最後）
    const double lastF0 = slot.f0[kChunkFrames - 1];
    pitchHz.store (static_cast<float> (lastF0));
    pitchVoiced.store (lastF0 > 0);

    // ---- 加工: かすれ（非周期性を、この割合だけ 1 = 雑音の側に寄せる） ----
    // 少し寄せるだけで効きが強いので、つまみの前半で細かく合わせられるよう、2乗のカーブにする
    const double knob = std::clamp (static_cast<double> (params.get (P::worldBreath)) * 0.01, 0.0, 1.0);
    const double breathiness = knob * knob;
    if (breathiness > 0)
        for (int f = 0; f < kChunkFrames; ++f)
            for (auto& v : slot.ap[static_cast<size_t> (f)])
                v += breathiness * (1.0 - v);

    // ---- 加工: 声の高さを倍率倍、声色をフォルマントの倍率だけ伸び縮み ----
    const double ratio = semitonesToRatio (params.get (P::pitch));
    const double formantRatio = semitonesToRatio (params.get (P::formant));
    // 分析は作業スレッドで、音より約 84ms 遅れて行うので、追いかけたフォルマントの時刻と合わない。
    // フォルマントごとは、帯域ごと（固定の目印）と同じに扱う（追いかけを渡さないと、帯域の目印を使う）
    warp.set (params);
    const double binHz = static_cast<double> (fs) / fftSize;
    const double emphasisAmount = params.get (P::intonation) * 0.01;
    for (int f = 0; f < kChunkFrames; ++f)
    {
        // 抑揚の強調: そのフレームの声の高さの、平均からのずれに応じて、さらに倍率をかける
        const double hz = slot.f0[static_cast<size_t> (f)];
        slot.f0[static_cast<size_t> (f)] *= ratio * intonation.factor (hz, hz > 0, emphasisAmount, kFramePeriodMs / 1000.0);
        if (! warp.isIdentity (1.0e-6))
        {
            // 声色の包絡（パワー）を、周波数 k にあった値が k × formantRatio に来るように読み直す（対数で補間）
            auto& sp = slot.sp[static_cast<size_t> (f)];
            for (int k = 0; k < specBins; ++k)
            {
                // 動かした後の位置 k に来る、元の位置（「低い側を残す」のときは FormantWarp で求める）
                const double from = warp.isUniform() ? k / formantRatio : warp.inverse (k * binHz) / binHz;
                const double src = std::min (static_cast<double> (specBins - 1), from);
                const int i0 = std::min (specBins - 2, static_cast<int> (src));
                const double t = src - i0;
                spScratch[static_cast<size_t> (k)] = std::exp (std::log (sp[static_cast<size_t> (i0)] + 1.0e-20) * (1 - t)
                                                             + std::log (sp[static_cast<size_t> (i0 + 1)] + 1.0e-20) * t);
            }
            std::copy (spScratch.begin(), spScratch.end(), sp.begin());
        }
    }

    // ---- 合成器に渡して、作れるだけ作る ----
    if (AddParameters (slot.f0.data(), kChunkFrames, slot.spPtr.data(), slot.apPtr.data(), &synth) == 1)
        slotIndex = (slotIndex + 1) % kSlots;
    const auto outSize = static_cast<std::int64_t> (output.size());
    auto produced = outWrite.load (std::memory_order_relaxed);
    while (Synthesis2 (&synth) != 0)
    {
        for (int i = 0; i < kSynthBuffer; ++i)
            output[static_cast<size_t> ((produced + i) % outSize)] = static_cast<float> (synth.buffer[i]);
        produced += kSynthBuffer;
    }
    outWrite.store (produced, std::memory_order_release);
    if (IsLocked (&synth) == 1)
        RefreshSynthesizer (&synth);

    nextChunk += chunkSamples;
}

WorldVoice::WorldVoice() : impl (std::make_unique<Impl>())
{
    impl->worker = std::thread ([this] { impl->workerLoop(); });
}

WorldVoice::~WorldVoice()
{
    impl->stopWorker.store (true);
    impl->wake.notify_all();
    if (impl->worker.joinable())
        impl->worker.join();
    if (impl->synthReady)
        DestroySynthesizer (&impl->synth);
}

void WorldVoice::setRealtime (bool isRealtime)
{
    impl->realtime.store (isRealtime);
}

const PitchEstimate* WorldVoice::pitchEstimate() const
{
    auto& m = *impl;
    m.estimate.hz = m.pitchHz.load();
    m.estimate.voiced = m.pitchVoiced.load();
    m.estimate.clarity = m.estimate.voiced ? 1.0f : 0.0f;
    return &m.estimate;
}

void WorldVoice::prepare (double sampleRate, int)
{
    auto& m = *impl;
    // 作業スレッドが分析している途中なら終わるのを待ってから、作り直す
    std::lock_guard<std::mutex> lock (m.chunkMutex);
    if (m.synthReady)
    {
        DestroySynthesizer (&m.synth);
        m.synthReady = false;
    }

    m.sr = sampleRate;
    m.fs = static_cast<int> (std::lround (sampleRate));
    m.frameSamples = static_cast<int> (std::lround (m.sr * kFramePeriodMs / 1000.0));
    m.chunkSamples = m.frameSamples * kChunkFrames;
    m.leadSamples = m.frameSamples * kLeadFrames;
    m.tailSamples = m.frameSamples * kTailFrames;

    m.harvest.prepare (sampleRate);
    InitializeCheapTrickOption (m.fs, &m.cheapTrick);
    m.cheapTrick.f0_floor = 71.0; // WORLD の標準。FFT の大きさが決まる（48kHz で 2048）
    m.fftSize = GetFFTSizeForCheapTrick (m.fs, &m.cheapTrick);
    m.cheapTrick.fft_size = m.fftSize;
    m.specBins = m.fftSize / 2 + 1;
    InitializeD4COption (&m.d4c);

    const int segmentLength = m.leadSamples + m.chunkSamples + m.tailSamples;
    m.input.assign (static_cast<size_t> (m.sr), 0.0f);       // 1秒分（区間の長さより十分長い）
    m.output.assign (static_cast<size_t> (m.sr * 0.5), 0.0f);
    m.segment.assign (static_cast<size_t> (segmentLength), 0.0);
    const int frames = GetSamplesForDIO (m.fs, segmentLength, kFramePeriodMs);
    m.temporal.assign (static_cast<size_t> (frames), 0.0);
    m.f0All.assign (static_cast<size_t> (frames), 0.0);
    m.refined.assign (static_cast<size_t> (frames), 0.0);
    m.spScratch.assign (static_cast<size_t> (m.specBins), 0.0);

    m.slots.assign (kSlots, {});
    for (auto& s : m.slots)
    {
        s.f0.assign (kChunkFrames, 0.0);
        s.sp.assign (kChunkFrames, std::vector<double> (static_cast<size_t> (m.specBins), 0.0));
        s.ap.assign (kChunkFrames, std::vector<double> (static_cast<size_t> (m.specBins), 0.0));
        s.spPtr.resize (kChunkFrames);
        s.apPtr.resize (kChunkFrames);
        for (int f = 0; f < kChunkFrames; ++f)
        {
            s.spPtr[static_cast<size_t> (f)] = s.sp[static_cast<size_t> (f)].data();
            s.apPtr[static_cast<size_t> (f)] = s.ap[static_cast<size_t> (f)].data();
        }
    }

    InitializeSynthesizer (m.fs, kFramePeriodMs, m.fftSize, kSynthBuffer, kSlots, &m.synth);
    m.synthReady = true;

    m.written.store (0);
    m.restartAt.store (0);
    m.outWrite.store (0);
    m.seenEpoch = -1;
    m.epoch.fetch_add (1);
}

void WorldVoice::reset()
{
    // 音の処理のスレッドから呼ばれることがある（方式を切り替えたとき）。ここでは「今の位置から始め直す」印だけ
    // 付け、実際の作り直しは分析する側が次に動くときに行う
    auto& m = *impl;
    m.restartAt.store (m.written.load());
    m.epoch.fetch_add (1, std::memory_order_acq_rel);
    m.pitchHz.store (0);
    m.pitchVoiced.store (false);
}

int WorldVoice::latencySamples (const ParamValues& params) const
{
    // 4フレームがそろって、後ろの余裕も入ってから分析する（chunk + tail）。合成器は、渡したフレームの
    // 少し先まで見てから音を作り、窓の半分（fft / 2）だけ遅れて出す。さらに出口で一塊（kSynthBuffer）待つ。
    // 実測では、分析の直後で合成が入力より約 47ms 遅れ、次の分析まで 20ms 待つので最悪約 67ms。これより
    // 約 17ms 長いので、作業スレッドの計算（1回に数 ms）はこの中に収まる。
    // Harvest のときは、後ろの余裕を Harvest の遅れまで長くする
    const auto& m = *impl;
    const int tail = params.index (P::worldF0Method) == 1 ? std::max (m.tailSamples, harvestTailFor (m.sr, params.get (P::pitchMinHz)))
                                                          : m.tailSamples;
    return m.chunkSamples + tail + m.fftSize / 2 + kSynthBuffer * 2;
}

void WorldVoice::process (float* data, int n, const ParamValues& params)
{
    auto& m = *impl;

    // 声の高さの求め方（と、Harvest のときは探す範囲）が変わったら、今の位置から始め直す
    const bool wantHarvest = params.index (P::worldF0Method) == 1;
    const double lo = params.get (P::pitchMinHz);
    const double hi = std::max (lo * 1.5, static_cast<double> (params.get (P::pitchMaxHz)));
    if (wantHarvest != m.useHarvest.load() || (wantHarvest && (lo != m.rangeMin.load() || hi != m.rangeMax.load())))
    {
        m.rangeMin.store (lo);
        m.rangeMax.store (hi);
        m.harvestTail.store (harvestTailFor (m.sr, lo));
        m.useHarvest.store (wantHarvest);
        reset();
    }

    const auto inSize = static_cast<std::int64_t> (m.input.size());
    const auto outSize = static_cast<std::int64_t> (m.output.size());
    const int latency = m.latency();

    // ---- 入力を渡す ----
    const auto base = m.written.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        m.input[static_cast<size_t> ((base + i) % inSize)] = data[i];
    m.writeParams (params);
    m.written.store (base + n, std::memory_order_release);

    if (m.realtime.load())
    {
        m.wake.notify_one(); // 作業スレッドを起こす
    }
    else
    {
        // リアルタイムでないときは、その場で、そろった分を全部分析する
        std::lock_guard<std::mutex> lock (m.chunkMutex);
        while (m.processOneChunk())
            ;
    }

    // ---- 出来上がった音を受け取る（入力より latency サンプル遅れた時刻の音。間に合っていなければ無音） ----
    const auto ready = m.outWrite.load (std::memory_order_acquire);
    const auto origin = m.restartAt.load (std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
    {
        const std::int64_t want = base + i - latency;
        if (want >= origin && want < ready && want >= ready - outSize)
            data[i] = m.output[static_cast<size_t> (want % outSize)];
        else
            data[i] = 0.0f;
    }
}

} // namespace vcab
