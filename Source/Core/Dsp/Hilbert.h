// ヒルベルト変換の近似（90 度ずれた2本の音を作る）
//
// 2本の全域通過フィルタの組で、音を「同相成分 I」と「直交成分 Q」に分ける。位相の差が、ほぼ全帯域
// （サンプルレートの約 0.001〜0.499 倍）で 90 度になる。周波数シフトの方式で使う。
// 係数は Olli Niemitalo の設計（2次の全域通過 × 4段 × 2本。各係数の2乗が c）。
// この係数の組では、I の側を1サンプル遅らせると Q が I より 90 度進んだ組になり、
// 「I cos(φ) + Q sin(φ)」で全部の周波数が φ の進む速さだけ上にずれる。
#pragma once

#include <array>

namespace vcab
{

class HilbertPair
{
public:
    HilbertPair()
    {
        constexpr double coeffsI[] = { 0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737 };
        constexpr double coeffsQ[] = { 0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278 };
        for (size_t i = 0; i < 4; ++i)
        {
            pathI.c[i] = coeffsI[i] * coeffsI[i];
            pathQ.c[i] = coeffsQ[i] * coeffsQ[i];
        }
    }

    void reset() noexcept
    {
        pathI.reset();
        pathQ.reset();
        delayed = 0;
    }

    void process (double x, double& outI, double& outQ) noexcept
    {
        const double i0 = pathI.process (x);
        outI = delayed;
        delayed = i0;
        outQ = pathQ.process (x);
    }

private:
    // 2次の全域通過（y[n] = c (x[n] + y[n-2]) - x[n-2]）を4段
    struct AllpassChain
    {
        std::array<double, 4> c {};
        std::array<double, 4> x1 {}, x2 {}, y1 {}, y2 {};

        double process (double x) noexcept
        {
            for (size_t i = 0; i < c.size(); ++i)
            {
                const double y = c[i] * (x + y2[i]) - x2[i];
                x2[i] = x1[i];
                x1[i] = x;
                y2[i] = y1[i];
                y1[i] = y;
                x = y;
            }
            return x;
        }

        void reset() noexcept { x1 = x2 = y1 = y2 = {}; }
    };

    AllpassChain pathI, pathQ;
    double delayed = 0;
};

} // namespace vcab
