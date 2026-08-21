#ifndef SKEY_SETTINGS_INFO_TAB_H
#define SKEY_SETTINGS_INFO_TAB_H

#include <QWidget>

#include "config_io.h" // UpdateChannel

class QLabel;
class QComboBox;
class QPushButton;
class QProgressBar;
class Updater;

class InfoTab : public QWidget {
    Q_OBJECT
public:
    explicit InfoTab(QWidget *parent = nullptr);

    /// Live update-channel selection (backed by the combo box).
    UpdateChannel updateChannel() const;
    /// Sync the combo to an externally-loaded channel (no config write).
    void setUpdateChannel(UpdateChannel channel);

signals:
    /// Emitted after a config restore so the parent window can reload all tabs.
    void configRestored();

private slots:
    void onCheckUpdate();
    void onChannelChanged(int index);
    void onRestartFcitx5();
    void onBackup();
    void onRestore();

    // Updater slots
    void onUpdateAvailable(const QString &newVersion,
                           const QString &downloadUrl,
                           const QString &releaseNotes);
    void onNoUpdate();
    void onCheckFailed(const QString &errorMessage);
    void onDownloadProgress(int percent);
    void onDownloadFinished(const QString &packagePath);
    void onDownloadFailed(const QString &errorMessage);
    void onInstallStarted();
    void onInstallFinished(bool success, const QString &message);

private:
    void setupUI();

    QLabel *versionLabel_;
    QLabel *statusLabel_;
    QComboBox *channelCombo_ = nullptr;
    QPushButton *updateBtn_;
    QPushButton *restartBtn_;
    QPushButton *backupButton_;
    QPushButton *restoreButton_;
    QProgressBar *progressBar_;
    Updater *updater_;

    // Stored for the "install now" action
    QString pendingDownloadUrl_;
    QString pendingVersion_;
};

#endif // SKEY_SETTINGS_INFO_TAB_H
