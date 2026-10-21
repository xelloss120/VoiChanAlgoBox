#include "WebRtcNs.h"

#if defined(_MSC_VER)
#pragma warning(push, 0) // WebRTC のヘッダの警告は出さない（こちらのコードではないため）
#endif
#include "modules/audio_processing/audio_buffer.h"
#include "modules/audio_processing/ns/noise_suppressor.h"
#include "modules/audio_processing/three_band_filter_bank.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <array>
#include <cmath>

namespace vcab
{

namespace
{
constexpr int kBands = webrtc::ThreeBandFilterBank::kNumBands;
constexpr int kBandSize = webrtc::ThreeBandFilterBank::kSplitBandSize;

webrtc::NsConfig::SuppressionLevel levelFor (int index)
{
    switch (index)
    {
        case 0: return webrtc::NsConfig::SuppressionLevel::k6dB;
        case 1: return webrtc::NsConfig::SuppressionLevel::k12dB;
        case 2: return webrtc::NsConfig::SuppressionLevel::k18dB;
        default: return webrtc::NsConfig::SuppressionLevel::k21dB;
    }
}
} // namespace

struct WebRtcNs::Impl
{
    explicit Impl (int levelIndex)
    {
        webrtc::NsConfig config;
        config.target_level = levelFor (levelIndex);
        ns = std::make_unique<webrtc::NoiseSuppressor> (config, 48000, 1);
        audio.setBands ({ { bands[0].data(), bands[1].data(), bands[2].data() } });
    }

    std::unique_ptr<webrtc::NoiseSuppressor> ns;
    webrtc::ThreeBandFilterBank filterBank;
    webrtc::AudioBuffer audio;
    std::array<std::array<float, kBandSize>, kBands> bands {};
    std::array<float, kFrame> full {};
};

WebRtcNs::WebRtcNs() = default;
WebRtcNs::~WebRtcNs() = default;

void WebRtcNs::reset()
{
    impl.reset();
    if (level >= 0)
        impl = std::make_unique<Impl> (level);
}

void WebRtcNs::setMaxReduction (float db)
{
    // 削る量の上限以上で一番近い段階（上限は、外で元の音を混ぜて守る）
    const int index = db <= 6.0f ? 0 : db <= 12.0f ? 1 : db <= 18.0f ? 2 : 3;
    if (index == level && impl != nullptr)
        return;
    level = index;
    impl = std::make_unique<Impl> (level);
}

float WebRtcNs::process (const float* in, float* out)
{
    if (impl == nullptr)
        setMaxReduction (12.0f);
    auto& m = *impl;
    double inPower = 0, outPower = 0;
    for (int i = 0; i < kFrame; ++i)
    {
        m.full[static_cast<size_t> (i)] = in[i] * 32768.0f; // 16 ビットの整数の範囲
        inPower += static_cast<double> (in[i]) * in[i];
    }
    std::array<rtc::ArrayView<float>, kBands> views { rtc::ArrayView<float> (m.bands[0]), rtc::ArrayView<float> (m.bands[1]),
                                                      rtc::ArrayView<float> (m.bands[2]) };
    m.filterBank.Analysis (m.full, views);
    m.ns->Analyze (m.audio);
    m.ns->Process (&m.audio);
    m.filterBank.Synthesis (views, m.full);
    for (int i = 0; i < kFrame; ++i)
    {
        out[i] = m.full[static_cast<size_t> (i)] * (1.0f / 32768.0f);
        outPower += static_cast<double> (out[i]) * out[i];
    }
    return static_cast<float> (std::sqrt (outPower / std::max (inPower, 1.0e-20)));
}

} // namespace vcab
