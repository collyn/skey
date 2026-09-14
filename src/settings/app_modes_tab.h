#ifndef SKEY_SETTINGS_APP_MODES_TAB_H
#define SKEY_SETTINGS_APP_MODES_TAB_H

#include <QWidget>

#include <map>
#include <string>

class QTableWidget;
class QPushButton;
class QComboBox;
class QLineEdit;

struct AppModesConfig;
struct AppDelayOverridesConfig;

class AppModesTab : public QWidget {
    Q_OBJECT
public:
    explicit AppModesTab(QWidget *parent = nullptr);

    /// delayOverrides: sanitized key ("app@x"/"app@w") → canonical value
    /// ("auto" or "pace,pre,post") from skey-app-delay-overrides.conf.
    void loadFromConfig(
        const AppModesConfig &cfg,
        const std::map<std::string, std::string> &delayOverrides = {});
    AppModesConfig collectConfig() const;
    AppDelayOverridesConfig collectOverrides() const;
    void setDefaults();

    std::string chromiumAddressBarMode() const;
    void setChromiumAddressBarMode(const std::string &mode);

private slots:
    void onAddApp();
    void onDeleteApp();
    void onEditDelay();
    void onFilterChanged(const QString &text);

private:
    void setupUI();
    void addRow(const std::string &name, const std::string &mode,
                const std::string &delayX11 = {},
                const std::string &delayWayland = {});
    // Resolve icons for rows that don't have one yet (deferred off the
    // startup path; also called after manually adding an app).
    void fillIcons();

    QTableWidget *table_;
    QPushButton  *addButton_;
    QComboBox    *addrBarModeCombo_;
    QLineEdit    *filterEdit_;
};

#endif // SKEY_SETTINGS_APP_MODES_TAB_H
