// EQ: ローカット、ハイカット、4つの帯域（低域シェルフ、中域ピーク2つ、高域シェルフ）
// 画面に特性の線を描くときは、画面の側で別にこのクラスを作り、同じ値を設定して magnitudeDbAt を使う
// （音の処理のスレッドで使っているものには触らない）。
#pragma once

#include "../Params.h"
#include "CutFilter.h"

#include <array>

namespace vcab
{

class Equalizer
{
public:
    struct BandSettings
    {
        float freq = 1000, gainDb = 0, q = 1;
        bool operator== (const BandSettings& o) const noexcept { return freq == o.freq && gainDb == o.gainDb && q == o.q; }
    };

    enum Band { lowShelf, peak1, peak2, highShelf, numBands };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        lowCut.prepare (sr);
        highCut.prepare (sr);
        for (auto& b : last) b.freq = -1;
        reset();
    }

    void reset()
    {
        lowCut.reset();
        highCut.reset();
        for (auto& f : bands) f.reset();
    }

    void setParams (const ParamValues& p)
    {
        lowCutOn = p.on (P::eqLowCutOn);
        highCutOn = p.on (P::eqHighCutOn);
        lowCut.setParams (p.get (P::eqLowCutFreq), p.index (P::eqLowCutSlope));
        highCut.setParams (p.get (P::eqHighCutFreq), p.index (P::eqHighCutSlope));

        // オフの帯域は、増減 0dB として扱う（0dB の帯域は処理を飛ばす）
        auto gain = [&] (P on, P g) { return p.on (on) ? p.get (g) : 0.0f; };
        setBand (lowShelf, { p.get (P::eqLowShelfFreq), gain (P::eqLowShelfOn, P::eqLowShelfGain), p.get (P::eqLowShelfQ) });
        setBand (peak1, { p.get (P::eqPeak1Freq), gain (P::eqPeak1On, P::eqPeak1Gain), p.get (P::eqPeak1Q) });
        setBand (peak2, { p.get (P::eqPeak2Freq), gain (P::eqPeak2On, P::eqPeak2Gain), p.get (P::eqPeak2Q) });
        setBand (highShelf, { p.get (P::eqHighShelfFreq), gain (P::eqHighShelfOn, P::eqHighShelfGain), p.get (P::eqHighShelfQ) });
    }

    void process (float* data, int n) noexcept
    {
        if (lowCutOn) lowCut.process (data, n);
        if (highCutOn) highCut.process (data, n);

        for (size_t b = 0; b < numBands; ++b)
        {
            if (last[b].gainDb == 0.0f)
                continue; // 0dB の帯域は何もしないので飛ばす
            auto& f = bands[b];
            for (int i = 0; i < n; ++i)
                data[i] = f.process (data[i]);
        }
    }

    // 周波数 freq での増減（dB）。画面に線を描くため
    double magnitudeDbAt (double freq) const
    {
        double m = 1;
        if (lowCutOn) m *= lowCut.magnitudeAt (freq);
        if (highCutOn) m *= highCut.magnitudeAt (freq);
        for (size_t b = 0; b < numBands; ++b)
            if (last[b].gainDb != 0.0f)
                m *= coeffs[b].magnitudeAt (sr, freq);
        return 20.0 * std::log10 (std::max (m, 1.0e-9));
    }

private:
    void setBand (Band b, BandSettings s)
    {
        const auto i = static_cast<size_t> (b);
        s.freq = std::clamp (s.freq, 10.0f, static_cast<float> (sr * 0.45));
        if (s == last[i])
            return;
        if (last[i].gainDb == 0.0f && s.gainDb != 0.0f)
            bands[i].reset(); // 飛ばしていた帯域を使い始めるときは、古い状態を消す
        last[i] = s;

        switch (b)
        {
            case lowShelf:  coeffs[i] = BiquadCoeffs::lowShelf (sr, s.freq, s.q, s.gainDb); break;
            case highShelf: coeffs[i] = BiquadCoeffs::highShelf (sr, s.freq, s.q, s.gainDb); break;
            default:        coeffs[i] = BiquadCoeffs::peak (sr, s.freq, s.q, s.gainDb); break;
        }
        bands[i].setCoeffs (coeffs[i]);
    }

    double sr = 48000;
    bool lowCutOn = true, highCutOn = true;
    CutFilter lowCut { CutFilter::Kind::lowCut };
    CutFilter highCut { CutFilter::Kind::highCut };
    std::array<Biquad, numBands> bands;
    std::array<BiquadCoeffs, numBands> coeffs;
    std::array<BandSettings, numBands> last;
};

} // namespace vcab
