#include "general_tab.h"
#include "config_io.h"
#include "hotkey_edit.h"
#include "tr.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

GeneralTab::GeneralTab(QWidget *parent) : QWidget(parent) { setupUI(); }

void GeneralTab::setupUI() {
  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(8, 8, 8, 8);
  mainLayout->setSpacing(8);

  // ── Enum section ──
  auto *enumFrame = new QFrame(this);
  enumFrame->setFrameStyle(QFrame::StyledPanel);
  auto *enumLayout = new QFormLayout(enumFrame);
  enumLayout->setLabelAlignment(Qt::AlignRight);
  enumLayout->setSpacing(6);
  enumLayout->setContentsMargins(12, 12, 12, 12);

  inputMethodCombo_ = new QComboBox(enumFrame);
  inputMethodCombo_->addItem("Telex", "Telex");
  inputMethodCombo_->addItem("VNI", "VNI");
  enumLayout->addRow(T("Kiểu gõ:"), inputMethodCombo_);

  outputModeCombo_ = new QComboBox(enumFrame);
  outputModeCombo_->addItem("Auto", "Auto");
  outputModeCombo_->addItem("Uinput", "Uinput");
  outputModeCombo_->addItem("Surrounding Text", "Surrounding Text");
  outputModeCombo_->addItem("Preedit", "Preedit");
  enumLayout->addRow(T("Chế độ xuất:"), outputModeCombo_);

  charsetCombo_ = new QComboBox(enumFrame);
  charsetCombo_->addItem("Unicode", "Unicode");
  charsetCombo_->addItem("TCVN3 (ABC)", "TCVN3 (ABC)");
  charsetCombo_->addItem("VNI Windows", "VNI Windows");
  charsetCombo_->addItem("Windows CP1258", "Windows CP1258");
  charsetCombo_->addItem("VIQR", "VIQR");
  charsetCombo_->addItem("VPS", "VPS");
  charsetCombo_->addItem("VISCII", "VISCII");
  charsetCombo_->addItem("BKHCM1", "BKHCM1");
  charsetCombo_->addItem("Vietware-F", "Vietware-F");
  charsetCombo_->addItem("ISC", "ISC");
  charsetCombo_->addItem("BKHCM2", "BKHCM2");
  charsetCombo_->addItem("Vietware-X", "Vietware-X");
  charsetCombo_->addItem("VNI-MAC", "VNI-MAC");
  charsetCombo_->addItem("Unicode NFD", "Unicode NFD");
  enumLayout->addRow(T("Bảng mã:"), charsetCombo_);

  languageCombo_ = new QComboBox(enumFrame);
  languageCombo_->addItem(T("Tiếng Việt"), "vi");
  languageCombo_->addItem(T("English"), "en");
  enumLayout->addRow(T("Ngôn ngữ:"), languageCombo_);

  triggerKeyEdit_ = new HotkeyEdit(enumFrame);
  triggerKeyEdit_->setToolTip(
      T("Nhấn tổ hợp phím để thay đổi"));
  enumLayout->addRow(T("Phím chuyển bộ gõ:"), triggerKeyEdit_);

  modeMenuKeyEdit_ = new HotkeyEdit(enumFrame);
  modeMenuKeyEdit_->setToolTip(
      T("Phím tắt để mở menu chế độ (mặc định: `)"));
  enumLayout->addRow(T("Phím menu chế độ:"), modeMenuKeyEdit_);

  mainLayout->addWidget(enumFrame);

  // ── Checkbox section (2 columns) ──
  auto *checkFrame = new QFrame(this);
  checkFrame->setFrameStyle(QFrame::StyledPanel);
  auto *checkLayout = new QGridLayout(checkFrame);
  checkLayout->setHorizontalSpacing(24);
  checkLayout->setVerticalSpacing(4);
  checkLayout->setContentsMargins(12, 12, 12, 12);
  checkLayout->setColumnStretch(0, 1);
  checkLayout->setColumnStretch(1, 1);

  // Telex-only options: 'w'→'ư' and '][' → 'ư'/'ơ'.
  // Enabled only when the current input method is Telex (see below).
  shortWCheck_ = new QCheckBox(T("Gõ w thành ư"), checkFrame);
  shortWCheck_->setToolTip(
      T("Chỉ Telex: gõ phím w đơn lẻ sẽ ra chữ ư."));
  checkLayout->addWidget(shortWCheck_, 0, 0);

  bracketUOCheck_ =
      new QCheckBox(T("Gõ ][ thành ư ơ"), checkFrame);
  bracketUOCheck_->setToolTip(
      T("Chỉ Telex: gõ [ ra ơ và ] ra ư (giống UniKey)."));
  checkLayout->addWidget(bracketUOCheck_, 0, 1);

  freeMarkingCheck_ =
      new QCheckBox(T("Đánh dấu tự do"), checkFrame);
  checkLayout->addWidget(freeMarkingCheck_, 1, 0);

  autoRestoreCheck_ =
      new QCheckBox(T("Tự động khôi phục"), checkFrame);
  checkLayout->addWidget(autoRestoreCheck_, 1, 1);

  dictCheck_ = new QCheckBox(T("Dùng từ điển"), checkFrame);
  dictCheck_->setToolTip(T(
      "Tự động khôi phục kiểm tra từ thật trong từ điển tiếng Việt "
      "thay vì luật âm tiết (khôi phục cả những âm tiết hợp lệ nhưng "
      "không phải từ, ví dụ \"lước\")."));
  checkLayout->addWidget(dictCheck_, 3, 0);

  showPreeditCheck_ =
      new QCheckBox(T("Hiện preedit"), checkFrame);
  checkLayout->addWidget(showPreeditCheck_, 2, 0);

  debugCheck_ = new QCheckBox(T("Ghi log debug"), checkFrame);
  checkLayout->addWidget(debugCheck_, 2, 1);

  mainLayout->addWidget(checkFrame);

  mainLayout->addStretch();

  // Telex-only options are disabled (greyed out) unless Telex is selected.
  auto syncTelexOptions = [this]() {
    bool isTelex =
        inputMethodCombo_->currentData().toString() == QLatin1String("Telex");
    shortWCheck_->setEnabled(isTelex);
    bracketUOCheck_->setEnabled(isTelex);
  };
  connect(inputMethodCombo_,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [syncTelexOptions](int) { syncTelexOptions(); });
  syncTelexOptions();
}

