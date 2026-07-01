#ifndef SKEY_SETTINGS_APP_MODES_TAB_H
#define SKEY_SETTINGS_APP_MODES_TAB_H

#include <QWidget>

class QTableWidget;
class QPushButton;

struct AppModesConfig;

class AppModesTab : public QWidget {
    Q_OBJECT
public:
    explicit AppModesTab(QWidget *parent = nullptr);

    void loadFromConfig(const AppModesConfig &cfg);
    AppModesConfig collectConfig() const;
    void setDefaults();

private slots:
    void onAddApp();
    void onDeleteApp();

private:
    void setupUI();
    void addRow(const std::string &name, const std::string &mode);

    QTableWidget *table_;
    QPushButton  *addButton_;
};

#endif // SKEY_SETTINGS_APP_MODES_TAB_H
