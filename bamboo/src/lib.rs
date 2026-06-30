//! Thin FFI wrapper around bamboo-core for skey C++ integration.
//!
//! Re-exports bamboo-core's built-in instance-based FFI functions and adds
//! a few missing ones needed by skey (reset, get_output, set_method per instance).

use std::ffi::CString;
use std::os::raw::c_char;

// Re-export all bamboo-core FFI symbols so they appear in our static lib.
pub use bamboo_core::ffi::*;

use bamboo_core::{Engine, InputMethod, Mode};

// The bamboo-core FFI already provides:
//   bamboo_engine_new(method: i32) -> *mut Engine
//   bamboo_engine_free(engine: *mut Engine)
//   bamboo_engine_process(engine: *mut Engine, key: u32) -> *mut c_char
//   bamboo_engine_process_key_buf(...)
//   bamboo_free_string(s: *mut c_char)
//   bamboo_remove_last_char() -- global only
//
// Below we add instance-level functions that skey needs.

/// Reset an engine instance, clearing all composition state.
///
/// # Safety
/// `engine` must be a valid pointer from `bamboo_engine_new`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn skey_engine_reset(engine: *mut Engine) {
    if !engine.is_null() {
        let e = unsafe { &mut *engine };
        e.reset();
    }
}

/// Get the current composed output from an engine instance.
/// Returns a pointer to a null-terminated UTF-8 C string.
/// The caller must free the string with `bamboo_free_string`.
///
/// # Safety
/// `engine` must be a valid pointer from `bamboo_engine_new`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn skey_engine_get_output(engine: *mut Engine) -> *mut c_char {
    if engine.is_null() {
        return std::ptr::null_mut();
    }
    let e = unsafe { &mut *engine };
    let out = e.output();
    CString::new(out).unwrap_or_default().into_raw()
}

/// Remove the last character (backspace) from an engine instance.
///
/// # Safety
/// `engine` must be a valid pointer from `bamboo_engine_new`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn skey_engine_remove_last_char(engine: *mut Engine) {
    if !engine.is_null() {
        let e = unsafe { &mut *engine };
        e.remove_last_char(true);
    }
}

/// Set the input method for a specific engine instance.
/// `method`: 0=Telex, 1=VNI, 2=VIQR, 3=Microsoft, 4=Telex2, 5=TelexW.
/// This recreates the engine internally with the new method.
///
/// # Safety
/// `engine` must be a valid pointer from `bamboo_engine_new`.
/// After this call, the old pointer is still valid (engine is reset in-place).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn skey_engine_set_method(engine: *mut Engine, method: i32) {
    if engine.is_null() {
        return;
    }
    let im = match method {
        0 => InputMethod::telex(),
        1 => InputMethod::vni(),
        2 => InputMethod::viqr(),
        3 => InputMethod::microsoft_layout(),
        4 => InputMethod::telex_2(),
        5 => InputMethod::telex_w(),
        _ => InputMethod::telex(),
    };
    // Replace the engine in-place
    let e = unsafe { &mut *engine };
    *e = Engine::new(im);
}

/// Check if the engine has any active composition (non-empty output).
///
/// # Safety
/// `engine` must be a valid pointer from `bamboo_engine_new`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn skey_engine_is_active(engine: *mut Engine) -> i32 {
    if engine.is_null() {
        return 0;
    }
    let e = unsafe { &*engine };
    if e.output().is_empty() { 0 } else { 1 }
}

/// Process a full raw input string on an engine instance.
/// Resets the engine first, then processes each character.
/// Returns a pointer to a null-terminated UTF-8 string with the composed output.
/// The caller must free the string with `bamboo_free_string`.
///
/// # Safety
/// `engine` must be a valid pointer from `bamboo_engine_new`.
/// `raw_input` must be a valid null-terminated UTF-8 C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn skey_engine_process_string(
    engine: *mut Engine,
    raw_input: *const c_char,
) -> *mut c_char {
    if engine.is_null() || raw_input.is_null() {
        return std::ptr::null_mut();
    }
    let e = unsafe { &mut *engine };
    let input = match unsafe { std::ffi::CStr::from_ptr(raw_input) }.to_str() {
        Ok(s) => s,
        Err(_) => return std::ptr::null_mut(),
    };

    e.reset();
    let output = e.process(input, Mode::Vietnamese);
    CString::new(output).unwrap_or_default().into_raw()
}
