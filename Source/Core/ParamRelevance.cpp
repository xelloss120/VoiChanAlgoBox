#include "ParamRelevance.h"

#include "Choices.h"
#include "Dsp/DenoiseSettings.h"

namespace vcab
{

namespace
{
Relevance shownIf (bool used) { return used ? Relevance::active : Relevance::hidden; }
Relevance enabledIf (bool used) { return used ? Relevance::active : Relevance::disabled; }

Relevance repairRelevance (P id, const ParamValues& p)
{
    switch (id)
    {
        // 「補間でつなぎ直す」は下げる量を使わない
        case P::repairTransientDepth: return shownIf (choiceAs (p.index (P::repairTransientMode), RepairFix::interpolate) == RepairFix::lower);
        case P::repairClickDepth:     return shownIf (choiceAs (p.index (P::repairClickMode), RepairFix::interpolate) == RepairFix::lower);
        default:                      return Relevance::active;
    }
}

Relevance denoiseRelevance (P id, const ParamValues& p)
{
    const auto s = DenoiseSettings::fromParams (p);
    const bool bandSplit = s.engine == DenoiseEngine::band || s.engine == DenoiseEngine::comb;
    const bool fft = s.isFftFamily();
    switch (id)
    {
        // 帯域分割式の決め方と、決め方ごとのつまみ
        case P::denoiseRule:        return shownIf (bandSplit);
        case P::denoiseThreshold:   return shownIf (bandSplit && s.rule != BandRule::snr);
        case P::denoiseRatio:       return shownIf (bandSplit && s.rule == BandRule::expander);
        case P::denoiseStrength:
            if (! fft && ! bandSplit)
                return Relevance::hidden;
            // ゲート式・エキスパンダー式では使わないが、行は残す（決め方を変えても、行がずれないように）
            return enabledIf (fft || s.rule == BandRule::snr);
        // FFT の方式のつまみ
        case P::denoiseFftRule:
        case P::denoiseFftSize:
        case P::denoiseSmoothing:
        case P::denoisePresence:    return shownIf (fft);
        case P::denoiseSmoothWidth: return shownIf (fft && s.smoothing == Smoothing::frequency);
        case P::denoiseComfort:     return shownIf (s.engine == DenoiseEngine::fft);                         // FFT で戻す方式だけ
        case P::denoiseOnset:       return shownIf (s.engine == DenoiseEngine::fftLowLatency && s.bandSynthesis);
        // 下げる量の動きの速さ（帯域分割式と、FFT の方式で時間でならすとき）
        case P::denoiseAttackMs:
        case P::denoiseReleaseMs:   return shownIf (bandSplit || (fft && s.smoothing == Smoothing::time));
        // DeepFilterNet
        case P::denoiseDfnModel:
        case P::denoiseDfnPostFilter: return shownIf (s.engine == DenoiseEngine::deepFilter);
        // 倍音を意識する方式（くし形フィルタと、FFT の倍音の保護）
        case P::denoisePitchMethod: return shownIf (s.needsPitch());
        case P::denoiseCombDepth:   return shownIf (s.engine == DenoiseEngine::comb);
        case P::denoiseProtect:     return shownIf (s.needsPitch() && s.engine != DenoiseEngine::comb);
        // ノイズを自分で見積もる方式（AI と WebRTC NS）は、ノイズの見積もりと残響の抑制を使わない
        case P::denoiseNoiseMode:
        case P::denoiseDereverbOn:
        case P::denoiseRt60:        return enabledIf (! s.estimatesNoiseItself());
        case P::denoiseLevelThreshold:
            if (noiseTracking (s.noiseMode) != NoiseTracking::level)
                return Relevance::hidden;
            return enabledIf (! s.estimatesNoiseItself());
        case P::denoiseRnnThreshold:
            if (noiseTracking (s.noiseMode) != NoiseTracking::rnnoise)
                return Relevance::hidden;
            return enabledIf (! s.estimatesNoiseItself());
        case P::denoiseMinWindow:
            if (noiseTracking (s.noiseMode) != NoiseTracking::minimum)
                return Relevance::hidden;
            return enabledIf (! s.estimatesNoiseItself());
        case P::denoiseSppSnr:
            if (noiseTracking (s.noiseMode) != NoiseTracking::gerkmann)
                return Relevance::hidden;
            return enabledIf (! s.estimatesNoiseItself());
        default:                    return Relevance::active;
    }
}

Relevance voiceRelevance (P id, const ParamValues& p, bool formantSupported)
{
    const auto& a = algorithmInfo (p.index (P::algorithm));
    using C = VoiceCore;
    switch (id)
    {
        case P::dlWindowMs:       return shownIf (a.core == C::delayLine);
        case P::wsolaWindowMs:
        case P::wsolaSearchMs:    return shownIf (a.core == C::wsola);
        case P::psolaMarking:     return shownIf (a.core == C::psola);
        case P::pvFftSize:
        case P::formantMethod:    return shownIf (a.core == C::phaseVocoder || a.core == C::sinusoidal);
        case P::pvPhaseLock:      return shownIf (a.core == C::phaseVocoder);
        case P::smMaxSines:
        case P::smNoiseOn:        return shownIf (a.core == C::sinusoidal);
        case P::fsReferenceHz:    return shownIf (a.core == C::frequencyShift);
        case P::bsBands:
        case P::bsFormantMethod:  return shownIf (a.core == C::bandShift);
        case P::cvBands:          return shownIf (a.core == C::channelVocoder);
        case P::mixedExcitation:
        case P::glottalShape:     return shownIf (a.isVocoder());
        case P::worldF0Method:
        case P::worldBreath:      return shownIf (a.core == C::world);
        case P::lpcOrder:
        case P::lpcBandwidth:
        case P::lpcInterpolation:
        case P::lpcEmphasis:      return shownIf (a.envelope == Envelope::lpc);
        // ピッチ検出（WORLD は自前の DIO か Harvest で求めるので、探す範囲だけ使う）
        case P::pitchMethod:      return shownIf (a.usesPitchDetection() && a.core != C::world);
        case P::pitchMinHz:
        case P::pitchMaxHz:
        case P::intonation:       return shownIf (a.usesPitchDetection());
        // フォルマントを分けられない方式では、フォルマントは灰色、動かし方は出さない
        case P::formant:          return enabledIf (formantSupported);
        case P::formantWarp:
        case P::formantF1:
        case P::formantF2:
        case P::formantF3:        return shownIf (formantSupported);
        // グラフの選択は、両方のグラフを出せる方式だけ（出せるのが 1 つなら、それを出すので選ぶ必要がない）
        case P::voiceGraph:       return shownIf (a.usesPitchDetection() && formantSupported);
        default:                  return Relevance::active;
    }
}
} // namespace

Relevance relevance (P id, const ParamValues& p, bool formantSupported)
{
    switch (id)
    {
        case P::gateRatio:
        case P::gateKnee: return enabledIf (choiceAs (p.index (P::gateMode), GateMode::expander) == GateMode::expander);
        default: break;
    }
    if (id >= P::repairOn && id <= P::repairBreathDepth)
        return repairRelevance (id, p);
    if (id >= P::denoiseOn && id <= P::denoiseRt60)
        return denoiseRelevance (id, p);
    if (id >= P::voiceOn && id <= P::worldBreath)
        return voiceRelevance (id, p, formantSupported);
    if (id == P::voiceGraph)
        return voiceRelevance (id, p, formantSupported);
    return Relevance::active;
}

} // namespace vcab
