#include "settings_window.h"
#include "app_modes_tab.h"
#include "appearance_tab.h"
#include "config_io.h"
#include "dict_tab.h"
#include "general_tab.h"
#include "info_tab.h"
#include "macro_tab.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>


SkeySettingsWindow::SkeySettingsWindow(QWidget *parent) : QWidget(parent) {
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
  setFixedSize(480, 600);

  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(12, 12, 12, 12);
  mainLayout->setSpacing(8);

  // ── Tab widget ──
  tabWidget_ = new QTabWidget(this);
  // No right-scroll arrows: tabs shrink to fit a single row instead.
  // (QTabBar has no native multi-row mode.)
  tabWidget_->setUsesScrollButtons(false);
  // Elide rather than overlap if space is ever genuinely short.
  tabWidget_->tabBar()->setElideMode(Qt::ElideRight);
  generalTab_ = new GeneralTab(this);
  appModesTab_ = new AppModesTab(this);
  macroTab_ = new MacroTab(this);
  dictTab_ = new DictTab(this);
  appearanceTab_ = new AppearanceTab(this);
  infoTab_ = new InfoTab(this);
  tabWidget_->addTab(generalTab_, QString::fromUtf8("Chung"));
  tabWidget_->addTab(appModesTab_, QString::fromUtf8("Apps"));
  tabWidget_->addTab(macroTab_, QString::fromUtf8("Gõ tắt"));
  tabWidget_->addTab(dictTab_, QString::fromUtf8("Từ điển"));
  tabWidget_->addTab(appearanceTab_, QString::fromUtf8("Icons"));
  tabWidget_->addTab(infoTab_, QString::fromUtf8("Info"));
  // Tab size hints vary per platform theme (GTK themes want wider tabs
  // than Fusion/KDE). Grow the window to fit the tabs at their natural
  // size instead of crushing the tab text.
  const int tabBarWidth = tabWidget_->tabBar()->sizeHint().width();
  setFixedWidth(qBound(480, tabBarWidth + 28, 640));
  mainLayout->addWidget(tabWidget_);

  // ── Hint label ──
  auto *hint = new QLabel(
      QString::fromUtf8("Nhấn Áp dụng để thay đổi có hiệu lực"), this);
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
  connect(applyButton_, &QPushButton::clicked, this,
          &SkeySettingsWindow::onApply);
  connect(defaultsButton_, &QPushButton::clicked, this,
          &SkeySettingsWindow::onDefaults);
  connect(closeButton_, &QPushButton::clicked, this,
          &SkeySettingsWindow::onClose);
  connect(infoTab_, &InfoTab::configRestored, this,
          &SkeySettingsWindow::loadSettings);
}

void SkeySettingsWindow::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  loadSettings(); // re-read config in case it changed outside the GUI
}

void SkeySettingsWindow::loadSettings() {
  auto cfg = readSkeyConfig();
  auto appModes = readAppModesConfig();
  auto macroCfg = readMacroConfig();
  std::string trigger = readTriggerKey();

  generalTab_->loadFromConfig(cfg);
  generalTab_->setTriggerKey(trigger);
  generalTab_->setModeMenuKey(cfg.modeMenuKey);
  appModesTab_->loadFromConfig(appModes);
  appModesTab_->setChromiumAddressBarMode(cfg.chromiumAddressBarMode);

  MacroTabData macroData;
  macroData.enableMacro = cfg.enableMacro;
  macroData.capitalizeMacro = cfg.capitalizeMacro;
  macroData.macroInOffMode = cfg.macroInOffMode;
  macroData.entries = macroCfg.entries;
  macroTab_->loadFromConfig(macroData);

  dictTab_->loadFromConfig(readUserDict());

  appearanceTab_->loadFromConfig(cfg);
}

void SkeySettingsWindow::onApply() {
  SKeyConfig cfg = generalTab_->collectConfig();
  AppModesConfig appModes = appModesTab_->collectConfig();
  MacroTabData macroData = macroTab_->collectConfig();
  std::string trigger = generalTab_->triggerKey();

  // Merge address bar mode from AppModes tab
  cfg.chromiumAddressBarMode = appModesTab_->chromiumAddressBarMode();
  // Merge macro bools into main config
  cfg.enableMacro = macroData.enableMacro;
  cfg.capitalizeMacro = macroData.capitalizeMacro;
  cfg.macroInOffMode = macroData.macroInOffMode;

  // Merge appearance (icon) fields
  SKeyConfig appearance = appearanceTab_->collectConfig();
  cfg.iconTheme = appearance.iconTheme;

  MacroConfig macroCfg;
  macroCfg.entries = macroData.entries;

  bool ok1 = writeSkeyConfig(cfg);
  bool ok2 = writeAppModesConfig(appModes);
  bool ok3 = writeMacroConfig(macroCfg);
  bool ok4 = writeTriggerKey(trigger);
  bool ok5 = writeUserDict(dictTab_->collectConfig());

  if (ok1 && ok2 && ok3 && ok4 && ok5) {
    reloadFcitx5();
    QMessageBox::information(
        this, QString::fromUtf8("Đã áp dụng"),
        QString::fromUtf8("Cấu hình đã được lưu và áp dụng."));
  } else {
    QMessageBox::warning(this, QString::fromUtf8("Lỗi"),
                         QString::fromUtf8("Không thể ghi file cấu hình."));
  }
}

void SkeySettingsWindow::onDefaults() {
  auto answer = QMessageBox::question(
      this, QString::fromUtf8("Khôi phục mặc định"),
      QString::fromUtf8("Đặt lại tất cả cấu hình về mặc định?"),
      QMessageBox::Yes | QMessageBox::No);

  if (answer == QMessageBox::Yes) {
    generalTab_->setDefaults();
    appModesTab_->setDefaults();
    macroTab_->setDefaults();
    dictTab_->setDefaults();
    appearanceTab_->setDefaults();
  }
}

void SkeySettingsWindow::onClose() { close(); }
