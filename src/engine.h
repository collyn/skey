#ifndef FCITX5_SKEY_ENGINE_H
#define FCITX5_SKEY_ENGINE_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcitx-utils/event.h>
#include <fcitx-utils/i18n.h>
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>
#include <fcitx/menu.h>

#include "config.h"
#include "charset.h"
#include "vietnamese.h"
#include "a11y_monitor.h"

namespace fcitx {

class SKeyEngine;

// Timing tunables for uinput backspace → commit coordination.
// Split into X11 / Wayland variants because Wayland omits the X server
// round-trip, yielding lower and more predictable latency.
struct UinputTiming;

/// Per-InputContext state: each window/app gets its own composition state.
class SKeyState : public InputContextProperty {
public:
    SKeyState(SKeyEngine *engine, InputContext *ic);
    ~SKeyState() override = default;

    void keyEvent(KeyEvent &keyEvent);
    void activate();
    void deactivate();
    void reset();

    // Mode switch menu (called from ModeCandidateWord)
    void dismissModeMenu();

private:
    friend class ModeCandidateWord;
    friend class ExcludeCandidateWord;
    friend class AddressBarModeCandidateWord;
    SKeyOutputMode effectiveMode() const;
    bool inChromiumAddressBar() const;
    bool isAutofillCertain() const;
    bool useSurroundingText() const;
    bool canEditWithSurroundingText() const;
    bool useNativeSurroundingApi() const;
    bool isWayland() const;
    const struct UinputTiming& uinputTiming() const;
    bool useHiddenComposition() const;
    bool useUinputMode() const;
    bool isChromiumCached() const;
    /// Per-app identity key.  The IBus frontend reports an empty program
    /// name for apps that only speak the ibus protocol (AppImages, some
    /// Electron apps) — resolve the real name from the focused X11
    /// window's WM_CLASS so per-app mode config doesn't collide under one
    /// shared "(IBus app)" key.  Resolution is attempted once per focus.
    const std::string &appProgram() const;
    /// True when the AT-SPI2 monitor has a fresh focus snapshot of a
    /// text-entry node inside a web document (Facebook chat, comments, web
    /// forms).  Chrome fires that focus event immediately on click, while
    /// its content-type caps may lag behind — used to upgrade bare-caps
    /// Uinput to SurroundingText.
    bool a11yFreshWebEditor() const;
    SKeyOutputMode detectAutoMode() const;
    bool connectUinputServer();
    void sendBackspaceUinput(int count, uint32_t flags = 0);
    bool handlePendingUinputBackspace(KeyEvent &keyEvent);
    void replayBufferedUinputKeys();
    void commitBuffer();
    void surroundingCommit(const std::string &oldComposed,
                           const std::string &newComposed);
    void surroundingBackspace();
    /// Arm the uinput safety timer.  If BS loopbacks are still outstanding
    /// when it fires, the window is extended once (slow apps) instead of
    /// force-committing early — an early commit plus late BS deletions
    /// corrupts the text on screen.
    void armUinputSafetyTimer();
    /// Load the user dictionary file (~/.local/share/fcitx5/skey/user-dict.txt,
    /// one word per line, # comments) into the engine.  Called when the
    /// dictionary option is applied.
    void loadUserDict();
    void reclaimLastWord();
    bool hasDeferredCommitPending() const;
    void scheduleDeferredCommit(const std::string &text,
                                const std::string &stablePrefix = "");
    void flushDeferredCommit();
    void forceFlushDeferredCommit();
    void updateDeferredPreedit();
    void forwardUtf8AsKeys(const std::string &text);
    void updatePreedit();
    void clearUI();
    void showModeMenu();
    void refreshAppMode();
    void saveLastWord();
    void clearLastWord();
    void flushAddrBarReplacement();
    void scheduleAddrBarReplacement(int bs, const std::string &text,
                                     int oldComposedLen = 0,
                                     int triggerKeySym = 0,
                                     const std::string &fullComposed = {},
                                     bool oldComposedIsAscii = false,
                                     const std::string &oldComposed = {});

    SKeyEngine *engine_;
    InputContext *ic_;
    skey::VietnameseEngine viet_;
    skey::Charset charset_ = skey::Charset::Unicode;
    int committedLen_ = 0;

