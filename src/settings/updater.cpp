#include "updater.h"
#include "config_io.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QVersionNumber>

static const char *kLatestReleaseUrl =
    "https://api.github.com/repos/collyn/skey/releases/latest";

Updater::Updater(const QString &currentVersion, QObject *parent)
    : QObject(parent),
      nam_(new QNetworkAccessManager(this)),
      currentVersion_(currentVersion) {}

// ── Check for update ────────────────────────────────────────────────────

void Updater::checkForUpdate() {
    if (checkReply_) {
        // Already checking
        return;
    }

    QUrl url{QString::fromUtf8(kLatestReleaseUrl)};
    QNetworkRequest req{url};
    req.setHeader(QNetworkRequest::UserAgentHeader, "fcitx5-skey-updater");
    req.setRawHeader("Accept", "application/vnd.github+json");

    checkReply_ = nam_->get(req);
    connect(checkReply_, &QNetworkReply::finished,
            this, &Updater::onCheckReplyFinished);
}

void Updater::onCheckReplyFinished() {
    auto *reply = checkReply_;
    checkReply_ = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit checkFailed(reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);

    if (doc.isNull()) {
        emit checkFailed(QString::fromUtf8("Lỗi phân tích JSON: %1")
                             .arg(parseErr.errorString()));
        return;
    }

    QJsonObject root = doc.object();
    QString tagName = root.value("tag_name").toString();

    // Strip leading 'v' from tag (e.g. "v0.1.13" → "0.1.13")
    QString remoteVersion = tagName;
    if (remoteVersion.startsWith('v') || remoteVersion.startsWith('V')) {
        remoteVersion = remoteVersion.mid(1);
    }

    QVersionNumber remote = QVersionNumber::fromString(remoteVersion);
    QVersionNumber current = QVersionNumber::fromString(currentVersion_);

    if (remote <= current) {
        emit noUpdateAvailable();
        return;
    }

    // Find .deb asset
    QString downloadUrl;
    QJsonArray assets = root.value("assets").toArray();
    for (const QJsonValue &v : assets) {
        QJsonObject asset = v.toObject();
        QString name = asset.value("name").toString();
        if (name.endsWith(".deb")) {
            downloadUrl = asset.value("browser_download_url").toString();
            break;
        }
    }

    QString body = root.value("body").toString();

    emit updateAvailable(remoteVersion, downloadUrl, body);
}

// ── Download and install ────────────────────────────────────────────────

void Updater::downloadAndInstall(const QString &downloadUrl,
                                 const QString &version) {
    if (downloadReply_) {
        // Already downloading
        return;
    }

    if (downloadUrl.isEmpty()) {
        emit downloadFailed(
            QString::fromUtf8("Không tìm thấy file .deb để tải."));
        return;
    }

    pendingDebPath_ = QStandardPaths::writableLocation(
                          QStandardPaths::TempLocation) +
                      QString("/fcitx5-skey_%1_amd64.deb").arg(version);

    QUrl url{downloadUrl};
    QNetworkRequest req{url};
    req.setHeader(QNetworkRequest::UserAgentHeader, "fcitx5-skey-updater");
    // Follow redirects (GitHub redirects to CDN)
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    downloadReply_ = nam_->get(req);
    connect(downloadReply_, &QNetworkReply::downloadProgress,
            this, &Updater::onDownloadProgress);
    connect(downloadReply_, &QNetworkReply::finished,
            this, &Updater::onDownloadFinished);
}

void Updater::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    if (bytesTotal > 0) {
        int pct = static_cast<int>(bytesReceived * 100 / bytesTotal);
        emit downloadProgress(pct);
    }
}

void Updater::onDownloadFinished() {
    auto *reply = downloadReply_;
    downloadReply_ = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit downloadFailed(reply->errorString());
        return;
    }

    // Save .deb to temp
    QFile file(pendingDebPath_);
    if (!file.open(QIODevice::WriteOnly)) {
        emit downloadFailed(
            QString::fromUtf8("Không thể ghi file: %1").arg(pendingDebPath_));
        return;
    }
    file.write(reply->readAll());
    file.close();

    emit downloadFinished(pendingDebPath_);

    // Install using pkexec dpkg -i
    emit installStarted();

    auto *proc = new QProcess(this);
    connect(proc,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                &QProcess::finished),
            this, [this, proc](int exitCode, QProcess::ExitStatus) {
                QString errOutput =
                    QString::fromUtf8(proc->readAllStandardError());
                proc->deleteLater();

                if (exitCode == 0) {
                    // Restart fcitx5 to load the new .so, with Wayland
                    // compositor reconnect so virtual keyboard stays bound.
                    restartFcitx5();
                    emit installFinished(
                        true,
                        QString::fromUtf8(
                            "Cập nhật thành công! Fcitx5 đã được "
                            "khởi động lại."));
                } else {
                    emit installFinished(
                        false,
                        QString::fromUtf8("Cài đặt thất bại (mã %1): %2")
                            .arg(exitCode)
                            .arg(errOutput.isEmpty()
                                     ? QString::fromUtf8("Người dùng đã hủy "
                                                         "hoặc lỗi quyền.")
                                     : errOutput));
                }
            });

    proc->start("pkexec", {"dpkg", "-i", pendingDebPath_});
}
