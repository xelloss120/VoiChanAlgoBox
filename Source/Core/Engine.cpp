#include "Engine.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
float peakDb (const float* data, int n)
{
    float m = 0;
    for (int i = 0; i < n; ++i)
        m = std::max (m, std::abs (data[i]));
    return 20.0f * std::log10 (std::max (m, 1.0e-6f));
}

float rmsDb (const float* data, int n)
{
    double sum = 0;
    for (int i = 0; i < n; ++i)
        sum += static_cast<double> (data[i]) * data[i];
    return static_cast<float> (10.0 * std::log10 (sum / std::max (1, n) + 1.0e-12));
}
} // namespace

Engine::Engine()
    : algorithms (createAllAlgorithms())
{
    setRealtime (true);
    for (auto& a : algorithms)
        a->setFormantTrack (&formantTracker.track());
}

void Engine::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;

    lowCut.prepare (sr);
    highCut.prepare (sr);
    notch.prepare (sr);
    declipper.prepare (sr);
    decrackler.prepare (sr);
    plosive.prepare (sr);
    breathRepair.prepare (sr);
    formantTracker.prepare (sr);
    transient.prepare (sr);
    mouthClick.prepare (sr);
    {
        // 補間で直すときの見つけ方は、下げる方式とそろえる（突発音: 2kHz より上、5ms 待つ。リップノイズ: 3kHz より上、2ms 待つ）
        ClickInterpolator::Shape keyboard;
        keyboard.detectHz = 2000; keyboard.windowMs = 5; keyboard.slowAttackMs = 100; keyboard.slowReleaseMs = 400;
        keyboard.fastAttackMs = 0.1; keyboard.fastReleaseMs = 1; keyboard.maxGapMs = 4;
        transientInterp.prepare (sr, keyboard);
        ClickInterpolator::Shape mouth;
        mouth.detectHz = 3000; mouth.windowMs = 2; mouth.slowAttackMs = 5; mouth.slowReleaseMs = 5;
        mouth.fastAttackMs = 0.05; mouth.fastReleaseMs = 0.5; mouth.maxGapMs = 2;
        mouthClickInterp.prepare (sr, mouth);
    }
    denoiser.prepare (sr, maxBlockSize);
    applyPendingLearned(); // 設定を読み込んだのが prepare より前なら、ここで反映する
    gate.prepare (sr);

    for (auto& a : algorithms)
        a->prepare (sr, maxBlockSize);

    eq.prepare (sr);
    deEsser.prepare (sr);
    compressor.prepare (sr);
    echo.prepare (sr);
    breath.prepare (sr);
    reverb.prepare (sr);
    limiter.prepare (sr);

    voiceStage.forget(); // 次の処理で、方式の状態を消してから使う
}

void Engine::importLearnedNoise (std::vector<float> data)
{
    std::lock_guard<std::mutex> lock (pendingLearnedMutex);
    pendingLearned = std::move (data);
    pendingLearnedReady.store (true);
}

void Engine::applyPendingLearned()
{
    if (! pendingLearnedReady.load())
        return;
    // 音の処理のスレッドから呼ぶので、待たない（取れなければ次のブロックで）
    std::unique_lock<std::mutex> lock (pendingLearnedMutex, std::try_to_lock);
    if (! lock.owns_lock())
        return;
    denoiser.importLearned (pendingLearned); // サンプルレートが違えば使わない
    pendingLearnedReady.store (false);
}

void Engine::reset()
{
    lowCut.reset();
    highCut.reset();
    notch.reset();
    declipper.reset();
    decrackler.reset();
    plosive.reset();
    breathRepair.reset();
    transient.reset();
    transientInterp.reset();
    mouthClick.reset();
    mouthClickInterp.reset();
    denoiser.reset();
    gate.reset();
    for (auto& a : algorithms)
        a->reset();
    eq.reset();
    deEsser.reset();
    compressor.reset();
    echo.reset();
    breath.reset();
    reverb.reset();
    limiter.reset();
}

