// ボイチェンの方式の共通の枠。方式を足すときは、これを継承し、Choices の表と Algorithms.cpp の createCore に足す。
#pragma once

#include "../Choices.h"
#include "../Dsp/FormantWarp.h"
#include "../Dsp/PitchDetector.h"
#include "../Params.h"

#include <cmath>
#include <memory>
#include <vector>

namespace vcab
{

class VoiceAlgorithm
{
public:
    virtual ~VoiceAlgorithm() = default;

    // ピッチとフォルマントを別々に動かせるか。false ならフォルマントのつまみを無効表示にする。
    virtual bool supportsFormant (const ParamValues&) const = 0;

    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void reset() = 0;

    // モノラルの音をその場で書き換える
    virtual void process (float* data, int n, const ParamValues& params) = 0;

    // 遅延（サンプル数、概算の平均値）
    virtual int latencySamples (const ParamValues& params) const = 0;

    // ピッチ検出を使う方式は、最新の検出結果を返す（画面に出すため）。使わない方式は nullptr
    virtual const PitchEstimate* pitchEstimate() const { return nullptr; }

    // リアルタイムで動いているか（WAV 処理ツールやホストの書き出しでは false）。重い処理を別のスレッドに
    // 分ける方式（WORLD）が、リアルタイムでないときはその場で計算して、結果が毎回同じになるようにするため
    virtual void setRealtime (bool) {}

    // 次の process で、ピッチ検出に使う音を別に渡す（LPC やケプストラムの方式が、残差ではなく元の声で検出するため）。
    // ピッチ検出を使わない部品では何もしない
    virtual void setDetectionSource (const float*) {}

    // 残差を使わずに、声帯の音を作り直す部品か（LPC ボコーダなど）。そのときは、渡した残差の代わりに
    // パワー 1 の音源を返すので、大きさは枠の側で付ける
    virtual bool makesOwnSource() const { return false; }

    // 動かし方が「フォルマントごと」のときに使う、追いかけたフォルマントの位置（Engine が持ち、毎回書き換える）。
    // 中に別の方式の部品を持つ方式は、その部品にも渡す
    virtual void setFormantTrack (const FormantTrack* track) { formantTrack = track; }

protected:
    const FormantTrack* formantTrack = nullptr;
};

// 方式の一覧（Choices.h の Algorithm の順番）
std::vector<std::unique_ptr<VoiceAlgorithm>> createAllAlgorithms();

inline double semitonesToRatio (double st) { return std::pow (2.0, st / 12.0); }

} // namespace vcab
