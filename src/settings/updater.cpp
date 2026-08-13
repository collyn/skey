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

// ── Distro detection ─────────────────────────────────────────────────────

Distro Updater::detectDistro() {
    // Check package managers — order matters: dpkg first because some
    // Fedora systems may have dpkg installed for cross-build tooling.
    if (!QStandardPaths::findExecutable("dpkg").isEmpty())
        return Distro::Debian;
    if (!QStandardPaths::findExecutable("rpm").isEmpty())
        return Distro::Fedora;
    if (!QStandardPaths::findExecutable("pacman").isEmpty())
        return Distro::Arch;
    return Distro::Unknown;
}

// ── Helpers ──────────────────────────────────────────────────────────────

static const char *packageExtension(Distro d) {
    switch (d) {
    case Distro::Debian:  return ".deb";
    case Distro::Fedora:  return ".rpm";
    case Distro::Arch:    return ".pkg.tar.zst";
    case Distro::Unknown: return ".deb"; // fallback
    }
    return ".deb";
}

static const char *distroName(Distro d) {
    switch (d) {
    case Distro::Debian:  return "Debian/Ubuntu";
    case Distro::Fedora:  return "Fedora/RHEL";
    case Distro::Arch:    return "Arch Linux";
    case Distro::Unknown: return "Linux";
    }
    return "Linux";
}

// Returns true when `name` matches the expected asset for `distro`.
// Filters out debuginfo / debugsource RPMs.
static bool assetMatchesDistro(const QString &name, Distro distro) {
    switch (distro) {
    case Distro::Debian:
        return name.endsWith(".deb");
    case Distro::Fedora: {
        if (!name.endsWith(".rpm"))
            return false;
        // Skip debuginfo / debugsource sub-packages
        const QString lower = name.toLower();
        if (lower.contains("debuginfo") || lower.contains("debugsource"))
            return false;
        return true;
    }
    case Distro::Arch:
        return name.endsWith(".pkg.tar.zst") || name.endsWith(".pkg.tar.xz");
    case Distro::Unknown:
        // Try all; prefer .deb for backward compat
        return name.endsWith(".deb") || name.endsWith(".rpm") ||
               name.endsWith(".pkg.tar.zst") || name.endsWith(".pkg.tar.xz");
    }
    return false;
}

// Pick the best asset for a distro out of a list. For Fedora, prefers the
// main package (shorter name) over sub-packages; for all others, returns
// the first match.
static QJsonObject pickBestAsset(const QJsonArray &assets, Distro distro) {
    QJsonObject best;
    for (const QJsonValue &v : assets) {
        QJsonObject asset = v.toObject();
        QString name = asset.value("name").toString();
        if (!assetMatchesDistro(name, distro))
            continue;
        if (best.isEmpty()) {
            best = asset;
            continue;
        }
        // Prefer shorter name (less likely to be a -debuginfo / -devel etc.)
        if (name.size() < best.value("name").toString().size())
            best = asset;
    }
    return best;
}

Updater::Updater(const QString &currentVersion, QObject *parent)
    : QObject(parent),
      nam_(new QNetworkAccessManager(this)),
      currentVersion_(currentVersion) {
    distro_ = detectDistro();
}

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

    // Find the right asset for this distro
    QJsonArray assets = root.value("assets").toArray();
    QJsonObject best = pickBestAsset(assets, distro_);

    QString downloadUrl;
    if (!best.isEmpty()) {
        downloadUrl = best.value("browser_download_url").toString();
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
            QString::fromUtf8("Không tìm thấy file cài đặt phù hợp cho %1.")
                .arg(distroName(distro_)));
        return;
    }

    const char *ext = packageExtension(distro_);
    pendingPackagePath_ = QStandardPaths::writableLocation(
                              QStandardPaths::TempLocation) +
                          QString("/fcitx5-skey_%1_amd64%2").arg(version, ext);

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

    // Save package to temp
    QFile file(pendingPackagePath_);
    if (!file.open(QIODevice::WriteOnly)) {
        emit downloadFailed(
            QString::fromUtf8("Không thể ghi file: %1").arg(pendingPackagePath_));
        return;
    }
    file.write(reply->readAll());
    file.close();

    emit downloadFinished(pendingPackagePath_);

    // Build the install command for this distro
    QStringList args;
    QString program = "pkexec";

    switch (distro_) {
    case Distro::Debian:
        args = {"dpkg", "-i", pendingPackagePath_};
        break;
    case Distro::Fedora:
        // dnf install resolves dependencies automatically
        args = {"dnf", "install", "-y", pendingPackagePath_};
        break;
    case Distro::Arch:
        args = {"pacman", "-U", "--noconfirm", pendingPackagePath_};
        break;
    case Distro::Unknown:
        // Fallback: try dpkg (best-effort)
        args = {"dpkg", "-i", pendingPackagePath_};
        break;
    }

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
                    // No restart here: the package's postinst already
                    // restarted fcitx5 and reconnected KWin (it runs
                    // inside pkexec).  A second restart here would tear
                    // down app text-input connections a second time.
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

    proc->start(program, args);
}
