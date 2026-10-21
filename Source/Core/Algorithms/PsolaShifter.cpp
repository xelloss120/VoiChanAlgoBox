#include "PsolaShifter.h"

#include "../Dsp/Biquad.h"
#include "../Dsp/FormantWarp.h"
#include "../Dsp/Lpc.h"
#include "LpcVoice.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr double kUnvoicedHopSeconds = 0.005; // 無声音の断片の間隔
constexpr int kMargin = 16;

// ピッチ検出の窓の中心が、ふつうの方式（窓 = 下限の周期の2つ分）より遅れる分
int detectorExtra (double sr, const ParamValues& p)
{
    return std::max (0, PitchDetector::analysisDelayFor (sr, p) - static_cast<int> (std::ceil (sr / p.get (P::pitchMinHz))));
}

// 声門閉鎖点で印を付けるときに、さらに先の音を見る分（平均の信号の窓の半分と、谷を探す幅。最大の周期で約 1.2 周期）
int glottalExtra (double sr, const ParamValues& p)
{
    return p.index (P::psolaMarking) == 1 ? static_cast<int> (std::ceil (1.2 * sr / p.get (P::pitchMinHz))) : 0;
}

int delayFor (double sr, const ParamValues& p)
{
    return 2 * static_cast<int> (std::ceil (sr / p.get (P::pitchMinHz))) + kMargin + detectorExtra (sr, p) + glottalExtra (sr, p);
}
} // namespace

PsolaShifter::PsolaShifter() : corrector (LpcVoice::makeFormantCorrector()) {}
PsolaShifter::~PsolaShifter() = default;

void PsolaShifter::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    corrector->prepare (sr, maxBlockSize);
    detector.prepare (sr);

    // 遅延の最大（下限 40Hz の周期の2倍）と、断片の長さに、余裕を足した 2 のべき乗
    int size = 1;
    while (size < static_cast<int> (sr * 0.25))
        size <<= 1;
    in.assign (static_cast<size_t> (size), 0.0f);
    out.assign (static_cast<size_t> (size), 0.0f);
    res.assign (static_cast<size_t> (size), 0.0f);
    lowIn.assign (static_cast<size_t> (size), 0.0f);
    resLowPass[0].setCoeffs (BiquadCoeffs::lowPass (sr, std::min (4000.0, sr * 0.45), 0.5412));
    resLowPass[1].setCoeffs (BiquadCoeffs::lowPass (sr, std::min (4000.0, sr * 0.45), 1.3066));
    resOrder = std::clamp (static_cast<int> (std::lround (sr / 2000.0)) + 4, 8, kResidualOrder);
    resHop = std::max (1, static_cast<int> (std::lround (0.005 * sr)));
    resWindow = static_cast<int> (std::lround (0.020 * sr));
    lpcWindow.resize (static_cast<size_t> (resWindow));
    for (int i = 0; i < resWindow; ++i)
        lpcWindow[static_cast<size_t> (i)] = 0.5 - 0.5 * std::cos (2 * kPi * (i + 0.5) / resWindow);
    mask = size - 1;
    reset();
}

void PsolaShifter::reset()
{
    detector.reset();
    std::fill (in.begin(), in.end(), 0.0f);
    std::fill (out.begin(), out.end(), 0.0f);
    std::fill (res.begin(), res.end(), 0.0f);
    std::fill (lowIn.begin(), lowIn.end(), 0.0f);
    resLowPass[0].reset();
    resLowPass[1].reset();
    std::fill (std::begin (resK), std::end (resK), 0.0);
    std::fill (std::begin (resState), std::end (resState), 0.0);
    sinceResHop = 0;
    resM2 = resM3 = 0;
    resPolarity = 1.0;
    glottal = false;
    now = 0;
    numMarks = markHead = 0;
    lastMark = -1;
    nextGrain = nextNoise = 0;
    voicedMix = 0;
    lastPeriod = 0;
    corrector->reset();
    correctorActive = false;
    intonation.reset();
}

