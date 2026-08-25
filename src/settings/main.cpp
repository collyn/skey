#include <QApplication>
#include <QIcon>
#include "config_io.h"
#include "settings_window.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("fcitx5-skey-settings");
    app.setApplicationDisplayName(QString::fromUtf8("Skey - Tùy chỉnh"));

    // Critical on Wayland: the compositor uses the .desktop file's Icon=
    // for the taskbar, not setWindowIcon().  desktopFileName must match
    // the basename of the installed .desktop file without the extension.
    app.setDesktopFileName("fcitx5-skey-settings");

    // Window icon — config-driven with fallback to the packaged default.
    SKeyConfig cfg = readSkeyConfig();
    QIcon icon(QString::fromStdString(effectiveIconPath(cfg)));
    if (icon.isNull())
        icon = QIcon(FCITX_SKEY_ICON_PATH);
    app.setWindowIcon(icon);

    SkeySettingsWindow window;
    window.show();

    return app.exec();
}

