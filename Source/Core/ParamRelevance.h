// 今の設定で、その項目が意味を持つか。画面でつまみを出すか・灰色にするかの判断を、ここ 1 か所にまとめる
// （方式の性質など、処理と同じ判断を使う）。表示のモード（簡単 / 普通 / 詳細）で隠すかどうかは別（Params の paramLevel）。
// JUCE には依存しない。
#pragma once

#include "Params.h"

namespace vcab
{

enum class Relevance
{
    hidden,   // 今の方式・設定では使わない（出さない）
    disabled, // 出すが、今は効かない（灰色）
    active
};

// formantSupported: 今の方式が、ピッチとは別にフォルマントを動かせるか（方式の部品に聞く。Engine::algorithmSupportsFormant）
Relevance relevance (P id, const ParamValues& p, bool formantSupported);

} // namespace vcab
