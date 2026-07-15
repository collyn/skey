#ifndef SKEY_SETTINGS_INFO_TAB_H
#define SKEY_SETTINGS_INFO_TAB_H

#include <QWidget>

class QLabel;
class QPushButton;
class QProgressBar;
class Updater;

class InfoTab : public QWidget {
    Q_OBJECT
public:
    explicit InfoTab(QWidget *parent = nullptr);

private slots:
    void onCheckUpdate();
    void onOpenGitHub();
    void onRestartFcitx5();

    // Updater slots
    void onUpdateAvailable(const QString &newVersion,
                           const QString &downloadUrl,
                           const QString &releaseNotes);
    void onNoUpdate();
    void onCheckFailed(const QString &errorMessage);
    void onDownloadProgress(int percent);
    void onDownloadFinished(const QString &debPath);
    void onDownloadFailed(const QString &errorMessage);
    void onInstallStarted();
    void onInstallFinished(bool success, const QString &message);

private:
    void setupUI();

    QLabel *versionLabel_;
    QLabel *statusLabel_;
    QPushButton *updateBtn_;
    QPushButton *restartBtn_;
    QProgressBar *progressBar_;
    Updater *updater_;

    // Stored for the "install now" action
    QString pendingDownloadUrl_;
    QString pendingVersion_;
};

#endif // SKEY_SETTINGS_INFO_TAB_H
