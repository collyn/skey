#include <QApplication>
#include <QIcon>
#include "settings_window.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("fcitx5-skey-settings");
    app.setApplicationDisplayName(QString::fromUtf8("Skey - Tùy chỉnh"));
    app.setWindowIcon(QIcon::fromTheme("fcitx-skey"));

    SkeySettingsWindow window;
    window.show();

    return app.exec();
}