VoiceAlgorithm& Engine::algorithmFor (const ParamValues& p) const
{
    const int idx = std::clamp (p.index (P::algorithm), 0, numAlgorithms() - 1);
    return *algorithms[static_cast<size_t> (idx)];
}

LatencyBreakdown Engine::latency (const ParamValues& p) const
{
    LatencyBreakdown l;
    if (p.on (P::repairOn))
        l.repair = (p.on (P::repairDeclipOn) ? declipper.latencySamples() : 0)
                 + (p.on (P::repairCrackleOn) ? decrackler.latencySamples() : 0)
                 + (p.on (P::repairTransientOn) ? (choiceAs (p.index (P::repairTransientMode), RepairFix::interpolate) == RepairFix::interpolate ? transientInterp.latencySamples() : transient.latencySamples()) : 0)
                 + (p.on (P::repairClickOn) ? (choiceAs (p.index (P::repairClickMode), RepairFix::interpolate) == RepairFix::interpolate ? mouthClickInterp.latencySamples() : mouthClick.latencySamples()) : 0)
                 + (p.on (P::repairBreathOn) ? breathRepair.latencySamples() : 0);
    l.denoise = p.on (P::denoiseOn) ? denoiser.latencySamples (DenoiseSettings::fromParams (p)) : 0;
    l.voice = p.on (P::voiceOn) ? algorithmFor (p).latencySamples (p) : 0;
    l.output = p.on (P::outputOn) ? limiter.latencySamples() : 0;
    return l;
}

bool Engine::algorithmSupportsFormant (const ParamValues& p) const
{
    return algorithmFor (p).supportsFormant (p);
}

void Engine::setRealtime (bool isRealtime)
{
    for (auto& a : algorithms)
        a->setRealtime (isRealtime);
    denoiser.setRealtime (isRealtime);
}

void Engine::process (float* data, int n, const ParamValues& p)
{
    meters.inputPeakDb.store (peakDb (data, n));

    processCut (data, n, p);
    processNotch (data, n, p);
    processRepair (data, n, p);
    processDenoise (data, n, p);
    processGate (data, n, p);
    processVoice (data, n, p);
    processBreath (data, n, p);
    processEq (data, n, p);
    processDeEsser (data, n, p);
    processCompressor (data, n, p);
    processEffects (data, n, p);
    processOutput (data, n, p);

    meters.outputPeakDb.store (peakDb (data, n));
}

// カット
void Engine::processCut (float* data, int n, const ParamValues& p)
{
    if (p.on (P::cutOn))
    {
        lowCut.setParams (p.get (P::lowCutFreq), p.index (P::lowCutSlope));
        highCut.setParams (p.get (P::highCutFreq), p.index (P::highCutSlope));
        if (p.on (P::lowCutOn))
            lowCut.process (data, n);
        if (p.on (P::highCutOn))
            highCut.process (data, n);
    }
}

// ノッチ
void Engine::processNotch (float* data, int n, const ParamValues& p)
{
    int notchShown = 0;
    if (p.on (P::notchOn))
    {
        AdaptiveNotch::Settings ns;
        ns.autoOn = p.on (P::notchAutoOn);
        ns.humOn = p.on (P::notchHumOn);
        ns.depthDb = p.get (P::notchDepth);
        ns.count = static_cast<int> (p.get (P::notchCount));
        ns.sensitivityDb = p.get (P::notchSensitivity);
        ns.mains = p.index (P::humFreq);
        ns.harmonics = static_cast<int> (p.get (P::humHarmonics));
        notch.process (data, n, ns);
        if (ns.autoOn)
        {
            float freqs[AdaptiveNotch::kMaxAuto];
            notchShown = notch.autoFrequencies (freqs);
            for (int i = 0; i < notchShown; ++i)
                meters.notchHz[i].store (freqs[i]);
        }
        meters.humMainsHz.store (static_cast<float> (notch.humMains()));
    }
    else
    {
        meters.humMainsHz.store (0);
    }
    meters.notchCount.store (notchShown);
}

