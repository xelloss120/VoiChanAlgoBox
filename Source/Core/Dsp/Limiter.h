// リミッター。先読み 1ms（固定）、アタックも先読みと同じ 1ms。
// 先読みで止めきれなかったわずかなはみ出しは、最後に上限で強制的に切る（ハードクリップ）。
#pragma once

#include <vector>

namespace vcab
{

class Limiter
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (float ceilingDb, float releaseMs) noexcept;

    void process (float* data, int n) noexcept;

    int latencySamples() const noexcept { return lookahead; }
    float gainReductionDb() const noexcept { return grDb; } // 表示用（0 以下）

private:
    double sr = 48000;
    int lookahead = 48;

    float ceiling = 0.89f;
    float releaseCoef = 0.999f;

    // 遅らせた音
    std::vector<float> delay;
    // 必要な倍率の履歴（最小値をとるため）
    std::vector<float> required;
    // 最小値をなめらかにする移動平均の履歴
    std::vector<float> held;
    int pos = 0;
    double heldSum = 0;

    float env = 1;
    float grDb = 0;
};

} // namespace vcab
