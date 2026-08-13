#ifndef SKEY_SETTINGS_DICT_VIEW_DIALOG_H
#define SKEY_SETTINGS_DICT_VIEW_DIALOG_H

#include <QAbstractTableModel>
#include <QDialog>
#include <QStringList>
#include <QVector>

class QComboBox;
class QLabel;
class QLineEdit;
class QTableView;

/// 4-column table model over a word list with a live search filter and a
/// personal-words-only filter.  Filtering only resets the model — the view
/// renders visible cells only, so per-keystroke cost stays O(n) string
/// compares with no item allocation (~8k words ≈ microseconds).
class DictTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    static constexpr int kCols = 4;

    explicit DictTableModel(const QStringList &words, QObject *parent = nullptr);

    void setFilter(const QString &text);
    void setPersonalOnly(bool personalOnly);
    int totalCount() const { return indices_.size(); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

private:
    void refilter();

    const QStringList &words_;
    QString            filter_;
    bool               personalOnly_ = false;
    QVector<int>       indices_; // filtered indices into words_
};

/// Wider popup showing the full dictionary (built-in words plus the
/// user's own words, marked with ✦) in 4 compact columns, with a live
/// search filter and a personal-words-only filter.
class DictViewDialog : public QDialog {
    Q_OBJECT
public:
    explicit DictViewDialog(const QStringList &words,
                            QWidget *parent = nullptr);

private:
    QLineEdit     *searchEdit_;
    QComboBox     *filterCombo_;
    QTableView    *view_;
    DictTableModel *model_;
    QLabel        *countLabel_;
};

#endif // SKEY_SETTINGS_DICT_VIEW_DIALOG_H
