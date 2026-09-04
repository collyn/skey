#include "app_modes_tab.h"
#include "config_io.h"
#include "tr.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

// ── Mode values as displayed in the per-app config ─────────────────────
static const char *kAppModeValues[] = {"Auto", "Uinput", "Surrounding Text", "Preedit", "Excluded", nullptr};

AppModesTab::AppModesTab(QWidget *parent) : QWidget(parent) {
    setupUI();
}

void AppModesTab::setupUI() {
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    // ── Filter row (compact; the description moved into the tooltip so
    //    the list keeps the vertical space) ──
    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(T("Tìm ứng dụng…"));
    filterEdit_->setClearButtonEnabled(true);
    filterEdit_->setToolTip(
        T("Cấu hình chế độ xuất riêng cho từng ứng dụng.\n"
          "Tên ứng dụng là tên file thực thi (vd: firefox, tabby, code).\n"
          "Gõ để lọc danh sách."));
    mainLayout->addWidget(filterEdit_);
    connect(filterEdit_, &QLineEdit::textChanged,
            this, &AppModesTab::onFilterChanged);

    // ── Table ──
    table_ = new QTableWidget(0, 3, this);
    table_->setHorizontalHeaderLabels({
        T("Tên ứng dụng"),
        T("Chế độ xuất"),
        T("Xóa")});
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setAlternatingRowColors(true);
    table_->setIconSize(QSize(20, 20));
    table_->verticalHeader()->setVisible(false);
    mainLayout->addWidget(table_, /*stretch=*/1);

    // ── Buttons row ──
    auto *btnRow = new QHBoxLayout();
    addButton_ = new QPushButton(T("Thêm ứng dụng"), this);
    btnRow->addWidget(addButton_);
    btnRow->addStretch();
    mainLayout->addLayout(btnRow);

    connect(addButton_, &QPushButton::clicked, this, &AppModesTab::onAddApp);

    // ── Chromium address bar mode (compact footer) ──
    auto *addrBarRow = new QHBoxLayout();
    addrBarRow->setSpacing(6);
    auto *addrBarLabel = new QLabel(
        T("Thanh địa chỉ Chromium:"), this);
    addrBarLabel->setToolTip(
        T("Chế độ gõ cho thanh địa chỉ Chrome/Edge/Brave.\n"
                          "Không ảnh hưởng đến nội dung trang web."));
    addrBarRow->addWidget(addrBarLabel);

    addrBarModeCombo_ = new QComboBox(this);
    addrBarModeCombo_->addItem("Auto", "Auto");
    addrBarModeCombo_->addItem("Uinput", "Uinput");
    addrBarModeCombo_->addItem("Surrounding Text", "Surrounding Text");
    addrBarModeCombo_->addItem("Preedit", "Preedit");
    addrBarModeCombo_->addItem(T("Không gõ tiếng Việt"),
                               "No Vietnamese");
    addrBarRow->addWidget(addrBarModeCombo_);

    mainLayout->addLayout(addrBarRow);
}

// ── App icon resolution ─────────────────────────────────────────────────
// Best-effort icon lookup for an executable name (window class / app_id):
//   1. Direct theme lookup — many themes ship <app>.svg (firefox, code…)
//   2. .desktop files: Exec=/StartupWMClass=/Name= → Icon= against the theme
//   3. Small alias table for hardcoded app_ids (LibreOffice = soffice.bin)
//   4. Scan /proc for a running binary with this name, probe Firefox-family
//      icon layout next to it (tarball browsers like zen have no .desktop)
//   5. Generic "application-x-executable" fallback
// Empty names (IBus apps) render text only.

static QString normalizeExecName(const QString &name) {
    QString n = name.toLower().trimmed();
    if (n.endsWith(QLatin1String(".bin"))) n.chop(4);
    return n;
}