// リペア（一瞬だけの音。音割れ → パチパチ → 吹かれ → 突発音 → リップノイズ → 息継ぎ）。オンにした瞬間は、前の状態を消してから使う
void Engine::processRepair (float* data, int n, const ParamValues& p)
{
    const bool on = p.on (P::repairOn);

    const bool declip = on && p.on (P::repairDeclipOn);
    if (declipStage.startsNow (declip))
        declipper.reset();
    if (declip)
        declipper.process (data, n, p.get (P::repairClipLevel));

    const bool crackle = on && p.on (P::repairCrackleOn);
    if (crackleStage.startsNow (crackle))
        decrackler.reset();
    if (crackle)
        decrackler.process (data, n, p.get (P::repairCrackleThreshold));

    const bool plosiveOn = on && p.on (P::repairPlosiveOn);
    if (plosiveStage.startsNow (plosiveOn))
        plosive.reset();
    if (plosiveOn)
        plosive.process (data, n, p.get (P::repairPlosiveFreq), p.get (P::repairPlosiveThreshold), p.get (P::repairPlosiveDepth));

    // 突発音とリップノイズは、直し方（下げる / 補間）で使う部品が違う。直し方を変えたときも作り直す
    const bool transientOn = on && p.on (P::repairTransientOn);
    const auto transientFix = choiceAs (p.index (P::repairTransientMode), RepairFix::interpolate);
    if (transientStage.startsNow (transientOn, static_cast<int> (transientFix)))
    {
        transient.reset();
        transientInterp.reset();
    }
    if (transientOn)
    {
        if (transientFix == RepairFix::interpolate)
            transientInterp.process (data, n, p.get (P::repairTransientThreshold));
        else
            transient.process (data, n, p.get (P::repairTransientThreshold), p.get (P::repairTransientDepth));
    }

    const bool clickOn = on && p.on (P::repairClickOn);
    const auto clickFix = choiceAs (p.index (P::repairClickMode), RepairFix::interpolate);
    if (clickStage.startsNow (clickOn, static_cast<int> (clickFix)))
    {
        mouthClick.reset();
        mouthClickInterp.reset();
    }
    if (clickOn)
    {
        if (clickFix == RepairFix::interpolate)
            mouthClickInterp.process (data, n, p.get (P::repairClickThreshold));
        else
            mouthClick.process (data, n, p.get (P::repairClickThreshold), p.get (P::repairClickDepth));
    }

    // 息継ぎ（言葉の合間）。一瞬の音を直した後に、まとめて下げる
    const bool breathOn = on && p.on (P::repairBreathOn);
    if (breathRepairStage.startsNow (breathOn))
        breathRepair.reset();
    if (breathOn)
        breathRepair.process (data, n, p.get (P::repairBreathMargin), p.get (P::repairBreathDepth));

    meters.repairTransients.store (transient.detections() + transientInterp.detections());
    meters.repairClips.store (declipper.detections());
    meters.repairPlosives.store (plosive.detections());
    meters.repairClicks.store (mouthClick.detections() + mouthClickInterp.detections());
    meters.repairCrackles.store (decrackler.detections());
    meters.repairBreaths.store (breathRepair.detections());
}

