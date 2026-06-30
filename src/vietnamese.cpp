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
      rawInput_(std::move(other.rawInput_)),
      composed_(std::move(other.composed_)),
      committed_(std::move(other.committed_)) {
    other.handle_ = nullptr;
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
        rawInput_ = std::move(other.rawInput_);
        composed_ = std::move(other.composed_);
        committed_ = std::move(other.committed_);
        other.handle_ = nullptr;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void VietnameseEngine::setMethod(InputMethod method) {
    method_ = method;
    int32_t m = (method == InputMethod::VNI) ? BAMBOO_METHOD_VNI
                                              : BAMBOO_METHOD_TELEX;
    skey_engine_set_method(handle_, m);
}

void VietnameseEngine::setToneStyle(ToneStyle style) {
    toneStyle_ = style;
    // bamboo-core handles tone placement internally.
    // TODO: Map to bamboo-core Config if exposed in future versions.
}

void VietnameseEngine::setFreeMarking(bool free) {
    freeMarking_ = free;
    // bamboo-core handles free marking via its Config.
    // TODO: Map to bamboo-core Config if exposed in future versions.
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

    // If rawInput_ already has content and we're starting what looks like
    // a new word, commit the old one first.
    // bamboo-core handles multi-syllable internally but skey expects
    // single-syllable composition, so we detect word boundaries.
    // A new word starts when adding a consonant after the previous syllable
    // ended. We let bamboo-core handle this by checking if the output changes.

    std::string oldComposed = composed_;
    rawInput_ += ch;
    recompose();

    if (composed_ != oldComposed) {
        return ProcessResult::Consumed;
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
}

} // namespace skey
