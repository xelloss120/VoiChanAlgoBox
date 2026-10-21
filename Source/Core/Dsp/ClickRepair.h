// クリックを「補間でつなぎ直す」直し方。リペアの段で使う。
//
// 最小二乗の AR 補間（Janssen・Veldhuis・Vries 1986 / Vaseghi・Rayner 1990）:
//   クリックの区間の値を未知数として、前後の音を LPC（AR モデル）で予測したときの予測の外れ（残差）の 2 乗の和が
//   一番小さくなるように決める。区間の前の音から LPC の係数を求め、区間の後ろの order 個の音も使う。
//   方程式の行列は対称なテプリッツ行列（帯の幅は order）になるので、コレスキー分解で解く。
//
// ClickInterpolator: 突発音・リップノイズの「補間でつなぎ直す」。見つけ方は、下げる方式（Transient.h、Repair.h）と同じ
//   （高い帯域の速い包絡が、ゆっくりした包絡よりしきい値以上大きくなり、窓の時間のうちに山から 6dB 以上下がったら）。
//   見つけたら、高域通過した音が「周りの音量 × しきい値の半分（dB）」を超えている最初から最後のサンプル（前後に 4 サンプルの余白）
//   だけを補間する（区間が長いと、声の高い音を補間で作り直せずに薄くなり、かえって元の声から離れた）。先読みは 窓 ＋ 0.3ms ほど。
// Decrackler: パチパチ（細かいクリックの連続）。LPC の予測の外れ（残差）が、周り（10ms でならした残差の大きさ）より
//   しきい値の倍数以上飛び抜けた点を見つけ、その 1〜8 サンプルを補間する。1 点の音は、前向きの残差では後ろへ次数の分だけ
//   尾を引く（それを点の続きとみなすと長すぎて直せなかった）ので、前向き（過去から当てる）と後ろ向き（未来から当てる）の
//   両方で飛び抜けた点だけを選ぶ。先読みは LPC の次数 × 2 ＋ 8 サンプル（48kHz で約 1.2ms）。
#pragma once

#include "Biquad.h"

#include <vector>

namespace vcab
{

// x[0..length) のうち [start, start + count) を補間で書き換える。前に contextBefore 個、後ろに order 個以上が要る。
// a は LPC の係数（a[0] = 1、order + 1 個）。work は作業領域
void lsarInterpolate (float* x, int start, int count, const double* a, int order, std::vector<double>& work);
// x[from, to) から LPC の係数を求める（a は order + 1 個）。求められなければ false
bool estimateAr (const float* x, int from, int to, int order, double* a, std::vector<double>& work);

class ClickInterpolator
{
public:
    struct Shape
    {
        double detectHz = 2000;          // 見つけるための高域通過
        double windowMs = 5;             // 立ち上がりから、下がるのを待つ時間
        double slowAttackMs = 100, slowReleaseMs = 400; // 周りの音量のならし方
        double fastAttackMs = 0.1, fastReleaseMs = 1;
        double maxGapMs = 4;             // これより長い区間は補間しない
    };

    void prepare (double sampleRate, const Shape& shape);
    void reset();
    void process (float* data, int n, double thresholdDb);
    int latencySamples() const noexcept { return latency; }
    int detections() const noexcept { return count; }

private:
    static constexpr int kOrder = 24;
    double sr = 48000;
    Shape shape;
    int window = 240, pre = 14, post = 14, maxGap = 192, contextBefore = 256, latency = 300, size = 1024, pos = 0, count = 0;
    float fast = 0, slow = 0, prevScore = -100;
    float fastAttack = 0, fastRelease = 0, slowAttack = 0, slowRelease = 0;
    Biquad detectHigh[2];
    std::vector<float> ring, fastRing, highRing, segment; // highRing: 高域通過した音の 2 乗（区間を絞るため）
    std::vector<unsigned char> onsetRing;
    std::vector<double> work, ar;
    long long lastRepairedEnd = -1000000; // 直した区間の終わり（同じところを二度直さないため。通し番号）
    long long sampleIndex = 0;
};

class Decrackler
{
public:
    void prepare (double sampleRate);
    void reset();
    void process (float* data, int n, double threshold);
    int latencySamples() const noexcept { return latency; }
    int detections() const noexcept { return count; }

private:
    static constexpr int kOrder = 24, kMaxRun = 8, kHistory = 1024;
    double sr = 48000;
    int hop = 256, sinceHop = 0, latency = 34, size = 2048, pos = 0, count = 0;
    long long runStart = -1;
    int runLength = 0, quiet = 0;
    double scale = 0, scaleBack = 0, scaleCoeff = 0.01;
    std::vector<float> ring, segment;
    std::vector<unsigned char> forwardSpike; // 前向きの残差で飛び抜けた点（通し番号で引く）
    std::vector<double> ar, work;
    long long sampleIndex = 0;
    bool haveAr = false;
};

} // namespace vcab
