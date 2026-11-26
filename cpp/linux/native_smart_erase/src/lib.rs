//! Linux 智能擦除桥接层：复用共享算法与原版 C 接口，不链接 Windows 专用模块。
//! Linux-only bridge for the shared algorithm and original C ABI, without Windows modules.

mod panic {
    use std::panic::{catch_unwind, AssertUnwindSafe};
    /// 捕获 Rust panic，避免异常跨越 C ABI 边界。
    /// Keep a Rust panic from unwinding across the C ABI boundary.
    pub fn guard<R>(failure: R, body: impl FnOnce() -> R) -> R {
        catch_unwind(AssertUnwindSafe(body)).unwrap_or(failure)
    }
}
mod ppocr {
    use std::ffi::c_void;
    /// 复用原版桥接层与 OCR 相同的取消回调签名，不依赖 OCR 库。
    /// The original bridge uses the same callback signature as OCR cancellation.
    pub type JbCancelled = Option<unsafe extern "C" fn(context: *const c_void) -> bool>;
}
#[path = "../../../native/src/smart_erase.rs"]
mod smart_erase;