static QHash<QString, QString> &desktopIconMap() {
    static QHash<QString, QString> map;
    static bool scanned = false;
    if (scanned) return map;
    scanned = true;

    // QStandardPaths::locateAll() wildcard matching proved unreliable on
    // some Qt 6 builds (returned 0 files), so walk the dirs manually.
    static const QRegularExpression kFieldCodes("%[fFuUdDnNickvm]");
    static const QRegularExpression kTokenSplit("[\\s\"']");
    const QStringList dirs =
        QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    for (const QString &dir : dirs) {
        const QStringList files =
            QDir(dir).entryList({QStringLiteral("*.desktop")}, QDir::Files);
        for (const QString &fileName : files) {
            QFile f(dir + QLatin1Char('/') + fileName);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            QString icon, exec, wmClass, name;
            bool inEntry = false;
            auto flush = [&]() {
                if (!icon.isEmpty()) {
                    if (!exec.isEmpty())
                        map.insert(normalizeExecName(exec), icon);
                    if (!wmClass.isEmpty())
                        map.insert(normalizeExecName(wmClass), icon);
                    if (!name.isEmpty())
                        map.insert(normalizeExecName(name), icon);
                }
            };
            while (!f.atEnd()) {
                const QString line = QString::fromUtf8(f.readLine()).trimmed();
                if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                    continue;
                if (line.startsWith(QLatin1Char('['))) {
                    flush();
                    inEntry = (line == QLatin1String("[Desktop Entry]"));
                    icon.clear();
                    exec.clear();
                    wmClass.clear();
                    name.clear();
                    continue;
                }
                if (!inEntry) continue;
                if (line.startsWith(QLatin1String("Exec="))) {
                    exec = line.mid(5);
                    exec.replace(kFieldCodes, QString()); // strip %u %F …
                    exec = exec.trimmed();
                    // Drop a leading quote, then take the first token.
                    if (exec.startsWith(QLatin1Char('"')) ||
                        exec.startsWith(QLatin1Char('\'')))
                        exec.remove(0, 1);
                    exec = exec.section(kTokenSplit, 0, 0);
                    if (!exec.isEmpty())
                        exec = QFileInfo(exec).completeBaseName();
                } else if (line.startsWith(QLatin1String("Icon="))) {
                    icon = line.mid(5).trimmed();
                } else if (line.startsWith(QLatin1String("StartupWMClass="))) {
                    wmClass = line.mid(16).trimmed();
                } else if (line.startsWith(QLatin1String("Name="))) {
                    name = line.mid(5).trimmed(); // skips localized Name[xx]=
                }
            }
            flush();
        }
    }
    return map;
}

// Hardcoded app_ids that don't match any desktop Exec= name.
static const char *iconNameAlias(const QString &normalizedExec) {
    if (normalizedExec == QLatin1String("soffice"))
        return "libreoffice-startcenter"; // LibreOffice's Wayland app_id
    return nullptr;
}

// One /proc pass mapping normalized exe base name → canonical path, for
// apps without any .desktop file (tarball browsers). Same-user scan;
// permission failures are skipped. Refreshes at most every 15 s so apps
// started after the first fill are still found, without rescanning per row.
static QHash<QString, QString> &runningExeMap() {
    static QHash<QString, QString> map;
    static QElapsedTimer lastScan;
    if (lastScan.isValid() && lastScan.elapsed() < 15000) return map;

    map.clear();
    const QDir proc(QStringLiteral("/proc"));
    const QStringList pids = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pid : pids) {
        bool numeric = true;
        for (const QChar &c : pid) {
            if (!c.isDigit()) { numeric = false; break; }
        }
        if (!numeric) continue;
        const QString target =
            QFileInfo(QStringLiteral("/proc/") + pid + QLatin1String("/exe"))
                .canonicalFilePath();
        if (target.isEmpty()) continue; // EACCES or process gone
        map.insert(normalizeExecName(QFileInfo(target).completeBaseName()),
                   target);
    }
    lastScan.start();
    return map;
}

// Probe icon layouts shipped next to a binary instead of the icon theme.
static QIcon iconNearExe(const QString &exePath) {
    const QDir binDir = QFileInfo(exePath).absoluteDir();
    static const char *kPatterns[] = {
        // Firefox-family tarballs (zen, firefox…)
        "browser/chrome/icons/default/default128.png",
        "browser/chrome/icons/default/default64.png",
        // Chromium-family product logos
        "product_logo_64.png",
        "icons/default128.png",
        nullptr,
    };
    for (int i = 0; kPatterns[i]; ++i) {
        const QString file = binDir.filePath(QLatin1String(kPatterns[i]));
        if (QFileInfo::exists(file)) return QIcon(file);
    }
    return QIcon();
}

