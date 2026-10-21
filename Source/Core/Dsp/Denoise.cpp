#include "Denoise.h"

#include "DenoiseDetail.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace vcab
{

using namespace denoise_detail;



void Denoiser::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    band.prepare (sr);
    for (int i = 0; i < 3; ++i)
    {
        ffts[static_cast<size_t> (i)].prepare (sr, fftSizeFor (sr, i));
        lowLatency[static_cast<size_t> (i)].prepare (sr, fftSizeFor (sr, i));
    }
    dry.assign (static_cast<size_t> (std::max (maxBlockSize, 4096)), 0.0f);
    comb.prepare (sr);
    pitch.prepare (sr);
    rnnoise.prepare (sr, static_cast<int> (dry.size()));
    deepFilter.prepare (sr, static_cast<int> (dry.size()));
    deepFilterLowLatency.prepare (sr, static_cast<int> (dry.size()));
    webrtcNs.prepare (sr, static_cast<int> (dry.size()));
    voice.prepare (sr, static_cast<int> (dry.size()));
    current = -1;
    reset();
}


void Denoiser::reset()
{
    band.reset();
    for (auto& f : ffts)
        f.reset();
    for (auto& l : lowLatency)
        l.reset();
    rnnoise.reset();
    deepFilter.reset();
    deepFilterLowLatency.reset();
    webrtcNs.reset();
    voice.reset();
    voiceProb = 0;
    comb.reset();
    pitch.reset();
    pitchHz = 0;
    inPower = outPower = 0;
    reduction = 0;
}


NoiseTracker& Denoiser::trackerFor (const DenoiseSettings& s)
{
    const auto i = static_cast<size_t> (std::clamp (s.fftSizeIndex, 0, 2));
    switch (s.engine)
    {
        case DenoiseEngine::fft:           return ffts[i].tracker();
        case DenoiseEngine::fftLowLatency: return lowLatency[i].tracker();
        default:                           return band.tracker();
    }
}


AiDenoiser* Denoiser::aiFor (const DenoiseSettings& s)
{
    switch (s.engine)
    {
        case DenoiseEngine::rnnoise:    return &rnnoise;
        case DenoiseEngine::deepFilter: return s.dfnLowLatency ? &deepFilterLowLatency : &deepFilter;
        case DenoiseEngine::webrtc:     return &webrtcNs;
        default:                        return nullptr;
    }
}


int Denoiser::latencySamples (const DenoiseSettings& s) const
{
    if (s.engine == DenoiseEngine::fft)
        return ffts[static_cast<size_t> (std::clamp (s.fftSizeIndex, 0, 2))].latencySamples();
    if (const auto* ai = aiFor (s))
        return s.bandSynthesis ? 0 : ai->latencySamples();
    return 0; // 帯域分割式と FFT の低遅延の部品は遅延 0
}


void Denoiser::startLearning()
{
    // 選んでいない方式の分も、同じ 2 秒で覚える（後で切り替えても、覚えた値を使えるように）
    band.tracker().startLearning();
    for (auto& f : ffts)
        f.tracker().startLearning();
    for (auto& l : lowLatency)
        l.tracker().startLearning();
}


float Denoiser::learnProgress() const noexcept
{
    return band.tracker().learning() ? band.tracker().learnProgress() : -1.0f;
}


bool Denoiser::hasLearned (const DenoiseSettings& s) const
{
    return trackerFor (s).hasLearned();
}


bool Denoiser::noiseCurve (const DenoiseSettings& s, const float* freqs, float* outDb, int count) const
{
    if (s.estimatesNoiseItself())
        return false;
    const auto i = static_cast<size_t> (std::clamp (s.fftSizeIndex, 0, 2));
    for (int k = 0; k < count; ++k)
    {
        float d;
        if (s.engine == DenoiseEngine::fft)
            d = ffts[i].noiseDensity (freqs[k], s.noiseMode);
        else if (s.engine == DenoiseEngine::fftLowLatency)
            d = lowLatency[i].noiseDensity (freqs[k], s.noiseMode);
        else
            d = band.noiseDensity (freqs[k], s.noiseMode);
        outDb[k] = 10.0f * std::log10 (std::max (d, 1.0e-20f));
    }
    return true;
}


std::vector<float> Denoiser::exportLearned() const
{
    // 学習を終えたときだけ書き換わる値を読む。学習の終わりと同時に読むと混ざることがあるが、次の保存で直る
    std::vector<float> out { static_cast<float> (sr), 7.0f };
    auto add = [&out] (const NoiseTracker& t)
    {
        const auto& v = t.learnedValues();
        out.push_back (static_cast<float> (v.size()));
        out.push_back (t.hasLearned() ? 1.0f : 0.0f);
        out.insert (out.end(), v.begin(), v.end());
    };
    add (band.tracker());
    for (const auto& f : ffts)
        add (f.tracker());
    for (const auto& l : lowLatency)
        add (l.tracker());
    return out;
}


bool Denoiser::importLearned (const std::vector<float>& data)
{
    if (data.size() < 2 || std::abs (data[0] - sr) > 0.5 || data[1] != 7.0f)
        return false;
    size_t pos = 2;
    auto take = [&] (NoiseTracker& t)
    {
        if (pos + 2 > data.size())
            return false;
        const auto n = static_cast<size_t> (data[pos]);
        const bool valid = data[pos + 1] > 0.5f;
        pos += 2;
        if (pos + n > data.size() || n != t.learnedValues().size())
            return false;
        if (valid)
            t.importLearned (data.data() + pos, static_cast<int> (n));
        pos += n;
        return true;
    };
    bool ok = take (band.tracker());
    for (auto& f : ffts)
        ok = ok && take (f.tracker());
    for (auto& l : lowLatency)
        ok = ok && take (l.tracker());
    return ok;
}


