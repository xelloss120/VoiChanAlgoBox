// WebRTC のノイズ抑制（external/webrtc_ns。Chrome などの WebRTC の音声処理の中の NS）を、48kHz で 480 サンプル（10ms）ずつ
// 動かす部品。AiDenoiser の枠（サンプルレートの変換、削る量の上限、帯域分割での抑制）の中で使う。
//
// 中の流れ（WebRTC の音声処理と同じ）: 48kHz の 480 サンプルを、3 帯域フィルタで 0〜8k / 8〜16k / 16〜24kHz の 3 つ
// （それぞれ 16kHz で 160 サンプル）に分ける → 0〜8kHz の帯域を FFT（256 点）で分析し、ウィーナー型で下げる（ノイズは
// 分位点で見積もり、声のありそうさは、尤度比・スペクトルの平らさ・ノイズとの形の違いから求める）。上の 2 帯域は、
// 0〜8kHz の結果から決めた 1 つの倍率で下げる → 3 帯域フィルタで戻す。
// 大きさは 16 ビットの整数の範囲で扱う（WebRTC の音声処理がそうしているため）。
// 下げる強さは WebRTC の 4 段階（6 / 12 / 18 / 21dB）。削る量の上限に一番近い段階（上限以上）を使う。
#pragma once

#include <memory>

namespace vcab
{

class WebRtcNs
{
public:
    static constexpr int kFrame = 480;
    WebRtcNs();
    ~WebRtcNs();
    WebRtcNs (const WebRtcNs&) = delete;
    WebRtcNs& operator= (const WebRtcNs&) = delete;

    void reset();
    void setMaxReduction (float db);       // 段階が変わったら作り直す（中の状態も始めから）
    float process (const float* in, float* out); // 480 サンプル。戻り値は、出した音と入れた音の大きさの比（0〜1 程度）

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    int level = -1;
};

} // namespace vcab
