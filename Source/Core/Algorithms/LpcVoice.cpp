#include "LpcVoice.h"

#include "../Dsp/Biquad.h"
#include "../Dsp/Lpc.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr int kHistorySets = 64;   // 合成のフィルタの履歴の数（5ms × 64 = 320ms。遅延の最大より長く）
constexpr int kWarpFftSize = 1024; // フォルマントを動かすときの包絡の細かさ
constexpr double kEmphasis = 0.9;  // 高域強調の強さ（1 - 0.9z^-1。高い音ほど持ち上がる）
constexpr double kLagWindowHz = 60; // 帯域拡張の量（山の幅をおよそこれだけ広げる）
} // namespace

LpcVoice::LpcVoice (std::unique_ptr<VoiceAlgorithm> s, Envelope e) : shifter (std::move (s)), envelope (e) {}

namespace
{
// 何もしない部品（補正の段で、残差をそのまま戻すため）
class PassThrough : public VoiceAlgorithm
{
public:
    bool supportsFormant (const ParamValues&) const override { return true; }
    void prepare (double, int) override {}
    void reset() override {}
    void process (float*, int, const ParamValues&) override {}
    int latencySamples (const ParamValues&) const override { return 0; }
};
} // namespace

std::unique_ptr<LpcVoice> LpcVoice::makeFormantCorrector()
{
    auto c = std::make_unique<LpcVoice> (std::make_unique<PassThrough>(), Envelope::cepstrum);
    c->correction = true;
    // 補正するのはピッチを変えた後の声なので、声が高くても倍音の凹凸を拾わないように、ケプストラムで残す長さを
    // 短くする（400Hz の周期の半分）
    c->lifterSeconds = 0.0012;
    return c;
}

void LpcVoice::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    shifter->prepare (sr, maxBlockSize);
    hop = std::max (1, static_cast<int> (std::lround (0.005 * sr)));
    window = static_cast<int> (std::lround (0.025 * sr));

    history.assign (static_cast<size_t> (window), 0.0f);
    frame.assign (static_cast<size_t> (window), 0.0);
    hann.resize (static_cast<size_t> (window));
    windowPower = 0;
    for (int i = 0; i < window; ++i)
    {
        hann[static_cast<size_t> (i)] = 0.5 - 0.5 * std::cos (2 * kPi * (i + 0.5) / window);
        windowPower += hann[static_cast<size_t> (i)] * hann[static_cast<size_t> (i)];
    }
    autocorr.assign (kMaxOrder + 1, 0.0);
    a.assign (kMaxOrder + 1, 0.0);
    tmp.assign (kMaxOrder + 1, 0.0);
    synthesisSets.assign (kHistorySets, {});
    analysisSets.assign (kHistorySets, {});
    analysisState.assign (kMaxOrder, 0.0);
    synthesisState.assign (kMaxOrder, 0.0);
    residual.assign (static_cast<size_t> (std::max (1, maxBlockSize)), 0.0f);
    original.assign (residual.size(), 0.0f);
    delayedInput.assign (static_cast<size_t> (sr * 0.25), 0.0f);
    sourceDelay.assign (static_cast<size_t> (window), 0.0f);

    int cepSize = 1;
    while (cepSize < window)
        cepSize <<= 1;
    cepstrumFft.prepare (cepSize);
    cepstrumOrder = std::clamp (static_cast<int> (std::lround (sr * lifterSeconds * 0.5)), 16, kMaxOrder);
    cepstrumSpec.assign (static_cast<size_t> (cepSize), {});
    cepstrumLog.assign (static_cast<size_t> (cepSize / 2 + 1), 0.0);

    // メルケプストラム: 曲げた軸と元の軸の対応を、先に求めておく
    melOrder = std::clamp (static_cast<int> (std::lround (sr / 1200.0)), 16, kMaxOrder);
    melAlpha = mlsa::alphaFor (sr);
    const int cepHalf = cepSize / 2;
    melLogAmp.assign (static_cast<size_t> (cepHalf + 1), 0.0);
    melPos.resize (static_cast<size_t> (cepHalf + 1));
    melCos.resize (static_cast<size_t> (cepHalf + 1));
    for (int j = 0; j <= cepHalf; ++j)
    {
        melPos[static_cast<size_t> (j)] = mlsa::warp (kPi * j / cepHalf, -melAlpha) / kPi * cepHalf; // 曲げた軸 → 元の軸
        melCos[static_cast<size_t> (j)] = mlsa::warp (kPi * j / cepHalf, melAlpha);                   // 元の軸 → 曲げた軸
    }
    melMc.assign (kMaxOrder + 1, 0.0);
    melB.assign (kMaxOrder + 1, 0.0);
    melB2.assign (kMaxOrder + 1, 0.0);
    analysisMlsa.prepare (kMaxOrder);
    synthesisMlsa.prepare (kMaxOrder);

    fft.prepare (kWarpFftSize);
    spec.assign (kWarpFftSize, {});
    power.assign (kWarpFftSize / 2 + 1, 0.0);
    reset();
}

