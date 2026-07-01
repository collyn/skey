#include "settings_window.h"
#include "general_tab.h"
#include "app_modes_tab.h"
#include "info_tab.h"
#include "config_io.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QTabWidget>
#include <QVBoxLayout>

SkeySettingsWindow::SkeySettingsWindow(QWidget *parent)
    : QWidget(parent) {
    setupUI();
    loadSettings();

    // Center on screen
    if (auto *screen = QApplication::primaryScreen()) {
        auto center = screen->geometry().center();
        move(center.x() - width() / 2, center.y() - height() / 2);
    }
}

void SkeySettingsWindow::setupUI() {
    setWindowTitle(QString::fromUtf8("Skey - Tùy chỉnh"));
    setFixedSize(440, 510);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    // ── Tab widget ──
    tabWidget_ = new QTabWidget(this);
    generalTab_  = new GeneralTab(this);
    appModesTab_ = new AppModesTab(this);
    infoTab_     = new InfoTab(this);
    tabWidget_->addTab(generalTab_,  QString::fromUtf8("Chung"));
    tabWidget_->addTab(appModesTab_, QString::fromUtf8("Ứng dụng"));
    tabWidget_->addTab(infoTab_,     QString::fromUtf8("Info"));
    mainLayout->addWidget(tabWidget_);

    // ── Hint label ──
    auto *hint = new QLabel(QString::fromUtf8("Nhấn Áp dụng để thay đổi có hiệu lực"), this);
    hint->setStyleSheet("color: #888; font-size: 11px;");
    mainLayout->addWidget(hint);

    // ── Bottom buttons ──
    auto *btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(8);

    applyButton_ = new QPushButton(QString::fromUtf8("Áp dụng"), this);
    applyButton_->setDefault(true);
    applyButton_->setMinimumWidth(100);

    defaultsButton_ = new QPushButton(QString::fromUtf8("Mặc định"), this);
    defaultsButton_->setMinimumWidth(100);

    closeButton_ = new QPushButton(QString::fromUtf8("Đóng"), this);
    closeButton_->setMinimumWidth(100);

    btnLayout->addWidget(applyButton_);
    btnLayout->addWidget(defaultsButton_);
    btnLayout->addStretch();
    btnLayout->addWidget(closeButton_);
    mainLayout->addLayout(btnLayout);

    // ── Connections ──
    connect(applyButton_,    &QPushButton::clicked, this, &SkeySettingsWindow::onApply);
    connect(defaultsButton_, &QPushButton::clicked, this, &SkeySettingsWindow::onDefaults);
    connect(closeButton_,    &QPushButton::clicked, this, &SkeySettingsWindow::onClose);
}

void SkeySettingsWindow::loadSettings() {
    auto cfg      = readSkeyConfig();
    auto appModes = readAppModesConfig();
    std::string trigger = readTriggerKey();

    generalTab_->loadFromConfig(cfg);
    generalTab_->setTriggerKey(trigger);
    appModesTab_->loadFromConfig(appModes);
}

void SkeySettingsWindow::onApply() {
    SKeyConfig cfg = generalTab_->collectConfig();
    AppModesConfig appModes = appModesTab_->collectConfig();
    std::string trigger = generalTab_->triggerKey();

    bool ok1 = writeSkeyConfig(cfg);
    bool ok2 = writeAppModesConfig(appModes);
    bool ok3 = writeTriggerKey(trigger);

    if (ok1 && ok2 && ok3) {
        reloadFcitx5();
        QMessageBox::information(this,
            QString::fromUtf8("Đã áp dụng"),
            QString::fromUtf8("Cấu hình đã được lưu và áp dụng."));
    } else {
        QMessageBox::warning(this,
            QString::fromUtf8("Lỗi"),
            QString::fromUtf8("Không thể ghi file cấu hình."));
    }
}

void SkeySettingsWindow::onDefaults() {
    auto answer = QMessageBox::question(this,
        QString::fromUtf8("Khôi phục mặc định"),
        QString::fromUtf8("Đặt lại tất cả cấu hình về mặc định?"),
        QMessageBox::Yes | QMessageBox::No);

    if (answer == QMessageBox::Yes) {
        generalTab_->setDefaults();
        appModesTab_->setDefaults();
    }
}

void SkeySettingsWindow::onClose() {
    close();
}
