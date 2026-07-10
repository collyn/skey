#ifndef FCITX5_SKEY_ENGINE_H
#define FCITX5_SKEY_ENGINE_H

#include <memory>
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
    bool useSurroundingText() const;
    bool canEditWithSurroundingText() const;
    bool useNativeSurroundingApi() const;
    bool useHiddenComposition() const;
    bool useUinputMode() const;
    bool connectUinputServer();
    void sendBackspaceUinput(int count, const std::string &text = "");
    bool handlePendingUinputBackspace(KeyEvent &keyEvent);
    void replayBufferedUinputKeys();
    void commitBuffer();
    void surroundingCommit(const std::string &oldComposed,
                           const std::string &newComposed);
    void surroundingBackspace();
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
    std::string cachedProgram_;
    std::unique_ptr<EventSourceTime> deferredCommitTimer_;
    std::string deferredCommitText_;
    std::string deferredPrefix_;
    uint64_t deferredBsSentAt_ = 0;
    std::string pendingFlushSuffix_;
    int uinputClientFd_ = -1;
    // Uinput replacement state (Lotus-style simple flow)
    bool uinputDeleting_ = false;
    std::unique_ptr<EventSourceTime> uinputCommitTimer_;
    int expectedUinputBackspaces_ = 0;
    int seenUinputBackspaces_ = 0;
    std::string pendingUinputCommit_;
    std::vector<KeySym> bufferedUinputKeys_;
    uint64_t bsSentAt_ = 0;        // timestamp when BS was sent
    uint64_t lastBsRoundTrip_ = 0; // last measured round-trip (usec)
    // Passthrough window: after uinput sends text via Ctrl+Shift+U, the
    // injected key events pass back through fcitx5.  Suppress engine
    // processing during this window so those keys reach the app unmodified.
    uint64_t uinputPassthroughUntil_ = 0;
    // Retroactive tone editing (Unikey-style): saved state of last committed word
    std::string lastRawInput_;      // Raw input of last committed word
    std::string lastComposed_;      // Composed text of last committed word
    int lastCommittedLen_ = 0;      // UTF-8 char count of last committed word
    bool reclaimReady_ = false;     // True after BS pressed while idle
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

private:
    void setupTrayMenu();

    Instance *instance_;
    SKeyConfig config_;
    FactoryFor<SKeyState> factory_;

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

    // Tray menu: Launch settings app
    SimpleAction settingsAction_;

    // AT-SPI2 accessibility monitor for address bar detection
    std::unique_ptr<A11yMonitor> a11yMonitor_;
};

class SKeyEngineFactory : public AddonFactory {
    AddonInstance *create(AddonManager *manager) override {
        registerDomain("fcitx5-skey", FCITX_INSTALL_LOCALEDIR);
        return new SKeyEngine(manager->instance());
    }
};

} // namespace fcitx

#endif // FCITX5_SKEY_ENGINE_H
