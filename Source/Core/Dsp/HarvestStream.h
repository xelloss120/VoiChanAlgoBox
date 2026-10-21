// Harvest（WORLD の声の高さの推定）を、少しずつ音を受け取りながら行う版
//
// WORLD の Harvest（external/World/src/harvest.cpp、Copyright 2012 Masanori Morise、修正 BSD ライセンス）を
// 元に、リアルタイムで使えるように組み直したもの。考え方と数値（チャンネルの数、しきい値など）は元のまま。
//
// 元の Harvest は、音全体を受け取ってから、1ms ごとに次の順で計算する。
//   1. 候補を探す: 声の高さの範囲を 1 オクターブあたり 40 の帯域に分け、帯域ごとに帯域通過フィルタをかける。
//      波形が 0 を横切る間隔（と山・谷の間隔）から周波数を求め、帯域の中心に近ければ、その帯域の候補とする。
//      隣り合う 10 以上の帯域が候補を持つところを、1つの候補にまとめる。
//   2. 候補を磨く: 前後 3 フレームの候補も借りてきて、それぞれを、倍音の瞬時周波数から細かく直し、
//      確からしさ（点数）を付ける。前後のフレームに似た候補がないものは捨てる。
//   3. つなぐ: 一番点数の高い候補を並べ、急な変化・短すぎる有声区間を取り除き、有声区間を候補に沿って
//      前後に伸ばし、短い無声の隙間を埋める。最後に、なめらかにする（前後両方向にかけるフィルタ）。
//
// 計算の大部分は 1 と 2 で、これはフレームごとに独立している。そこで、1 と 2 は新しいフレームの分だけ
// 計算して覚えておき、軽い 3 だけを、直近の約 0.15 秒について毎回やり直す。
// 帯域通過フィルタは前後対称（先の音も使う）で、3 も先のフレームを見るので、その分だけ遅れて決まる。
#pragma once

#include "Biquad.h"

#include <cstdint>
#include <vector>

namespace vcab
{

class HarvestStream
{
public:
    // inputRate: 渡す音のサンプルレート。内部では約 8kHz に間引いて計算する
    void prepare (double inputRate);

    // 声の高さを探す範囲。変わったら、作り直して始め直す
    void setRange (double floorHz, double ceilHz);

    void reset();

    // 音を渡す。計算できるようになったフレームを、その場で計算する
    void push (const float* x, int n);

    // 決まったフレーム（1ms ごと、最初に渡した音の時刻が 0）の数。[0, finishedFrames()) が決まっている
    std::int64_t finishedFrames() const noexcept { return outNext; }

    // 決まったフレームの声の高さ（Hz、無声なら 0）。直近の 512 フレームより古いものは返せない（0 を返す）
    double f0AtFrame (std::int64_t frame) const noexcept;

    // 音を渡してから、その時刻のフレームが決まるまでの遅れ（ミリ秒、最大）
    double delayMs() const noexcept { return delayMsFor (floorHz); }
    static double delayMsFor (double floorHz);

    // つなぐ処理で、決める前に待つ先のフレームの数（評価用に変えられる）
    void setFixTailFrames (int frames) noexcept { fixTail = frames; }

private:
    struct Events  // 0 を横切る点（など）の間隔の記録。1つの帯域の、1つの種類
    {
        static constexpr int kSize = 32;
        double loc[kSize] {}, hz[kSize] {};
        int count = 0, head = 0;
        double lastEdge = 0;
        bool hasEdge = false;
        double prevValue = 0;  // 1つ前の値（横切ったかを調べるため）
        bool hasPrev = false;

        void clear() { count = head = 0; hasEdge = hasPrev = false; }
        void feed (double value, double index, double fs);
        double at (double t, double stale) const;  // 時刻 t（間引いた後のサンプル）の周波数。分からなければ 0
    };

    struct Channel
    {
        double boundary = 0;          // 帯域の中心の周波数
        int half = 0;                 // フィルタの長さの半分
        std::vector<double> taps;     // フィルタ（中心から片側、half + 1 個）
        std::int64_t next = 0;        // 次に計算するフィルタの出力の位置
        double prevF = 0;             // 1つ前のフィルタの出力
        double prevD = 0;             // 1つ前の差分
        bool hasPrevF = false;
        Events events[4];             // 正→負、負→正、山、谷
    };

    double y (std::int64_t i) const noexcept { return ring[static_cast<size_t> (i & ringMask)]; }
    void configure();
    void filterChannels();
    void computeOfficial (std::int64_t k);
    void refine (std::int64_t k);
    void removeUnreliable (std::int64_t k);
    void fixWindow (std::int64_t end);
    void refineOne (double t, double f0, double& refined, double& score);

    double* officialRow (std::int64_t k) noexcept { return &official[static_cast<size_t> ((k & frameMask) * maxOfficial)]; }
    double* refinedRow (std::int64_t k) noexcept { return &refinedF0[static_cast<size_t> ((k & frameMask) * numCandidates)]; }
    double* refinedScoreRow (std::int64_t k) noexcept { return &refinedScore[static_cast<size_t> ((k & frameMask) * numCandidates)]; }
    double* finalRow (std::int64_t k) noexcept { return &finalF0[static_cast<size_t> ((k & frameMask) * numCandidates)]; }
    double* finalScoreRow (std::int64_t k) noexcept { return &finalScore[static_cast<size_t> ((k & frameMask) * numCandidates)]; }

    double inputRate = 48000, fs = 8000;  // 渡される音と、間引いた後のサンプルレート
    int ratio = 6, phase = 0;
    Biquad antiAlias[2];
    double dcPrevIn = 0, dcPrevOut = 0, dcCoeff = 0.99;
    double floorHz = 80, ceilHz = 400;

    std::vector<double> ring;             // 間引いた後の音
    std::int64_t ringMask = 0, written = 0;
    std::vector<Channel> channels;
    double rawLag = 0;                    // 候補を探せるようになるまでに必要な、先の音（間引いた後のサンプル）
    int refineHalfMax = 0;

    // フレームごとの記録（リング）
    static constexpr int kFrames = 512;
    static constexpr std::int64_t frameMask = kFrames - 1;
    int maxOfficial = 10, numCandidates = 70;
    std::vector<double> official, refinedF0, refinedScore, finalF0, finalScore, output;
    std::int64_t rawNext = 0, refineNext = 0, finalNext = 0, outNext = 0;
    int fixTail = 20;

    // つなぐ処理の作業領域
    std::vector<const double*> candRows, scoreRows;
    std::vector<double> work1, work2, work3, smoothed;
    std::vector<int> boundary, order, vuvScratch;
    std::vector<std::vector<double>> sections;
    std::vector<double> win, dwin;        // 磨く処理の窓
};

} // namespace vcab
