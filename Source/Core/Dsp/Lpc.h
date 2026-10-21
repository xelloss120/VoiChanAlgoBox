// LPC（線形予測）の計算。スペクトル包絡と、LPC の方式で共有する。
//
// 声を「直前の p 個のサンプルの重み付きの和で、次のサンプルを予測する」形で近似する。
// 予測の重み（予測係数）が「口と喉のフィルタ」を表し、予測しきれなかった残り（残差）が「声帯の音」に近い。
#pragma once

#include <algorithm>
#include <cmath>

namespace vcab::lpc
{

// レビンソン・ダービンの再帰。自己相関 r[0..order] から、予測係数 a[0..order]（a[0] = 1、
// A(z) = 1 + a1 z^-1 + ... + ap z^-p）と、反射係数 k[1..order] を求める。tmp は order + 1 個の作業領域。
// 戻り値は予測の誤差（残差のパワー。r と同じ単位）。
inline double levinson (const double* r, int order, double* a, double* k, double* tmp)
{
    std::fill (a, a + order + 1, 0.0);
    std::fill (k, k + order + 1, 0.0);
    a[0] = 1;
    double err = r[0];
    for (int i = 1; i <= order; ++i)
    {
        if (err <= 0)
            break;
        double acc = r[i];
        for (int j = 1; j < i; ++j)
            acc += a[j] * r[i - j];
        const double ki = std::clamp (-acc / err, -0.9999, 0.9999); // |k| < 1 なら合成のフィルタは安定
        std::copy (a, a + i, tmp);
        for (int j = 1; j < i; ++j)
            a[j] = tmp[j] + ki * tmp[i - j];
        a[i] = ki;
        k[i] = ki;
        err *= 1 - ki * ki;
    }
    return err;
}

// 格子型（ラティス）の分析フィルタ A(z)。音を通すと残差になる。state は order 個
inline double analysisLattice (double x, const double* k, int order, double* state)
{
    // f: 前向きの予測誤差、b: 後ろ向きの予測誤差。state[i] は1サンプル前の b_i
    double f = x, bPrev = x;
    for (int i = 1; i <= order; ++i)
    {
        const double bOld = state[i - 1];
        const double fi = f + k[i] * bOld;
        const double bi = k[i] * f + bOld;
        state[i - 1] = bPrev;
        bPrev = bi;
        f = fi;
    }
    return f;
}

// 格子型の合成フィルタ 1 / A(z)。残差を通すと声に戻る。state は order 個
inline double synthesisLattice (double e, const double* k, int order, double* state)
{
    // 分析と逆向きに、上の段から f を戻していく。state[i] は1サンプル前の b_i
    double f = e;
    for (int i = order; i >= 1; --i)
    {
        f -= k[i] * state[i - 1];               // f_{i-1}
        if (i < order)
            state[i] = k[i] * f + state[i - 1]; // b_i（一番上の段の b は使わない）
    }
    state[0] = f; // b_0 = 出力
    return f;
}

} // namespace vcab::lpc
