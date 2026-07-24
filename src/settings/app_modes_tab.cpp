#include "app_modes_tab.h"
#include "config_io.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

// ── Mode values as displayed in the per-app config ─────────────────────
static const char *kAppModeValues[] = {"Auto", "Uinput", "SurroundingText", "Preedit", "Excluded", nullptr};

AppModesTab::AppModesTab(QWidget *parent) : QWidget(parent) {
    setupUI();
}

void AppModesTab::setupUI() {
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    // ── Label ──
    auto *label = new QLabel(
        QString::fromUtf8("Cấu hình chế độ xuất riêng cho từng ứng dụng.\n"
                          "Tên ứng dụng là tên file thực thi (vd: firefox, tabby, code)."),
        this);
    label->setWordWrap(true);
    mainLayout->addWidget(label);

    // ── Table ──
    table_ = new QTableWidget(0, 3, this);
    table_->setHorizontalHeaderLabels({
        QString::fromUtf8("Tên ứng dụng"),
        QString::fromUtf8("Chế độ xuất"),
        QString::fromUtf8("Xóa")});
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    mainLayout->addWidget(table_);

    // ── Buttons row ──
    auto *btnRow = new QHBoxLayout();
    addButton_ = new QPushButton(QString::fromUtf8("Thêm ứng dụng"), this);
    btnRow->addWidget(addButton_);
    btnRow->addStretch();
    mainLayout->addLayout(btnRow);

    connect(addButton_, &QPushButton::clicked, this, &AppModesTab::onAddApp);
}

// ── Add a single row to the table ──────────────────────────────────────
void AppModesTab::addRow(const std::string &name, const std::string &mode) {
    int row = table_->rowCount();
    table_->insertRow(row);

    // Column 0 — read-only app name.
    // IBus frontend reports empty program name (AppImages like Viber).
    // Display a readable label but keep the real key for save/load.
    QString displayName = name.empty()
        ? QString::fromUtf8("(IBus app)")
        : QString::fromStdString(name);
    auto *nameItem = new QTableWidgetItem(displayName);
    nameItem->setData(Qt::UserRole, QString::fromStdString(name)); // real key
    nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, 0, nameItem);

    // Column 1 — mode combobox
    auto *combo = new QComboBox(table_);
    for (int i = 0; kAppModeValues[i]; ++i) {
        combo->addItem(kAppModeValues[i], kAppModeValues[i]);
    }
    int idx = combo->findData(QString::fromStdString(mode));
    if (idx >= 0) combo->setCurrentIndex(idx);
    table_->setCellWidget(row, 1, combo);

    // Column 2 — delete button
    auto *delBtn = new QPushButton(QString::fromUtf8("Xóa"), table_);
    connect(delBtn, &QPushButton::clicked, this, &AppModesTab::onDeleteApp);
    table_->setCellWidget(row, 2, delBtn);
}

// ── Load from config ────────────────────────────────────────────────────
void AppModesTab::loadFromConfig(const AppModesConfig &cfg) {
    table_->setRowCount(0);
    for (auto &[name, mode] : cfg.entries) {
        addRow(name, mode);
    }
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
    dlg.setWindowTitle(QString::fromUtf8("Thêm ứng dụng mới"));
    dlg.setFixedSize(340, 150);

    auto *layout = new QFormLayout(&dlg);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);

    auto *nameEdit = new QLineEdit(&dlg);
    nameEdit->setPlaceholderText(QString::fromUtf8("vd: firefox, tabby, code"));
    layout->addRow(QString::fromUtf8("Tên ứng dụng:"), nameEdit);

    auto *modeCombo = new QComboBox(&dlg);
    for (int i = 0; kAppModeValues[i]; ++i) {
        modeCombo->addItem(kAppModeValues[i], kAppModeValues[i]);
    }
    modeCombo->setCurrentIndex(0); // default Uinput
    layout->addRow(QString::fromUtf8("Chế độ xuất:"), modeCombo);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QString::fromUtf8("Thêm"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QString::fromUtf8("Hủy"));
    layout->addRow(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        std::string name = nameEdit->text().trimmed().toStdString();
        std::string mode = modeCombo->currentData().toString().toStdString();
        if (!name.empty()) {
            addRow(name, mode);
        }
    }
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