void Denoiser::process (float* data, int n, const DenoiseSettings& s)
{
    const int sizeIndex = std::clamp (s.fftSizeIndex, 0, 2);
    auto& fft = ffts[static_cast<size_t> (sizeIndex)];
    auto& low = lowLatency[static_cast<size_t> (sizeIndex)];
    auto* ai = aiFor (s);
    // 部品・窓の長さ・決め方・モデル・帯域分割でかけるかのどれかが変わったら、前の状態が残らないようにする（覚えたノイズの値は残る）
    const int key = ((((static_cast<int> (s.engine) * 3 + sizeIndex) * 16 + static_cast<int> (s.fftRule)) * 2 + (s.dfnLowLatency ? 1 : 0)) * 2)
                    + (s.bandSynthesis ? 1 : 0);
    if (key != current)
    {
        if (s.needsPitch())
        {
            comb.reset();
            pitch.reset();
        }
        switch (s.engine)
        {
            case DenoiseEngine::fft:           fft.reset(); break;
            case DenoiseEngine::fftLowLatency: low.reset(); break;
            case DenoiseEngine::band:
            case DenoiseEngine::comb:          band.reset(); break;
            default:                           ai->reset(); break;
        }
        current = key;
    }
    if (! s.needsPitch())
        pitchHz = 0;

    for (int start = 0; start < n;)
    {
        const int len = std::min (n - start, static_cast<int> (dry.size()));
        float* d = data + start;
        std::copy (d, d + len, dry.begin());

        // 倍音を意識する方式: 声の高さを求める（デノイズの前の音で）
        float hz = 0;
        bool voiced = false;
        if (s.needsPitch() && s.params != nullptr)
        {
            pitchParams = *s.params;
            pitchParams.set (P::pitchMethod, static_cast<float> (s.pitchMethod));
            pitch.push (d, len, pitchParams);
            hz = pitch.latest().hz;
            voiced = pitch.latest().voiced && hz > 0;
            pitchHz = voiced ? hz : 0.0f;
        }

        // ノイズの見積もり方を伝える。RNNoise の声の判定を使うときは、先に RNNoise で声かどうかを判定して、見積もりに渡す
        if (! s.estimatesNoiseItself())
        {
            auto& tracker = trackerFor (s);
            tracker.setMode (s.noiseMode);
            tracker.setLevelThreshold (s.levelThresholdDb);
            tracker.setMinWindow (s.minWindowSeconds);
            tracker.setSppSnr (s.sppSnrDb);
        }
        const bool useVoice = noiseTracking (s.noiseMode) == NoiseTracking::rnnoise && ! s.estimatesNoiseItself();
        if (useVoice != voiceActive)
        {
            voice.reset(); // 切り替えた直後に、前の判定が残らないように
            voiceActive = useVoice;
        }
        if (useVoice)
        {
            voice.setThreshold (s.rnnThreshold);
            voice.process (d, len);
            trackerFor (s).setVoiceQuiet (voice.quietSamples());
        }

        // 下げる。あわせて、声のありそうさ（ゲートとの連携）
        switch (s.engine)
        {
            case DenoiseEngine::band:
                band.process (d, len, s);
                voiceProb = presenceFromSnr (band.snrDb());
                break;
            case DenoiseEngine::comb:
                band.process (d, len, s);
                comb.process (d, len, hz, voiced, s.combDepthDb);
                voiceProb = presenceFromSnr (band.snrDb());
                break;
            case DenoiseEngine::fft:
                fft.setPitch (hz, voiced);
                fft.process (d, len, s);
                voiceProb = presenceFromSnr (fft.snrDb());
                break;
            case DenoiseEngine::fftLowLatency:
                low.setPitch (hz, voiced);
                low.process (d, len, s);
                voiceProb = presenceFromSnr (low.snrDb());
                break;
            default:
                if (s.engine == DenoiseEngine::deepFilter)
                    ai->setPostFilter (s.dfnPostFilter);
                ai->process (d, len, s);
                voiceProb = ai->voiceProbability();
                break;
        }

        // 学習中は、選んでいない部品にも同じ音を見せる
        if (band.tracker().learning() || ffts[0].tracker().learning())
        {
            const bool bandUsed = s.engine == DenoiseEngine::band || s.engine == DenoiseEngine::comb;
            if (! bandUsed && band.tracker().learning())
                band.analyzeOnly (dry.data(), len);
            for (int i = 0; i < 3; ++i)
            {
                if (! (s.engine == DenoiseEngine::fft && i == sizeIndex) && ffts[static_cast<size_t> (i)].tracker().learning())
                    ffts[static_cast<size_t> (i)].analyzeOnly (dry.data(), len);
                if (! (s.engine == DenoiseEngine::fftLowLatency && i == sizeIndex) && lowLatency[static_cast<size_t> (i)].tracker().learning())
                    lowLatency[static_cast<size_t> (i)].analyzeOnly (dry.data(), len);
            }
        }

        // 削っている量（約 0.3 秒でならす。FFT の遅れはならす時間に比べて小さいので無視する）
        double in = 0, out = 0;
        for (int i = 0; i < len; ++i)
        {
            in += static_cast<double> (dry[static_cast<size_t> (i)]) * dry[static_cast<size_t> (i)];
            out += static_cast<double> (d[i]) * d[i];
        }
        const double a = 1.0 - std::exp (-len / (0.3 * sr));
        inPower += (in / len - inPower) * a;
        outPower += (out / len - outPower) * a;
        reduction = inPower > 1.0e-10 ? static_cast<float> (std::max (0.0, 10.0 * std::log10 ((inPower + 1.0e-12) / (outPower + 1.0e-12)))) : 0.0f;

        start += len;
    }
}

} // namespace vcab
