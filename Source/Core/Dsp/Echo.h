// エコー（やまびこ）
//
// 遅らせた音を、繰り返しながら足す。遅れた音を、もう一度遅れの入口に戻す（フィードバック）と、
// 何度も繰り返す。戻すたびに高い音を少し削る（1次の低域通過）ので、繰り返すほどこもっていく（テープエコー風）。
// 元の音はそのまま残し、遅れた音を「混ぜる量」だけ足す。
#pragma once

#include "Biquad.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vcab
{

class Echo
{
public:
    struct Settings
    {
        double timeMs = 250;
        double feedback = 0.3;   // 0〜0.9
        double dampingHz = 5000; // 繰り返すたびに、これより上を削る
        double mix = 0.25;       // 遅れた音を足す量（0〜1）
    };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        buffer.assign (static_cast<size_t> (sr * 1.1) + 4, 0.0f);
        reset();
    }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
        lowpass = 0;
        smoothedDelay = -1;
    }

    void process (float* data, int n, const Settings& s)
    {
        const double target = std::clamp (s.timeMs * 0.001 * sr, 1.0, static_cast<double> (buffer.size() - 3));
        if (smoothedDelay < 0)
            smoothedDelay = target;
        // 時間のつまみを動かしたときにプツッとならないよう、遅れの長さをゆっくり追いかける（約 50ms）
        const double delayCoeff = 1.0 - std::exp (-1.0 / (0.05 * sr));
        const double lpCoeff = 1.0 - std::exp (-2 * kPi * s.dampingHz / sr);
        const auto size = static_cast<int> (buffer.size());

        for (int i = 0; i < n; ++i)
        {
            smoothedDelay += delayCoeff * (target - smoothedDelay);
            const double readPos = writePos - smoothedDelay;
            const int i0 = static_cast<int> (std::floor (readPos));
            const double frac = readPos - i0;
            const float a = buffer[static_cast<size_t> ((i0 % size + size) % size)];
            const float b = buffer[static_cast<size_t> (((i0 + 1) % size + size) % size)];
            const double delayed = a + (b - a) * frac;

            lowpass += lpCoeff * (delayed - lowpass);
            buffer[static_cast<size_t> (writePos)] = static_cast<float> (data[i] + s.feedback * lowpass);
            writePos = (writePos + 1) % size;

            data[i] = static_cast<float> (data[i] + s.mix * lowpass);
        }
    }

private:
    double sr = 48000;
    std::vector<float> buffer;
    int writePos = 0;
    double lowpass = 0, smoothedDelay = -1;
};

} // namespace vcab
