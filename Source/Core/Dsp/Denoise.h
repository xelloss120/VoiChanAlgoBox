// デノイズ（持続するノイズを下げる）。リペアの後、ゲートの前の段。
//
// 方式（設定の denoise_method。一覧と性質は Choices.h。処理では DenoiseSettings::engine と bandSynthesis で振り分ける）:
//   帯域分割式: フィルタで 20 の帯域に分け、帯域ごとに下げる量を決めて足し戻す。遅延 0。
//     分け方は「低域通過の差」（帯域 k = LP_k − LP_{k−1}）。全部の帯域を 1 倍にすると、足し戻した音は元と同じになる。
//     下げる量を決めるための音量は、別のバンドパス（12dB/oct）で測る（低域通過の差は、帯域の外へのもれが大きいため）。
//     下げる量の決め方（rule）: ゲート式 / エキスパンダー式 / SNR 式（ウィーナー型）。
//   くし形フィルタ＋帯域分割: 帯域分割式で下げた後、有声音のときだけ、声の周期 T ずつずらした 4 つの音の平均
//     （倍音は重なって残り、倍音の間は打ち消し合う）を混ぜて、倍音の間を下げる。4kHz で低い側と高い側に分け、
//     低い側だけにかける（高い倍音ほど、声の高さの小さな誤差で位置がずれるため）。遅延 0（分けた分の位相は回る）。
//   FFT: 平方根ハン窓、重なり 50% で分析して下げ、FFT で戻す。遅延は窓の長さ。下げる量の決め方（fftRule）:
//     スペクトル減算、ウィーナー（判定指向型）、MMSE-STSA / log-MMSE（Ephraim・Malah 1984 / 1985）、
//     ウィーナー＋倍音の再生（HRNR、Plapous ら 2006: ウィーナーで下げた音を半波整流して倍音を作り直し、それと合わせた SNR で決め直す）、
//     ウィーナー＋倍音の保護（検出した倍音の位置〈5kHz まで〉だけ下げる量をゆるめる）、過剰減算（Berouti）、
//     帯域ごとの減算（Kamath・Loizou）、2 段階（TSNR）、聴覚のマスキング（Virag）。
//   FFT＋FIR: 下げる量は FFT で決め、実際に下げるのは、その量から作った最小位相の FIR フィルタ（今の音にかける）。
//     遅延はほぼ 0（反応は、分析の窓の分だけ遅れる）。
//   FFT＋帯域分割: FFT で決めた量を 32 帯域（BandSynth）にまとめ、帯域分割で今の音にかける。遅延 0。
//     FFT の反応の遅れで話し始めの子音が削られないように、「立ち上がりの保護」がある（BandSynth の説明）。
//   WebRTC NS: Chrome などの WebRTC の音声処理の中のノイズ抑制（AI ではない。WebRtcNs.h）。
//   RNNoise / DeepFilterNet（AI）: ニューラルネットが下げる量を決める。DeepFilterNet は Rust のラッパー（Source/DeepFilter）を
//     静的ライブラリにして組み込む。Rust がない環境でビルドしたときは VCAB_HAS_DEEPFILTER が定義されず、素通しになる。
//   WebRTC NS と AI の方式は同じ枠（AiDenoiser。48kHz で 480 サンプルずつ、ほかのサンプルレートでは中で変換して戻す）で動かし、
//     ノイズの見積もりは自分で行う。削る量の上限は、同じだけ遅らせた元の音を混ぜて実現する。
//     「＋帯域分割」では、出した音と入れた音の帯域ごとの大きさの比を、遅らせていない今の音にかける（補聴器などの
//     「サイドブランチ方式」。遅延 0 だが、方式の遅れの分だけ反応が遅れる）。
// 倍音を意識する方式（くし形フィルタと、FFT の倍音の保護）の声の高さは、ボイチェンと同じ部品（PitchDetector）で求める。
//   方式はデノイズのタブで選び、探す範囲はボイチェンのタブの値を使う。
// 下げる量のならし方（FFT の方式。ミュージカルノイズ＝ピロピロ音を減らす）:
//   時間（アタック / リリース）、周波数（前後の区画の平均）、ケプストラム（Breithaupt ら 2007: 残す割合の対数の
//   ケプストラムを、包絡はそのまま、声の高さの山は弱く、それ以外は強く、時間方向にならす）。
// 声のありそうさでの調整（FFT の方式。OM-LSA の考え方、Cohen 2002）: 残す割合 G を、声がある確率 p で G^p × 下限^(1−p) にする。
// 残響の抑制（ノイズを自分で見積もる方式）: 残響の後ろの部分を「80ms 前の音 × 残響の長さで決まる減衰」で見積もり、
//   ノイズに足して下げる（Lebart ら 2001）。
// ノイズの見積もり（ノイズを自分で見積もる方式で共通。DenoiseSettings::noiseMode）:
//   自動（最小値）: 区画ごとの、ならした大きさの最小値を、直近の「最小値を探す長さ」（初期値 1.6 秒）で追いかける
//         （Martin の minimum statistics の簡単な形）。最小値は平均より小さく出るので、白色雑音で測った倍率で直す
//         （prepare のときに、探す長さの 7 通りで測り、その間は補間する）。
//   自動（音量の判定、初期値）: 全体の音量が、見積もりの合計より「判定のしきい値」以上大きければ「声あり」とみなし、「声なし」の間
//         （「声あり」の後 0.1 秒は除く）だけ、約 0.4 秒でならして更新する。「声あり」が続いても見積もりは動かさない
//         （話し続けた声を覚えないように）。しきい値を超えるほどノイズが大きくなったときは、使う人がしきい値を上げて合わせ直す。
//   自動（Gerkmann の声の推定）: 区画ごとに「声がある確率」を、「声の SNR の仮定」から求め、声がなさそうな分だけノイズの見積もりを
//         更新する（Gerkmann・Hendriks 2012）。ノイズが変わったときの追いつきが速い。
//   自動（RNNoise の声の判定）: RNNoise の声の判定（VAD。確率が「声とみなす確率」を超えたら声）を別に動かし、「声なし」と確かめられた間の大きさだけで、ノイズの見積もりを
//         約 0.4 秒でならして更新する。判定は少し遅れて出るので、分析した大きさを 60ms 遅らせ、その前後がずっと「声なし」のものだけ使う。
//         話し続けても、声をノイズと取り違えない（最小値や Gerkmann は、話し続けると見積もりが上がり、声を削る）。
//         まだ「声なし」を確かめられていないときは最小値の見積もりを使う。4 秒以上「声あり」が続くと、最小値の見積もりの方が
//         小さい区画だけ、そちらへゆっくり下げる（上げる向きには寄せない。声を削らない側に倒す）。
//         RNNoise は 10ms ごとにまとめて重い計算をするので、リアルタイムのときは作業スレッドで判定する（VoiceDetector）。遅延は増えない。
//   学習: 2 秒間の平均。覚えるまでは自動（最小値）の値を使う。
//   学習＋自動（音量の判定 / Gerkmann の声の推定 / RNNoise の声の判定）: 学習した値から始めて、その後はそれぞれの方法で追う。
// 削る量の上限は全方式で共通（完全に消そうとすると、声が水っぽく、ロボットのようになるので、ノイズを少し残す）。
#pragma once

