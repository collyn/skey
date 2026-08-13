/**
 * dict_view_dialog.cpp — Full dictionary viewer popup.
 *
 * Shows every word of the built-in Vietnamese dictionary (from the engine)
 * plus the user's personal words (marked with ✦) in 4 compact columns.
 * Filters: live search + "personal words only".  Backed by a plain table
 * model — filtering resets the model without allocating per-cell items.
 */

#include "dict_view_dialog.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTableView>
#include <QVBoxLayout>

// ── DictTableModel ────────────────────────────────────────────────────────

DictTableModel::DictTableModel(const QStringList &words, QObject *parent)
    : QAbstractTableModel(parent), words_(words) {
  refilter();
}

void DictTableModel::setFilter(const QString &text) {
  filter_ = text.trimmed();
  refilter();
}

void DictTableModel::setPersonalOnly(bool personalOnly) {
  personalOnly_ = personalOnly;
  refilter();
}

void DictTableModel::refilter() {
  beginResetModel();
  indices_.clear();
  indices_.reserve(words_.size());
  for (int i = 0; i < words_.size(); ++i) {
    const QString &w = words_.at(i);
    if (personalOnly_ && !w.startsWith(QStringLiteral("✦ "))) continue;
    if (!filter_.isEmpty() &&
        !w.contains(filter_, Qt::CaseInsensitive)) {
      continue;
    }
    indices_.push_back(i);
  }
  endResetModel();
}

int DictTableModel::rowCount(const QModelIndex &) const {
  return (indices_.size() + kCols - 1) / kCols;
}

int DictTableModel::columnCount(const QModelIndex &) const { return kCols; }

QVariant DictTableModel::data(const QModelIndex &index, int role) const {
  if (role != Qt::DisplayRole || !index.isValid()) return {};
  int pos = index.row() * kCols + index.column();
  if (pos < 0 || pos >= indices_.size()) return {};
  return words_.at(indices_[pos]);
}

// ── DictViewDialog ────────────────────────────────────────────────────────

DictViewDialog::DictViewDialog(const QStringList &words, QWidget *parent)
    : QDialog(parent) {
  setWindowTitle(QString::fromUtf8("Toàn bộ từ điển tiếng Việt"));
  resize(880, 640);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(8);

  // ── Search + filter ──
  auto *filterLayout = new QHBoxLayout();
  filterLayout->setSpacing(6);
  searchEdit_ = new QLineEdit(this);
  searchEdit_->setPlaceholderText(QString::fromUtf8("Tìm từ…"));
  filterCombo_ = new QComboBox(this);
  filterCombo_->addItem(QString::fromUtf8("Tất cả từ"));
  filterCombo_->addItem(QString::fromUtf8("Chỉ từ cá nhân (✦)"));
  filterLayout->addWidget(searchEdit_, 1);
  filterLayout->addWidget(filterCombo_);
  layout->addLayout(filterLayout);

  // ── Word table: 4 compact columns ──
  model_ = new DictTableModel(words, this);
  view_ = new QTableView(this);
  view_->setModel(model_);
  view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  view_->setShowGrid(false);
  view_->verticalHeader()->hide();
  view_->verticalHeader()->setDefaultSectionSize(26);
  view_->horizontalHeader()->hide();
  view_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  view_->setFocusPolicy(Qt::NoFocus);
  layout->addWidget(view_, 1);

  // ── Count label ──
  countLabel_ = new QLabel(this);
  countLabel_->setStyleSheet("color: #888; font-size: 11px;");
  layout->addWidget(countLabel_);

  auto updateCount = [this]() {
    countLabel_->setText(
        QString::fromUtf8("%1 từ (✦ = từ cá nhân)").arg(model_->totalCount()));
  };
  updateCount();
  connect(model_, &QAbstractItemModel::modelReset, this,
          [updateCount]() { updateCount(); });

  connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString &t) {
    model_->setFilter(t);
  });
  connect(filterCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
    model_->setPersonalOnly(i == 1);
  });

  searchEdit_->setFocus();
}