float PsolaShifter::inputAt (double pos) const noexcept
{
    const auto i0 = static_cast<std::int64_t> (std::floor (pos));
    const double frac = pos - static_cast<double> (i0);
    const float a = in[static_cast<size_t> (i0 & mask)];
    const float b = in[static_cast<size_t> ((i0 + 1) & mask)];
    return static_cast<float> (a + (b - a) * frac);
}

void PsolaShifter::placeMarks (double period)
{
    const auto latest = static_cast<double> (now - markLag); // 声の高さが分かっている時刻まで
    const int half = static_cast<int> (period * 0.5);
    const int search = std::max (2, static_cast<int> (period * 0.15));

    // 印が古すぎる（無声音の後など）ときは、直近の 1 周期の中で一番大きい山から始め直す
    if (lastMark < 0 || lastMark < latest - 4 * period)
    {
        const double from = latest - 2 * period - half;
        double best = from, bestAbs = -1;
        for (int k = 0; k < static_cast<int> (period); ++k)
        {
            const float v = std::abs (inputAt (from + k));
            if (v > bestAbs)
            {
                bestAbs = v;
                best = from + k;
            }
        }
        numMarks = 0;
        addMark (best);
    }

    // 次の印の候補（前の印 + 1 周期）の前後を探し、前の印の周りの波形と一番よく重なる位置にする。
    // 候補の周りを半周期ずつ見るので、候補 + 探す幅 + 半周期 まで書き込まれている必要がある
    while (lastMark + period + search + half < latest)
    {
        const double guess = lastMark + period;
        double best = guess, bestScore = -2;
        for (int s = -search; s <= search; ++s)
        {
            double cross = 0, e1 = 0, e2 = 0;
            for (int k = -half; k < half; k += 2)
            {
                const float a = inputAt (lastMark + k);
                const float b = inputAt (guess + s + k);
                cross += a * b;
                e1 += a * a;
                e2 += b * b;
            }
            const double score = cross / std::sqrt (e1 * e2 + 1.0e-12);
            if (score > bestScore)
            {
                bestScore = score;
                best = guess + s;
            }
        }
        addMark (best);
    }
}

void PsolaShifter::addMark (double pos)
{
    lastMark = pos;
    marks[static_cast<size_t> (markHead)] = pos;
    markHead = (markHead + 1) % static_cast<int> (marks.size());
    numMarks = std::min (numMarks + 1, static_cast<int> (marks.size()));
}

void PsolaShifter::analyzeResidual()
{
    // 直近 20ms の入力から LPC を求める（これから来るサンプルの残差に使う）
    double frame[4096];
    const int len = std::min (resWindow, 4096);
    for (int i = 0; i < len; ++i)
        frame[i] = lowIn[static_cast<size_t> ((now - len + i) & mask)] * lpcWindow[static_cast<size_t> (i)];
    double r[kResidualOrder + 1], a[kResidualOrder + 1], tmp[kResidualOrder + 1];
    for (int lag = 0; lag <= resOrder; ++lag)
    {
        double sum = 0;
        for (int i = lag; i < len; ++i)
            sum += frame[i] * frame[i - lag];
        r[lag] = sum;
    }
    // 4kHz より上がない音なので、少し多めに持ち上げて、計算を安定させる
    r[0] = r[0] * (1.0 + 1.0e-3) + 1.0e-12;
    lpc::levinson (r, resOrder, a, resK, tmp);
}

