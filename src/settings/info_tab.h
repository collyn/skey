#ifndef SKEY_SETTINGS_INFO_TAB_H
#define SKEY_SETTINGS_INFO_TAB_H

#include <QWidget>

class QLabel;
class QPushButton;

class InfoTab : public QWidget {
    Q_OBJECT
public:
    explicit InfoTab(QWidget *parent = nullptr);

private slots:
    void onCheckUpdate();
    void onOpenGitHub();

private:
    void setupUI();

    QLabel *versionLabel_;
};

#endif // SKEY_SETTINGS_INFO_TAB_H
