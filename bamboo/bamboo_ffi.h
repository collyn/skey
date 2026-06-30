/**
 * bamboo_ffi.h — C header for bamboo-core Rust FFI layer.
 *
 * Instance-based API for skey per-InputContext engine instances.
 * All functions are thread-safe for distinct engine handles.
 */

#ifndef SKEY_BAMBOO_FFI_H
#define SKEY_BAMBOO_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque engine handle */
typedef void BambooEngine;

/* Input method constants */
#define BAMBOO_METHOD_TELEX   0
#define BAMBOO_METHOD_VNI     1
#define BAMBOO_METHOD_VIQR    2
#define BAMBOO_METHOD_MS      3
#define BAMBOO_METHOD_TELEX2  4
#define BAMBOO_METHOD_TELEXW  5

/* ── Instance lifecycle ─────────────────────────────────────────────── */

/**
 * Create a new engine instance.
 * @param method  Input method (BAMBOO_METHOD_*).
 * @return Opaque handle. Must be freed with bamboo_engine_free().
 */
BambooEngine *bamboo_engine_new(int32_t method);

/**
 * Free an engine instance.
 */
void bamboo_engine_free(BambooEngine *engine);

/* ── Per-key processing ─────────────────────────────────────────────── */

/**
 * Process a single Unicode key on an engine instance.
 * @param engine  Engine handle.
 * @param key     Unicode code point (e.g., 'a' = 0x61).
 * @return Null-terminated UTF-8 string with current word output.
 *         Must be freed with bamboo_free_string().
 */
char *bamboo_engine_process(BambooEngine *engine, uint32_t key);

/**
 * Process a key and get delta output (for efficient preedit updates).
 * @param engine           Engine handle.
 * @param key              Unicode code point.
 * @param is_vietnamese    Non-zero for Vietnamese mode.
 * @param out_buf          Buffer for inserted bytes.
 * @param out_cap          Capacity of out_buf.
 * @param out_len          [out] Actual length of inserted bytes.
 * @param backspaces_chars [out] Number of chars to delete from previous preedit.
 * @param backspaces_bytes [out] Number of bytes to delete.
 * @return 0 on success, 1 if buffer too small, -1 on null pointer, -2 on null engine.
 */
int32_t bamboo_engine_process_key_buf(
    BambooEngine *engine,
    uint32_t key,
    int32_t is_vietnamese,
    uint8_t *out_buf,
    size_t out_cap,
    size_t *out_len,
    size_t *backspaces_chars,
    size_t *backspaces_bytes
);

/* ── skey extension functions ───────────────────────────────────────── */

/**
 * Reset engine state (clear all composition).
 */
void skey_engine_reset(BambooEngine *engine);

/**
 * Get the current composed output.
 * @return Null-terminated UTF-8 string. Must be freed with bamboo_free_string().
 */
char *skey_engine_get_output(BambooEngine *engine);

/**
 * Remove the last character from composition (backspace).
 */
void skey_engine_remove_last_char(BambooEngine *engine);

/**
 * Set the input method for this engine instance.
 * @param method  Input method (BAMBOO_METHOD_*).
 */
void skey_engine_set_method(BambooEngine *engine, int32_t method);

/**
 * Check if the engine has any active composition.
 * @return 1 if active, 0 if empty.
 */
int32_t skey_engine_is_active(BambooEngine *engine);

/**
 * Process a full raw input string (resets first, then processes each char).
 * @param raw_input  Null-terminated UTF-8 string.
 * @return Null-terminated UTF-8 string with composed output.
 *         Must be freed with bamboo_free_string().
 */
char *skey_engine_process_string(BambooEngine *engine, const char *raw_input);

/**
 * Check if the current composition forms a valid Vietnamese syllable.
 * @return 1 if valid, 0 if invalid.
 */
int32_t skey_engine_is_valid(BambooEngine *engine);

/**
 * Restore the last word to its un-transformed (raw) state.
 * @param to_vietnamese  Non-zero to re-apply Vietnamese transformations.
 */
void skey_engine_restore_last_word(BambooEngine *engine, int32_t to_vietnamese);

/* ── Memory management ──────────────────────────────────────────────── */

/**
 * Free a string returned by any bamboo/skey FFI function.
 */
void bamboo_free_string(char *s);

#ifdef __cplusplus
}
#endif

#endif /* SKEY_BAMBOO_FFI_H */
