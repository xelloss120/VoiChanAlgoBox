// WORLD（分析合成方式。ライブラリ mmorise/World を使う）
//
// 声を「基本周波数（声の高さ）」「スペクトル包絡（声色）」「非周期性（息やかすれ）」の3つに分解し、
// 加工してから合成し直す。
//   声の高さ   DIO で求め、StoneMask で細かく直す
//   声色       CheapTrick（声の高さに合わせた窓で、倍音の影響を取り除いた包絡を求める）
//   かすれ     D4C
// 加工は、声の高さを倍率倍にし、声色を周波数方向にフォルマントの倍率だけ伸び縮みさせる。
// 合成は、WORLD のリアルタイム用の合成器（声の高さに合わせて1周期ずつ波形を作って重ねる）に渡す。
//
// WORLD の分析関数は録音全体を受け取る作りなので、20ms（4フレーム）ごとに、前に 120ms、後ろに 40ms の余裕を
// 足した区間を切り出して分析し、その4フレームだけを使う（区間の端では分析が正しくないため。また DIO は、
// 声の区間がある程度続かないと声と認めないので、区間を長くする必要がある）。後ろの余裕の分と、切り出す
// 間隔の分、合成器の分が遅延になる（前の余裕は過去の音なので遅延にならない）。
//
// 処理のスレッド: 20ms ごとの分析と合成は1回に数 ms かかり、音の処理のスレッドで直接行うと、短いバッファ
// （ASIO の 128 サンプル = 2.7ms など）では受け渡しに間に合わず、20ms ごとに音が途切れる。また WORLD は途中で
// メモリを確保する。そこでリアルタイムのときは、専用の作業スレッドで分析と合成を行い、音の処理のスレッドは
// 入力を渡して出来上がった音を受け取るだけにする。遅延には、必要な分より余裕を持たせてあるので、作業スレッドの
// 計算はその中に収まる。間に合わなかったときは無音を出す。
// リアルタイムでないとき（WAV 処理ツール、ホストの書き出し）は、その場で計算する（結果が毎回同じになる）。
#pragma once

#include "../Dsp/PitchDetector.h"
#include "VoiceAlgorithm.h"

#include <memory>

namespace vcab
{

class WorldVoice : public VoiceAlgorithm
{
public:
    WorldVoice();
    ~WorldVoice() override;

    bool supportsFormant (const ParamValues&) const override { return true; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process (float* data, int n, const ParamValues& params) override;
    int latencySamples (const ParamValues& params) const override;
    const PitchEstimate* pitchEstimate() const override;
    void setRealtime (bool isRealtime) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace vcab
