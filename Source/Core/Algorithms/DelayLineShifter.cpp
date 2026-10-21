#include "DelayLineShifter.h"

#include "../Dsp/Biquad.h"

#include <algorithm>

namespace vcab
{

void DelayLineShifter::prepare (double sampleRate, int)
{
    sr = sampleRate;
    // 窓の長さの上限（100ms）に余裕を足した大きさ
    buffer.assign (static_cast<size_t> (sr * 0.12) + 4, 0.0f);
    reset();
}

void DelayLineShifter::reset()
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    writePos = 0;
    phase = 0;
}

float DelayLineShifter::readAt (double delaySamples) const noexcept
{
    // 直線補間で、書き込み位置から delaySamples 前の音を読む
    const auto size = static_cast<int> (buffer.size());
    const double readPos = writePos - delaySamples;
    const int i0 = static_cast<int> (std::floor (readPos));
    const double frac = readPos - i0;
    const int a = ((i0 % size) + size) % size;
    const int b = (a + 1) % size;
    return static_cast<float> (buffer[static_cast<size_t> (a)] * (1 - frac) + buffer[static_cast<size_t> (b)] * frac);
}

void DelayLineShifter::process (float* data, int n, const ParamValues& params)
{
    const double ratio = semitonesToRatio (params.get (P::pitch));
    const double window = std::max (8.0, params.get (P::dlWindowMs) * 0.001 * sr);
    const auto size = static_cast<int> (buffer.size());

    // 1サンプルごとに遅延が (1 - ratio) だけ変わる。ratio > 1 なら遅延が縮み、読み出しが速くなる。
    const double phaseStep = (1.0 - ratio) / window;

    for (int i = 0; i < n; ++i)
    {
        buffer[static_cast<size_t> (writePos)] = data[i];

        const double p1 = phase;
        const double p2 = std::fmod (phase + 0.5, 1.0);

        // 遅延が 0 または窓の端（= 戻る瞬間）で音量が 0 になる重み。2つの和は常に 1。
        const double g1 = std::sin (kPi * p1) * std::sin (kPi * p1);
        const double g2 = 1.0 - g1;

        const float y = static_cast<float> (g1 * readAt (p1 * window) + g2 * readAt (p2 * window));

        data[i] = y;

        phase += phaseStep;
        phase -= std::floor (phase);

        writePos = (writePos + 1) % size;
    }
}

int DelayLineShifter::latencySamples (const ParamValues& params) const
{
    // 遅延は 0〜窓の長さを行き来するので、平均は窓の半分
    return static_cast<int> (params.get (P::dlWindowMs) * 0.001 * sr * 0.5);
}

} // namespace vcab
