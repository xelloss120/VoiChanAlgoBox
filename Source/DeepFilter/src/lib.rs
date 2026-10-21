//! DeepFilterNet を C++ から使うための入口。
//! どの関数も、中で起きた panic を外へ出さない（ホストの DAW を巻き込んで落とさないため）。
//!
//! モデルは2つ:
//!   標準（DeepFilterNet3。deep_filter の default-model で埋め込む）
//!   低遅延（DeepFilterNet3_ll。先読みなし。feature "ll-model" のときだけ、環境変数 VCAB_DFN_LL_MODEL が指す
//!   ファイルを埋め込む。CMake が取得して照合したものを渡す）

use std::panic::{catch_unwind, AssertUnwindSafe};

use df::tract::{DfParams, DfTract, ReduceMask, RuntimeParams};
use ndarray::{ArrayView2, ArrayViewMut2};

pub struct VcabDfn {
    m: DfTract,
    // 作った直後の状態の写し。リセットはこれに戻す
    // （DfTract::init は、中の待ち行列の片方を空にせずに足すので、呼ぶたびに遅れが 1 回分ずつ増える）
    fresh: DfTract,
}

fn model_params(low_latency: bool) -> Option<DfParams> {
    if low_latency {
        #[cfg(feature = "ll-model")]
        {
            return DfParams::from_bytes(include_bytes!(env!("VCAB_DFN_LL_MODEL"))).ok();
        }
        #[cfg(not(feature = "ll-model"))]
        {
            return None;
        }
    }
    Some(DfParams::default())
}

/// 低遅延のモデルを組み込んでいるか（1 / 0）
#[no_mangle]
pub extern "C" fn vcab_dfn_has_low_latency() -> i32 {
    cfg!(feature = "ll-model") as i32
}

/// 作る。atten_lim_db は削る量の上限（dB）、low_latency が 0 以外なら低遅延のモデル。失敗したら NULL
#[no_mangle]
pub extern "C" fn vcab_dfn_create(atten_lim_db: f32, low_latency: i32) -> *mut VcabDfn {
    let r = catch_unwind(|| {
        // 設定は DeepFilterNet の C 向けの入口（capi.rs）と同じ
        let rp = RuntimeParams::default_with_ch(1)
            .with_atten_lim(atten_lim_db)
            .with_thresholds(-15.0, 35.0, 35.0)
            .with_post_filter(0.0)
            .with_mask_reduce(ReduceMask::MAX);
        let m = DfTract::new(model_params(low_latency != 0)?, &rp).ok()?;
        let fresh = m.clone();
        Some(Box::into_raw(Box::new(VcabDfn { m, fresh })))
    });
    match r {
        Ok(Some(p)) => p,
        _ => std::ptr::null_mut(),
    }
}

/// 1 回に処理するサンプル数（48kHz で 480）
#[no_mangle]
pub unsafe extern "C" fn vcab_dfn_frame_length(st: *const VcabDfn) -> usize {
    st.as_ref().map(|s| s.m.hop_size).unwrap_or(0)
}

/// 中の遅れ（サンプル数）: FFT の窓 − 1 回分 ＋ 先を見る回数 × 1 回分
#[no_mangle]
pub unsafe extern "C" fn vcab_dfn_delay(st: *const VcabDfn) -> usize {
    st.as_ref()
        .map(|s| s.m.fft_size - s.m.hop_size + s.m.lookahead * s.m.hop_size)
        .unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn vcab_dfn_set_atten_lim(st: *mut VcabDfn, atten_lim_db: f32) {
    if let Some(s) = st.as_mut() {
        let _ = catch_unwind(AssertUnwindSafe(|| s.m.set_atten_lim(atten_lim_db)));
    }
}

/// 後段のフィルタ（声を少し強調する）の強さ。0 で使わない（目安は 0.02）
#[no_mangle]
pub unsafe extern "C" fn vcab_dfn_set_post_filter(st: *mut VcabDfn, beta: f32) {
    if let Some(s) = st.as_mut() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            s.m.set_pf_beta(beta);
            s.fresh.set_pf_beta(beta);
        }));
    }
}

/// 1 回分を処理する。戻り値はそのフレームの局所的な SNR（dB）。失敗したら入力をそのまま出して NaN を返す
#[no_mangle]
pub unsafe extern "C" fn vcab_dfn_process(st: *mut VcabDfn, input: *const f32, output: *mut f32) -> f32 {
    let Some(s) = st.as_mut() else { return f32::NAN };
    let n = s.m.hop_size;
    let r = catch_unwind(AssertUnwindSafe(|| {
        let inp = ArrayView2::from_shape_ptr((1, n), input);
        let out = ArrayViewMut2::from_shape_ptr((1, n), output);
        s.m.process(inp, out).ok()
    }))
    .ok()
    .flatten();
    match r {
        Some(lsnr) => lsnr,
        None => {
            std::ptr::copy_nonoverlapping(input, output, n);
            f32::NAN
        }
    }
}

/// 状態を初めに戻す
#[no_mangle]
pub unsafe extern "C" fn vcab_dfn_reset(st: *mut VcabDfn) {
    if let Some(s) = st.as_mut() {
        let _ = catch_unwind(AssertUnwindSafe(|| s.m = s.fresh.clone()));
    }
}

#[no_mangle]
pub unsafe extern "C" fn vcab_dfn_destroy(st: *mut VcabDfn) {
    if !st.is_null() {
        drop(Box::from_raw(st));
    }
}
