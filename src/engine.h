#ifndef FCITX5_SKEY_ENGINE_H
#define FCITX5_SKEY_ENGINE_H

#include <fcitx-utils/event.h>
#include <fcitx-utils/i18n.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

#include "config.h"
#include "vietnamese.h"

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
    SKeyOutputMode effectiveMode() const;
    bool useSurroundingText() const;
    bool canEditWithSurroundingText() const;
    bool useNativeSurroundingApi() const;
    bool useHiddenComposition() const;
    bool useSlowMode() const;
    void commitBuffer();
    void surroundingCommit(const std::string &oldComposed,
                           const std::string &newComposed);
    void surroundingBackspace();
    bool hasDeferredCommitPending() const;
    void scheduleDeferredCommit(const std::string &text,
                                const std::string &stablePrefix = "");
    void flushDeferredCommit();
    void updatePreedit();
    void clearUI();
    void showModeMenu();

    SKeyEngine *engine_;
    InputContext *ic_;
    skey::VietnameseEngine viet_;
    int committedLen_ = 0;
    bool modeMenuActive_ = false;
    bool hasAppModeOverride_ = false;
    SKeyOutputMode appModeOverride_ = SKeyOutputMode::SurroundingText;
    std::unique_ptr<EventSourceTime> deferredCommitTimer_;
    std::string deferredCommitText_;
    std::string deferredPrefix_;
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

    const SKeyConfig &config() const { return config_; }
    Instance *instance() { return instance_; }
    void setOutputMode(SKeyOutputMode mode);
    void saveAppMode(const std::string &app, SKeyOutputMode mode);
    SKeyOutputMode loadAppMode(const std::string &app) const;

private:
    Instance *instance_;
    SKeyConfig config_;
    FactoryFor<SKeyState> factory_;
};

class SKeyEngineFactory : public AddonFactory {
    AddonInstance *create(AddonManager *manager) override {
        registerDomain("fcitx5-skey", FCITX_INSTALL_LOCALEDIR);
        return new SKeyEngine(manager->instance());
    }
};

} // namespace fcitx

#endif // FCITX5_SKEY_ENGINE_H