void PsolaShifter::placeMarksGlottal (double period)
{
    const auto latest = static_cast<double> (now - markLag); // 声の高さが分かっている時刻まで
    const double T = period;
    const int halfWindow = std::max (2, static_cast<int> (std::lround (0.875 * T))); // 平均の信号の窓の半分（1.75 周期の半分）
    // 残差の偏りで決めた向き。ふつうの向きでは、声門閉鎖点で残差が負に突き出る（3 乗の平均が負）
    const double pol = resPolarity;
    auto residualAt = [&] (std::int64_t i) { return -pol * res[static_cast<size_t> (i & mask)]; };

    const int hw = std::min (halfWindow, 2047);
    const double reach = 1.35 * T + hw + 2; // 次の印を決めるのに要る、前の印からの先の音
    const bool restart = lastMark < 0 || lastMark < latest - 4 * T;
    if (! restart && lastMark + reach >= latest)
        return; // まだ次の印を決められない（毎サンプル呼ばれるので、ここで早く戻る）

    // 平均の信号（周期の 1.75 倍のブラックマン窓で平均した波形）。窓は、長さが変わったときだけ作り直す。
    // 平均の信号はなめらかなので、4 サンプルおきに足して計算を減らす
    constexpr int stride = 4;
    if (hw != meanHalf)
    {
        meanHalf = hw;
        meanWeights.resize (static_cast<size_t> (2 * hw + 1));
        meanWeightSum = 0;
        for (int m = -hw; m <= hw; ++m)
        {
            const double w = 0.42 + 0.5 * std::cos (kPi * m / hw) + 0.08 * std::cos (2 * kPi * m / hw);
            meanWeights[static_cast<size_t> (m + hw)] = w;
            if ((m + hw) % stride == 0)
                meanWeightSum += w;
        }
    }
    auto meanAt = [&] (std::int64_t n)
    {
        double s = 0;
        for (int m = -hw; m <= hw; m += stride)
            s += meanWeights[static_cast<size_t> (m + hw)] * in[static_cast<size_t> ((n + m) & mask)];
        return pol * s / meanWeightSum;
    };
    // 残差が一番突き出たところ。雑音の多い声では、残差に雑音の細かい山がたくさん立つので、まず約 1ms でならした
    // 残差の山を探し、その前後 0.5ms の中で一番突き出たサンプルにする
    const int smooth = static_cast<int> (std::lround (0.0005 * sr));
    // expected が 0 以上なら、その位置から離れるほど点数を下げる（雑音の山より、1 周期後らしい位置の山を選ぶ）
    auto argmaxResidual = [&] (double from, double to, double expected = -1.0)
    {
        const auto first = static_cast<std::int64_t> (std::floor (from)), last = static_cast<std::int64_t> (std::ceil (to));
        std::int64_t best = first;
        double bestValue = -1.0e30;
        double sum = 0;
        for (auto i = first - smooth; i <= first + smooth; ++i)
            sum += residualAt (i);
        const double width = 0.05 * T; // 雑音の多い合成音で 0.05〜0.2 周期を試して、一番安定した幅
        for (auto i = first; i <= last; ++i)
        {
            if (i > first)
                sum += residualAt (i + smooth) - residualAt (i - smooth - 1);
            double score = sum;
            if (expected >= 0)
            {
                const double z = (i - expected) / width;
                score = std::max (sum, 0.0) * std::exp (-0.5 * z * z);
            }
            if (score > bestValue)
            {
                bestValue = score;
                best = i;
            }
        }
        if (smooth > 0)
        {
            const auto centre = best;
            bestValue = -1.0e30;
            for (auto i = centre - smooth; i <= centre + smooth; ++i)
                if (residualAt (i) > bestValue)
                {
                    bestValue = residualAt (i);
                    best = i;
                }
        }
        return static_cast<double> (best);
    };

    // 印が古すぎる（無声音の後など）ときは、見られる範囲の一番後ろの 1 周期で、残差が一番突き出たところから始め直す
    if (restart)
    {
        const double end = latest - reach;
        numMarks = 0;
        addMark (argmaxResidual (end - T, end));
    }

    while (lastMark + reach < latest)
    {
        const double guess = lastMark + T;
        // 谷を、次の印の候補の少し前から探す（谷のすぐ後に声門閉鎖点が来る）。粗く 4 サンプルおきに探して、前後を細かく
        const auto from = static_cast<std::int64_t> (guess - 0.6 * T), to = static_cast<std::int64_t> (guess + 0.2 * T);
        std::int64_t valley = from;
        double valleyValue = 1.0e30;
        for (auto n = from; n <= to; n += 4)
        {
            const double v = meanAt (n);
            if (v < valleyValue)
            {
                valleyValue = v;
                valley = n;
            }
        }
        for (auto n = valley - 3; n <= valley + 3; ++n)
        {
            const double v = meanAt (n);
            if (v < valleyValue)
            {
                valleyValue = v;
                valley = n;
            }
        }
        double mark = argmaxResidual (static_cast<double> (valley), valley + 0.35 * T, guess);
        // 前の印から周期の 0.5〜1.5 倍の範囲にないときは、谷の探し方がずれたとみなして、候補の前後で一番突き出たところ
        if (mark - lastMark < 0.5 * T || mark - lastMark > 1.5 * T)
            mark = argmaxResidual (guess - 0.25 * T, guess + 0.25 * T);
        addMark (mark);
    }
}

