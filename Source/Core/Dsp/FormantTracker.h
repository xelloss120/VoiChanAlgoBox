// フォルマントの追いかけ。動かし方が「フォルマントごと」のときに、入力の声から F1・F2・F3 を求める。
//
// 1. 5kHz の低域通過のあと、12kHz 前後に落とす（男声の F1〜F3 は 3.5kHz より下。F4・F5 も入るが、選ぶときに除く）。
// 2. 5ms ごとに、直近 25ms（ハミング窓、高域強調 50Hz〜）から 14 次の LPC を求め（12 次では「お」の F2 が 20〜27%、16 次では余計な山を F2 に選んで 68% 高く出た。14 次で 12〜14%）、A(z) の根を求める（デュラン・ケルナー法。
//    前の回の根から始めるので、数回で収まる）。根の角度が周波数、半径が幅（帯域幅）になる。
// 3. 候補（幅 700Hz 未満）の中から、低い順の 3 つの組を全部試し、次の和が一番小さい組を F1・F2・F3 にする:
//      目安の位置（500 / 1500 / 2500Hz）からの離れ方（対数）＋ 前の回の値からの離れ方（対数、2 倍の重み）＋ 幅
//    それぞれの範囲（F1 180〜1100Hz、F2 550〜3000Hz、F3 1500〜4000Hz）に入らない組は使わない。
// 4. 声の区間（倍音らしさ 0.5 以上、最近の一番大きい音から 40dB 以内）だけで追いかける。それ以外は、150ms は最後の値を保ち、
//    その後 100ms かけて帯域の目印（FormantWarp::kBandHz）に戻る。値は周波数の対数の上で約 10ms でならす。
// 遅れ: 窓の中心は 12.5ms 前。LPC の方式の包絡（直近 25ms）と、ほぼ同じ時刻になる。
#pragma once

#include "Biquad.h"
#include "FormantWarp.h"

#include <complex>
#include <vector>

namespace vcab
{

class FormantTracker
{
public:
    static constexpr int kOrder = 14;

    void prepare (double sampleRate);
    void reset();
    void process (const float* data, int n);

    const FormantTrack& track() const noexcept { return current; }
    // 今の回の、選ぶ前の候補（画面や調べもの用）
    double voicing() const noexcept { return lastVoicing; }

private:
    void analyze();
    bool findRoots();

    double sr = 48000, fsLow = 12000;
    int decim = 4, decimPhase = 0, hop = 60, window = 300, sinceHop = 0, minLag = 30, maxLag = 150, voicingWindow = 360;
    Biquad antiAlias[2];
    std::vector<float> ring;        // 落とした音（2 のべき乗の長さ）
    long long count = 0;
    std::vector<double> frame, hamming, autocorr, a, k, tmp;
    std::complex<double> roots[kOrder];
    bool rootsWarm = false;
    double peakDb = -200;           // 最近の一番大きい音（毎秒 3dB で下がる）
    double prevHz[3] { 500, 1400, 2700 };
    bool prevValid = false;
    int sinceValid = 1000000;       // 最後に追いかけられてからの回数
    double smoothCoeff = 0.4, returnCoeff = 0.05;
    double lastVoicing = 0;
    FormantTrack current;
};

} // namespace vcab
