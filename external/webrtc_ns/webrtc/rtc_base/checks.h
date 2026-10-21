// VoiChanAlgoBox: WebRTC の rtc_base/checks.h の代わり（元のファイルは abseil を使うため）。
// ノイズ抑制と 3 帯域フィルタが使う、デバッグ時だけ働く確認（RTC_DCHECK 系）だけを、assert で置き換える。
// リリースのビルドでは何もしない（元の WebRTC と同じ）。
#ifndef RTC_BASE_CHECKS_H_
#define RTC_BASE_CHECKS_H_

#include <cassert>

#define RTC_DCHECK(condition) assert (condition)
#define RTC_DCHECK_EQ(a, b) assert ((a) == (b))
#define RTC_DCHECK_NE(a, b) assert ((a) != (b))
#define RTC_DCHECK_LE(a, b) assert ((a) <= (b))
#define RTC_DCHECK_LT(a, b) assert ((a) < (b))
#define RTC_DCHECK_GE(a, b) assert ((a) >= (b))
#define RTC_DCHECK_GT(a, b) assert ((a) > (b))
#define RTC_DCHECK_NOTREACHED() assert (false)

#endif // RTC_BASE_CHECKS_H_
