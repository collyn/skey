#include "general_tab.h"
#include "config_io.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QFrame>

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

    tonePositionCombo_ = new QComboBox(enumFrame);
    tonePositionCombo_->addItem(QString::fromUtf8("Modern (hoà)"),
                                QString::fromUtf8("Modern (hoà)"));
    tonePositionCombo_->addItem(QString::fromUtf8("Traditional (hòa)"),
                                QString::fromUtf8("Traditional (hòa)"));
    enumLayout->addRow(QString::fromUtf8("Dấu thanh:"), tonePositionCombo_);

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
    setCombo(tonePositionCombo_, cfg.tonePosition);

    freeMarkingCheck_->setChecked(cfg.freeMarking);
    autoRestoreCheck_->setChecked(cfg.autoRestore);
    showPreeditCheck_->setChecked(cfg.showPreedit);
    debugCheck_->setChecked(cfg.debug);
}

SKeyConfig GeneralTab::collectConfig() const {
    SKeyConfig cfg;
    cfg.inputMethod  = inputMethodCombo_->currentData().toString().toStdString();
    cfg.outputMode   = outputModeCombo_->currentData().toString().toStdString();
    cfg.tonePosition = tonePositionCombo_->currentData().toString().toStdString();
    cfg.freeMarking  = freeMarkingCheck_->isChecked();
    cfg.autoRestore  = autoRestoreCheck_->isChecked();
    cfg.showPreedit  = showPreeditCheck_->isChecked();
    cfg.debug        = debugCheck_->isChecked();
    return cfg;
}

void GeneralTab::setDefaults() {
    loadFromConfig(defaultConfig());
}