    /// Commit text to the app, converting to the configured charset.
    void commitText(const std::string &utf8);
    void commitText(const std::string &utf8, const std::string &fallbackCharset);
    bool modeMenuActive_ = false;
    bool modeMenuForAddressBar_ = false;
    bool hasAppModeOverride_ = false;
    bool appExcluded_ = false;
    SKeyOutputMode appModeOverride_ = SKeyOutputMode::SurroundingText;
    std::string cachedProgram_{"\x01"};  // sentinel ≠ any real program name, incl. empty
    // X11 WM_CLASS resolution for empty-program (IBus frontend) apps.
    // Cleared on each activate(); see appProgram().
    mutable bool appNameAttempted_ = false;
    mutable std::string resolvedProgram_;
    mutable bool modeCacheValid_ = false;
    mutable SKeyOutputMode cachedMode_ = SKeyOutputMode::SurroundingText;
    mutable int cachedIsChromium_ = -1;  // tristate: -1=unset, 0=false, 1=true
    mutable int cachedIsFirefoxOrSnap_ = -1;
    // Sticky Uinput for Chromium-family apps that initially report bare caps
    // (0x72, no content hints).  Caps may later gain hints after the app
    // enters edit mode, but commitString still won't work without Uinput.
    mutable bool chromiumBareCapsUinput_ = false;
    // SurroundingText capability was advertised but the runtime cache is
    // invalid — the app never reports surrounding text (LibreOffice,
    // Telegram...).  The per-replacement fallback cannot be verified, so
    // downgrade the session to Uinput (re-checked on each focus).
    mutable bool surroundingTextFailed_ = false;
    // Deferred mode decision: bare Chromium caps may be a stale
    // window-focus state — Chrome only re-syncs caps on text-input re-entry
    // (an IC focus cycle), not when focus moves within the page (Facebook
    // chat: 0x72 at window focus, 0x90072 after re-entry).  While pending,
    // detectAutoMode() is re-evaluated at word boundaries; strong hints or
    // the AT-SPI2 web-editor signal upgrade to SurroundingText, otherwise
    // Uinput locks in at the deadline.
    mutable bool modeDecisionPending_ = false;
    mutable uint64_t modeDecisionDeadlineUsec_ = 0;
    bool isFirefoxOrSnap() const;
    std::unique_ptr<EventSourceTime> deferredCommitTimer_;
    std::string deferredCommitText_;
    std::string deferredPrefix_;
    uint64_t deferredBsSentAt_ = 0;
    std::string pendingFlushSuffix_;
    int uinputClientFd_ = -1;
    // Uinput replacement state
    bool uinputDeleting_ = false;
    std::unique_ptr<EventSourceTime> uinputCommitTimer_;
    std::unique_ptr<EventSourceTime> uinputSafetyTimer_;
    int expectedUinputBackspaces_ = 0;
    int seenUinputBackspaces_ = 0;
    int uinputPendingFinalLen_ = 0; // expected committedLen_ after BS+commit
    // BS we injected via uinput but have not yet seen loop back through
    // fcitx5.  Late loopbacks (beyond the safety window) are swallowed
    // instead of being mistaken for fresh user backspaces.
    int uinputBsOutstanding_ = 0;
    // Safety window already extended once — a second timeout force-commits.
    bool uinputSafetyRetried_ = false;
    // Address bar deferred replacement state
    int addrBarPendingBs_ = 0;
    std::string addrBarPendingText_;
    // Spurious Deactivate/Reset/Activate detection for Chromium
    // address bar. Set before sending forwardKey/commitString and
    // cleared after a reactivate or 200ms timeout.
    bool addrBarExpectCycle_ = false;
    // Set before forwarding a raw key in Uinput mode for Firefox/Snap
    // apps.  fcitx5 calls reset() after unfiltered keys, which clears
    // viet_ state.  When set, reset()/deactivate() skip viet_ cleanup
    // to preserve ongoing composition.  Only used for Firefox/Snap
    // (non-Chromium) apps — Chromium/Electron need clearUI() D-Bus
    // and are not affected by this guard.
    bool uinputKeyForwarded_ = false;
    // KeySym of the key that triggered the current address bar replacement.
    // X11 may re-deliver this key after Chrome's spurious focus cycles;
    // we drop it within a 200ms window to avoid double-processing.
    int addrBarLastTriggerKey_ = 0;
    uint64_t addrBarTriggerDeadline_ = 0;  // CLOCK_MONOTONIC deadline
    // True when the next replacement is for the first word after focus or
    // after backspacing to empty.  Only the first word may trigger Chrome
    // autocomplete; subsequent words (after space) don't need extra BS.
    bool addrBarIsFirstWord_ = false;
    // True when a space has been typed since activation in the address bar.
    // Prevents re-arming addrBarIsFirstWord_ after backspacing a non-first
    // word to empty — without this guard the fullReplace logic would send
    // extra BS that deletes text before the cursor.
    bool addrBarHadSpace_ = false;
    std::unique_ptr<EventSourceTime> addrBarCycleTimer_;
    // CLOCK_MONOTONIC timestamp of the most recent deactivate().
    // Used in activate() to detect spurious focus cycles that arrive
    // when addrBarExpectCycle_ was not armed — if reactivation happens
    // within 500ms in the same address bar, we preserve first-word/space
    // tracking to prevent fullReplace from deleting text before cursor.
    uint64_t lastDeactivateTime_ = 0;
    // Spurious-cycle detection: when preeditWasPending_ is true and
    // the next activate is for the same IC+program, the app auto-committed
    // on focus loss (e.g., LibreOffice) — skip the engine fallback commit.
    bool preeditWasPending_ = false;
    std::string preeditPendingProgram_;
    std::string pendingUinputCommit_;
    std::vector<KeySym> bufferedUinputKeys_;
    uint64_t bsSentAt_ = 0;        // timestamp when BS was sent
    uint64_t lastBsRoundTrip_ = 0; // last measured round-trip (usec)
    // EWMA of BS round-trip times for adaptive commit delay (usec)
    uint64_t bsRtEwma_ = 10000;    // seeded with kBsRtInitialUsec
    // Retroactive tone editing (Unikey-style): saved state of last committed word
    std::string lastRawInput_;      // Raw input of last committed word
    std::string lastComposed_;      // Composed text of last committed word
    int lastCommittedLen_ = 0;      // UTF-8 char count of last committed word
    bool reclaimReady_ = false;     // True after BS pressed while idle
    bool sepAlreadyDeleted_ = false; // Separator already deleted by first BS
    bool wordWasBackspaced_ = false; // Word deleted by backspace, block reclaim
    bool addrBarDidFullReplace_ = false; // FullReplace done, reset engine on commit
    bool addrBarHadFirstWord_ = false;  // First word already done, block fullReplace
    bool addrBarKeepState_ = false;     // Keep-state active, reset engine on BS
    int addrBarPrevCommittedLen_ = 0;  // committedLen_ snapshot before the
                                        // current replacement.  Used to check
                                        // if text existed on screen.
                                        // <=0 = bar was empty → FullReplace
                                        // safe even when addrBarHadSpace_ set.
    // Genuine cross-app focus change: the omnibox content is no longer
    // tracked (it almost always holds the page URL).  Blocks first-word
    // FullReplace unless the caret jumped far left since the word started
    // — proof the typed word replaced a selection (e.g. Ctrl+L), leaving
    // nothing before the cursor.
    bool addrBarContentUnknown_ = false;
    // Caret X (cursorRect().left()) at the first forwarded key of the
    // current word.  Rect updates lag one key, which is fine — it still
    // reflects the word-start region.  -1 = not recorded.
    int addrBarWordStartCaretX_ = -1;
    // True when the user pressed BackSpace since the current word began.
    // Gates the a11y desync guard — only backspace-driven edits can
    // desync the engine from the screen, so the guard must not run
    // (and risk a stale-snapshot false reset) during normal typing.
    bool addrBarSawBsInWord_ = false;
    // True while processing a reclaimed-word replacement — forces
    // surroundingCommit to use forwardKey instead of the native
    // surrounding-text API, which may be out-of-sync after reclaim.
    bool reclaimInProgress_ = false;
};

/// Main fcitx5 engine class.
class SKeyEngine : public InputMethodEngineV2 {
public:
    SKeyEngine(Instance *instance);
    ~SKeyEngine() override = default;

