/**
 * dict_tab.cpp — User dictionary editor for the settings GUI.
 *
 * Edits ~/.local/share/fcitx5/skey/user-dict.txt — one word per line.
 * The addon merges these words into the built-in Vietnamese dictionary
 * when "Dùng từ điển" (General tab) is enabled, so words the dictionary
 * lacks are not auto-restored while typing.  A wider popup shows the
 * complete dictionary.  words_ is the source of truth; the visible list
 * is a filtered view rebuilt on demand (debounced for search input).
 */

#include "dict_tab.h"
#include "config_io.h"
#include "dict_view_dialog.h"
#include "tr.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "skey_engine.h"

DictTab::DictTab(QWidget *parent) : QWidget(parent) { setupUI(); }

void DictTab::setupUI() {
  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(8, 8, 8, 8);
  mainLayout->setSpacing(8);

  auto *frame = new QGroupBox(T("Từ điển cá nhân"), this);
  auto *layout = new QVBoxLayout(frame);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(8);

  // ── Add / edit row ──
  auto *addLayout = new QHBoxLayout();
  addLayout->setSpacing(6);
  wordEdit_ = new QLineEdit(frame);
  wordEdit_->setPlaceholderText(
      T("Thêm từ mới (ví dụ: phược)"));
  addButton_ = new QPushButton(T("Thêm"), frame);
  addLayout->addWidget(wordEdit_, 1);
  addLayout->addWidget(addButton_);
  layout->addLayout(addLayout);

  // ── Personal search ──
  searchEdit_ = new QLineEdit(frame);
  searchEdit_->setPlaceholderText(T("Tìm từ cá nhân…"));
  layout->addWidget(searchEdit_);

  // ── Word list (per-row edit/delete buttons + checkboxes) ──
  list_ = new QListWidget(frame);
  list_->setSelectionMode(QAbstractItemView::NoSelection);
  list_->setFocusPolicy(Qt::NoFocus);
  layout->addWidget(list_, 1);

  // ── Multi-select delete + view full dictionary ──
  auto *bottomLayout = new QHBoxLayout();
  bottomLayout->setSpacing(6);
  selectAllButton_ = new QPushButton(T("Chọn tất cả"), frame);
  deleteSelectedButton_ =
      new QPushButton(T("Xóa từ đã chọn"), frame);
  deleteSelectedButton_->setEnabled(false);
  viewAllButton_ = new QPushButton(T("Xem từ điển"), frame);
  bottomLayout->addWidget(selectAllButton_);
  bottomLayout->addWidget(deleteSelectedButton_);
  bottomLayout->addWidget(viewAllButton_, 1);
  layout->addLayout(bottomLayout);

  mainLayout->addWidget(frame);

  // ── Hint ──
  hintLabel_ =
      new QLabel(T(
                     "Các từ này được thêm vào từ điển tiếng Việt khi bật\n"
                     "\"Dùng từ điển\" ở tab Chung — những từ không có trong\n"
                     "từ điển gốc sẽ không bị tự động khôi phục khi gõ.\n"
                     "Nhấn Áp dụng để lưu."),
                 this);
  hintLabel_->setStyleSheet("color: #888; font-size: 11px;");
  mainLayout->addWidget(hintLabel_);

  // Debounced rebuild for search keystrokes.
  rebuildTimer_ = new QTimer(this);
  rebuildTimer_->setSingleShot(true);
  rebuildTimer_->setInterval(120);
  connect(rebuildTimer_, &QTimer::timeout, this, &DictTab::rebuildList);

  // ── Connections ──
  connect(addButton_, &QPushButton::clicked, this, &DictTab::onAdd);
  connect(wordEdit_, &QLineEdit::returnPressed, this, &DictTab::onAdd);
  connect(viewAllButton_, &QPushButton::clicked, this, &DictTab::onViewAll);
  connect(selectAllButton_, &QPushButton::clicked, this,
          &DictTab::onToggleSelectAll);
  connect(deleteSelectedButton_, &QPushButton::clicked, this,
          &DictTab::onDeleteSelected);
  connect(searchEdit_, &QLineEdit::textChanged, this,
          [this](const QString &) { scheduleRebuild(); });
}

