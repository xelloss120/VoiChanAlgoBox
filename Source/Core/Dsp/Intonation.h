// 抑揚の強調（声の高さの、平均からの上下の幅を広げる）
//
// ピッチ検出で求めた声の高さ f0 の、最近の平均からのずれ（オクターブの単位）を d とすると、出力の声の高さに、
// さらに 2^(量 × d) をかける。量 100% なら、平均からのずれが 2 倍になる（平均より 2 半音高いところは 4 半音高く）。
// 平均は、有声音の声の高さ（対数）を約 2 秒でならして、自動で追う。
// 安全策: ピッチ検出の間違い（特に 1 オクターブの間違い）まで強調すると声が跳ねるので、
//   - ずれは ±1 オクターブまでにする
//   - 隣の検出から 5 半音以上跳んだときは、間違いとみなして、前のずれのままにする
//   - かける倍率の変化は、約 20ms でなめらかにする
// ピッチ検出を使う方式（TD-PSOLA、WORLD、チャンネルボコーダ、LPC とケプストラムの PSOLA 版とボコーダ）で使う。
#pragma once

#include <algorithm>
#include <cmath>

namespace vcab
{

class IntonationEmphasis
{
public:
    void reset()
    {
        hasMean = false;
        meanLog = lastLog = deviation = target = 0;
    }

    // 声の高さを渡して、基本の倍率にかける追加の倍率を返す。amount: 0〜1（100% = 1）。dt: 前に呼んでからの秒数
    double factor (double hz, bool voiced, double amount, double dt)
    {
        if (voiced && hz > 0)
        {
            const double l = std::log2 (hz);
            if (! hasMean)
            {
                meanLog = lastLog = l;
                hasMean = true;
            }
            meanLog += (1.0 - std::exp (-dt / kMeanSeconds)) * (l - meanLog);
            // 隣の検出から大きく跳んだら、間違いとみなして前のずれのまま
            if (std::abs (l - lastLog) < kJumpOctaves)
                target = std::clamp (l - meanLog, -1.0, 1.0);
            lastLog = l;
        }
        // 無声音の間は、前のずれのまま（声の高さがないので、かけても音は変わらない）
        deviation += (1.0 - std::exp (-dt / kSmoothSeconds)) * (target - deviation);
        return amount > 0 ? std::exp2 (amount * deviation) : 1.0;
    }

private:
    static constexpr double kMeanSeconds = 2.0;
    static constexpr double kSmoothSeconds = 0.020;
    static constexpr double kJumpOctaves = 5.0 / 12.0;

    bool hasMean = false;
    double meanLog = 0, lastLog = 0, deviation = 0, target = 0;
};

} // namespace vcab