#include "AiDenoise.h"
#include "BandDenoise.h"
#include "DenoiseSettings.h"
#include "NoiseTracker.h"
#include "PitchDetector.h"
#include "SpectralDenoise.h"

#include <array>
#include <vector>

namespace vcab
{

class Denoiser
{
public:
    static constexpr double kLearnSeconds = 2.0;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void process (float* data, int n, const DenoiseSettings& s);
    int latencySamples (const DenoiseSettings& s) const;
    // リアルタイムでないとき（書き出し）は、声の判定（RNNoise）を作業スレッドではなく、その場で計算する
    void setRealtime (bool isRealtime) noexcept { voice.setRealtime (isRealtime); }

    void startLearning();
    // 学習の進み具合（0〜1）。学習していないときは -1
    float learnProgress() const noexcept;
    bool hasLearned (const DenoiseSettings& s) const;
    // 削っている量（dB、正の値）。入力と出力の音量の差をならしたもの
    float reductionDb() const noexcept { return reduction; }
    // 倍音を意識する方式で検出している声の高さ（無声音や、ほかの方式のときは 0）
    float detectedPitchHz() const noexcept { return pitchHz; }
    // 声のありそうさ（0〜1。ゲートとの連携に使う）
    float voiceProbability() const noexcept { return voiceProb; }
    // 表示用: freqs の各周波数でのノイズの見積もり（白色雑音に直したときの 1 サンプルあたりの 2 乗を dB で）。
    // ノイズを自分で見積もらない方式（AI）では false
    bool noiseCurve (const DenoiseSettings& s, const float* freqs, float* outDb, int count) const;

    // 学習した値の書き出しと読み込み（保存のため）。形式: サンプルレート、見積もりの数、（区画の数、学習済みか、値…）× 数
    std::vector<float> exportLearned() const;
    bool importLearned (const std::vector<float>& data); // サンプルレートや区画の数が違えば false

private:
    NoiseTracker& trackerFor (const DenoiseSettings& s);
    const NoiseTracker& trackerFor (const DenoiseSettings& s) const { return const_cast<Denoiser*> (this)->trackerFor (s); }
    AiDenoiser* aiFor (const DenoiseSettings& s); // ノイズを自分で見積もる方式の部品（ほかの方式では nullptr）
    const AiDenoiser* aiFor (const DenoiseSettings& s) const { return const_cast<Denoiser*> (this)->aiFor (s); }

    double sr = 48000;
    int current = -1;
    BandDenoiser band;
    std::array<FftDenoiser, 3> ffts;
    std::array<LowLatencyDenoiser, 3> lowLatency;
    AiDenoiser rnnoise { AiDenoiser::Model::rnnoise };
    AiDenoiser deepFilter { AiDenoiser::Model::deepFilter };
    AiDenoiser deepFilterLowLatency { AiDenoiser::Model::deepFilterLowLatency };
    AiDenoiser webrtcNs { AiDenoiser::Model::webrtc };
    VoiceDetector voice; // ノイズの見積もりが「RNNoise の声の判定」のときだけ動かす
    bool voiceActive = false;
    float voiceProb = 0;
    HarmonicComb comb;
    PitchDetector pitch;
    ParamValues pitchParams;
    float pitchHz = 0;
    std::vector<float> dry;
    double inPower = 0, outPower = 0;
    float reduction = 0;
};

} // namespace vcab