void LpcVoice::reset()
{
    shifter->reset();
    std::fill (history.begin(), history.end(), 0.0f);
    std::fill (analysisState.begin(), analysisState.end(), 0.0);
    std::fill (synthesisState.begin(), synthesisState.end(), 0.0);
    analysisMlsa.reset();
    synthesisMlsa.reset();
    for (auto& s : synthesisSets)
        s = {};
    for (auto& s : analysisSets)
        s = {};
    emphasisState = deemphasisState = 0;
    std::fill (delayedInput.begin(), delayedInput.end(), 0.0f);
    std::fill (sourceDelay.begin(), sourceDelay.end(), 0.0f);
    sourceDelayPos = 0;
    delayedPos = 0;
    inPower = inPowerNow = outPower = 0;
    guardGain = 1;
    analysisSet = {};
    historyPos = sinceHop = 0;
    now = hopCount = 0;
}

int LpcVoice::latencySamples (const ParamValues& params) const
{
    // 音源を作り直す方式は、係数の窓（直近 25ms）の中心の時刻の音を作るので、窓の半分（と、係数を求め直す
    // 間隔の半分）より短くはできない
    const int inner = shifter->latencySamples (params);
    return shifter->makesOwnSource() ? std::max (inner, window / 2 + hop / 2 + 1) : inner;
}

