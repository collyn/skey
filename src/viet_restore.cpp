/**
 * viet_restore.cpp — Auto-restore logic for VietnameseEngine.
 *
 * Auto-restore is now handled directly by the Rust engine via
 * skey_engine_set_auto_restore().  The engine checks whether the
 * composed output is valid Vietnamese and, if not, returns the raw
 * input instead.
 *
 * The functions below are kept as no-ops for API compatibility.
 */

#include "vietnamese.h"

namespace skey {

bool VietnameseEngine::shouldRestoreToRaw() const {
    return false; // engine handles this now
}

void VietnameseEngine::maybeAutoRestoreRealTime() {
    // no-op — engine handles this
}

void VietnameseEngine::autoRestore() {
    // no-op — engine handles this
}

} // namespace skey
