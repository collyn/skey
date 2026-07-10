#include "general_tab.h"
#include "hotkey_edit.h"
#include "config_io.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

GeneralTab::GeneralTab(QWidget *parent) : QWidget(parent) {
    setupUI();
}

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
    inputMethodCombo_->addItem("Telex",    "Telex");
    inputMethodCombo_->addItem("VNI",      "VNI");
    enumLayout->addRow(QString::fromUtf8("Kiểu gõ:"), inputMethodCombo_);

    outputModeCombo_ = new QComboBox(enumFrame);
    outputModeCombo_->addItem("Uinput",            "Uinput");
    outputModeCombo_->addItem("Surrounding Text",  "Surrounding Text");
    outputModeCombo_->addItem("Preedit",           "Preedit");
    enumLayout->addRow(QString::fromUtf8("Chế độ xuất:"), outputModeCombo_);

    charsetCombo_ = new QComboBox(enumFrame);
    charsetCombo_->addItem("Unicode",          "Unicode");
    charsetCombo_->addItem("TCVN3 (ABC)",      "TCVN3 (ABC)");
    charsetCombo_->addItem("VNI Windows",      "VNI Windows");
    enumLayout->addRow(QString::fromUtf8("Bảng mã:"), charsetCombo_);


    triggerKeyEdit_ = new HotkeyEdit(enumFrame);
    triggerKeyEdit_->setToolTip(
        QString::fromUtf8("Nhấn tổ hợp phím để thay đổi"));
    enumLayout->addRow(QString::fromUtf8("Phím chuyển bộ gõ:"), triggerKeyEdit_);

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
    shortWCheck_ = new QCheckBox(QString::fromUtf8("Gõ w thành ư"), checkFrame);
    shortWCheck_->setToolTip(
        QString::fromUtf8("Chỉ Telex: gõ phím w đơn lẻ sẽ ra chữ ư."));
    checkLayout->addWidget(shortWCheck_, 0, 0);

    bracketUOCheck_ = new QCheckBox(QString::fromUtf8("Gõ ][ thành ư ơ"), checkFrame);
    bracketUOCheck_->setToolTip(
        QString::fromUtf8("Chỉ Telex: gõ [ ra ơ và ] ra ư (giống UniKey)."));
    checkLayout->addWidget(bracketUOCheck_, 0, 1);

    freeMarkingCheck_ = new QCheckBox(QString::fromUtf8("Đánh dấu tự do"), checkFrame);
    checkLayout->addWidget(freeMarkingCheck_, 1, 0);

    autoRestoreCheck_ = new QCheckBox(QString::fromUtf8("Tự động khôi phục"), checkFrame);
    checkLayout->addWidget(autoRestoreCheck_, 1, 1);

    showPreeditCheck_ = new QCheckBox(QString::fromUtf8("Hiện preedit"), checkFrame);
    checkLayout->addWidget(showPreeditCheck_, 2, 0);

    debugCheck_ = new QCheckBox(QString::fromUtf8("Ghi log debug"), checkFrame);
    checkLayout->addWidget(debugCheck_, 2, 1);

    mainLayout->addWidget(checkFrame);

    // ── Chromium address bar section ──
    auto *addrBarGroup = new QGroupBox(
        QString::fromUtf8("Thanh địa chỉ ở trình duyệt họ chromium"), this);
    auto *addrBarLayout = new QFormLayout(addrBarGroup);
    addrBarLayout->setLabelAlignment(Qt::AlignRight);
    addrBarLayout->setSpacing(6);
    addrBarLayout->setContentsMargins(12, 12, 12, 12);

    addrBarModeCombo_ = new QComboBox(addrBarGroup);
    addrBarModeCombo_->addItem("Uinput", "Uinput");
    addrBarModeCombo_->addItem("Surrounding Text", "Surrounding Text");
    addrBarModeCombo_->addItem("Preedit", "Preedit");
    addrBarModeCombo_->addItem(QString::fromUtf8("Không gõ tiếng Việt"),
                               "No Vietnamese");
    addrBarModeCombo_->setToolTip(
        QString::fromUtf8("Chế độ gõ chỉ áp dụng cho thanh địa chỉ trình duyệt.\n"
                          "Web content vẫn dùng chế độ xuất đã cấu hình."));
    addrBarLayout->addRow(QString::fromUtf8("Chế độ gõ:"), addrBarModeCombo_);

    mainLayout->addWidget(addrBarGroup);
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
        if (idx >= 0) c->setCurrentIndex(idx);
    };

    setCombo(inputMethodCombo_,  cfg.inputMethod);
    setCombo(outputModeCombo_,   cfg.outputMode);
    setCombo(charsetCombo_,      cfg.charset);

    shortWCheck_->setChecked(cfg.shortW);
    bracketUOCheck_->setChecked(cfg.bracketUO);
    freeMarkingCheck_->setChecked(cfg.freeMarking);
    autoRestoreCheck_->setChecked(cfg.autoRestore);
    showPreeditCheck_->setChecked(cfg.showPreedit);

    std::string addressBarMode = cfg.chromiumAddressBarMode;
    if (addressBarMode == "NoVietnamese")
        addressBarMode = "No Vietnamese";
    setCombo(addrBarModeCombo_, addressBarMode);

    debugCheck_->setChecked(cfg.debug);
}

SKeyConfig GeneralTab::collectConfig() const {
    SKeyConfig cfg;
    cfg.inputMethod  = inputMethodCombo_->currentData().toString().toStdString();
    cfg.outputMode   = outputModeCombo_->currentData().toString().toStdString();
    cfg.charset      = charsetCombo_->currentData().toString().toStdString();
    cfg.shortW       = shortWCheck_->isChecked();
    cfg.bracketUO    = bracketUOCheck_->isChecked();
    cfg.freeMarking  = freeMarkingCheck_->isChecked();
    cfg.autoRestore  = autoRestoreCheck_->isChecked();
    cfg.showPreedit  = showPreeditCheck_->isChecked();
    cfg.chromiumAddressBarMode =
        addrBarModeCombo_->currentData().toString().toStdString();
    cfg.debug        = debugCheck_->isChecked();
    return cfg;
}

void GeneralTab::setDefaults() {
    loadFromConfig(defaultConfig());
    triggerKeyEdit_->setFcitx5Value("Control+space");
}

std::string GeneralTab::triggerKey() const {
    return triggerKeyEdit_->fcitx5Value();
}

void GeneralTab::setTriggerKey(const std::string &fcitx5Key) {
    triggerKeyEdit_->setFcitx5Value(fcitx5Key);
}