void LpcVoice::analyze (const ParamValues& params)
{
    if (envelope == Envelope::melCepstrum)
    {
        analyzeMel (params);
        return;
    }
    const bool cep = envelope == Envelope::cepstrum;
    const int order = cep ? cepstrumOrder : std::clamp (static_cast<int> (params.get (P::lpcOrder)), 2, kMaxOrder);

    // 窓をかけて、自己相関を求める
    for (int i = 0; i < window; ++i)
        frame[static_cast<size_t> (i)] = history[static_cast<size_t> ((historyPos + i) % window)] * hann[static_cast<size_t> (i)];
    if (cep)
    {
        // ケプストラムでなめらかにした包絡（パワー）を逆 FFT して、自己相関にする（ウィーナー・ヒンチンの定理）
        const int n = cepstrumFft.getSize(), half = n / 2;
        double energy = 0;
        for (int i = 0; i < n; ++i)
        {
            const double x = i < window ? frame[static_cast<size_t> (i)] : 0.0;
            energy += x * x;
            cepstrumSpec[static_cast<size_t> (i)] = { x, 0.0 };
        }
        cepstrumFft.forward (cepstrumSpec.data());
        for (int k = 0; k <= half; ++k)
            cepstrumLog[static_cast<size_t> (k)] = std::log (std::abs (cepstrumSpec[static_cast<size_t> (k)]) + 1.0e-9);
        for (int k = 0; k < n; ++k)
            cepstrumSpec[static_cast<size_t> (k)] = { cepstrumLog[static_cast<size_t> (k <= half ? k : n - k)], 0.0 };
        cepstrumFft.inverse (cepstrumSpec.data());
        const int lifter = std::min (half - 1, static_cast<int> (sr * lifterSeconds));
        for (int q = lifter + 1; q < n - lifter; ++q)
            cepstrumSpec[static_cast<size_t> (q)] = {};
        cepstrumFft.forward (cepstrumSpec.data());
        for (int k = 0; k < n; ++k)
            cepstrumSpec[static_cast<size_t> (k)] = { std::exp (2.0 * cepstrumSpec[static_cast<size_t> (k)].real()), 0.0 };
        cepstrumFft.inverse (cepstrumSpec.data());
        // 大きさは、窓の中の音のエネルギーに合わせる（ケプストラムの包絡は、対数で平均するので低めに出る）
        const double scale = energy / std::max (cepstrumSpec[0].real(), 1.0e-30);
        for (int lag = 0; lag <= order; ++lag)
            autocorr[static_cast<size_t> (lag)] = cepstrumSpec[static_cast<size_t> (lag)].real() * scale;
    }
    else
    {
        for (int lag = 0; lag <= order; ++lag)
        {
            double sum = 0;
            for (int i = lag; i < window; ++i)
                sum += frame[static_cast<size_t> (i)] * frame[static_cast<size_t> (i - lag)];
            autocorr[static_cast<size_t> (lag)] = sum;
        }
    }
    // 帯域拡張: 自己相関にガウス形のラグ窓をかける（スペクトルでは、包絡をその幅でぼかすことになる）
    if (! cep && params.on (P::lpcBandwidth))
        for (int lag = 1; lag <= order; ++lag)
        {
            const double w = 2 * kPi * kLagWindowHz * lag / sr;
            autocorr[static_cast<size_t> (lag)] *= std::exp (-0.5 * w * w);
        }
    // わずかに持ち上げて計算を安定させる（無音でも割り算が壊れないように、小さな値も足す）
    autocorr[0] = autocorr[0] * (1.0 + 1.0e-6) + 1.0e-12;

    analysisSet = {};
    analysisSet.order = order;
    analysisSet.gain = 1;
    const double err = lpc::levinson (autocorr.data(), order, a.data(), analysisSet.k, tmp.data());
    analysisSet.level = std::sqrt (std::max (err, 0.0) / windowPower);
    analysisSets[static_cast<size_t> (hopCount % kHistorySets)] = analysisSet;

    // 合成のフィルタ。フォルマントを動かさないときは、分析と同じ係数（元の声に戻る）
    auto& out = synthesisSets[static_cast<size_t> (hopCount % kHistorySets)];
    const double formantRatio = semitonesToRatio (params.get (P::formant));
    warp.set (params, formantTrack);
    if (warp.isIdentity (1.0e-4) || (correction && warp.isUniform()))
        out = analysisSet;
    else
        warpFormant (analysisSet, formantRatio, out);
    ++hopCount;
}

