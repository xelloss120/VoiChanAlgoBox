// ピッチ検出の DIO と Harvest（WORLD の声の高さの推定。TD-PSOLA などでも使えるようにしたもの）
//
// DIO: WORLD の方式の DioVoice と同じ区切り方。20ms（4 フレーム）ごとに、前に 120ms、後ろに 40ms の余裕を
//      付けた区間を DIO に渡し、真ん中の 4 フレームだけを使う（区間の端では正しく求まらないため）。
//      StoneMask で細かく直す。結果は 60ms（後ろの余裕 + 4 フレーム）遅れる。
//      DIO は一度に区間をまとめて計算するので、20ms に一度、少し長めの計算が入る（約 0.3ms）。
// Harvest: 少しずつ計算する版（HarvestStream）に、元のサンプルレートの音を渡し、決まったフレームを読む。
#include "PitchDetector.h"

#include "world/dio.h"
#include "world/stonemask.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr int kDioChunk = 4;   // 一度に使うフレームの数
constexpr int kDioLead = 24;   // 前の余裕（フレーム）
constexpr int kDioTail = 8;    // 後ろの余裕（フレーム）
} // namespace

int PitchDetector::dioDelayHops() { return kDioChunk + kDioTail; }
int PitchDetector::dioSegmentHops() { return kDioLead + kDioChunk + kDioTail; }

void PitchDetector::analyzeDio()
{
    const int phaseInChunk = static_cast<int> (hopCount % kDioChunk);
    ++hopCount;
    if (phaseInChunk == 0)
    {
        // 直近の区間を取り出して分析する（入力をためる長さより短い）
        const auto size = static_cast<int> (ring.size());
        const int length = dioSegmentHops() * hop;
        dioSegment.resize (static_cast<size_t> (length));
        for (int k = 0; k < length; ++k)
            dioSegment[static_cast<size_t> (k)] = ring[static_cast<size_t> ((ringPos - length + k + 2 * size) % size)];

        const int fsInt = static_cast<int> (std::lround (fs));
        DioOption option;
        InitializeDioOption (&option);
        option.frame_period = hop * 1000.0 / fs; // 5ms（この検出のやり直しの間隔と同じにする）
        option.f0_floor = curMinHz;
        option.f0_ceil = curMaxHz;
        option.speed = 1;
        const int frames = GetSamplesForDIO (fsInt, length, option.frame_period);
        dioTemporal.resize (static_cast<size_t> (frames));
        dioF0.resize (static_cast<size_t> (frames));
        Dio (dioSegment.data(), length, fsInt, &option, dioTemporal.data(), dioF0.data());

        double positions[kDioChunk], f0[kDioChunk];
        for (int f = 0; f < kDioChunk; ++f)
        {
            const int idx = std::min (frames - 1, kDioLead + f);
            positions[f] = dioTemporal[static_cast<size_t> (idx)];
            f0[f] = dioF0[static_cast<size_t> (idx)];
        }
        StoneMask (dioSegment.data(), length, fsInt, positions, f0, kDioChunk, dioOut);
    }

    // この回に出すのは、4 フレームのうち phaseInChunk 番目（今から dioDelayHops() 前の時刻）
    const double hz = dioOut[phaseInChunk];
    estimate.hz = static_cast<float> (hz);
    estimate.voiced = hz > 0;
    estimate.clarity = estimate.voiced ? 1.0f : 0.0f;
}

void PitchDetector::analyzeHarvest()
{
    // 今から delaySamples 前の時刻のフレーム（1ms ごと）
    const auto t = static_cast<double> (inputCount - harvestOrigin - delaySamples);
    const auto frameIndex = static_cast<std::int64_t> (std::llround (t / sr * 1000.0));
    const double hz = harvestStream.f0AtFrame (frameIndex);
    estimate.hz = static_cast<float> (hz);
    estimate.voiced = hz > 0;
    estimate.clarity = estimate.voiced ? 1.0f : 0.0f;
}

} // namespace vcab
