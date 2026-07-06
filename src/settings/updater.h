#ifndef SKEY_SETTINGS_UPDATER_H
#define SKEY_SETTINGS_UPDATER_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

class Updater : public QObject {
    Q_OBJECT
public:
    explicit Updater(const QString &currentVersion, QObject *parent = nullptr);

    void checkForUpdate();
    void downloadAndInstall(const QString &downloadUrl, const QString &version);

signals:
    void updateAvailable(const QString &newVersion,
                         const QString &downloadUrl,
                         const QString &releaseNotes);
    void noUpdateAvailable();
    void checkFailed(const QString &errorMessage);

    void downloadProgress(int percent);
    void downloadFinished(const QString &debPath);
    void downloadFailed(const QString &errorMessage);

    void installStarted();
    void installFinished(bool success, const QString &message);

private slots:
    void onCheckReplyFinished();
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadFinished();

private:
    QNetworkAccessManager *nam_;
    QString currentVersion_;

    QNetworkReply *checkReply_ = nullptr;
    QNetworkReply *downloadReply_ = nullptr;
    QString pendingDebPath_;
};

#endif // SKEY_SETTINGS_UPDATER_H
