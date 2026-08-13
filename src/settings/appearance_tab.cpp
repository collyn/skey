#include "appearance_tab.h"
#include "config_io.h"
#include "../icon_resolver.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyle>
#include <QVBoxLayout>

// ── Visual constants ──────────────────────────────────────────────────────
namespace {

constexpr int kTileBtnSize   = 80;   // button total size
constexpr int kTileIconSize  = kTileBtnSize - 4;  // leave room for border only
constexpr int kGridSpacing   = 12;
constexpr int kTileRadius     = 14;

const char *kStyleTile =
    "QPushButton {"
    "  border: 2px solid #444;"
    "  border-radius: %1px;"
    "  background: transparent;"
    "  padding: 0px;"
    "}"
    "QPushButton:hover {"
    "  border-color: #1a73e8;"
    "}";

const char *kStyleTileSelected =
    "QPushButton {"
    "  border: 3px solid #1a73e8;"
    "  border-radius: %1px;"
    "  background: transparent;"
    "  padding: 0px;"
    "}"
    "QPushButton:hover {"
    "  border-color: #2979ff;"
    "}";

const char *kStyleAddTile =
    "QPushButton {"
    "  border: 2px dashed #555;"
    "  border-radius: %1px;"
    "  background: transparent;"
    "  font-size: 22px;"
    "  color: #777;"
    "}"
    "QPushButton:hover {"
    "  border-color: #1a73e8;"
    "  color: #1a73e8;"
    "  background: #222;"
    "}";



QIcon makeRoundedIcon(const QString &iconPath, int size,
                       const QColor &bgColor = QColor(0x2d, 0x2d, 0x2d)) {
    if (iconPath.isEmpty()) return {};
    QIcon icon(iconPath);
    if (icon.isNull()) return {};

    QPixmap src = icon.pixmap(size, size);
    QPixmap rounded(size, size);
    rounded.fill(Qt::transparent);
    QPainter p(&rounded);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Clip to rounded rect
    QPainterPath pp;
    pp.addRoundedRect(QRectF(0, 0, size, size), kTileRadius - 2, kTileRadius - 2);
    p.setClipPath(pp);

    // Solid background fill
    if (bgColor.alpha() > 0) {
        p.fillRect(QRectF(0, 0, size, size), bgColor);
    }

    // Icon on top
    p.drawPixmap(QRectF(0, 0, size, size), src, QRectF(0, 0, size, size));
    p.end();
    return QIcon(rounded);
}

// Build absolute path for a given theme key.
QString themeToPath(const std::string &theme) {
    skey::IconSearchPaths paths;
    paths.userDataDir = userDataDir();
    paths.systemDirs = {
        "/usr/share/icons/hicolor/128x128/apps",
        "/usr/share/icons/hicolor/scalable/apps",
        "/usr/share/pixmaps",
    };
    paths.fallback = "/usr/share/icons/hicolor/128x128/apps/fcitx-skey.png";
    return QString::fromStdString(skey::resolveIconPath(theme, paths));
}

} // anonymous namespace

// ── Constructor ───────────────────────────────────────────────────────────

AppearanceTab::AppearanceTab(QWidget *parent)
    : QWidget(parent)
    , grid_(nullptr)
    , mainLayout_(nullptr)
    , addTile_(nullptr)
    , selectedTheme_(QString::fromUtf8(skey::kIconThemeDefault))
{
    setupUI();
}

// ── setupUI ───────────────────────────────────────────────────────────────