    void keyEvent(const InputMethodEntry &entry,
                  KeyEvent &keyEvent) override;
    void activate(const InputMethodEntry &entry,
                  InputContextEvent &event) override;
    void deactivate(const InputMethodEntry &entry,
                    InputContextEvent &event) override;
    void reset(const InputMethodEntry &entry,
               InputContextEvent &event) override;
    void save() override;

    const Configuration *getConfig() const override;
    void setConfig(const RawConfig &config) override;
    const Configuration *getSubConfig(const std::string &path) const override;
    void setSubConfig(const std::string &path, const RawConfig &config) override;
    void reloadConfig() override;

    std::string subMode(const InputMethodEntry &entry,
                        InputContext &ic) override;
    std::string subModeIconImpl(const InputMethodEntry &entry,
                                InputContext &ic) override;

    const SKeyConfig &config() const { return config_; }
    Instance *instance() { return instance_; }
    void setOutputMode(SKeyOutputMode mode);
    void setChromiumAddressBarMode(SKeyChromiumAddressBarMode mode);
    void setInputMethod(SKeyInputMethod method);
    void saveAppMode(const std::string &app, SKeyOutputMode mode);
    SKeyOutputMode loadAppMode(const std::string &app) const;
    void saveAppExcluded(const std::string &app, bool excluded);
    bool isAppExcluded(const std::string &app) const;
    void updateMenuActions();
    A11yMonitor *a11yMonitor() const { return a11yMonitor_.get(); }
    const Key &modeMenuKey() const { return modeMenuKey_; }

