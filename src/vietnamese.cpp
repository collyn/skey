#include "vietnamese.h"

#include <cstdlib>
#include <cstring>

// bamboo-core FFI
#include "bamboo_ffi.h"

namespace skey {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

VietnameseEngine::VietnameseEngine() {
    handle_ = bamboo_engine_new(BAMBOO_METHOD_TELEX);
}

VietnameseEngine::~VietnameseEngine() {
    if (handle_) {
        bamboo_engine_free(handle_);
        handle_ = nullptr;
    }
}

VietnameseEngine::VietnameseEngine(VietnameseEngine &&other) noexcept
    : handle_(other.handle_),
      method_(other.method_),
      toneStyle_(other.toneStyle_),
      freeMarking_(other.freeMarking_),
      autoRestore_(other.autoRestore_),
      rawInput_(std::move(other.rawInput_)),
      composed_(std::move(other.composed_)),
      englishBypass_(other.englishBypass_),
      committed_(std::move(other.committed_)) {
    other.handle_ = nullptr;
    other.englishBypass_ = false;
}

VietnameseEngine &VietnameseEngine::operator=(VietnameseEngine &&other) noexcept {
    if (this != &other) {
        if (handle_) {
            bamboo_engine_free(handle_);
        }
        handle_ = other.handle_;
        method_ = other.method_;
        toneStyle_ = other.toneStyle_;
        freeMarking_ = other.freeMarking_;
        autoRestore_ = other.autoRestore_;
        rawInput_ = std::move(other.rawInput_);
        composed_ = std::move(other.composed_);
        englishBypass_ = other.englishBypass_;
        committed_ = std::move(other.committed_);
        other.handle_ = nullptr;
        other.englishBypass_ = false;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void VietnameseEngine::setMethod(InputMethod method) {
    method_ = method;
    int32_t m;
    if (method == InputMethod::VNI) {
        m = BAMBOO_METHOD_VNI;
    } else if (method == InputMethod::TelexW) {
        m = BAMBOO_METHOD_TELEXW;
    } else {
        m = BAMBOO_METHOD_TELEX;
    }
    skey_engine_set_method(handle_, m);
}

void VietnameseEngine::setToneStyle(ToneStyle style) {
    toneStyle_ = style;
    // Modern = "hòa" (std_tone_style=true), Traditional = "hoà" (std_tone_style=false)
    skey_engine_set_std_tone_style(handle_, style == ToneStyle::Modern ? 1 : 0);
}

void VietnameseEngine::setFreeMarking(bool free) {
    freeMarking_ = free;
    // bamboo-core's free_tone_marking=true means "enable smart tone relocation"
    // (the engine auto-moves tone marks to standard position).
    // User's "Đánh dấu tự do" = true means "let me place tone freely" →
    // so we INVERT: user free=true → bamboo free_tone_marking=false.
    skey_engine_set_free_marking(handle_, free ? 0 : 1);
}

void VietnameseEngine::setAutoRestore(bool restore) {
    autoRestore_ = restore;
}

// ---------------------------------------------------------------------------
// Input processing
// ---------------------------------------------------------------------------

ProcessResult VietnameseEngine::processKey(char ch) {
    // Check if this is a letter that can start or continue composition
    bool isLetter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    bool isDigit = (ch >= '0' && ch <= '9');

    if (!isLetter && !(method_ == InputMethod::VNI && isDigit && !rawInput_.empty())) {
        return ProcessResult::Ignored;
    }

    std::string oldComposed = composed_;
    std::string oldRawInput = rawInput_;
    rawInput_ += ch;

    // English bypass: after an undo was detected, skip Vietnamese
    // processing for the remainder of the current word.  Just append
    // the raw character so the caller sees a simple ASCII append.
    if (englishBypass_) {
        composed_ = rawInput_;
        return ProcessResult::Consumed;
    }

    recompose();

    // Detect undo: bamboo-core cancelled the transformation.
    // Before adding this key, there was an active transformation
    // (oldComposed != oldRawInput, e.g. "ư" != "w", or "đ" != "dd").
    // After adding this key, the transformation is gone
    // (composed_ is pure ASCII — no Vietnamese chars remain).
    //
    // When undo is detected, whether the undo trigger key was consumed
    // depends on the transformation:
    //   - Multi-char transforms (oo→ô, dd→đ): trigger consumed.
    //     composed_ is shorter than rawInput_. Commit all.
    //     ooo → "oo" (2<3) → commit "oo"
    //     ddd → "dd" (2<3) → commit "dd"
    //   - Single-char transforms (w→ư in TelexW): trigger NOT consumed.
    //     composed_ equals rawInput_. Strip last char (the trigger).
    //     ww  → "ww" (2=2) → commit "w"
    if (rawInput_.size() > 1 && oldComposed != oldRawInput) {
        bool newIsAllAscii = true;
        for (unsigned char c : composed_) {
            if (c > 127) { newIsAllAscii = false; break; }
        }
        if (newIsAllAscii) {
            if (composed_.size() < rawInput_.size()) {
                // Trigger consumed by bamboo — commit all
                committed_ += composed_;
            } else if (composed_.size() > 0) {
                // Trigger still present at end — strip it
                committed_ += composed_.substr(0, composed_.size() - 1);
            }

            // Clear composition entirely — undo key consumed
            rawInput_.clear();
            composed_.clear();
            skey_engine_reset(handle_);

            // Enter English bypass mode: subsequent keys in this word
            // will be forwarded as raw ASCII without Vietnamese processing.
            englishBypass_ = true;

            return ProcessResult::Committed;
        }
    }

    return ProcessResult::Consumed;
}

void VietnameseEngine::backspace() {
    if (rawInput_.empty()) return;

    rawInput_.pop_back();
    if (rawInput_.empty()) {
        composed_.clear();
        skey_engine_reset(handle_);
    } else {
        recompose();
    }
}

void VietnameseEngine::reset() {
    rawInput_.clear();
    composed_.clear();
    committed_.clear();
    englishBypass_ = false;
    skey_engine_reset(handle_);
}

std::string VietnameseEngine::getComposed() const {
    return composed_;
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

void VietnameseEngine::recompose() {
    char *result = skey_engine_process_string(handle_, rawInput_.c_str());
    if (result) {
        composed_ = result;
        bamboo_free_string(result);
    } else {
        composed_ = rawInput_;
    }

    // Real-time auto-restore (à la Lotus autoNonVnRestore + ddFreeStyle).
    // If composed differs from raw AND is not valid Vietnamese:
    //   - If it contains Vietnamese vowels/tone marks → restore to raw
    //   - If it only contains đ/Đ (no vowels) → keep (abbreviations like đc)
    if (autoRestore_ &&
        composed_ != rawInput_ && !rawInput_.empty() &&
        skey_engine_is_valid(handle_) == 0) {

        // Check if composed has any non-ASCII char that isn't đ (U+0111) / Đ (U+0110).
        // đ = UTF-8 C4 91, Đ = UTF-8 C4 90.
        // Any other non-ASCII = Vietnamese vowel/tone mark → should restore.
        bool hasVietnameseVowel = false;
        for (size_t i = 0; i < composed_.size(); ) {
            unsigned char c = composed_[i];
            if (c <= 127) { i++; continue; }

            // Determine UTF-8 sequence length
            int len = 1;
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;

            // Check for đ (C4 91) or Đ (C4 90)
            if (len == 2 && i + 1 < composed_.size() &&
                composed_[i] == '\xC4' &&
                (composed_[i + 1] == '\x91' || composed_[i + 1] == '\x90')) {
                i += len;
                continue;  // It's đ/Đ — skip
            }

            // Any other non-ASCII char = Vietnamese vowel/tone
            hasVietnameseVowel = true;
            break;
        }

        if (hasVietnameseVowel) {
            composed_ = rawInput_;
        }
        // else: only đ/Đ present (ddFreeStyle) → keep composed_
    }
}

void VietnameseEngine::autoRestore() {
    if (!autoRestore_) return;
    if (rawInput_.empty() || composed_ == rawInput_) return;

    if (skey_engine_is_valid(handle_) == 0) {
        composed_ = rawInput_;
    }
}

} // namespace skey
