#include "vietnamese.h"

#include <cstdlib>
#include <cstring>

// bamboo-core FFI
#include "bamboo_ffi.h"

// Shared utility helpers
#include "viet_util.h"

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
      shortW_(other.shortW_),
      bracketUO_(other.bracketUO_),
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
        shortW_ = other.shortW_;
        bracketUO_ = other.bracketUO_;
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
    } else if (shortW_) {
        // Telex + "bare w → ư" option → bamboo telex_w variant
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

void VietnameseEngine::setShortW(bool enabled) {
    if (shortW_ == enabled) return;
    shortW_ = enabled;
    // Re-apply method so bamboo switches between telex() and telex_w().
    setMethod(method_);
}

void VietnameseEngine::setBracketUO(bool enabled) {
    bracketUO_ = enabled;
}

// ---------------------------------------------------------------------------
// Input processing
// ---------------------------------------------------------------------------

bool VietnameseEngine::isCompositionKey(char ch) const {
    bool isLetter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    bool isDigit = (ch >= '0' && ch <= '9');
    bool bracketActive = bracketUO_ && method_ == InputMethod::Telex &&
                         (ch == '[' || ch == ']');

    if (isLetter || bracketActive) return true;
    if (method_ == InputMethod::VNI && isDigit && !rawInput_.empty()) return true;
    return false;
}

ProcessResult VietnameseEngine::processKey(char ch) {
    if (!isCompositionKey(ch)) return ProcessResult::Ignored;       // P1

    std::string oldComposed = composed_;
    std::string oldRawInput = rawInput_;
    rawInput_ += ch;

    if (processEnglishBypass()) return ProcessResult::Consumed;     // P2

    recompose();                                                     // P3

    if (tryUndoTransform(ch, oldComposed, oldRawInput))             // P4
        return ProcessResult::Committed;

    if (tryToneKeyUndo(ch, oldComposed, oldRawInput))               // P5
        return ProcessResult::Committed;

    applyFallbackPairSubstitution();                                 // P6
    maybeAutoRestoreRealTime();                                      // R5 — after P6 so is_valid() sees final state

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

bool VietnameseEngine::isValid() const {
    return skey_engine_is_valid(handle_) != 0;
}

} // namespace skey
