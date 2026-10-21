// VoiChanAlgoBox: WebRTC の AudioBuffer の代わり（元のクラスは、帯域分割・サンプルレートの変換・チャンネルの管理まで
// 持っていて、多くのファイルを連れてくるため）。ノイズ抑制（NoiseSuppressor）が使うのは、帯域に分けた後の
// 10ms 分（帯域ごとに 160 サンプル）の配列を読み書きする split_bands / split_bands_const だけなので、それだけを持つ。
// 帯域の配列は、使う側（Source/Core/Dsp/WebRtcNs.cpp）が用意して、setBands で渡す。
#ifndef MODULES_AUDIO_PROCESSING_AUDIO_BUFFER_H_
#define MODULES_AUDIO_PROCESSING_AUDIO_BUFFER_H_

#include <cstddef>
#include <vector>

namespace webrtc {

class AudioBuffer {
 public:
  // channelBands[ch][b] = チャンネル ch の帯域 b の 160 サンプル
  void setBands(std::vector<std::vector<float*>> channelBands) { bands_ = std::move(channelBands); }

  float* const* split_bands(size_t channel) { return bands_[channel].data(); }
  const float* const* split_bands_const(size_t channel) const {
    return bands_[channel].data();
  }

 private:
  std::vector<std::vector<float*>> bands_;
};

}  // namespace webrtc

#endif  // MODULES_AUDIO_PROCESSING_AUDIO_BUFFER_H_
