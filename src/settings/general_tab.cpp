#include "general_tab.h"
#include "hotkey_edit.h"
#include "config_io.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
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
    inputMethodCombo_->addItem("Telex W",  "Telex W");
    enumLayout->addRow(QString::fromUtf8("Kiểu gõ:"), inputMethodCombo_);

    outputModeCombo_ = new QComboBox(enumFrame);
    outputModeCombo_->addItem("Uinput",            "Uinput");
    outputModeCombo_->addItem("Surrounding Text",  "Surrounding Text");
    outputModeCombo_->addItem("Preedit",           "Preedit");
    enumLayout->addRow(QString::fromUtf8("Chế độ xuất:"), outputModeCombo_);


    triggerKeyEdit_ = new HotkeyEdit(enumFrame);
    triggerKeyEdit_->setToolTip(
        QString::fromUtf8("Nhấn tổ hợp phím để thay đổi"));
    enumLayout->addRow(QString::fromUtf8("Phím chuyển bộ gõ:"), triggerKeyEdit_);

    mainLayout->addWidget(enumFrame);

    // ── Checkbox section ──
    auto *checkFrame = new QFrame(this);
    checkFrame->setFrameStyle(QFrame::StyledPanel);
    auto *checkLayout = new QVBoxLayout(checkFrame);
    checkLayout->setSpacing(4);
    checkLayout->setContentsMargins(12, 12, 12, 12);

    freeMarkingCheck_ = new QCheckBox(QString::fromUtf8("Đánh dấu tự do"), checkFrame);
    checkLayout->addWidget(freeMarkingCheck_);

    autoRestoreCheck_ = new QCheckBox(QString::fromUtf8("Tự động khôi phục"), checkFrame);
    checkLayout->addWidget(autoRestoreCheck_);

    showPreeditCheck_ = new QCheckBox(QString::fromUtf8("Hiện preedit"), checkFrame);
    checkLayout->addWidget(showPreeditCheck_);

    chromiumAddressBarPreeditCheck_ = new QCheckBox(
        QString::fromUtf8("Tự động Preedit ở thanh địa chỉ"), checkFrame);
    chromiumAddressBarPreeditCheck_->setToolTip(
        QString::fromUtf8("Tự động chuyển sang chế độ Preedit khi gõ ở thanh\n"
                          "địa chỉ trình duyệt (Chrome, Chromium, Edge...).\n"
                          "Web content vẫn dùng chế độ gõ đã cấu hình."));
    checkLayout->addWidget(chromiumAddressBarPreeditCheck_);

    debugCheck_ = new QCheckBox(QString::fromUtf8("Ghi log debug"), checkFrame);
    checkLayout->addWidget(debugCheck_);

    mainLayout->addWidget(checkFrame);
    mainLayout->addStretch();
}

void GeneralTab::loadFromConfig(const SKeyConfig &cfg) {
    auto setCombo = [](QComboBox *c, const std::string &val) {
        int idx = c->findData(QString::fromStdString(val));
        if (idx >= 0) c->setCurrentIndex(idx);
    };

    setCombo(inputMethodCombo_,  cfg.inputMethod);
    setCombo(outputModeCombo_,   cfg.outputMode);

    freeMarkingCheck_->setChecked(cfg.freeMarking);
    autoRestoreCheck_->setChecked(cfg.autoRestore);
    showPreeditCheck_->setChecked(cfg.showPreedit);
    chromiumAddressBarPreeditCheck_->setChecked(cfg.chromiumAddressBarPreedit);
    debugCheck_->setChecked(cfg.debug);
}

SKeyConfig GeneralTab::collectConfig() const {
    SKeyConfig cfg;
    cfg.inputMethod  = inputMethodCombo_->currentData().toString().toStdString();
    cfg.outputMode   = outputModeCombo_->currentData().toString().toStdString();
    cfg.freeMarking  = freeMarkingCheck_->isChecked();
    cfg.autoRestore  = autoRestoreCheck_->isChecked();
    cfg.showPreedit  = showPreeditCheck_->isChecked();
    cfg.chromiumAddressBarPreedit = chromiumAddressBarPreeditCheck_->isChecked();
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
