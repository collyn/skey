#ifndef SKEY_SETTINGS_APP_DELAY_DIALOG_H
#define SKEY_SETTINGS_APP_DELAY_DIALOG_H

#include <QDialog>

#include <map>

#include "../app_delay_key.h"
#include "config_io.h"

class QCheckBox;
class QLabel;
class QSpinBox;
class QPushButton;

/// Per-app advanced delay editor: three tunables (pacing between
/// backspaces, wait before commit, wait after commit).  Only the platform
/// of the current session (X11 or Wayland) is shown; the other platform's
/// values pass through unchanged.  A "Tùy chỉnh" (Custom) checkbox is the
/// explicit override switch: unchecked = automatic (spinboxes show the
/// engine's current effective values, disabled); checked = the displayed
/// values are applied verbatim.  The "Auto" button unchecks it.
class AppDelayDialog : public QDialog {
    Q_OBJECT
public:
    AppDelayDialog(const QString &appName,
                   const skey::AppDelayOverride &x11,
                   const skey::AppDelayOverride &wayland,
                   QWidget *parent = nullptr);

    skey::AppDelayOverride x11Override() const;
    skey::AppDelayOverride waylandOverride() const;

private:
    struct Group {
        QCheckBox *customCheck;
        QSpinBox *paceSpin;
        QSpinBox *preSpin;
        QSpinBox *postSpin;
        QLabel *hintLabel;
        QLabel *warningLabel;
    };

    Group buildGroup(const std::string &appKey,
                     const std::map<std::string, LearnedDelay> &learned);

    Group active_;
    bool waylandSession_ = false;
    skey::AppDelayOverride inactiveInitial_;
    // Current effective values (defaults merged with any existing
    // override) — shown while the Custom checkbox is off.
    int effPaceMs_ = 1;
    int effPreMs_ = 0;
    int effPostMs_ = 0;
};

#endif // SKEY_SETTINGS_APP_DELAY_DIALOG_H
