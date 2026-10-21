// LPC の方式（LPC＋ディレイライン、LPC＋PSOLA、LPC ボコーダ）と、ケプストラムの方式（ケプストラム＋ディレイライン、
// ケプストラム＋PSOLA、ケプストラムボコーダ）の共通の枠
//
// 声色（口と喉のフィルタ）の求め方が違うだけで、取り除き方・付け直し方は同じ:
//   LPC          声の自己相関から、そのまま LPC（線形予測）を求める。フィルタは山（フォルマント）を鋭くとらえる。
//   ケプストラム 対数振幅スペクトルのケプストラムの、ゆっくり変わる成分（2.5ms まで）だけを残して、なめらかな包絡を
//                求める（フェーズボコーダなどのケプストラムの包絡と同じ）。その包絡に、高い次数（ケプストラムで残す
//                長さの半分、48kHz で 60 次）の LPC を合わせて、同じ格子型のフィルタにする。次数は 40〜128 で
//                試して、フォルマントの正確さはほとんど変わらなかった。包絡の形はケプストラムのもの（山と谷をなだらかになぞる）。
//                最小位相の FIR で直接作る方法も試したが、係数を切り替えながら逆に使うと不安定になることがあった
//                （声が高いと、包絡に倍音の凹凸が残り、フィルタが鋭くなるため）。格子型は、係数が ±1 の内側なら
//                いつも安定で、係数を切り替えても、取り除いたものをちょうど元に戻せる。
//   メルケプストラム
//                周波数の軸を耳の感じ方（メル尺度）に近く曲げた上でのケプストラム（Mlsa.h）。曲げた軸の上で等間隔に
//                読み直した対数振幅スペクトルの、M 次（サンプルレート ÷ 1200。48kHz で 40 次）までを残す。低い周波数ほど
//                細かく表せる。フィルタは格子型ではなく MLSA フィルタ（メルケプストラムから直接作る、指数関数の近似）。
//                大きさの b(0) は分析では使わず（LPC の A(z) と同じく、インパルス応答の最初が 1 のフィルタにする）、
//                フォルマントを動かしたときの大きさの差だけを合成の出力にかける。
//   LPC の改善のスイッチ（帯域拡張、係数補間、高域強調）は LPC だけのもの。ケプストラムとメルケプストラムでは、
//   係数補間だけをいつも使う。
//
// 以下は LPC の場合の説明（ケプストラムも 1 以外は同じ）。
//
// 1. 5ms ごとに、直近 25ms の音から LPC（線形予測）を求める。予測係数が「口と喉のフィルタ」を表す。
// 2. 音を分析のフィルタ A(z) に通して、残差（声帯の音に近い、平らなスペクトルの音）を取り出す。
// 3. 残差のピッチを変える。変え方は方式ごとに違う（ディレイライン式、または TD-PSOLA）。
//    残差にはフォルマントがほとんど含まれないので、早回ししても声色は変わらない。
//    LPC ボコーダは、残差の代わりに、作り直した声帯の音（パルス列と雑音、SourceExciter）を使う。
// 4. 合成のフィルタ 1/A'(z) に通して、声に戻す。フォルマントを動かすときは、A(z) の特性（包絡）を
//    周波数方向にフォルマントの倍率だけ伸び縮みさせてから、LPC を求め直したもの（A'）を使う。
//
// 音質を上げる定番の対策（それぞれオン・オフできる）:
//   帯域拡張  自己相関にラグ窓をかけて、フィルタの山を少しなだらかにする（鋭すぎる山の「ビーン」という響きを抑える）
//   係数補間  5ms ごとの係数を、1サンプルずつ直線でつなぐ（切り替えのザラつきを抑える）
//   高域強調  分析の前に高い音を持ち上げ（1 - 0.9z^-1）、合成の後で戻す（高いフォルマントまで正しくとらえる）
// どれも分析と合成で同じことをするので、ピッチもフォルマントも 0 なら元の音に戻る。
//
// 音量の暴れ止め: 声の出始めなどで係数が急に変わると、平らになっていない残差が、ピッチを変える部品から
// 違う時刻の（鋭い山の）合成フィルタに入り、大きく持ち上がることがある（特にディレイライン式は、2つの読み出し位置を
// 混ぜるので、残差の時刻を1つに決められない）。出力が、同じ時刻の入力より 6dB 以上大きくなったら、その分だけ下げる。
// 基準は「遅延の分だけ遅らせた入力」と「今の入力」の大きいほう（ディレイライン式は実際の遅れが動くため）。
//
// フィルタは格子型（ラティス）。係数を切り替えても不安定にならず、次数が高くても計算が崩れにくい。
// 残差はピッチを変える部品の遅延の分だけ遅れて出てくるので、合成には、その時刻の係数を履歴から取り出して使う。
#pragma once

#include "../Dsp/Fft.h"
#include "../Dsp/FormantWarp.h"
#include "../Dsp/Mlsa.h"
#include "VoiceAlgorithm.h"

#include <memory>

namespace vcab
{

class LpcVoice : public VoiceAlgorithm
{
public:
    // shifter: 残差のピッチを変える部品（ディレイライン式、TD-PSOLA、または音源を作り直す SourceExciter）
    // envelope: 声色の求め方（Envelope::none 以外）
    LpcVoice (std::unique_ptr<VoiceAlgorithm> shifter, Envelope envelope);