void AppearanceTab::setupUI() {
    mainLayout_ = new QVBoxLayout(this);
    mainLayout_->setContentsMargins(12, 12, 12, 12);
    mainLayout_->setSpacing(10);

    // Section label
    auto *header = new QLabel(QString::fromUtf8("Chọn biểu tượng"), this);
    QFont hf = header->font();
    hf.setBold(true);
    header->setFont(hf);
    mainLayout_->addWidget(header);

    // Grid container inside a frame
    auto *frame = new QFrame(this);
    frame->setStyleSheet(
        "QFrame { background: palette(window); border-radius: 8px; }");
    grid_ = new QGridLayout(frame);
    grid_->setSpacing(kGridSpacing);
    grid_->setContentsMargins(4, 4, 4, 4);
    mainLayout_->addWidget(frame);

    // Add-tile placeholder (created in rebuildGrid)
    addTile_ = nullptr;

    // Hint
    auto *hint = new QLabel(
        QString::fromUtf8("Biểu tượng hiển thị ở khay hệ thống,\n"
                          "cửa sổ cài đặt và trang Info."),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet("QLabel { color: palette(mid); font-size: 11px; }");
    mainLayout_->addWidget(hint);

    mainLayout_->addStretch();
}

// ── Tile factories ────────────────────────────────────────────────────────

QPushButton *AppearanceTab::makeTile(const QString &iconPath,
                                     const QString &themeKey,
                                     const QString &tooltip,
                                     bool isPreset,
                                     const QColor &bgColor) {
    auto *btn = new QPushButton(this);
    btn->setFixedSize(kTileBtnSize, kTileBtnSize);
    btn->setToolTip(tooltip);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setProperty("themeKey", themeKey);
    btn->setProperty("isPreset", isPreset);

    QIcon icon = makeRoundedIcon(iconPath, kTileIconSize, bgColor);
    if (!icon.isNull()) {
        btn->setIcon(icon);
        btn->setIconSize(QSize(kTileIconSize, kTileIconSize));
    } else {
        // Icon failed to load (missing SVG plugin, no PNG fallback).
        // Show the theme key as text so the tile is never a blank dark box.
        QString label = isPreset
            ? QString::fromUtf8(skey::presetIconBaseName(themeKey.toStdString()))
                      .remove("fcitx-skey-")
            : themeKey;
        btn->setText(label);
        btn->setStyleSheet(btn->styleSheet() +
            "QPushButton { color: #888; font-size: 9px; font-weight: bold; }");
    }

    QString style = QString(kStyleTile).arg(kTileRadius);
    btn->setStyleSheet(style);

    // Context menu for custom tiles: delete
    if (!isPreset) {
        btn->setContextMenuPolicy(Qt::ActionsContextMenu);
        auto *delAction = new QAction(QString::fromUtf8("Xóa biểu tượng này"), btn);
        QString filename = themeKey;
        connect(delAction, &QAction::triggered, this, [this, filename]() {
            onDeleteCustom(filename);
        });
        btn->addAction(delAction);
    }

    connect(btn, &QPushButton::clicked, this, &AppearanceTab::onTileClicked);
    return btn;
}

QPushButton *AppearanceTab::makeAddTile() {
    auto *btn = new QPushButton("+", this);
    btn->setFixedSize(kTileBtnSize, kTileBtnSize);
    btn->setToolTip(QString::fromUtf8("Thêm biểu tượng tùy chỉnh"));
    btn->setCursor(Qt::PointingHandCursor);
    QString style = QString(kStyleAddTile).arg(kTileRadius);
    btn->setStyleSheet(style);
    connect(btn, &QPushButton::clicked, this, &AppearanceTab::onAddCustom);
    return btn;
}

// ── Grid management ───────────────────────────────────────────────────────

void AppearanceTab::rebuildGrid() {
    // Clear existing tiles. hide() ensures widgets disappear immediately;
    // deleteLater() defers deletion to the event loop to avoid dangling refs.
    QLayoutItem *child;
    while ((child = grid_->takeAt(0)) != nullptr) {
        if (child->widget()) {
            child->widget()->hide();
            child->widget()->deleteLater();
        }
        delete child;
    }

    // Scan custom icons
    customIcons_ = listCustomIcons();

    // Build tile list in order: presets first, then custom, then add-tile
    struct TileSpec {
        QString themeKey;
        QString tooltip;
        bool    isPreset;
    };
    std::vector<TileSpec> specs;

    specs.push_back({QString::fromUtf8(skey::kIconThemeDefault),
                     QString::fromUtf8("Mặc định\nVi trên nền xanh"), true});
    specs.push_back({QString::fromUtf8(skey::kIconThemeVBlue),
                     QString::fromUtf8("Chữ V trên nền xanh"), true});
    specs.push_back({QString::fromUtf8(skey::kIconThemeVDark),
                     QString::fromUtf8("Chữ V trắng trong suốt\nPhù hợp dark theme"), true});
    specs.push_back({QString::fromUtf8(skey::kIconThemeVLight),
                     QString::fromUtf8("Chữ V đen trong suốt\nPhù hợp light theme"), true});
    specs.push_back({QString::fromUtf8(skey::kIconThemeVRed),
                     QString::fromUtf8("Chữ V trên nền đỏ"), true});
    specs.push_back({QString::fromUtf8(skey::kIconThemeVnFlag),
                     QString::fromUtf8("Ngôi sao vàng trên nền đỏ\nQuốc kỳ Việt Nam"), true});

    for (const auto &fn : customIcons_) {
        QString fname = QString::fromStdString(fn);
        specs.push_back({fname,
                         QString::fromUtf8("Tùy chỉnh: %1").arg(fname), false});
    }

    int cols = 3;
    int row = 0, col = 0;
    for (const auto &s : specs) {
        // All tiles use dark bg; v-dark gets a slightly different tone,
        // v-light needs a light bg so the black V is visible
        QColor bg = QColor(0x2d, 0x2d, 0x2d);
        if (s.themeKey == QString::fromUtf8(skey::kIconThemeVDark))
            bg = QColor(0x3A, 0x3A, 0x3A);
        else if (s.themeKey == QString::fromUtf8(skey::kIconThemeVLight))
            bg = QColor(0xE8, 0xE8, 0xE8);
        QPushButton *tile = makeTile(themeToPath(s.themeKey.toStdString()),
                                     s.themeKey, s.tooltip, s.isPreset, bg);
        grid_->addWidget(tile, row, col, Qt::AlignCenter);
        col++;
        if (col >= cols) { col = 0; row++; }
    }

    // Add-tile always at the next slot
    addTile_ = makeAddTile();
    grid_->addWidget(addTile_, row, col, Qt::AlignCenter);

    updateSelection();
}

// ── Config API ────────────────────────────────────────────────────────────

void AppearanceTab::loadFromConfig(const SKeyConfig &cfg) {
    selectedTheme_ = QString::fromStdString(
        cfg.iconTheme.empty() ? skey::kIconThemeDefault : cfg.iconTheme);
    rebuildGrid();
}

SKeyConfig AppearanceTab::collectConfig() const {
    SKeyConfig cfg;
    cfg.iconTheme = selectedTheme_.toStdString();
    return cfg;
}

void AppearanceTab::setDefaults() {
    selectedTheme_ = QString::fromUtf8(skey::kIconThemeDefault);
    rebuildGrid();
}

// ── Slots ─────────────────────────────────────────────────────────────────

void AppearanceTab::onTileClicked() {
    auto *btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;
    selectedTheme_ = btn->property("themeKey").toString();
    updateSelection();
}

void AppearanceTab::onAddCustom() {
    QString path = QFileDialog::getOpenFileName(
        this,
        QString::fromUtf8("Chọn biểu tượng"),
        QDir::homePath(),
        QString::fromUtf8("Hình ảnh (*.png *.svg)"));
    if (path.isEmpty()) return;

    // Ask for a short name for this icon
    QFileInfo fi(path);
    QString defaultName = fi.completeBaseName();
    // Sanitize: no spaces, no special chars
    defaultName.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "-");
    if (defaultName.isEmpty()) defaultName = "custom-icon";

    QString name = defaultName;
    std::string stored = importCustomIcon(path, name);
    if (stored.empty()) {
        QMessageBox::warning(this,
            QString::fromUtf8("Lỗi"),
            QString::fromUtf8("Không thể lưu biểu tượng.\n"
                              "Vui lòng kiểm tra quyền ghi."));
        return;
    }

    // Select the newly imported icon
    selectedTheme_ = QString::fromStdString(stored);
    rebuildGrid();
}

void AppearanceTab::onDeleteCustom(const QString &filename) {
    // Confirm
    auto answer = QMessageBox::question(
        this, QString::fromUtf8("Xóa biểu tượng"),
        QString::fromUtf8("Xóa biểu tượng \"%1\"?").arg(filename),
        QMessageBox::Yes | QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    removeCustomIcon(filename.toStdString());

    // If the deleted icon was selected, revert to default
    if (selectedTheme_ == filename)
        selectedTheme_ = QString::fromUtf8(skey::kIconThemeDefault);

    rebuildGrid();
}

// ── Visual update ─────────────────────────────────────────────────────────

void AppearanceTab::updateSelection() {
    // Walk all tiles and update their styles
    for (int i = 0; i < grid_->count(); i++) {
        auto *item = grid_->itemAt(i);
        if (!item) continue;
        auto *btn = qobject_cast<QPushButton*>(item->widget());
        if (!btn || btn == addTile_) continue;

        QString key = btn->property("themeKey").toString();
        bool isSel = (key == selectedTheme_);
        btn->setStyleSheet(
            QString(isSel ? kStyleTileSelected : kStyleTile)
                .arg(kTileRadius));
    }
}