// Cache probed icons by exe path — several app names can share one binary.
static QIcon iconForExe(const QString &exePath) {
    static QHash<QString, QIcon> cache;
    auto it = cache.constFind(exePath);
    if (it != cache.constEnd()) return *it;
    QIcon icon = iconNearExe(exePath);
    cache.insert(exePath, icon); // may be null
    return icon;
}

static QIcon resolveAppIcon(const QString &execName) {
    if (execName.isEmpty()) return QIcon();
    const QString key = execName.toLower();

    static QHash<QString, QIcon> cache;
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;

    const QString norm = normalizeExecName(execName);

    // 1. Theme icon named after the executable itself.
    QIcon icon = QIcon::fromTheme(execName);
    if (icon.isNull() && key != execName)
        icon = QIcon::fromTheme(key);
    if (icon.isNull() && norm != key)
        icon = QIcon::fromTheme(norm);

    // 2. Desktop-file mapping (Exec=/StartupWMClass=/Name= → Icon=).
    if (icon.isNull()) {
        const QString &iconName = desktopIconMap().value(norm);
        if (!iconName.isEmpty()) {
            icon = iconName.contains(QLatin1Char('/'))
                ? QIcon(iconName)              // absolute icon path
                : QIcon::fromTheme(iconName);
        }
    }

    // 3. Alias table.
    if (icon.isNull()) {
        const char *alias = iconNameAlias(norm);
        if (alias) icon = QIcon::fromTheme(QString::fromLatin1(alias));
    }

    // 4. Running binary → icons shipped next to it.
    if (icon.isNull()) {
        const QString exePath = runningExeMap().value(norm);
        if (!exePath.isEmpty()) icon = iconForExe(exePath);
    }

    // 5. Generic executable icon.
    if (icon.isNull())
        icon = QIcon::fromTheme(QStringLiteral("application-x-executable"));

    cache.insert(key, icon);
    return icon;
}

// ── Add a single row to the table ──────────────────────────────────────
void AppModesTab::addRow(const std::string &name, const std::string &mode) {
    int row = table_->rowCount();
    table_->insertRow(row);

    // Column 0 — read-only app name.
    // IBus frontend reports empty program name (AppImages like Viber).
    // Display a readable label but keep the real key for save/load.
    QString displayName = name.empty()
        ? T("(IBus app)")
        : QString::fromStdString(name);
    auto *nameItem = new QTableWidgetItem(displayName);
    nameItem->setData(Qt::UserRole, QString::fromStdString(name)); // real key
    nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, 0, nameItem); // icon resolved later in fillIcons()

    // Column 1 — mode combobox
    auto *combo = new QComboBox(table_);
    for (int i = 0; kAppModeValues[i]; ++i) {
        combo->addItem(kAppModeValues[i], kAppModeValues[i]);
    }
    int idx = combo->findData(QString::fromStdString(mode));
    if (idx >= 0) combo->setCurrentIndex(idx);
    table_->setCellWidget(row, 1, combo);

    // Column 2 — delete button
    auto *delBtn = new QPushButton(T("Xóa"), table_);
    connect(delBtn, &QPushButton::clicked, this, &AppModesTab::onDeleteApp);
    table_->setCellWidget(row, 2, delBtn);
}

// ── Filter rows by app name (display text + real key) ──────────────────
void AppModesTab::onFilterChanged(const QString &text) {
    const QString filter = text.trimmed();
    for (int r = 0; r < table_->rowCount(); ++r) {
        auto *item = table_->item(r, 0);
        bool show = filter.isEmpty();
        if (item && !show) {
            show = item->text().contains(filter, Qt::CaseInsensitive) ||
                   item->data(Qt::UserRole)
                       .toString()
                       .contains(filter, Qt::CaseInsensitive);
        }
        table_->setRowHidden(r, !show);
    }
}

