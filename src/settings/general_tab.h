#ifndef SKEY_SETTINGS_GENERAL_TAB_H
#define SKEY_SETTINGS_GENERAL_TAB_H

#include <QWidget>

class QComboBox;
class QCheckBox;
class QRadioButton;
class HotkeyEdit;

struct SKeyConfig;

class GeneralTab : public QWidget {
    Q_OBJECT
public:
    explicit GeneralTab(QWidget *parent = nullptr);

    void loadFromConfig(const SKeyConfig &cfg);
    SKeyConfig collectConfig() const;
    void setDefaults();

    std::string triggerKey() const;
    void setTriggerKey(const std::string &fcitx5Key);

private:
    void setupUI();

    QComboBox *inputMethodCombo_;
    QComboBox *outputModeCombo_;
    HotkeyEdit *triggerKeyEdit_;
    QCheckBox *shortWCheck_;
    QCheckBox *bracketUOCheck_;
    QCheckBox *freeMarkingCheck_;
    QCheckBox *autoRestoreCheck_;
    QCheckBox *showPreeditCheck_;
    QRadioButton *addrBarPreeditRadio_;
    QRadioButton *addrBarNoVietRadio_;
    QCheckBox *debugCheck_;
};

#endif // SKEY_SETTINGS_GENERAL_TAB_H
