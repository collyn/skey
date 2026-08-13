#ifndef SKEY_SETTINGS_DICT_TAB_H
#define SKEY_SETTINGS_DICT_TAB_H

#include <QSet>
#include <QWidget>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;

class DictTab : public QWidget {
    Q_OBJECT
public:
    explicit DictTab(QWidget *parent = nullptr);

    void loadFromConfig(const std::vector<std::string> &words);
    std::vector<std::string> collectConfig() const;
    void setDefaults();

private slots:
    void onAdd();
    void onEditWord(const std::string &word);
    void onDeleteWord(const std::string &word);
    void onDeleteSelected();
    void onToggleSelectAll();
    void onViewAll();

private:
    void setupUI();
    void addRow(const std::string &word);
    /// Rebuild the visible rows from words_ + the current search filter.
    /// Debounced via rebuildTimer_ for keystroke-driven changes.
    void rebuildList();
    void scheduleRebuild();
    void updateDeleteSelected();

    QListWidget              *list_;
    QLineEdit                *searchEdit_;
    QLineEdit                *wordEdit_;
    QPushButton              *addButton_;   // "Thêm" / "Cập nhật"
    QPushButton              *selectAllButton_;
    QPushButton              *deleteSelectedButton_;
    QPushButton              *viewAllButton_;
    QLabel                   *hintLabel_;
    QTimer                   *rebuildTimer_;
    std::vector<std::string>  words_;       // source of truth
    std::string               editingWord_; // word being edited, empty = add
    QSet<QString>             checkedWords_; // multi-select deletion state
    bool                      updatingList_ = false;
};

#endif // SKEY_SETTINGS_DICT_TAB_H
