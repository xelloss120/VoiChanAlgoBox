// 方式の一覧。Choices.h の Algorithm の順番に、表（中心の部品 × 声色の求め方）から作る
#include "BandShifter.h"
#include "ChannelVocoder.h"
#include "DelayLineShifter.h"
#include "FrequencyShifter.h"
#include "LpcVoice.h"
#include "PhaseVocoder.h"
#include "PsolaShifter.h"
#include "SinusoidalModel.h"
#include "SourceExciter.h"
#include "WorldVoice.h"
#include "WsolaShifter.h"

namespace vcab
{

namespace
{
std::unique_ptr<VoiceAlgorithm> createCore (VoiceCore core)
{
    switch (core)
    {
        case VoiceCore::delayLine:      return std::make_unique<DelayLineShifter>();
        case VoiceCore::wsola:          return std::make_unique<WsolaShifter>();
        case VoiceCore::psola:          return std::make_unique<PsolaShifter>();
        case VoiceCore::phaseVocoder:   return std::make_unique<PhaseVocoder>();
        case VoiceCore::sinusoidal:     return std::make_unique<SinusoidalModel>();
        case VoiceCore::frequencyShift: return std::make_unique<FrequencyShifter>();
        case VoiceCore::bandShift:      return std::make_unique<BandShifter>();
        case VoiceCore::channelVocoder: return std::make_unique<ChannelVocoder>();
        case VoiceCore::sourceExciter:  return std::make_unique<SourceExciter>();
        case VoiceCore::world:          return std::make_unique<WorldVoice>();
    }
    return nullptr;
}
} // namespace

std::vector<std::unique_ptr<VoiceAlgorithm>> createAllAlgorithms()
{
    std::vector<std::unique_ptr<VoiceAlgorithm>> list;
    for (int i = 0; i < static_cast<int> (Algorithm::count); ++i)
    {
        const auto& info = algorithmInfo (i);
        auto core = createCore (info.core);
        if (info.envelope == Envelope::none)
            list.push_back (std::move (core));
        else
            list.push_back (std::make_unique<LpcVoice> (std::move (core), info.envelope)); // 声色と声帯の音に分けて、声帯の音を中心の部品へ
    }
    return list;
}

} // namespace vcab
