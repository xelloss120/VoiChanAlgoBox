// スペクトル包絡（声色の形、フォルマントの山並み）を、振幅スペクトルから求める
//
// 振幅スペクトルには、細かいギザギザ（倍音 = 声の高さ）と、なだらかな山並み（口と喉の響き = フォルマント）が
// 重なっている。なだらかな山並みのほうだけを取り出す。
//   ケプストラム   対数の振幅スペクトルをもう一度フーリエ変換し、ゆっくり変わる成分（低いケフレンシー）だけを残して戻す。
//                  倍音の山と谷の平均をなぞるので、山の高さより低めに出る。
//   LPC 包絡       声を「口と喉のフィルタ」で近似し（線形予測）、そのフィルタの周波数特性を包絡とする。
//                  山（フォルマント）を鋭くとらえるが、次数が高すぎると倍音まで拾う。
//   True Envelope  ケプストラムで滑らかにしては、元のスペクトルより低いところを元の値まで持ち上げる、を繰り返す。
//                  倍音の山の頂点をなぞる包絡になる。
#pragma once

#include "Fft.h"

#include <vector>

namespace vcab
{

class SpectralEnvelope
{
public:
    enum Method { none = 0, cepstrum, lpc, trueEnvelope };

    // fftSize は 2 のべき乗。振幅は fftSize / 2 + 1 個
    void prepare (int fftSize, double sampleRate);

    // mag から包絡を求めて env に入れる（どちらも fftSize / 2 + 1 個、振幅の単位）
    void estimate (int method, const double* mag, double* env);

private:
    void cepstralSmooth (const double* logMag, double* smoothed); // 対数振幅を、ケプストラムで滑らかにする
    void lpcEnvelope (const double* mag, double* env);

    int n = 0, half = 0;
    double sr = 48000;
    int lifter = 120;  // ケプストラムで残す長さ（サンプル数、2.5ms）
    int lpcOrder = 40;

    Fft fft;
    std::vector<Fft::Complex> buf;
    std::vector<double> logMag, smooth, lifted, autocorr, coeffs, reflection, tmp;
};

} // namespace vcab
