#include "WsolaShifter.h"

#include "../Dsp/Biquad.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

void WsolaShifter::prepare (double sampleRate, int)
{
    sr = sampleRate;
    // 窓の長さの上限（100ms）、探す範囲（窓の 4 割まで）、比べる長さ（10ms）に、余裕を足した大きさ
    buffer.assign (static_cast<size_t> (sr * 0.16) + 8, 0.0f);
    reset();
}

void WsolaShifter::reset()
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    writePos = 0;
    lastWindow = 0; // 次の process で、2つの読み出し位置を窓の 0 と半分に置き直す
}

float WsolaShifter::readAt (double delaySamples) const noexcept
{
    // 直線補間で、書き込み位置から delaySamples 前の音を読む
    const auto size = static_cast<int> (buffer.size());
    const double readPos = writePos - delaySamples;
    const int i0 = static_cast<int> (std::floor (readPos));
    const double frac = readPos - i0;
    const int a = ((i0 % size) + size) % size;
    const int b = (a + 1) % size;
    return static_cast<float> (buffer[static_cast<size_t> (a)] * (1 - frac) + buffer[static_cast<size_t> (b)] * frac);
}

double WsolaShifter::findBestDelay (double from, double to, double otherDelay) const noexcept
{
    // 相手の読み出し位置の直前（過去側）の波形と、候補の位置の直前の波形を比べる。
    // 過去側なら、0 に近い遅延でも書き込み済みの音だけで比べられる
    const int compareLength = static_cast<int> (0.010 * sr);
    constexpr int step = 2; // 計算を軽くするため、2サンプルおきに比べる

    double best = from, bestScore = -2;
    for (double d = from; d <= to; d += step)
    {
        double cross = 0, e1 = 0, e2 = 0;
        for (int k = 0; k < compareLength; k += step)
        {
            const float a = readAt (otherDelay + k);
            const float b = readAt (d + k);
            cross += a * b;
            e1 += a * a;
            e2 += b * b;
        }
        const double score = cross / std::sqrt (e1 * e2 + 1.0e-12);
        if (score > bestScore)
        {
            bestScore = score;
            best = d;
        }
    }
    return best;
}

void WsolaShifter::process (float* data, int n, const ParamValues& params)
{
    const double ratio = semitonesToRatio (params.get (P::pitch));
    const double window = std::max (16.0, params.get (P::wsolaWindowMs) * 0.001 * sr);
    const double search = std::min (window * 0.4, params.get (P::wsolaSearchMs) * 0.001 * sr);
    const auto size = static_cast<int> (buffer.size());

    if (window != lastWindow)
    {
        // 窓の長さが変わったら（または最初）、2つの読み出し位置を半周期ずらして置き直す
        delay = { window * 0.5, window };
        lastWindow = window;
    }

    // 1サンプルごとに遅延が (1 - ratio) だけ変わる。ratio > 1 なら遅延が縮み、読み出しが速くなる
    const double delayStep = 1.0 - ratio;

    for (int i = 0; i < n; ++i)
    {
        buffer[static_cast<size_t> (writePos)] = data[i];

        // 重み: 遅延が 0 と窓の端で 0 になる（戻る瞬間の音を消す）。2つの重みで割って、音量を一定に保つ
        double out = 0, gainSum = 0;
        for (size_t t = 0; t < 2; ++t)
        {
            const double x = std::clamp (delay[t] / window, 0.0, 1.0);
            const double g = std::sin (kPi * x) * std::sin (kPi * x);
            out += g * readAt (delay[t]);
            gainSum += g;
        }
        data[i] = static_cast<float> (gainSum > 1.0e-6 ? out / gainSum : 0.0);

        writePos = (writePos + 1) % size;

        for (size_t t = 0; t < 2; ++t)
        {
            delay[t] += delayStep;
            const auto other = delay[1 - t];

            if (delay[t] <= 0.0)
            {
                // ピッチを上げるとき: 窓の端へ戻る。窓の端から探す範囲の中で、相手と一番よく重なる位置にする
                delay[t] = findBestDelay (window - search, window, other);
            }
            else if (delay[t] >= window)
            {
                // ピッチを下げるとき: 0 の側へ戻る。同じく、相手と一番よく重なる位置にする
                delay[t] = findBestDelay (1.0, 1.0 + search, other);
            }
        }
    }
}

int WsolaShifter::latencySamples (const ParamValues& params) const
{
    // 遅延は 0〜窓の長さを行き来するので、平均は窓の半分
    return static_cast<int> (params.get (P::wsolaWindowMs) * 0.001 * sr * 0.5);
}

} // namespace vcab
