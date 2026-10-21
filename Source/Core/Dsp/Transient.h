// 突発音の抑制（キーボードやクリックなど、短く鋭い音だけを下げる）
#pragma once

#include "Biquad.h"

#include <vector>

namespace vcab
{

// 突発音の抑制（キーボードやクリックなど、短く鋭い音だけを下げる）
class TransientSuppressor
{
public:
    void prepare (double sampleRate);
    void reset();
    void process (float* data, int n, double thresholdDb, double depthDb);
    int latencySamples() const noexcept { return lookahead; }
    int detections() const noexcept { return count; } // 見つけた回数（表示用）

private:
    double sr = 48000;
    int lookahead = 240, pos = 0, hold = 0, holdExtra = 240, count = 0;
    float fast = 0, slow = 0, gainDb = 0, prevScore = -100;
    float fastAttack = 0, fastRelease = 0, slowAttack = 0, slowRelease = 0, closeCoeff = 0, openCoeff = 0;
    Biquad highPass[2];
    std::vector<float> delay, fastRing;
    std::vector<unsigned char> onsetRing;
};

} // namespace vcab