void LpcVoice::analyzeMel (const ParamValues& params)
{
    // 窓をかけて FFT し、対数振幅スペクトルにする
    const int n = cepstrumFft.getSize(), half = n / 2;
    double energy = 0;
    for (int i = 0; i < n; ++i)
    {
        const double x = i < window ? history[static_cast<size_t> ((historyPos + i) % window)] * hann[static_cast<size_t> (i)] : 0.0;
        energy += x * x;
        cepstrumSpec[static_cast<size_t> (i)] = { x, 0.0 };
    }
    cepstrumFft.forward (cepstrumSpec.data());
    double maxPower = 0;
    for (int k = 0; k <= half; ++k)
        maxPower = std::max (maxPower, std::norm (cepstrumSpec[static_cast<size_t> (k)]));
    // 谷の深さは、一番大きいところから 60dB までにする（MLSA フィルタのパデ近似は、包絡の山と谷の差が大きすぎると
    // 外れるため。倍音の間の深い谷が、平均を引き下げすぎないようにもなる）
    const double floor = std::max (maxPower * 1.0e-6, 1.0e-12);
    for (int k = 0; k <= half; ++k)
        melLogAmp[static_cast<size_t> (k)] = 0.5 * std::log (std::max (std::norm (cepstrumSpec[static_cast<size_t> (k)]), floor));

    melCoeffs (false, 1.0, melB.data());
    analysisSet = {};
    analysisSet.order = melOrder;
    analysisSet.gain = 1; // b(0) は使わない（インパルス応答の最初が 1 のフィルタ）
    for (int m = 1; m <= melOrder; ++m)
        analysisSet.k[m] = melB[static_cast<size_t> (m)];
    // 音源を作り直す方式の大きさ: パワー 1 の音源を exp(F) に通した音のパワーが、窓の中の音のパワーと同じになるように
    const double meanPower = melFilterPower (melB[0]);
    analysisSet.level = std::sqrt (energy / windowPower / std::max (meanPower, 1.0e-30));
    analysisSets[static_cast<size_t> (hopCount % kHistorySets)] = analysisSet;

    // 合成のフィルタ。フォルマントを動かすときは、包絡を動かしてから読み直す
    auto& out = synthesisSets[static_cast<size_t> (hopCount % kHistorySets)];
    const double formantRatio = semitonesToRatio (params.get (P::formant));
    warp.set (params, formantTrack);
    if (warp.isIdentity (1.0e-4))
        out = analysisSet;
    else
    {
        melCoeffs (true, formantRatio, melB2.data());
        out = analysisSet;
        for (int m = 1; m <= melOrder; ++m)
            out.k[m] = melB2[static_cast<size_t> (m)];
        // 大きさ: 動かした後のパワーが、元のスペクトル（なめらかにする前）を動かしたときのパワーの変わり方と同じになるように。
        // メルケプストラムの包絡の b(0) の差をそのまま使うと、動かした後のほうが倍音の山を細かくなぞってしまい
        // （低い周波数ほど細かく表せるため）、音声合成の声の初期値（+6）で平均 4dB、最大 16dB 大きくなった
        const double top = cepstrumFft.getSize() / 2;
        const double binHz = sr / cepstrumFft.getSize();
        double raw = 0, rawShifted = 0;
        for (int k = 0; k <= static_cast<int> (top); ++k)
        {
            const double w = k == 0 || k == static_cast<int> (top) ? 0.5 : 1.0;
            raw += w * std::exp (2.0 * melLogAmp[static_cast<size_t> (k)]);
            const double from = std::clamp (warp.isUniform() ? k / formantRatio : warp.inverse (k * binHz) / binHz, 0.0, top);
            const int i0 = std::min (static_cast<int> (top) - 1, static_cast<int> (from));
            const double f = from - i0;
            rawShifted += w * std::exp (2.0 * (melLogAmp[static_cast<size_t> (i0)] * (1 - f) + melLogAmp[static_cast<size_t> (i0 + 1)] * f));
        }
        out.gain = std::sqrt (rawShifted / std::max (raw, 1.0e-30) * meanPower / std::max (melFilterPower (melB2[0]), 1.0e-30));
    }
    ++hopCount;
}

double LpcVoice::melFilterPower (double b0) const
{
    // exp(F) の、元の軸の上での平均のパワー。log|exp(F)| = log|H| - b(0) = Σ c~(m) cos(m ω~) - b(0)（melMc を使う）
    const int half = cepstrumFft.getSize() / 2;
    double sum = 0;
    for (int k = 0; k <= half; ++k)
    {
        // cos(m ω~) は、cos(m ω~) = 2 cos ω~ cos((m-1) ω~) - cos((m-2) ω~) で順に求める（cos を毎回呼ぶと重い）
        const double c1 = std::cos (melCos[static_cast<size_t> (k)]);
        double logAmp = melMc[0] - b0, prev = 1.0, cur = c1;
        for (int m = 1; m <= melOrder; ++m)
        {
            logAmp += melMc[static_cast<size_t> (m)] * cur;
            const double next = 2.0 * c1 * cur - prev;
            prev = cur;
            cur = next;
        }
        sum += std::exp (2.0 * logAmp) * (k == 0 || k == half ? 0.5 : 1.0);
    }
    return sum / half;
}

void LpcVoice::melCoeffs (bool shift, double formantRatio, double* b)
{
    const int n = cepstrumFft.getSize(), half = n / 2;
    const double binHz = sr / n;
    // 曲げた軸の上の等間隔の点で、対数振幅スペクトルを読む（フォルマントを動かすときは、動かす前の位置を読む）
    for (int j = 0; j <= half; ++j)
    {
        double from = melPos[static_cast<size_t> (j)];
        if (shift)
            from = warp.isUniform() ? from / formantRatio : warp.inverse (from * binHz) / binHz;
        const double src = std::clamp (from, 0.0, static_cast<double> (half));
        const int i0 = std::min (half - 1, static_cast<int> (src));
        const double f = src - i0;
        cepstrumSpec[static_cast<size_t> (j)] = { melLogAmp[static_cast<size_t> (i0)] * (1 - f) + melLogAmp[static_cast<size_t> (i0 + 1)] * f, 0.0 };
    }
    for (int j = half + 1; j < n; ++j)
        cepstrumSpec[static_cast<size_t> (j)] = cepstrumSpec[static_cast<size_t> (n - j)];
    cepstrumFft.inverse (cepstrumSpec.data());
    // 実数のケプストラム（両側）を、最小位相の片側に（1 次から上は 2 倍）。M 次までを残す
    melMc[0] = cepstrumSpec[0].real();
    for (int m = 1; m <= melOrder; ++m)
        melMc[static_cast<size_t> (m)] = 2.0 * cepstrumSpec[static_cast<size_t> (m)].real();
    mlsa::toFilterCoeffs (melMc.data(), melOrder, melAlpha, b);
}

