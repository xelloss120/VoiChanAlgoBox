// ハイカット・ローカット。傾き 6 / 12 / 24 / 48 dB/oct を、バターワース特性の段を重ねて作る。
#pragma once

#include "Biquad.h"

#include <algorithm>
#include <array>

namespace vcab
{

class CutFilter
{
public:
    enum class Kind { lowCut, highCut };

    explicit CutFilter (Kind k) : kind (k) {}

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        lastFreq = -1;
        reset();
    }

    void reset()
    {
        for (auto& s : stages)
            s.reset();
    }

    // slopeIndex: 0 = 6, 1 = 12, 2 = 24, 3 = 48 dB/oct
    void setParams (float freq, int slopeIndex)
    {
        freq = std::clamp (freq, 10.0f, static_cast<float> (sr * 0.45));
        if (freq == lastFreq && slopeIndex == lastSlope)
            return;

        if (slopeIndex != lastSlope)
            reset();

        lastFreq = freq;
        lastSlope = slopeIndex;

        // バターワースの各段の Q（次数ごと）
        static const double q2[] = { 0.70710678 };
        static const double q4[] = { 0.54119610, 1.30656296 };
        static const double q8[] = { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };

        const bool hp = kind == Kind::lowCut;
        numStages = 0;

        auto add = [&] (const BiquadCoeffs& c)
        {
            coeffs[static_cast<size_t> (numStages)] = c;
            stages[static_cast<size_t> (numStages++)].setCoeffs (c);
        };

        switch (slopeIndex)
        {
            case 0:
                add (hp ? BiquadCoeffs::firstOrderHighPass (sr, freq) : BiquadCoeffs::firstOrderLowPass (sr, freq));
                break;
            case 1:
                for (double q : q2) add (hp ? BiquadCoeffs::highPass (sr, freq, q) : BiquadCoeffs::lowPass (sr, freq, q));
                break;
            case 2:
                for (double q : q4) add (hp ? BiquadCoeffs::highPass (sr, freq, q) : BiquadCoeffs::lowPass (sr, freq, q));
                break;
            default:
                for (double q : q8) add (hp ? BiquadCoeffs::highPass (sr, freq, q) : BiquadCoeffs::lowPass (sr, freq, q));
                break;
        }
    }

    // 周波数 freq の音を何倍にするか（画面に線を描くため）
    double magnitudeAt (double freq) const
    {
        double m = 1;
        for (int s = 0; s < numStages; ++s)
            m *= coeffs[static_cast<size_t> (s)].magnitudeAt (sr, freq);
        return m;
    }

    void process (float* data, int n) noexcept
    {
        for (int s = 0; s < numStages; ++s)
        {
            auto& st = stages[static_cast<size_t> (s)];
            for (int i = 0; i < n; ++i)
                data[i] = st.process (data[i]);
        }
    }

private:
    Kind kind;
    double sr = 48000;
    float lastFreq = -1;
    int lastSlope = -1;
    int numStages = 0;
    std::array<Biquad, 4> stages;
    std::array<BiquadCoeffs, 4> coeffs;
};

} // namespace vcab