// ── Resolve icons for rows that don't have one yet ─────────────────────
void AppModesTab::fillIcons() {
    for (int r = 0; r < table_->rowCount(); ++r) {
        auto *item = table_->item(r, 0);
        if (!item || !item->icon().isNull()) continue;
        const QString name = item->data(Qt::UserRole).toString();
        if (name.isEmpty()) continue; // IBus apps have no executable
        item->setIcon(resolveAppIcon(name));
    }
}

// ── Load from config ────────────────────────────────────────────────────
void AppModesTab::loadFromConfig(const AppModesConfig &cfg) {
    table_->setRowCount(0);
    for (auto &[name, mode] : cfg.entries) {
        addRow(name, mode);
    }
    // Icon lookup (desktop scan + /proc pass) is deferred so the window
    // opens instantly; icons appear on the next event-loop iteration.
    QTimer::singleShot(0, this, &AppModesTab::fillIcons);
}

// ── Collect to config ───────────────────────────────────────────────────
AppModesConfig AppModesTab::collectConfig() const {
    AppModesConfig cfg;
    for (int r = 0; r < table_->rowCount(); ++r) {
        auto *nameItem = table_->item(r, 0);
        auto *combo    = qobject_cast<QComboBox *>(table_->cellWidget(r, 1));
        if (!nameItem || !combo) continue;

        // Use the real key stored in UserRole (may be empty for IBus apps)
        std::string name = nameItem->data(Qt::UserRole).toString().toStdString();
        // Fallback to display text for manually added entries
        if (name.empty() && !nameItem->text().isEmpty()) {
            name = nameItem->text().toStdString();
        }
        std::string mode = combo->currentData().toString().toStdString();
        if (!mode.empty()) {
            cfg.entries.emplace_back(name, mode);
        }
    }
    return cfg;
}

// ── Defaults ────────────────────────────────────────────────────────────
void AppModesTab::setDefaults() {
    table_->setRowCount(0);
}

// ── Add app dialog ──────────────────────────────────────────────────────
void AppModesTab::onAddApp() {
    QDialog dlg(this);
    dlg.setWindowTitle(T("Thêm ứng dụng mới"));
    dlg.setFixedSize(340, 150);

    auto *layout = new QFormLayout(&dlg);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);

    auto *nameEdit = new QLineEdit(&dlg);
    nameEdit->setPlaceholderText(T("vd: firefox, tabby, code"));
    layout->addRow(T("Tên ứng dụng:"), nameEdit);

    auto *modeCombo = new QComboBox(&dlg);
    for (int i = 0; kAppModeValues[i]; ++i) {
        modeCombo->addItem(kAppModeValues[i], kAppModeValues[i]);
    }
    modeCombo->setCurrentIndex(0); // default Uinput
    layout->addRow(T("Chế độ xuất:"), modeCombo);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(T("Thêm"));
    buttons->button(QDialogButtonBox::Cancel)->setText(T("Hủy"));
    layout->addRow(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        std::string name = nameEdit->text().trimmed().toStdString();
        std::string mode = modeCombo->currentData().toString().toStdString();
        if (!name.empty()) {
            addRow(name, mode);
            fillIcons(); // resolves only the new row (caches are warm)
        }
    }
}

// ── Chromium address bar mode ───────────────────────────────────────────
std::string AppModesTab::chromiumAddressBarMode() const {
    return addrBarModeCombo_->currentData().toString().toStdString();
}

void AppModesTab::setChromiumAddressBarMode(const std::string &mode) {
    int idx = addrBarModeCombo_->findData(QString::fromStdString(mode));
    if (idx >= 0) addrBarModeCombo_->setCurrentIndex(idx);
}

// ── Delete current selection ────────────────────────────────────────────
void AppModesTab::onDeleteApp() {
    // Find which button was clicked
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (!btn) return;

    for (int r = 0; r < table_->rowCount(); ++r) {
        if (table_->cellWidget(r, 2) == btn) {
            table_->removeRow(r);
            return;
        }
    }
}