void LpcVoice::warpFormant (const CoeffSet& in, double formantRatio, CoeffSet& out)
{
    const int order = in.order;
    const int half = kWarpFftSize / 2;
    out = {};

    // 反射係数から予測係数に戻して、A の周波数特性 → 包絡 1/|A|^2
    std::fill (a.begin(), a.end(), 0.0);
    a[0] = 1;
    for (int i = 1; i <= order; ++i)
    {
        std::copy (a.begin(), a.begin() + i, tmp.begin());
        for (int j = 1; j < i; ++j)
            a[static_cast<size_t> (j)] = tmp[static_cast<size_t> (j)] + in.k[i] * tmp[static_cast<size_t> (i - j)];
        a[static_cast<size_t> (i)] = in.k[i];
    }
    for (int i = 0; i < kWarpFftSize; ++i)
        spec[static_cast<size_t> (i)] = { i <= order ? a[static_cast<size_t> (i)] : 0.0, 0.0 };
    fft.forward (spec.data());
    for (int j = 0; j <= half; ++j)
        power[static_cast<size_t> (j)] = 1.0 / std::max (std::norm (spec[static_cast<size_t> (j)]), 1.0e-12);

    // 包絡を周波数方向に伸び縮みさせる（倍率 2 なら、500Hz にあった山が 1000Hz に来る）。
    // 動かし方が「低い側を残す」のときは、j に来る元の位置を FormantWarp で求める。
    // 補正の段では、入ってくる声がもう一律に倍率だけ動いているので、元の位置をさらに倍率倍した位置を読む
    // （1kHz より上は一律なので、読む位置は j のまま = 補正しない）
    const double binHz = sr / kWarpFftSize;
    for (int j = 0; j <= half; ++j)
    {
        double from = j / formantRatio;
        if (correction)
            from = formantRatio * warp.inverse (j * binHz) / binHz;
        else if (! warp.isUniform())
            from = warp.inverse (j * binHz) / binHz;
        const double src = std::min (static_cast<double> (half), from);
        const int i0 = std::min (half - 1, static_cast<int> (src));
        const double f = src - i0;
        const double p = std::exp (std::log (power[static_cast<size_t> (i0)]) * (1 - f) + std::log (power[static_cast<size_t> (i0 + 1)]) * f);
        spec[static_cast<size_t> (j)] = { p, 0.0 };
    }
    for (int j = half + 1; j < kWarpFftSize; ++j)
        spec[static_cast<size_t> (j)] = spec[static_cast<size_t> (kWarpFftSize - j)];

    // パワースペクトルを逆 FFT すると自己相関。そこから LPC を求め直す
    fft.inverse (spec.data());
    for (int i = 0; i <= order; ++i)
        autocorr[static_cast<size_t> (i)] = spec[static_cast<size_t> (i)].real();
    autocorr[0] *= 1.0 + 1.0e-6;
    out.order = order;
    out.level = in.level;
    const double err = lpc::levinson (autocorr.data(), order, a.data(), out.k, tmp.data());
    // 伸び縮みさせた包絡は 1/|A(ω/倍率)|^2 ≒ err / |A'|^2 なので、合成の出力に √err をかけると大きさが合う
    out.gain = std::sqrt (std::max (err, 0.0));
}

