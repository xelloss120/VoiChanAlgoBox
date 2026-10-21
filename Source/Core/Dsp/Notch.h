// ノッチ（細い持続音を削る）。デノイズの前の段。
//
// 自動: 周波数の分からない、動く細い持続音（コイル鳴き、ファンのうなりなど、いわゆる「ふぃーん」）を FFT で探して
//       追いかけ、ノッチで削る。FFT は探すためだけに使い、音は通さないので、遅延は増えない。
//   1. 約 21ms ごとに、直近の音を FFT（48kHz で 4096 点）し、周り（前後 ±16 区画の平均）より「感度」以上突き出た山を探す
//      （100Hz〜16kHz）。
//   2. 声の倍音と見分ける: 等間隔に並んだ山（基音 70〜500Hz で 4 本以上）は声の倍音とみなして対象外にする。
//      そういうフレーム（声が出ている）では、新しい山を拾わず、今追いかけている山を「消えた」とも数えない
//      （声に隠れて見えないだけなので、しゃべっている間にノッチが外れないように）。
//   3. 1 秒以上居続けた山にノッチをかける（長く伸ばした母音や歌の倍音を誤って削らないように）。山の動きに合わせて
//      ノッチの周波数も動かす。消えたら 2 秒かけて外す。同時にかけるのは、突き出た量の大きい順に「本数」まで。
//   ノッチの幅は細め（Q 28.6。2kHz で幅 70Hz）。
// ハム: 電源の 50Hz / 60Hz とその倍音を、固定のノッチで削る（幅 4Hz）。電源の周波数が「自動」のときは、50Hz と
//       60Hz の倍音の並びのどちらが目立つかを、約 3 秒ならして決める。
// 深さ（1 本のノッチで削る量）は2つで共通。
#pragma once

#include "Biquad.h"
#include "Fft.h"

#include <array>
#include <vector>

namespace vcab
{

class AdaptiveNotch
{
public:
    static constexpr int kMaxAuto = 8;
    static constexpr int kMaxHum = 20;

    struct Settings
    {
        bool autoOn = true, humOn = false;
        double depthDb = 20;
        int count = 4;
        double sensitivityDb = 12;
        int mains = 0;      // 0 = 自動、1 = 50Hz、2 = 60Hz
        int harmonics = 10;
    };

    void prepare (double sampleRate);
    void reset();
    void process (float* data, int n, const Settings& s);

    // 画面に出すため: 今かけている自動のノッチの周波数（out に最大 kMaxAuto 個。戻り値は個数）
    int autoFrequencies (float* out) const;
    // ハムの電源の周波数（50 か 60。ハムがオフなら 0）
    double humMains() const noexcept { return humActive ? mainsHz : 0.0; }

private:
    struct Track
    {
        double freq = 0, prominence = 0;
        double presentSec = 0, missingSec = 0;
        double fade = 0;     // ノッチの効き（0〜1）。かけ始めと外すときに、なめらかに変える
        bool active = false; // ノッチをかけている（かける対象に選ばれている）
        bool used = false;
        Biquad filter;
    };
    struct Candidate
    {
        double freq, prominence;
        bool harmonic;
    };

    void analyze (const Settings& s);
    void updateTracks (const std::vector<Candidate>& cands, bool voiced, const Settings& s);

    double sr = 48000;
    int fftSize = 4096, hop = 1024, sinceHop = 0, ringPos = 0;
    double hopSeconds = 0.02;
    std::vector<float> ring;
    std::vector<double> window, magDb, prefix;
    std::vector<Fft::Complex> spec;
    Fft fft;
    std::vector<Candidate> candidates;

    std::array<Track, 16> tracks {};       // 追いかけている山（ノッチをかける前のものも含む）
    std::array<Biquad, kMaxHum> humFilters {};
    double mainsHz = 50, score50 = 0, score60 = 0;
    bool humActive = false;
    int humConfiguredHarmonics = -1;
    double humConfiguredDepth = -1, humConfiguredMains = -1;
};

} // namespace vcab
