#include "vietnamese.h"

#include <cstdlib>
#include <cstring>

// skey-engine FFI
#include "skey_engine.h"

// Shared utility helpers
#include "viet_util.h"

namespace skey {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

VietnameseEngine::VietnameseEngine() {
    handle_ = skey_engine_new(SKEY_METHOD_TELEX);
}

VietnameseEngine::~VietnameseEngine() {
    if (handle_) {
        skey_engine_free(handle_);
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
            skey_engine_free(handle_);
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
    int32_t m = (method == InputMethod::VNI) ? SKEY_METHOD_VNI : SKEY_METHOD_TELEX;
    skey_engine_set_method(handle_, m);
}

void VietnameseEngine::setToneStyle(ToneStyle style) {
    toneStyle_ = style;
    skey_engine_set_tone_style(handle_, style == ToneStyle::Modern ? 1 : 0);
}

void VietnameseEngine::setFreeMarking(bool free) {
    freeMarking_ = free;
    skey_engine_set_free_marking(handle_, free ? 1 : 0);
}

void VietnameseEngine::setAutoRestore(bool restore) {
    autoRestore_ = restore;
}

void VietnameseEngine::setShortW(bool enabled) {
    shortW_ = enabled;
    skey_engine_set_short_w(handle_, enabled ? 1 : 0);
}

void VietnameseEngine::setBracketUO(bool enabled) {
    bracketUO_ = enabled;
    skey_engine_set_bracket_uo(handle_, enabled ? 1 : 0);
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
    if (!isCompositionKey(ch)) return ProcessResult::Ignored;

    std::string oldComposed = composed_;
    std::string oldRawInput = rawInput_;
    rawInput_ += ch;

    if (processEnglishBypass()) return ProcessResult::Consumed;

    recompose();

    if (tryUndoTransform(ch, oldComposed, oldRawInput))
        return ProcessResult::Committed;

    if (tryToneKeyUndo(ch, oldComposed, oldRawInput))
        return ProcessResult::Committed;

    // R5: auto-restore runs after recompose.
    // Pair substitution is now built into the engine, so is_valid()
    // sees the final composed_ state directly.
    maybeAutoRestoreRealTime();

    return ProcessResult::Consumed;
}

void VietnameseEngine::backspace() {
    if (rawInput_.empty()) return;

    rawInput_.pop_back();
    if (rawInput_.empty()) {
        composed_.clear();
    } else {
        recompose();
    }
}

void VietnameseEngine::reset() {
    rawInput_.clear();
    composed_.clear();
    committed_.clear();
    englishBypass_ = false;
}

std::string VietnameseEngine::getComposed() const {
    return composed_;
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

bool VietnameseEngine::isValid() const {
    return skey_engine_is_valid(composed_.c_str()) != 0;
}

} // namespace skey