void LpcVoice::coeffsAt (std::int64_t t, const std::vector<CoeffSet>& sets, bool interpolate, CoeffSet& out) const
{
    // 係数は (t + 1) が hop の倍数になったサンプルの直前に求め直すので、時刻 t のサンプルには
    // (t + 1) / hop - 1 番目の係数を使う。係数補間のときは、その1つ前の係数から、この hop の間に少しずつ近づける
    const std::int64_t idx = t >= 0 ? (t + 1) / hop - 1 : -1;
    auto setAt = [&] (std::int64_t i) -> const CoeffSet&
    {
        return i < 0 || i < hopCount - kHistorySets || i >= hopCount ? identity : sets[static_cast<size_t> (i % kHistorySets)];
    };
    const auto& to = setAt (idx);
    if (! interpolate)
    {
        out = to;
        return;
    }
    const auto& from = setAt (idx - 1);
    const double x = static_cast<double> ((t + 1) % hop) / hop;
    out.order = std::max (from.order, to.order);
    for (int i = 1; i <= out.order; ++i)
        out.k[i] = from.k[i] + (to.k[i] - from.k[i]) * x; // |k| < 1 どうしの間なので、つないでも安定
    out.gain = from.gain + (to.gain - from.gain) * x;
    out.level = from.level + (to.level - from.level) * x;
}

