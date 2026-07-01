#ifndef SKEY_SETTINGS_WINDOW_H
#define SKEY_SETTINGS_WINDOW_H

#include <QWidget>

class GeneralTab;
class AppModesTab;
class InfoTab;
class QPushButton;
class QTabWidget;

class SkeySettingsWindow : public QWidget {
    Q_OBJECT
public:
    explicit SkeySettingsWindow(QWidget *parent = nullptr);

private slots:
    void onApply();
    void onDefaults();
    void onClose();

private:
    void loadSettings();
    void setupUI();

    GeneralTab   *generalTab_;
    AppModesTab  *appModesTab_;
    InfoTab      *infoTab_;
    QTabWidget   *tabWidget_;
    QPushButton  *applyButton_;
    QPushButton  *defaultsButton_;
    QPushButton  *closeButton_;
};

#endif // SKEY_SETTINGS_WINDOW_H
