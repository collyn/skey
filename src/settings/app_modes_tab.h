#ifndef SKEY_SETTINGS_APP_MODES_TAB_H
#define SKEY_SETTINGS_APP_MODES_TAB_H

#include <QWidget>

class QTableWidget;
class QPushButton;
class QComboBox;
class QLineEdit;

struct AppModesConfig;

class AppModesTab : public QWidget {
    Q_OBJECT
public:
    explicit AppModesTab(QWidget *parent = nullptr);

    void loadFromConfig(const AppModesConfig &cfg);
    AppModesConfig collectConfig() const;
    void setDefaults();

    std::string chromiumAddressBarMode() const;
    void setChromiumAddressBarMode(const std::string &mode);

private slots:
    void onAddApp();
    void onDeleteApp();
    void onFilterChanged(const QString &text);

private:
    void setupUI();
    void addRow(const std::string &name, const std::string &mode);
    // Resolve icons for rows that don't have one yet (deferred off the
    // startup path; also called after manually adding an app).
    void fillIcons();

    QTableWidget *table_;
    QPushButton  *addButton_;
    QComboBox    *addrBarModeCombo_;
    QLineEdit    *filterEdit_;
};

#endif // SKEY_SETTINGS_APP_MODES_TAB_H
