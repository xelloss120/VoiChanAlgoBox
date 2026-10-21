// 2次の IIR フィルタ（双2次フィルタ）。係数は RBJ の Audio EQ Cookbook の式。
#pragma once

#include <cmath>

namespace vcab
{

constexpr double kPi = 3.14159265358979323846;

struct BiquadCoeffs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static BiquadCoeffs highPass (double sampleRate, double freq, double q)
    {
        const double w = 2 * kPi * freq / sampleRate;
        const double cw = std::cos (w), alpha = std::sin (w) / (2 * q);
        const double a0 = 1 + alpha;
        return { (1 + cw) / 2 / a0, -(1 + cw) / a0, (1 + cw) / 2 / a0, -2 * cw / a0, (1 - alpha) / a0 };
    }

    static BiquadCoeffs lowPass (double sampleRate, double freq, double q)
    {
        const double w = 2 * kPi * freq / sampleRate;
        const double cw = std::cos (w), alpha = std::sin (w) / (2 * q);
        const double a0 = 1 + alpha;
        return { (1 - cw) / 2 / a0, (1 - cw) / a0, (1 - cw) / 2 / a0, -2 * cw / a0, (1 - alpha) / a0 };
    }

    // バンドパス（中心の周波数で 0dB。中心から離れるほど下がる）
    static BiquadCoeffs bandPass (double sampleRate, double freq, double q)
    {
        const double w = 2 * kPi * freq / sampleRate;
        const double cw = std::cos (w), alpha = std::sin (w) / (2 * q);
        const double a0 = 1 + alpha;
        return { alpha / a0, 0, -alpha / a0, -2 * cw / a0, (1 - alpha) / a0 };
    }

    // 1次（6dB/oct）。双一次変換で作る。b2 と a2 は 0。
    static BiquadCoeffs firstOrderHighPass (double sampleRate, double freq)
    {
        const double k = std::tan (kPi * freq / sampleRate);
        const double a0 = k + 1;
        return { 1 / a0, -1 / a0, 0, (k - 1) / a0, 0 };
    }

    static BiquadCoeffs firstOrderLowPass (double sampleRate, double freq)
    {
        const double k = std::tan (kPi * freq / sampleRate);
        const double a0 = k + 1;
        return { k / a0, k / a0, 0, (k - 1) / a0, 0 };
    }

    // ピーク（その周波数の周りを持ち上げる・下げる）。gainDb は増減の量
    static BiquadCoeffs peak (double sampleRate, double freq, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2 * kPi * freq / sampleRate;
        const double cw = std::cos (w), alpha = std::sin (w) / (2 * q);
        const double a0 = 1 + alpha / A;
        return { (1 + alpha * A) / a0, -2 * cw / a0, (1 - alpha * A) / a0, -2 * cw / a0, (1 - alpha / A) / a0 };
    }

    // 低域シェルフ（この周波数より下をまとめて持ち上げる・下げる）
    static BiquadCoeffs lowShelf (double sampleRate, double freq, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2 * kPi * freq / sampleRate;
        const double cw = std::cos (w), alpha = std::sin (w) / (2 * q);
        const double sq = 2 * std::sqrt (A) * alpha;
        const double a0 = (A + 1) + (A - 1) * cw + sq;
        return { A * ((A + 1) - (A - 1) * cw + sq) / a0,
                 2 * A * ((A - 1) - (A + 1) * cw) / a0,
                 A * ((A + 1) - (A - 1) * cw - sq) / a0,
                 -2 * ((A - 1) + (A + 1) * cw) / a0,
                 ((A + 1) + (A - 1) * cw - sq) / a0 };
    }

    // 高域シェルフ（この周波数より上をまとめて持ち上げる・下げる）
    static BiquadCoeffs highShelf (double sampleRate, double freq, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2 * kPi * freq / sampleRate;
        const double cw = std::cos (w), alpha = std::sin (w) / (2 * q);
        const double sq = 2 * std::sqrt (A) * alpha;
        const double a0 = (A + 1) - (A - 1) * cw + sq;
        return { A * ((A + 1) + (A - 1) * cw + sq) / a0,
                 -2 * A * ((A - 1) + (A + 1) * cw) / a0,
                 A * ((A + 1) + (A - 1) * cw - sq) / a0,
                 2 * ((A - 1) - (A + 1) * cw) / a0,
                 ((A + 1) - (A - 1) * cw - sq) / a0 };
    }

    // この係数のフィルタが、周波数 freq の音を何倍にするか（画面に EQ の線を描くため）
    double magnitudeAt (double sampleRate, double freq) const
    {
        const double w = 2 * kPi * freq / sampleRate;
        const double c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2 * w), s2 = std::sin (2 * w);
        const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
        const double dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
        return std::sqrt ((nr * nr + ni * ni) / (dr * dr + di * di));
    }

    // ノッチ（その周波数だけを細く削る）。depth は 0〜1 の残す割合（0 で完全に消す）。
    static BiquadCoeffs notch (double sampleRate, double freq, double q, double depthGain)
    {
        // ピーキング EQ で、削る量を有限にしたノッチを作る
        const double A = std::sqrt (depthGain);
        const double w = 2 * kPi * freq / sampleRate;
        const double cw = std::cos (w), alpha = std::sin (w) / (2 * q);
        const double a0 = 1 + alpha / A;
        return { (1 + alpha * A) / a0, -2 * cw / a0, (1 - alpha * A) / a0, -2 * cw / a0, (1 - alpha / A) / a0 };
    }
};

class Biquad
{
public:
    void setCoeffs (const BiquadCoeffs& c) noexcept { k = c; }
    void reset() noexcept { z1 = z2 = 0; }

    float process (float x) noexcept
    {
        // 転置直接形 II
        const double y = k.b0 * x + z1;
        z1 = k.b1 * x - k.a1 * y + z2;
        z2 = k.b2 * x - k.a2 * y;
        return static_cast<float> (y);
    }

private:
    BiquadCoeffs k;
    double z1 = 0, z2 = 0;
};

} // namespace vcab