    // TD-PSOLA の「低い側を残す」動かし方のための、声色の補正の段。TD-PSOLA は一律にしか動かせないので、
    // 一律に動かした後の声から包絡を求め（ケプストラム）、一律と「低い側を残す」の差の分だけ、包絡を動かし直す。
    // 間の部品は素通しで、遅延は増えない
    static std::unique_ptr<LpcVoice> makeFormantCorrector();

    bool supportsFormant (const ParamValues&) const override { return true; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process (float* data, int n, const ParamValues& params) override;
    int latencySamples (const ParamValues& params) const override;
    const PitchEstimate* pitchEstimate() const override { return shifter->pitchEstimate(); }
    void setFormantTrack (const FormantTrack* track) override
    {
        formantTrack = track;
        shifter->setFormantTrack (track);
    }

    static constexpr int kMaxOrder = 128;

private:
    struct CoeffSet
    {
        double k[kMaxOrder + 1] {}; // 反射係数（k[1..order]）。メルケプストラムでは MLSA フィルタの係数 b(1..order)
        double gain = 1;
        double level = 0;           // 予測の誤差の大きさ（1サンプルあたり）。音源を作り直す方式で、音源にかける
        int order = 0;
    };

    void analyze (const ParamValues& params); // 直近の窓から係数を求めて、履歴に入れる
    void warpFormant (const CoeffSet& in, double formantRatio, CoeffSet& out);
    // メルケプストラム: 対数振幅スペクトル（melLogAmp）を、曲げた軸の上で読み直して、MLSA の係数 b(0..order) にする。
    // formantRatio が 1 でなければ、包絡をその倍率で動かしてから読む
    void melCoeffs (bool shift, double formantRatio, double* b);
    double melFilterPower (double b0) const; // 今の melMc の exp(F)（b(0) を除く）の平均のパワー
    void analyzeMel (const ParamValues& params); // メルケプストラムの analyze
    // 時刻 t のサンプルに使う係数を、履歴 sets から求めて out に入れる（係数補間のときは、前の係数との間をつなぐ）
    void coeffsAt (std::int64_t t, const std::vector<CoeffSet>& sets, bool interpolate, CoeffSet& out) const;

    std::unique_ptr<VoiceAlgorithm> shifter;
    Envelope envelope = Envelope::lpc;
    int cepstrumOrder = 60;          // ケプストラムの包絡に合わせる LPC の次数
    double lifterSeconds = 0.0025;   // ケプストラムで残す長さ
    bool correction = false;         // makeFormantCorrector で作った、補正の段か
    FormantWarp warp;
    double sr = 48000;
    int hop = 240, window = 1200, sinceHop = 0;
    std::int64_t now = 0;            // 受け取ったサンプルの数
    std::int64_t hopCount = 0;       // 係数を求めた回数

    std::vector<float> history;      // 直近の入力（窓の長さ分）
    int historyPos = 0;
    std::vector<double> frame, autocorr, a, tmp, hann;
    double windowPower = 1;          // 窓の2乗の和（予測の誤差を、1サンプルあたりの大きさに直すため）

    CoeffSet analysisSet;            // 今求めた分析のフィルタ（作業用）
    CoeffSet current;                // 今のサンプルに使うフィルタ（作業用）
    const CoeffSet identity {};      // 何もしないフィルタ（次数 0）
    std::vector<CoeffSet> analysisSets, synthesisSets; // 分析と合成のフィルタの履歴（hopCount で引く）
    double emphasisState = 0, deemphasisState = 0;     // 高域強調と、その戻し

    std::vector<float> delayedInput; // 暴れ止めのために、入力を遅延の分だけ遅らせる
    std::vector<float> sourceDelay;  // 音源を作り直す方式で、音源を遅らせる（係数の窓の中心に時刻を合わせるため）
    int sourceDelayPos = 0;
    int delayedPos = 0;
    double inPower = 0, inPowerNow = 0, outPower = 0, guardGain = 1;
    std::vector<double> analysisState, synthesisState;
    std::vector<float> residual;     // 1ブロック分の残差
    std::vector<float> original;     // 1ブロック分の元の声（TD-PSOLA のピッチ検出に渡す）

    Fft cepstrumFft;                 // ケプストラムの包絡の計算（窓の長さ以上の 2 のべき乗）
    std::vector<Fft::Complex> cepstrumSpec;
    std::vector<double> cepstrumLog;
    // メルケプストラム
    int melOrder = 40;
    double melAlpha = 0.55;
    std::vector<double> melLogAmp;   // 今の窓の対数振幅スペクトル（cepstrumFft の 0〜半分）
    std::vector<double> melPos;      // 曲げた軸の等間隔の点が、元の軸のどの位置（FFT の番号）に当たるか
    std::vector<double> melCos;      // 元の軸の点 k の、曲げた軸での角度（大きさを測るため）
    std::vector<double> melMc, melB, melB2;
    mlsa::Filter analysisMlsa, synthesisMlsa;

    Fft fft;                         // フォルマントを動かすための包絡の計算
    std::vector<Fft::Complex> spec;
    std::vector<double> power;
};

} // namespace vcab
