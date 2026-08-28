#include <clocale>
#include <cstdlib>
#include <cstring>

#include <QApplication>
#include <QIcon>

#include "config_io.h"
#include "settings_window.h"
#include "tr.h"

// ABI-safe — recipe từ src/engine.h:416-423: header fcitx5 5.1 chỉ khai báo
// overload std::filesystem::path (không có trong lib 5.0, gây dlopen fail);
// overload const char* có ở cả hai ABI (nm: _ZN5fcitx14registerDomainEPKcS1_)
// nên phải tự khai báo và gọi trực tiếp.
namespace fcitx {
void registerDomain(const char *domain, const char *dir);
}

static SkeySettingsWindow *g_window = nullptr;

// skey.conf UILanguage là nguồn sự thật; LANGUAGE env là kênh vận chuyển.
// setlocale() buộc libintl đọc lại binding sau khi setenv.
static void applyLanguage(const QString &lang) {
    setenv("LANGUAGE", lang == QLatin1String("en") ? "en" : "vi", 1);
    setlocale(LC_ALL, "");
    // glibc gettext BỎ QUA LANGUAGE khi locale đang là "C" (hệ thống không
    // cấu hình LANG/LC_ALL, hoặc LANG trỏ vào locale chưa được generate).
    // Tiếng Việt không sao (msgid đã là tiếng Việt), nhưng tiếng Anh cần
    // một locale thật — en_US.UTF-8 có sẵn trên mọi glibc.
    if (lang == QLatin1String("en") &&
        std::strcmp(setlocale(LC_MESSAGES, nullptr), "C") == 0) {
        setlocale(LC_MESSAGES, "en_US.UTF-8");
    }
}

static void showSettingsWindow() {
    // KHÔNG delete sender khi signal còn đang emit: hide + deleteLater
    // (hủy hoãn về event loop) — an toàn với Qt.
    if (g_window) {
        g_window->hide();
        g_window->deleteLater();
    }
    g_window = new SkeySettingsWindow; // mọi chuỗi đọc lại theo ngôn ngữ mới
    QObject::connect(g_window, &SkeySettingsWindow::languageChanged,
                     [](const QString &lang) {
                         applyLanguage(lang);
                         showSettingsWindow();
                     });
    g_window->show();
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, ""); // gettext cần locale khác "C"
    fcitx::registerDomain("fcitx5-skey", FCITX_INSTALL_LOCALEDIR);

    QApplication app(argc, argv);
    app.setApplicationName("fcitx5-skey-settings");
    app.setApplicationDisplayName(T("Skey - Tùy chỉnh"));

    // Critical on Wayland: the compositor uses the .desktop file's Icon=
    // for the taskbar, not setWindowIcon().  desktopFileName must match
    // the basename of the installed .desktop file without the extension.
    app.setDesktopFileName("fcitx5-skey-settings");

    // Window icon — config-driven with fallback to the packaged default.
    SKeyConfig cfg = readSkeyConfig();
    applyLanguage(QString::fromStdString(cfg.uiLanguage)); // trước _() đầu tiên

    QIcon icon(QString::fromStdString(effectiveIconPath(cfg)));
    if (icon.isNull())
        icon = QIcon(FCITX_SKEY_ICON_PATH);
    app.setWindowIcon(icon);

    showSettingsWindow();
    return app.exec();
}