void LpcVoice::process (float* data, int n, const ParamValues& params)
{
    if (static_cast<int> (residual.size()) < n)
    {
        // ホストが約束より大きな塊を渡したとき
        residual.resize (static_cast<size_t> (n));
        original.resize (static_cast<size_t> (n));
    }
    std::copy (data, data + n, original.begin());

    // LPC の改善のスイッチは LPC だけのもの。ケプストラムは、係数補間だけをいつも使う
    const bool mel = envelope == Envelope::melCepstrum;
    const bool cep = envelope == Envelope::cepstrum || mel;
    const bool interpolate = cep || params.on (P::lpcInterpolation);
    const double guardCoeff = 1.0 - std::exp (-1.0 / (0.005 * sr));
    const double guardAttack = 1.0 - std::exp (-1.0 / (0.0005 * sr));
    const double guardRelease = 1.0 - std::exp (-1.0 / (0.05 * sr));
    const double emphasis = ! cep && params.on (P::lpcEmphasis) ? kEmphasis : 0.0;

    // ---- 分析: 係数を 5ms ごとに求め直しながら、残差を取り出す ----
    for (int i = 0; i < n; ++i)
    {
        // 高域強調: x - 0.9 × 1つ前の x
        const double x = data[i] - emphasis * emphasisState;
        emphasisState = data[i];

        history[static_cast<size_t> (historyPos)] = static_cast<float> (x);
        historyPos = (historyPos + 1) % window;
        if (++sinceHop >= hop)
        {
            sinceHop = 0;
            analyze (params);
        }
        coeffsAt (now + i, analysisSets, interpolate, current);
        residual[static_cast<size_t> (i)] = static_cast<float> (mel ? analysisMlsa.process (x, current.k, current.order, melAlpha, true)
                                                                    : lpc::analysisLattice (x, current.k, current.order, analysisState.data()));
    }

    // ---- 残差のピッチを変える（フォルマントは LPC で扱うので、部品にはフォルマント 0 を渡す） ----
    auto inner = params;
    inner.set (P::formant, 0.0f);
    inner.set (P::formantWarp, 0.0f); // 部品の側では動かさない（TD-PSOLA の声色の補正の段も使わない）
    // ピッチ検出は元の声で行う（残差は予測でほとんど消えて小さく、無音と判定されることがあるため）
    shifter->setDetectionSource (original.data());
    shifter->process (residual.data(), n, inner);

    // ---- 合成: 残差が出てきた時刻の係数で、声に戻す ----
    // 音源を作り直す方式（LPC ボコーダ）は、パワー 1 の音源に、その時刻の予測の誤差の大きさをかける
    // 係数は、直近 25ms の窓から求めたもの。残差を使う方式は、残差を作ったのと同じ係数で戻す必要があるが、
    // 音源を作り直す方式は、窓の中心がその時刻に来る係数を使う（でないと、大きさの変化が窓の半分だけ遅れる）
    const bool ownSource = shifter->makesOwnSource();
    const int innerDelay = shifter->latencySamples (params);
    const int delay = latencySamples (params);
    // 係数は hop ごとに求め直すので、使う係数の窓の終わりは、平均して hop の半分だけ前になる。その分も先の係数を使う
    const std::int64_t coeffOffset = ownSource ? window / 2 + hop / 2 : 0;
    const int sourceSize = static_cast<int> (sourceDelay.size());
    const int extra = std::min (delay - innerDelay, sourceSize - 1); // 音源をさらに遅らせる分
    for (int i = 0; i < n; ++i)
    {
        const std::int64_t t = now + i - delay; // この残差のサンプルが、元の音で何サンプル目か
        if (ownSource)
        {
            sourceDelay[static_cast<size_t> (sourceDelayPos)] = residual[static_cast<size_t> (i)];
            residual[static_cast<size_t> (i)] = sourceDelay[static_cast<size_t> ((sourceDelayPos - extra + sourceSize) % sourceSize)];
            sourceDelayPos = (sourceDelayPos + 1) % sourceSize;
        }
        // 始めたばかりの間は、平らにしていない音（係数がまだないときの残差）が、ピッチを変える部品から
        // ばらばらの遅れで出てくる。それを鋭い山の合成フィルタに通すと大きく持ち上がってしまうので、
        // 部品の中の音が入れ替わるまで（遅延の2倍 + 余裕）は、合成のフィルタを素通しにする
        // （音源を作り直す方式は、残差を使わないので要らない。係数がまだないときは、大きさが 0 なので無音になる）
        const bool warmingUp = ! ownSource && now + i < 2 * static_cast<std::int64_t> (delay) + 2 * hop + window;
        if (warmingUp)
            current = identity;
        else
            coeffsAt (t + coeffOffset, synthesisSets, interpolate, current);
        const double source = residual[static_cast<size_t> (i)] * (ownSource ? current.level : 1.0);
        const double y = (mel ? synthesisMlsa.process (source, current.k, current.order, melAlpha, false)
                              : lpc::synthesisLattice (source, current.k, current.order, synthesisState.data())) * current.gain;
        // 高域強調を戻す: y + 0.9 × 1つ前の出力
        deemphasisState = y + emphasis * deemphasisState;

        // 音量の暴れ止め: 同じ時刻の入力（遅延の分だけ遅らせたもの）と、出力の大きさを比べる（約 5ms でならす）。
        // ディレイライン式は、実際の遅れが 0〜窓の長さの間を動くので、声の出だしが遅延の平均より早く出てくることが
        // ある。そのとき、遅らせた入力だけと比べると「入力に対して大きすぎる」と誤って下げ、戻るのが遅いので、
        // 出だしが弱く、遅れて聞こえる。そこで、遅らせない今の入力とも比べ、大きいほうを基準にする
        // （本当の暴れは、どちらよりも大きくなる）
        const auto size = static_cast<int> (delayedInput.size());
        delayedInput[static_cast<size_t> (delayedPos)] = original[static_cast<size_t> (i)];
        const float inThen = delayedInput[static_cast<size_t> ((delayedPos - std::min (delay, size - 1) + size) % size)];
        delayedPos = (delayedPos + 1) % size;
        inPower += guardCoeff * (inThen * inThen - inPower);
        inPowerNow += guardCoeff * (static_cast<double> (original[static_cast<size_t> (i)]) * original[static_cast<size_t> (i)] - inPowerNow);
        // 出力の大きさは、上がるときは速く（約 0.5ms）、下がるときはゆっくり（約 5ms）追う。声の出だしや母音から
        // 子音への変わり目の、数 ms だけの突き出た音も、間に合って止められるように
        const double outSq = deemphasisState * deemphasisState;
        outPower += (outSq > outPower ? guardAttack : guardCoeff) * (outSq - outPower);
        const double limit = 4.0 * std::max (inPower, inPowerNow) + 1.0e-10; // パワーで 4 倍 = +6dB まで
        // 音源を作り直す方式は、大きさをその時刻の予測の誤差から付けるので暴れない。止めると、声の出だしで
        // 出力が少し早く大きくなっただけで下げてしまい、戻るのが遅いので、大きさの変化が遅れて聞こえる
        const double target = (outPower > limit && ! ownSource) ? std::sqrt (limit / outPower) : 1.0;
        // 下げるときはすぐ、戻すときはゆっくり
        guardGain = target < guardGain ? target : guardGain + guardRelease * (target - guardGain);
        data[i] = static_cast<float> (deemphasisState * guardGain);
    }
    now += n;
}

} // namespace vcab
