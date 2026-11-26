//! C FFI 接口，用于从 C++ 调用 PP-OCR

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_uint};
use std::ptr;
use std::slice;
use crate::engine::{Engine, OcrLine};

/// PP-OCR 引擎实例（不透明指针）
pub struct JbPpocr {
    engine: Engine,
}

/// OCR 识别结果的一行（C 兼容）
#[repr(C)]
pub struct JbPpocrLine {
    pub text: *mut c_char,
    pub score: f32,
    pub box_x: *mut i32,
    pub box_y: *mut i32,
    pub box_points: u32,
}

/// 写入错误信息到缓冲区
fn write_error(error_buf: *mut c_char, buf_len: usize, msg: &str) {
    if error_buf.is_null() || buf_len == 0 {
        return;
    }
    let bytes = msg.as_bytes();
    let copy_len = bytes.len().min(buf_len - 1);
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), error_buf as *mut u8, copy_len);
        *error_buf.add(copy_len) = 0;
    }
}

/// 初始化 ONNX Runtime（动态加载模式）
#[no_mangle]
pub extern "C" fn jb_ppocr_init_runtime(
    runtime_path: *const c_char,
    error_buf: *mut c_char,
    buf_len: usize,
) -> bool {
    if runtime_path.is_null() {
        write_error(error_buf, buf_len, "Runtime path is null");
        return false;
    }
    
    let path_str = unsafe {
        match CStr::from_ptr(runtime_path).to_str() {
            Ok(s) => s,
            Err(_) => {
                write_error(error_buf, buf_len, "Invalid UTF-8 in runtime path");
                return false;
            }
        }
    };
    
    #[cfg(feature = "load-dynamic")]
    {
        match crate::engine::init_runtime(std::path::Path::new(path_str)) {
            Ok(_) => true,
            Err(e) => {
                write_error(error_buf, buf_len, &e);
                false
            }
        }
    }
    
    #[cfg(not(feature = "load-dynamic"))]
    {
        let _ = path_str;
        true  // 静态链接模式不需要加载
    }
}

/// 打开 PP-OCR 引擎
#[no_mangle]
pub extern "C" fn jb_ppocr_open(
    det_model: *const c_char,
    rec_model: *const c_char,
    dict_path: *const c_char,
    error_buf: *mut c_char,
    buf_len: usize,
) -> *mut JbPpocr {
    if det_model.is_null() || rec_model.is_null() {
        write_error(error_buf, buf_len, "Model path is null");
        return ptr::null_mut();
    }
    
    let det_str = unsafe {
        match CStr::from_ptr(det_model).to_str() {
            Ok(s) => s,
            Err(_) => {
                write_error(error_buf, buf_len, "Invalid UTF-8 in det model path");
                return ptr::null_mut();
            }
        }
    };
    
    let rec_str = unsafe {
        match CStr::from_ptr(rec_model).to_str() {
            Ok(s) => s,
            Err(_) => {
                write_error(error_buf, buf_len, "Invalid UTF-8 in rec model path");
                return ptr::null_mut();
            }
        }
    };
    
    let dict_str = if dict_path.is_null() {
        None
    } else {
        unsafe {
            match CStr::from_ptr(dict_path).to_str() {
                Ok(s) => Some(s),
                Err(_) => {
                    write_error(error_buf, buf_len, "Invalid UTF-8 in dict path");
                    return ptr::null_mut();
                }
            }
        }
    };
    
    let engine = match Engine::open_with_dict(det_str, rec_str, dict_str) {
        Ok(e) => e,
        Err(e) => {
            write_error(error_buf, buf_len, &e);
            return ptr::null_mut();
        }
    };
    
    Box::into_raw(Box::new(JbPpocr { engine }))
}

/// 释放引擎资源
#[no_mangle]
pub extern "C" fn jb_ppocr_free(engine: *mut JbPpocr) {
    if !engine.is_null() {
        unsafe {
            drop(Box::from_raw(engine));
        }
    }
}

/// 关闭引擎（同 jb_ppocr_free）
#[no_mangle]
pub extern "C" fn jb_ppocr_close(engine: *mut JbPpocr) {
    jb_ppocr_free(engine);
}

/// 识别图像中的文字
#[no_mangle]
pub extern "C" fn jb_ppocr_recognize(
    engine: *mut JbPpocr,
    rgb_data: *const u8,
    data_len: usize,
    width: c_uint,
    height: c_uint,
    stride: c_uint,
    cancel_cb: Option<extern "C" fn(*mut std::ffi::c_void) -> bool>,
    cancel_data: *mut std::ffi::c_void,
    out_lines: *mut *mut JbPpocrLine,
    out_count: *mut usize,
    error_buf: *mut c_char,
    buf_len: usize,
) -> bool {
    if engine.is_null() || rgb_data.is_null() || out_lines.is_null() || out_count.is_null() {
        write_error(error_buf, buf_len, "Null pointer argument");
        return false;
    }
    
    let engine = unsafe { &mut *engine };
    let rgb_slice = unsafe { slice::from_raw_parts(rgb_data, data_len) };
    
    // 包装取消回调
    let cancel_fn = || {
        if let Some(cb) = cancel_cb {
            cb(cancel_data)
        } else {
            false
        }
    };
    
    let lines = match engine.engine.recognize(
        rgb_slice,
        width as usize,
        height as usize,
        stride as usize,
        &cancel_fn,
    ) {
        Ok(Some(l)) => l,
        Ok(None) => {
            write_error(error_buf, buf_len, "Cancelled by user");
            return false;
        }
        Err(e) => {
            write_error(error_buf, buf_len, &e);
            return false;
        }
    };
    
    // 转换结果为 C 结构
    let mut c_lines = Vec::with_capacity(lines.len());
    for line in lines {
        let text_cstr = match CString::new(line.text) {
            Ok(s) => s,
            Err(_) => continue,
        };
        
        // box_pts 应该有 4 个点，每个点是 [x, y] 数组
        if line.box_pts.len() < 4 {
            continue;
        }
        
        let mut xs = Vec::with_capacity(4);
        let mut ys = Vec::with_capacity(4);
        for pt in &line.box_pts[0..4] {
            xs.push(pt[0] as i32);
            ys.push(pt[1] as i32);
        }
        
        c_lines.push(JbPpocrLine {
            text: text_cstr.into_raw(),
            score: line.score,
            box_x: xs.as_mut_ptr(),
            box_y: ys.as_mut_ptr(),
            box_points: 4,
        });
        
        std::mem::forget(xs);
        std::mem::forget(ys);
    }
    
    unsafe {
        *out_count = c_lines.len();
        *out_lines = c_lines.as_mut_ptr();
    }
    
    std::mem::forget(c_lines);
    
    true
}

/// 释放识别结果
#[no_mangle]
pub extern "C" fn jb_ppocr_lines_free(lines: *mut JbPpocrLine, count: usize) {
    if lines.is_null() || count == 0 {
        return;
    }
    
    let lines_vec = unsafe { Vec::from_raw_parts(lines, count, count) };
    
    for line in lines_vec {
        if !line.text.is_null() {
            unsafe {
                drop(CString::from_raw(line.text));
            }
        }
        if !line.box_x.is_null() {
            unsafe {
                drop(Vec::from_raw_parts(line.box_x, line.box_points as usize, line.box_points as usize));
            }
        }
        if !line.box_y.is_null() {
            unsafe {
                drop(Vec::from_raw_parts(line.box_y, line.box_points as usize, line.box_points as usize));
            }
        }
    }
}