double PsolaShifter::findMarkNear (double pos, double halfSource) const noexcept
{
    // pos に一番近い印。ただし断片の後ろの端まで書き込まれているものに限る
    double best = -1, bestDist = 1.0e18;
    for (int k = 0; k < numMarks; ++k)
    {
        const double m = marks[static_cast<size_t> ((markHead - 1 - k + static_cast<int> (marks.size())) % static_cast<int> (marks.size()))];
        if (m + halfSource + 1 >= static_cast<double> (now))
            continue;
        const double dist = std::abs (m - pos);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = m;
        }
    }
    return best;
}

void PsolaShifter::markNeighbours (double mark, double& prev, double& next) const noexcept
{
    prev = next = -1;
    for (int k = 0; k < numMarks; ++k)
    {
        const double m = marks[static_cast<size_t> ((markHead - 1 - k + static_cast<int> (marks.size())) % static_cast<int> (marks.size()))];
        if (m < mark && (prev < 0 || m > prev))
            prev = m;
        if (m > mark && (next < 0 || m < next))
            next = m;
    }
}

void PsolaShifter::addGrain (double centreOut, double centreIn, double halfOutLeft, double halfOutRight, double formantRatio, double gain)
{
    // 出力の centreOut を中心に、左に halfOutLeft、右に halfOutRight の長さの断片を足す（左右で長さの違うハン窓）。
    // 中身は入力の centreIn の周りを、formantRatio 倍の速さで読んだもの（速く読むと断片が縮み、響きが高くなる）
    const auto first = static_cast<std::int64_t> (std::ceil (centreOut - halfOutLeft));
    const auto last = static_cast<std::int64_t> (std::floor (centreOut + halfOutRight));
    for (auto p = first; p <= last; ++p)
    {
        const double u = static_cast<double> (p) - centreOut;
        const double w = 0.5 + 0.5 * std::cos (kPi * u / (u < 0 ? halfOutLeft : halfOutRight));
        out[static_cast<size_t> (p & mask)] += static_cast<float> (gain * w * inputAt (centreIn + u * formantRatio));
    }
}

