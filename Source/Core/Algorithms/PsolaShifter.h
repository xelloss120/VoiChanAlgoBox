// TD-PSOLA（時間領域のピッチ同期の重ね足し）
//
// 1. ピッチ検出で声の周期を求め、入力の音に1周期ごとの印（ピッチマーク）を付ける。
//    印の位置は、前の印の周りの波形と一番よく重なる位置に合わせる（周期がずれていかないように）。
// 2. 出力側で、「周期 ÷ ピッチの倍率」の間隔で、近くの印を中心に、前の印から次の印までの断片を切り出し、
//    ハン窓をかけて重ね足す。間隔を詰めればピッチが上がり、広げれば下がる。
//    窓は左右で長さが違う（非対称窓）: 左半分は前の印まで、右半分は次の印までの長さ（今の周期の 0.5〜1.5 倍に収める）。
//    声の高さが動いているとき、隣の周期の中まで切り出したり、足りなかったりしないので、つなぎ目が乱れにくい。
//    断片の中身（口や喉の響き＝フォルマント）はそのままなので、フォルマントは動かない。
// 3. フォルマントは、断片を伸び縮みさせて動かす（縮めると響きが高くなる）。
//    下げる（伸ばす）ときは、切り出しを短くして、断片の長さが2周期を超えないようにする（遅延を増やさないため）。
// 4. 無声音（息やサ行の子音）では、周期に合わせた切り出しをやめ、5ms ごとにその場で切り出して重ねる。
//    ピッチは変えず、フォルマントだけ動かす。
//    有声と無声の断片は別々の流れで並べ、有声らしさの重み（10ms かけて 0〜1 を動かす）で等パワーに重ねてつなぐ
//    （一瞬で切り替えると、窓の重なりが途切れて、境目で音量が跳ねたり落ちたりするため）。無声になった直後も、
//    最後の周期で有声の流れを続けてフェードアウトする。
//
// 遅延は、探す範囲の下限の周期の2倍（下限 80Hz なら約25ms）。ピッチ検出の窓が長い方式や、先の音を使う方式（SWIPE、RAPT、DIO、Harvest）では、
// 声の高さが分かるのが遅れる分だけ、印を付けるのも音を出すのも遅らせて時刻を合わせる（その分、遅延が増える）。
//
// 印付けの方式（psolaMarking）:
//   波形の重なり  1. のとおり。印の位置は、波形の形で決まる（どこが周期の頭になるかは、始めたときの位置しだい）。
//   声門閉鎖点    声帯が閉じる瞬間（音のエネルギーが一番集まる瞬間）に印を付ける。定番の SEDREAMS の考え方に沿う:
//                 周期の 1.75 倍の窓で平均した波形（平均の信号）の谷のすぐ後（0.35 周期以内）で、LPC の残差（声色を
//                 取り除いた声帯の音）が一番鋭く突き出るところ。マイクの向き（正負）で残差の突き出る向きが逆になるので、
//                 残差の偏り（3 乗の平均の符号）から向きを決める。約 1.2 周期先の音を見るので、その分遅延が増える。
//                 高い帯域の雑音で残差の山が埋もれないように、4kHz より上を落とした音で残差を求め、約 1ms でならした
//                 残差の山を、1 周期後らしい位置に近いほど優先して選ぶ。
//                 評価: 雑音のない声では正確（印の間隔のばらつき 0）だが、雑音があると、1 点の山だけを見るこの方式は
//                 ぶれやすく、1 周期分の波形を比べる「波形の重なり」より印がばらつく（合成音で、ばらつき 7〜20 サンプル
//                 に対して 3〜6 サンプル）。聞き比べのために残している。
//
// フォルマントの動かし方が「低い側を残す」のときは、3. で一律に動かした後に、声色の補正の段（LpcVoice の
// makeFormantCorrector）を通して、一律との差の分だけ包絡を動かし直す（遅延は増えない）。
#pragma once

#include "../Dsp/Biquad.h"
#include "../Dsp/Intonation.h"
#include "../Dsp/PitchDetector.h"
#include "VoiceAlgorithm.h"

#include <array>
#include <cstdint>
#include <memory>

namespace vcab
{

class LpcVoice;

class PsolaShifter : public VoiceAlgorithm
{
public:
    PsolaShifter();
    ~PsolaShifter() override;

    bool supportsFormant (const ParamValues&) const override { return true; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process (float* data, int n, const ParamValues& params) override;
    int latencySamples (const ParamValues& params) const override;

    const PitchEstimate* pitchEstimate() const override { return &detector.latest(); }

    // 次の process で、ピッチ検出に使う音を別に渡す（LPC＋PSOLA は、残差ではなく元の声で検出する）。
    // 渡した配列は、process に渡すのと同じ長さで、process が終わるまで有効なこと
    void setFormantTrack (const FormantTrack* track) override; // 声色の補正の段にも渡す
    void setDetectionSource (const float* source) override { detectionSource = source; }

private:
    float inputAt (double pos) const noexcept;  // 絶対位置（何サンプル目か）の入力を、直線補間で読む
    void placeMarks (double period);            // 読めるところまで印を足す
    void placeMarksGlottal (double period);     // 声門閉鎖点で印を付ける
    double findMarkNear (double pos, double halfSource) const noexcept;
    void markNeighbours (double mark, double& prev, double& next) const noexcept; // 前後の印（なければ -1）
    void addMark (double pos);
    void analyzeResidual();                     // 声門閉鎖点のための LPC を求め直す
    void addGrain (double centreOut, double centreIn, double halfOutLeft, double halfOutRight, double formantRatio, double gain);

    double sr = 48000;
    PitchDetector detector;

    std::vector<float> in, out;
    int mask = 0;
    std::int64_t now = 0;        // 書き込んだサンプルの数

    // 印（入力の絶対位置）。新しいものから順に、最近のものだけ持つ
    std::array<double, 32> marks {};
    int numMarks = 0, markHead = 0;
    double lastMark = -1;

    const float* detectionSource = nullptr;
    int markLag = 0;             // ピッチ検出が遅れて分かる分、印を付けるのを遅らせる（サンプル数）
    double nextGrain = 0;        // 次の有声の断片の中心（出力の時刻を、入力の位置に合わせた値）
    double nextNoise = 0;        // 次の無声の断片の中心
    double voicedMix = 0;        // 有声らしさの重み（0〜1。10ms かけて動かす）
    double lastPeriod = 0;       // 最後に分かった周期（無声になった後、有声の流れをフェードアウトするのに使う）

    IntonationEmphasis intonation;

    // 声門閉鎖点のための LPC の残差（入力と同じ位置に並べる）と、残差の偏り（向きを決める）
    static constexpr int kResidualOrder = 32;
    std::vector<float> res, lowIn;       // 残差と、4kHz より上を落とした入力
    Biquad resLowPass[2];
    std::vector<double> lpcWindow;
    double resK[kResidualOrder + 1] {}, resState[kResidualOrder] {};
    int resOrder = 24, resHop = 240, resWindow = 960, sinceResHop = 0;
    double resM2 = 0, resM3 = 0, resPolarity = 1.0;
    std::vector<double> meanWeights;     // 平均の信号の窓（長さが変わったときだけ作り直す）
    int meanHalf = -1;
    double meanWeightSum = 1;
    bool glottal = false;                // 今、声門閉鎖点で印を付けているか
    std::unique_ptr<LpcVoice> corrector; // 「低い側を残す」のときの、声色の補正の段
    bool correctorActive = false;
};

} // namespace vcab