// デノイズ
void Engine::processDenoise (float* data, int n, const ParamValues& p)
{
    const auto ds = DenoiseSettings::fromParams (p);
    applyPendingLearned();
    if (learnRequested.exchange (false))
        denoiser.startLearning();
    if (denoiseStage.startsNow (p.on (P::denoiseOn)))
        denoiser.reset();
    if (p.on (P::denoiseOn))
    {
        denoiser.process (data, n, ds);
        meters.denoiseReductionDb.store (denoiser.reductionDb());
    }
    else
    {
        meters.denoiseReductionDb.store (0);
    }
    meters.denoiseLearnProgress.store (denoiser.learnProgress());
    meters.denoisePitchHz.store (p.on (P::denoiseOn) ? denoiser.detectedPitchHz() : 0.0f);
    meters.denoiseVoiceProbability.store (denoiser.voiceProbability());
    // ノイズの見積もりの表示（約 20 回 / 秒。画面がスペクトルを出していないとき〈閉じている、モニターが非表示など〉は計算しない）
    if (p.on (P::denoiseOn) && p.on (P::showNoiseEstimate) && meters.noiseCurveWanted.load (std::memory_order_relaxed))
    {
        noiseCurveCounter += n;
        if (noiseCurveCounter >= static_cast<int> (sr / 20))
        {
            noiseCurveCounter = 0;
            float freqs[EngineMeters::kNoiseCurvePoints], db[EngineMeters::kNoiseCurvePoints];
            for (int i = 0; i < EngineMeters::kNoiseCurvePoints; ++i)
                freqs[i] = EngineMeters::noiseCurveHz (i);
            const bool ok = denoiser.noiseCurve (ds, freqs, db, EngineMeters::kNoiseCurvePoints);
            if (ok)
                for (int i = 0; i < EngineMeters::kNoiseCurvePoints; ++i)
                    meters.noiseCurveDb[i].store (db[i]);
            meters.noiseCurveValid.store (ok);
        }
    }
    else
    {
        meters.noiseCurveValid.store (false);
    }
    meters.denoiseLearned.store (denoiser.hasLearned (ds));
}

// ゲート
void Engine::processGate (float* data, int n, const ParamValues& p)
{
    if (p.on (P::gateOn))
    {
        Gate::Settings gs;
        gs.thresholdDb = p.get (P::gateThreshold);
        gs.attackMs = p.get (P::gateAttackMs);
        gs.holdMs = p.get (P::gateHoldMs);
        gs.releaseMs = p.get (P::gateReleaseMs);
        gs.hysteresisDb = p.get (P::gateHysteresis);
        gs.rangeDb = p.get (P::gateRange);
        gs.expander = choiceAs (p.index (P::gateMode), GateMode::expander) == GateMode::expander;
        gs.ratio = p.get (P::gateRatio);
        gs.kneeDb = p.get (P::gateKnee);
        // デノイズの判断で開く（デノイズがオフのときは、いつもどおり音量で）
        gs.voiceProbability = p.on (P::gateFromDenoise) && p.on (P::denoiseOn) ? denoiser.voiceProbability() : -1.0f;
        gate.setSettings (gs);
        gate.process (data, n);

        meters.gateOpen.store (gate.isOpen());
        meters.gateLevelDb.store (gate.levelDb());
        meters.gateGainDb.store (gate.gainDb());
    }
}

// ボイチェン
void Engine::processVoice (float* data, int n, const ParamValues& p)
{
    const int algoIdx = std::clamp (p.index (P::algorithm), 0, numAlgorithms() - 1);
    // オンにしたときと方式を切り替えたときは、前の状態が残らないようにする（切り替えのノイズは許容する仕様）
    if (voiceStage.startsNow (p.on (P::voiceOn), algoIdx))
        algorithms[static_cast<size_t> (algoIdx)]->reset();
    if (p.on (P::voiceOn))
    {
        auto& algo = *algorithms[static_cast<size_t> (algoIdx)];
        // 動かし方が「フォルマントごと」のときは、5ms ずつに分けて、先に追いかけてから方式に渡す（方式が包絡を求める
        // 時刻と、追いかけた時刻をそろえるため。どちらも直近 25ms ほどの窓）
        const bool tracking = p.index (P::formantWarp) == FormantWarp::tracked;
        if (formantTrackingStage.startsNow (tracking))
            formantTracker.reset();
        if (tracking)
        {
            const int chunk = std::max (1, static_cast<int> (std::lround (0.005 * sr)));
            for (int pos = 0; pos < n; pos += chunk)
            {
                const int len = std::min (chunk, n - pos);
                formantTracker.process (data + pos, len);
                algo.process (data + pos, len, p);
            }
            const auto& t = formantTracker.track();
            for (int k = 0; k < 3; ++k)
                meters.formantHz[k].store (static_cast<float> (t.hz[k]));
            meters.formantValid.store (t.valid);
        }
        else
            algo.process (data, n, p);
        meters.formantTracking.store (tracking);

        if (const auto* est = algo.pitchEstimate())
        {
            meters.pitchHz.store (est->hz);
            meters.pitchVoiced.store (est->voiced);
            meters.pitchActive.store (true);
        }
        else
        {
            meters.pitchActive.store (false);
        }
    }
    else
    {
        meters.pitchActive.store (false);
    }
}

