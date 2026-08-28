#ifndef SKEY_SETTINGS_WINDOW_H
#define SKEY_SETTINGS_WINDOW_H

#include <QWidget>

class AppearanceTab;
class GeneralTab;
class AppModesTab;
class MacroTab;
class DictTab;
class InfoTab;
class QPushButton;
class QTabWidget;

class SkeySettingsWindow : public QWidget {
    Q_OBJECT
public:
    explicit SkeySettingsWindow(QWidget *parent = nullptr);

signals:
    /// Emitted sau Apply khi ngôn ngữ GUI đổi; main.cpp tái tạo cửa sổ
    /// để mọi chuỗi đọc lại theo ngôn ngữ mới.
    void languageChanged(const QString &lang);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onApply();
    void onDefaults();
    void onClose();

private:
    void loadSettings();
    void setupUI();

    AppearanceTab *appearanceTab_;
    GeneralTab    *generalTab_;
    AppModesTab   *appModesTab_;
    MacroTab      *macroTab_;
    DictTab       *dictTab_;
    InfoTab       *infoTab_;
    QTabWidget   *tabWidget_;
    QPushButton  *applyButton_;
    QPushButton  *defaultsButton_;
    QPushButton  *closeButton_;
};

#endif // SKEY_SETTINGS_WINDOW_H