void DictTab::loadFromConfig(const std::vector<std::string> &words) {
  words_ = words;
  rebuildList();
  wordEdit_->clear();
  editingWord_.clear();
  addButton_->setText(T("Thêm"));
}

std::vector<std::string> DictTab::collectConfig() const { return words_; }

void DictTab::setDefaults() {
  words_.clear();
  rebuildList();
  wordEdit_->clear();
  editingWord_.clear();
  addButton_->setText(T("Thêm"));
}

// ── Row management ────────────────────────────────────────────────────────

void DictTab::scheduleRebuild() { rebuildTimer_->start(); }

void DictTab::rebuildList() {
  rebuildTimer_->stop();
  QString filter = searchEdit_->text().trimmed();

  updatingList_ = true;
  list_->setUpdatesEnabled(false);
  list_->clear();
  for (const auto &w : words_) {
    if (!filter.isEmpty() &&
        !QString::fromStdString(w).contains(filter, Qt::CaseInsensitive)) {
      continue;
    }
    addRow(w);
  }
  list_->setUpdatesEnabled(true);
  updatingList_ = false;
  updateDeleteSelected();
}

void DictTab::addRow(const std::string &word) {
  QString qword = QString::fromStdString(word);
  auto *item = new QListWidgetItem();
  // Keep the item's own text empty — the delegate paints it behind the
  // row widget, which would show the word twice.  Store the word in
  // Qt::UserRole instead.
  item->setData(Qt::UserRole, qword);
  // Valid width is required — QSize(-1, h) is treated as invalid by Qt
  // and ignored, which would clip the row widget.
  item->setSizeHint(QSize(64, 40));

  auto *row = new QWidget();
  auto *rowLayout = new QHBoxLayout(row);
  rowLayout->setContentsMargins(6, 2, 6, 2);
  rowLayout->setSpacing(6);

  // Real checkbox inside the row widget — an item-level check indicator
  // would be painted behind the widget and stay unclickable.
  auto *check = new QCheckBox(row);
  check->setChecked(checkedWords_.contains(qword));
  connect(check, &QCheckBox::toggled, this, [this, word](bool on) {
    if (updatingList_)
      return;
    if (on) {
      checkedWords_.insert(QString::fromStdString(word));
    } else {
      checkedWords_.remove(QString::fromStdString(word));
    }
    rebuildList(); // refresh Sửa enable state + button labels
  });
  rowLayout->addWidget(check);

  auto *label = new QLabel(qword, row);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  rowLayout->addWidget(label, 1);

  auto *editBtn = new QPushButton(T("Sửa"), row);
  editBtn->setFixedSize(56, 28);
  // Multi-select mode disables editing.
  editBtn->setEnabled(checkedWords_.isEmpty());
  auto *delBtn = new QPushButton(T("Xóa"), row);
  delBtn->setFixedSize(56, 28);
  rowLayout->addWidget(editBtn);
  rowLayout->addWidget(delBtn);

  connect(editBtn, &QPushButton::clicked, this,
          [this, word]() { onEditWord(word); });
  connect(delBtn, &QPushButton::clicked, this,
          [this, word]() { onDeleteWord(word); });

  list_->addItem(item);
  list_->setItemWidget(item, row);
}

