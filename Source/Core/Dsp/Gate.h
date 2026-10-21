// ゲート。声がぶつ切りにならないように、ホールド、ヒステリシス、レンジを持つ。
// 下方エキスパンダーにもなる（閉じている間、しきい値より下の分を比率に応じて下げる。下げる量はレンジまで）。
// 開くかどうかの判断にだけ 200Hz〜4kHz のフィルタをかける（音そのものにはかけない）。
// しきい値は手動だけ（自動で決める機能は、動きが読みにくく複雑になるので持たない）。
// 先読みはしない（遅延 0）。
#pragma once

#include "Biquad.h"

#include <cmath>

namespace vcab
{

class Gate
{
public:
    struct Settings
    {
        float thresholdDb = -50;
        float attackMs = 1;
        float holdMs = 50;
        float releaseMs = 150;
        float hysteresisDb = 6;
        float rangeDb = -40;
        float voiceProbability = -1; // 0〜1 ならデノイズの「声のありそうさ」で開閉する（負なら、いつもどおり音量で判断）
        bool expander = false;       // 下方エキスパンダー
        float ratio = 2;             // エキスパンダーの比率（しきい値より 1dB 下がるごとに ratio dB 下がる）
        float kneeDb = 6;            // エキスパンダーのニー（しきい値の前後この幅で、下がり方を 2 次曲線でつなぐ）
    };

    void prepare (double sampleRate);
    void reset();
    void setSettings (const Settings& s) noexcept { settings = s; }

    void process (float* data, int n) noexcept;

    // エキスパンダーで下げる量（dB、0 以下。レンジの下限はかけない）。表示のグラフでも使う
    static float expanderGainDb (float levelDb, const Settings& s) noexcept
    {
        const float d = levelDb - s.thresholdDb, half = 0.5f * s.kneeDb, slope = s.ratio - 1.0f;
        if (d >= half)
            return 0.0f;
        if (d <= -half)
            return d * slope;
        const float e = d - half; // ニーの中: 両端で値と傾きがつながる 2 次曲線
        return -slope * e * e / (2.0f * s.kneeDb);
    }

    // 表示用
    bool isOpen() const noexcept { return open; }
    float currentThresholdDb() const noexcept { return settings.thresholdDb; }
    float levelDb() const noexcept { return envDb; }
    float gainDb() const noexcept { return 20.0f * std::log10 (gain + 1.0e-9f); } // 今下げている量（0 以下）

private:
    double sr = 48000;
    Settings settings;

    Biquad scHighPass, scLowPass;

    // 判断用の音量（dB）。速い追従。
    float envSq = 0;
    float envCoef = 0;
    float envDb = -120;

    bool open = false;
    int holdCounter = 0;
    float gain = 0;          // 今かけている倍率（0〜1）
};

} // namespace vcab
