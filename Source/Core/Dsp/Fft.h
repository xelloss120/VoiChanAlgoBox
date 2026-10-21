// FFT（高速フーリエ変換）。基数2の、その場で書き換える形。
// 大きさは 2 のべき乗。ピッチ検出のケプストラム法や、フェーズボコーダなどで共有する。
#pragma once

#include "Biquad.h"

#include <complex>
#include <vector>

namespace vcab
{

class Fft
{
public:
    using Complex = std::complex<double>;

    void prepare (int newSize)
    {
        size = newSize;
        twiddle.resize (static_cast<size_t> (size / 2));
        for (int k = 0; k < size / 2; ++k)
            twiddle[static_cast<size_t> (k)] = std::polar (1.0, -2 * kPi * k / size);
    }

    int getSize() const noexcept { return size; }

    // 時間 → 周波数
    void forward (Complex* x) const noexcept { transform (x, false); }

    // 周波数 → 時間（1/N をかけて、forward と対になるようにする）
    void inverse (Complex* x) const noexcept
    {
        transform (x, true);
        for (int i = 0; i < size; ++i)
            x[i] /= static_cast<double> (size);
    }

private:
    void transform (Complex* x, bool inv) const noexcept
    {
        // ビットを反転させた順番に並べ替える
        for (int i = 1, j = 0; i < size; ++i)
        {
            int bit = size >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap (x[i], x[j]);
        }

        // 2つずつ、4つずつ … と組み合わせていく（バタフライ演算）
        for (int len = 2; len <= size; len <<= 1)
        {
            const int step = size / len;
            for (int i = 0; i < size; i += len)
            {
                for (int k = 0; k < len / 2; ++k)
                {
                    auto w = twiddle[static_cast<size_t> (k * step)];
                    if (inv)
                        w = std::conj (w);
                    const auto u = x[i + k];
                    const auto v = x[i + k + len / 2] * w;
                    x[i + k] = u + v;
                    x[i + k + len / 2] = u - v;
                }
            }
        }
    }

    int size = 0;
    std::vector<Complex> twiddle;
};

} // namespace vcab