void DictTab::updateDeleteSelected() {
  QString filter = searchEdit_->text().trimmed();
  int n = 0; // checked and visible
  int visible = 0;
  for (const auto &w : words_) {
    QString q = QString::fromStdString(w);
    if (!filter.isEmpty() && !q.contains(filter, Qt::CaseInsensitive)) {
      continue;
    }
    ++visible;
    if (checkedWords_.contains(q))
      ++n;
  }
  deleteSelectedButton_->setEnabled(n > 0);
  deleteSelectedButton_->setText(
      n > 0 ? T("Xóa từ đã chọn (%1)").arg(n)
            : T("Xóa từ đã chọn"));
  selectAllButton_->setText(visible > 0 && n == visible
                                ? T("Bỏ chọn tất cả")
                                : T("Chọn tất cả"));
}

// ── Slots ─────────────────────────────────────────────────────────────────

void DictTab::onAdd() {
  QString word = wordEdit_->text().trimmed();
  wordEdit_->clear();
  if (word.isEmpty())
    return;

  // Skip duplicates (case-insensitive), except the word being edited.
  for (const auto &w : words_) {
    if (QString::fromStdString(w).compare(word, Qt::CaseInsensitive) == 0) {
      if (!editingWord_.empty() && w == editingWord_) {
        continue; // the word being edited may match itself
      }
      return; // real duplicate
    }
  }

  if (!editingWord_.empty()) {
    std::replace(words_.begin(), words_.end(), editingWord_,
                 word.toStdString());
    editingWord_.clear();
    addButton_->setText(T("Thêm"));
  } else {
    words_.push_back(word.toStdString());
  }
  rebuildList();
  list_->scrollToBottom();
}

void DictTab::onEditWord(const std::string &word) {
  wordEdit_->setText(QString::fromStdString(word));
  editingWord_ = word;
  addButton_->setText(T("Cập nhật"));
  wordEdit_->setFocus();
}

void DictTab::onDeleteWord(const std::string &word) {
  words_.erase(std::remove(words_.begin(), words_.end(), word), words_.end());
  checkedWords_.remove(QString::fromStdString(word));
  // If the word being edited was removed, leave edit mode.
  if (editingWord_ == word) {
    editingWord_.clear();
    addButton_->setText(T("Thêm"));
    wordEdit_->clear();
  }
  rebuildList();
}

void DictTab::onDeleteSelected() {
  if (checkedWords_.isEmpty())
    return;
  for (const auto &q : checkedWords_) {
    std::string w = q.toStdString();
    words_.erase(std::remove(words_.begin(), words_.end(), w), words_.end());
    if (editingWord_ == w) {
      editingWord_.clear();
      addButton_->setText(T("Thêm"));
      wordEdit_->clear();
    }
  }
  checkedWords_.clear();
  rebuildList();
}

void DictTab::onToggleSelectAll() {
  QString filter = searchEdit_->text().trimmed();
  bool allVisibleChecked = true;
  int visible = 0;
  for (const auto &w : words_) {
    QString q = QString::fromStdString(w);
    if (!filter.isEmpty() && !q.contains(filter, Qt::CaseInsensitive)) {
      continue;
    }
    ++visible;
    if (!checkedWords_.contains(q)) {
      allVisibleChecked = false;
      break;
    }
  }
  if (visible == 0)
    return;
  for (const auto &w : words_) {
    QString q = QString::fromStdString(w);
    if (!filter.isEmpty() && !q.contains(filter, Qt::CaseInsensitive)) {
      continue;
    }
    if (allVisibleChecked) {
      checkedWords_.remove(q); // uncheck all visible
    } else {
      checkedWords_.insert(q); // check all visible
    }
  }
  rebuildList();
}

void DictTab::onViewAll() {
  // Merge: user words first (marked ✦), then the full built-in list.
  QStringList words;
  for (const auto &w : words_) {
    words << QString::fromUtf8("✦ ") + QString::fromStdString(w);
  }
  char *all = skey_engine_dict_words();
  if (all) {
    QString allStr = QString::fromUtf8(all);
    skey_free_string(all);
    words += allStr.split('\n', Qt::SkipEmptyParts);
  }
  DictViewDialog dlg(words, this);
  dlg.exec();
}