void GeneralTab::loadFromConfig(const SKeyConfig &cfg) {
  auto setCombo = [](QComboBox *c, const std::string &val) {
    int idx = c->findData(QString::fromStdString(val));
    if (idx >= 0)
      c->setCurrentIndex(idx);
  };

  setCombo(inputMethodCombo_, cfg.inputMethod);
  setCombo(outputModeCombo_, cfg.outputMode);
  setCombo(charsetCombo_, cfg.charset);
  setCombo(languageCombo_, cfg.uiLanguage);

  shortWCheck_->setChecked(cfg.shortW);
  bracketUOCheck_->setChecked(cfg.bracketUO);
  freeMarkingCheck_->setChecked(cfg.freeMarking);
  autoRestoreCheck_->setChecked(cfg.autoRestore);
  dictCheck_->setChecked(cfg.dict);
  showPreeditCheck_->setChecked(cfg.showPreedit);

  debugCheck_->setChecked(cfg.debug);
}

SKeyConfig GeneralTab::collectConfig() const {
  SKeyConfig cfg;
  cfg.inputMethod = inputMethodCombo_->currentData().toString().toStdString();
  cfg.outputMode = outputModeCombo_->currentData().toString().toStdString();
  cfg.charset = charsetCombo_->currentData().toString().toStdString();
  cfg.uiLanguage = languageCombo_->currentData().toString().toStdString();
  if (cfg.uiLanguage != "en") cfg.uiLanguage = "vi";
  cfg.shortW = shortWCheck_->isChecked();
  cfg.bracketUO = bracketUOCheck_->isChecked();
  cfg.freeMarking = freeMarkingCheck_->isChecked();
  cfg.autoRestore = autoRestoreCheck_->isChecked();
  cfg.dict = dictCheck_->isChecked();
  cfg.showPreedit = showPreeditCheck_->isChecked();
  cfg.debug = debugCheck_->isChecked();
  cfg.modeMenuKey = modeMenuKeyEdit_->fcitx5Value();
  return cfg;
}

void GeneralTab::setDefaults() {
  loadFromConfig(defaultConfig());
  triggerKeyEdit_->setFcitx5Value("Control+space");
  modeMenuKeyEdit_->setFcitx5Value("grave");
}

std::string GeneralTab::triggerKey() const {
  return triggerKeyEdit_->fcitx5Value();
}

void GeneralTab::setTriggerKey(const std::string &fcitx5Key) {
  triggerKeyEdit_->setFcitx5Value(fcitx5Key);
}

std::string GeneralTab::modeMenuKey() const {
  return modeMenuKeyEdit_->fcitx5Value();
}

void GeneralTab::setModeMenuKey(const std::string &fcitx5Key) {
  modeMenuKeyEdit_->setFcitx5Value(fcitx5Key);
}