    /// Look up a macro expansion by shortcut key.
    /// Returns empty string if no match.
    std::string lookupMacro(const std::string &key) const;

private:
    void setupTrayMenu();

    Instance *instance_;
    SKeyConfig config_;
    FactoryFor<SKeyState> factory_;

    friend class SKeyState;
    // Pending preedit text saved on focus loss, keyed by program name.
    // Survives IC destruction — committed when the program is reactivated.
    std::map<std::string, std::string> pendingPreedits_;

    // Tray menu: Input Method selector
    SimpleAction imAction_;
    Menu imMenu_;
    SimpleAction imTelex_;
    SimpleAction imVni_;

    // Tray menu: Output Mode selector
    SimpleAction omAction_;
    Menu omMenu_;
    SimpleAction omSurrounding_;
    SimpleAction omPreedit_;
    SimpleAction omUinput_;
    SimpleAction omAuto_;

    // Tray menu: Launch settings app
    SimpleAction settingsAction_;

    // Engine-level sticky Uinput: when a Chromium browser program reports
    // truly bare caps (0x72), remember it so subsequent IC re-creations
    // (which may report content hints like UppercaseWords) stay in Uinput.
    // Only applies when the new IC lacks "strong" content hints (Alpha,
    // SpellCheck, etc.) — real editors bypass this sticky flag.
    std::string chromiumBareCapsProgram_;
    bool chromiumHadBareCaps_ = false;

    // AT-SPI2 accessibility monitor for address bar detection
    std::unique_ptr<A11yMonitor> a11yMonitor_;

    // Cached mode-menu key parsed from config string (default: grave/backtick `)
    Key modeMenuKey_;

    // Macro table: shortcut → expansion (O(1) lookup)
    std::unordered_map<std::string, std::string> macroTable_;

    // Fcitx5 config for macro table (editable in addon settings)
    skeyMacroTableConfig macroTableConfig_;

    // Cached tray/sub-mode icon path + the theme it was resolved from.
    // Re-resolved automatically when IconTheme changes (no need for explicit
    // cache invalidation on reloadConfig).
    std::string iconCachePath_;
    std::string iconCacheTheme_;

    // Reload the O(1) lookup map from the config structure
    void rebuildMacroLookup();
};

class SKeyEngineFactory : public AddonFactory {
    AddonInstance *create(AddonManager *manager) override {
        registerDomain("fcitx5-skey", FCITX_INSTALL_LOCALEDIR);
        return new SKeyEngine(manager->instance());
    }
};

} // namespace fcitx

#endif // FCITX5_SKEY_ENGINE_H