// ブレス
void Engine::processBreath (float* data, int n, const ParamValues& p)
{
    if (breathStage.startsNow (p.on (P::breathOn)))
        breath.reset();
    if (p.on (P::breathOn))
        breath.process (data, n, p.get (P::breathAmount) * 0.01, p.get (P::breathToneHz));
}

// EQ
void Engine::processEq (float* data, int n, const ParamValues& p)
{
    if (p.on (P::eqOn))
    {
        eq.setParams (p);
        eq.process (data, n);
    }
}

// ディエッサー
void Engine::processDeEsser (float* data, int n, const ParamValues& p)
{
    if (p.on (P::deessOn))
    {
        deEsser.setParams (p.get (P::deessFreq), p.get (P::deessThreshold), p.get (P::deessMaxReduction));
        deEsser.process (data, n);
        meters.deessGrDb.store (deEsser.gainReductionDb());
    }
    else
    {
        meters.deessGrDb.store (0);
    }
}

// コンプ
void Engine::processCompressor (float* data, int n, const ParamValues& p)
{
    if (p.on (P::compOn))
    {
        Compressor::Settings cs;
        cs.thresholdDb = p.get (P::compThreshold);
        cs.ratio = p.get (P::compRatio);
        cs.attackMs = p.get (P::compAttackMs);
        cs.releaseMs = p.get (P::compReleaseMs);
        cs.kneeDb = p.get (P::compKnee);
        compressor.setSettings (cs);
        compressor.process (data, n);
        meters.compLevelDb.store (compressor.inputLevelDb());
        meters.compGrDb.store (compressor.gainReductionDb());
    }
    else
    {
        // コンプがオフでも、グラフの点を出すために、コンプに入る音量だけは測る（オンのときと同じく、ならした音量）
        meters.compLevelDb.store (rmsDb (data, n));
        meters.compGrDb.store (0);
    }
}

// リバーブ（エコー → リバーブ）
void Engine::processEffects (float* data, int n, const ParamValues& p)
{
    const bool echoNow = p.on (P::fxOn) && p.on (P::echoOn);
    const bool reverbNow = p.on (P::fxOn) && p.on (P::reverbOn);
    if (echoStage.startsNow (echoNow))
        echo.reset(); // オンにした瞬間に、前の響きの残りが出ないように
    if (reverbStage.startsNow (reverbNow))
        reverb.reset();
    if (echoNow)
    {
        Echo::Settings es;
        es.timeMs = p.get (P::echoTimeMs);
        es.feedback = p.get (P::echoFeedback) * 0.01;
        es.dampingHz = p.get (P::echoDampingHz);
        es.mix = p.get (P::echoMix) * 0.01;
        echo.process (data, n, es);
    }
    if (reverbNow)
    {
        Reverb::Settings rs;
        rs.size = p.get (P::reverbSize) * 0.01;
        rs.damping = p.get (P::reverbDamping) * 0.01;
        rs.preDelayMs = p.get (P::reverbPreDelayMs);
        rs.mix = p.get (P::reverbMix) * 0.01;
        reverb.process (data, n, rs);
    }
}

// 出力（出力ゲインとリミッター。タブのオン・オフで両方まとめて切り替える）
void Engine::processOutput (float* data, int n, const ParamValues& p)
{
    if (p.on (P::outputOn))
    {
        const float outGain = std::pow (10.0f, p.get (P::outputGain) / 20.0f);
        for (int i = 0; i < n; ++i)
            data[i] *= outGain;
        limiter.setParams (p.get (P::limiterCeiling), p.get (P::limiterReleaseMs));
        limiter.process (data, n);
    }
}

} // namespace vcab
