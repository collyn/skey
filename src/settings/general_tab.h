#ifndef SKEY_SETTINGS_GENERAL_TAB_H
#define SKEY_SETTINGS_GENERAL_TAB_H

#include <QWidget>

class QComboBox;
class QCheckBox;

struct SKeyConfig;

class GeneralTab : public QWidget {
    Q_OBJECT
public:
    explicit GeneralTab(QWidget *parent = nullptr);

    void loadFromConfig(const SKeyConfig &cfg);
    SKeyConfig collectConfig() const;
    void setDefaults();

private:
    void setupUI();

    QComboBox *inputMethodCombo_;
    QComboBox *outputModeCombo_;
    QComboBox *tonePositionCombo_;
    QCheckBox *freeMarkingCheck_;
    QCheckBox *autoRestoreCheck_;
    QCheckBox *showPreeditCheck_;
    QCheckBox *debugCheck_;
};

#endif // SKEY_SETTINGS_GENERAL_TAB_H