void PsolaShifter::process (float* data, int n, const ParamValues& paramsIn)
{
    // 帯域ごと・フォルマントごと: 断片の伸び縮みで、全体を F2 の差の分まで一律に動かし、残り（F1 と F3 の、F2 との差）
    // だけを声色の補正の段で直す。補正の段の包絡は粗い（声が高くなった後でも倍音を拾わないように）ので、全部を補正の段で
    // 動かすと、合成の母音で包絡の誤差が一律の 2〜4dB から 6〜9dB に増えた。動かし方そのもの（どの周波数がどれだけ
    // 動くか）は変わらない（つまみの値に F2 の差を足し、F1〜F3 の差から引くだけ）
    ParamValues adjusted;
    const ParamValues* use = &paramsIn;
    if (paramsIn.index (P::formantWarp) >= FormantWarp::bands)
    {
        adjusted = paramsIn;
        const float base = paramsIn.get (P::formantF2);
        adjusted.set (P::formant, paramsIn.get (P::formant) + base);
        adjusted.set (P::formantF1, paramsIn.get (P::formantF1) - base);
        adjusted.set (P::formantF2, 0.0f);
        adjusted.set (P::formantF3, paramsIn.get (P::formantF3) - base);
        use = &adjusted;
    }
    const ParamValues& params = *use;
    const double formantRatio = semitonesToRatio (params.get (P::formant));
    const double minHz = params.get (P::pitchMinHz);
    const int delay = delayFor (sr, params);
    markLag = detectorExtra (sr, params);
    const double maxPeriod = sr / minHz;
    const bool wantGlottal = params.index (P::psolaMarking) == 1;
    if (wantGlottal != glottal)
    {
        // 切り替えたら、印を付け直す（残差の履歴は、これから作る）
        glottal = wantGlottal;
        lastMark = -1;
        numMarks = 0;
    }

    detector.push (detectionSource != nullptr ? detectionSource : data, n, params);
    detectionSource = nullptr;
    const auto& est = detector.latest();
    const bool voiced = est.voiced && est.hz > 0;
    if (voiced)
        lastPeriod = std::clamp (sr / est.hz, 8.0, maxPeriod);
    // 有声の流れに使う周期（無声になった直後は、フェードアウトの間だけ最後の周期を使う）
    const double period = lastPeriod;
    const double mixStep = 1.0 / (0.010 * sr);
    // フォルマントを動かすと、無声の流れで重なる2つの断片が別々の音になり、足しても窓の和ほどには大きくならない
    // （ハン窓を半分ずつ重ねると、平均で 0.75 倍の大きさ = -1.25dB）。その分を補う。
    // フォルマントが 0 のときは同じ音どうしなので補わない（半音ほどで切り替わるようにつなぐ）
    const double noiseMix = std::min (1.0, static_cast<double> (std::abs (params.get (P::formant))));
    const double noiseGain = 1.0 + noiseMix * (1.0 / std::sqrt (0.75) - 1.0);
    // ピッチの倍率（抑揚の強調のときは、平均からのずれに応じて、さらに倍率をかける）
    const double ratio = semitonesToRatio (params.get (P::pitch))
                       * intonation.factor (est.hz, voiced, params.get (P::intonation) * 0.01, n / sr);

    for (int i = 0; i < n; ++i)
    {
        in[static_cast<size_t> (now & mask)] = data[i];
        if (glottal)
        {
            // 声門閉鎖点のための LPC の残差と、その偏り（約 1 秒でならす）。向きは、偏りがはっきりしたときだけ変える
            // （雑音の多い声では偏りが小さく、0 の前後で揺れて、印が正の山と負の山の間を行き来してしまうため）
            // 高い帯域の雑音が残差の山を埋もれさせないように、4kHz より上を落とした音で LPC と残差を求める
            const float low = resLowPass[1].process (resLowPass[0].process (data[i]));
            lowIn[static_cast<size_t> (now & mask)] = low;
            const double e = lpc::analysisLattice (low, resK, resOrder, resState);
            res[static_cast<size_t> (now & mask)] = static_cast<float> (e);
            const double a = 1.0 / (1.0 * sr);
            resM2 += a * (e * e - resM2);
            resM3 += a * (e * e * e - resM3);
            const double skew = resM3 / std::pow (std::max (resM2, 1.0e-30), 1.5);
            if (skew < -0.2)
                resPolarity = 1.0;  // ふつうの向き（声門閉鎖点で残差が負に突き出る）
            else if (skew > 0.2)
                resPolarity = -1.0;
        }
        ++now;
        if (glottal && ++sinceResHop >= resHop)
        {
            sinceResHop = 0;
            analyzeResidual();
        }

        if (voiced)
        {
            if (glottal)
                placeMarksGlottal (period);
            else
                placeMarks (period);
        }

        // 有声らしさの重み。有声の流れと無声の流れを、等パワー（重みの平方根）で重ねる
        voicedMix = std::clamp (voicedMix + (voiced ? mixStep : -mixStep), 0.0, 1.0);
        const double wVoiced = std::sqrt (voicedMix), wNoise = std::sqrt (1.0 - voicedMix);

        // 出力は、入力より delay サンプル遅れた時刻を読む。そこに重なる断片を、先に足しておく
        const auto t = static_cast<double> (now - 1 - delay);

        // 無声の断片: その場で切り出して重ねる。ハン窓を半分ずつ重ねると、足した窓がちょうど 1 になる
        auto addNoiseGrain = [&] (double centreOut, double weight)
        {
            const double halfOut = kUnvoicedHopSeconds * sr;
            const double halfSource = halfOut * formantRatio;
            const double centreIn = std::min (centreOut, static_cast<double> (now - 2) - halfSource);
            addGrain (centreOut, centreIn, halfOut, halfOut, formantRatio, noiseGain * weight);
        };

        // 無声の流れ（重みがあるときだけ。重みが 0 の間は、始めの位置を今に合わせておく）
        if (wNoise > 0)
        {
            if (nextNoise < t)
                nextNoise = t;
            while (nextNoise <= t + maxPeriod + 1)
            {
                addNoiseGrain (nextNoise, wNoise);
                nextNoise += kUnvoicedHopSeconds * sr;
            }
        }
        else
            nextNoise = t;

        // 有声の流れ
        if (nextGrain < t)
            nextGrain = t; // 始めたときや、遅れたとき
        while (wVoiced > 0 && period > 0 && nextGrain <= t + maxPeriod + 1)
        {
            double hop = 0;
            bool placed = false;
            {
                // 断片は、前の印から次の印まで（非対称窓。それぞれ今の周期の 0.5〜1.5 倍に収め、前後の印がないときは
                // 今の周期）。フォルマントを下げるときは、切り出しを短くする
                const double shrink = std::min (1.0, formantRatio);
                const double halfSource = period * shrink;
                const double mark = findMarkNear (nextGrain, halfSource);
                if (mark >= 0)
                {
                    double prev, next;
                    markNeighbours (mark, prev, next);
                    const double left = std::clamp (prev >= 0 ? mark - prev : period, 0.5 * period, 1.5 * period) * shrink;
                    // 右の端は、書き込まれているところまで（findMarkNear は今の周期の分までしか確かめていない）
                    const double right = std::min (std::clamp (next >= 0 ? next - mark : period, 0.5 * period, 1.5 * period) * shrink,
                                                    std::max (halfSource, static_cast<double> (now) - 2.0 - mark));
                    hop = period / ratio;
                    // 間隔を詰めるとパルスの数が増えて音が大きくなるので、音量（RMS）がほぼ変わらないように下げる。
                    // フォルマントを上げると断片が縮んでエネルギーが減るので、その分を補う
                    const double gain = std::sqrt (hop / period) * std::sqrt (std::max (1.0, formantRatio));
                    addGrain (nextGrain, mark, left / formantRatio, right / formantRatio, formantRatio, gain * wVoiced);
                    placed = true;
                }
            }
            if (! placed)
            {
                // 印がまだないとき（有声になった直後）は、無声と同じく、その場で切り出して重ねる
                hop = kUnvoicedHopSeconds * sr;
                addNoiseGrain (nextGrain, wVoiced);
            }
            nextGrain += hop;
        }

        const auto r = static_cast<size_t> (static_cast<std::int64_t> (t) & mask);
        data[i] = t >= 0 ? out[r] : 0.0f;
        out[r] = 0;
    }

    // 動かし方が「低い側を残す」のときは、一律に動かした声の包絡を、一律との差の分だけ動かし直す
    const bool curved = FormantWarp::isCurved (params);
    if (curved)
    {
        if (! correctorActive)
            corrector->reset();
        corrector->process (data, n, params);
    }
    correctorActive = curved;
}

void PsolaShifter::setFormantTrack (const FormantTrack* track)
{
    formantTrack = track;
    corrector->setFormantTrack (track);
}

int PsolaShifter::latencySamples (const ParamValues& params) const
{
    return delayFor (sr, params);
}

} // namespace vcab
