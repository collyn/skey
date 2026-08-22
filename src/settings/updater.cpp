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
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVersionNumber>

static const char *kLatestReleaseUrl =
    "https://api.github.com/repos/collyn/skey/releases/latest";
// Dev channel: list releases (newest first, includes prereleases) and scan
// for the newest published dev prerelease.  `releases/latest` always skips
// prereleases, so the stable channel is immune to dev builds by construction.
static const char *kReleasesListUrl =
    "https://api.github.com/repos/collyn/skey/releases?per_page=20";

// ── Distro detection ─────────────────────────────────────────────────────

Distro Updater::detectDistro() {
    // Check package managers — order matters: dnf/rpm first because some
    // Fedora systems may have dpkg installed for cross-build tooling,
    // which would cause a false Debian detection.
    if (!QStandardPaths::findExecutable("dnf").isEmpty() ||
        !QStandardPaths::findExecutable("rpm").isEmpty())
        return Distro::Fedora;
    if (!QStandardPaths::findExecutable("dpkg").isEmpty())
        return Distro::Debian;
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

// Returns true when the RPM filename contains a Fedora dist tag (.fc\d+).
static bool hasFedoraDistTag(const QString &name) {
    // Match patterns like ".fc42." or ".fc42.x86_64.rpm"
    static const QRegularExpression re(R"(\.fc\d+\.)");
    return re.match(name).hasMatch();
}

// ── Dev build version helpers ─────────────────────────────────────────────
// Dev builds carry a monotonic counter in their version string.  The tag
// uses "v0.7.5-dev.123" ('~' is invalid in git refs); deb/rpm package
// versions use "0.7.5~dev.123" and Arch uses "0.7.5.dev.123".  QVersionNumber
// cannot distinguish two dev builds of the same base (it truncates at the
// first non-digit char), so dev→dev comparison must use the counter.
static const QRegularExpression kDevSuffixRe(
    QStringLiteral(R"(^(.*)[-~.]dev\.(\d+)$)"));

/// Dev build counter, or -1 when `version` is not a dev build.
static int devCounterOf(const QString &version) {
    const auto m = kDevSuffixRe.match(version);
    return m.hasMatch() ? m.captured(2).toInt() : -1;
}

/// Base version ("0.7.5"), or the whole string when not a dev build.
static QString devBaseOf(const QString &version) {
    const auto m = kDevSuffixRe.match(version);
    return m.hasMatch() ? m.captured(1) : version;
}

// Pick the best asset for a distro out of a list.
// For Fedora, prefers RPMs with a .fc dist tag (Fedora-built) over generic
// ones (e.g. OpenSUSE-built), since they have the correct Requires.
// Falls back to shorter name when both candidates are equivalent.
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
        QString bestName = best.value("name").toString();

        if (distro == Distro::Fedora) {
            bool curHasFc  = hasFedoraDistTag(name);
            bool bestHasFc = hasFedoraDistTag(bestName);
            // Strongly prefer the Fedora-specific RPM
            if (curHasFc && !bestHasFc) {
                best = asset;
                continue;
            }
            if (!curHasFc && bestHasFc)
                continue;
        }

        // Tie-break: prefer shorter name (less likely to be -debuginfo etc.)
        if (name.size() < bestName.size())
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

void Updater::setChannel(UpdateChannel channel) {
    // Only the pending selection changes; activeChannel_ is snapshotted in
    // checkForUpdate() so a switch mid-request can't corrupt the reply parse.
    channel_ = channel;
}

// ── Check for update ────────────────────────────────────────────────────

void Updater::checkForUpdate() {
    if (checkReply_) {
        // Already checking
        return;
    }

    activeChannel_ = channel_;
    QUrl url{QString::fromUtf8(activeChannel_ == UpdateChannel::Dev
                                   ? kReleasesListUrl : kLatestReleaseUrl)};
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

    if (activeChannel_ == UpdateChannel::Dev) {
        // ── Dev channel ──
        // Scan the release list (API returns newest first, includes
        // prereleases) for the newest published dev build.
        if (!doc.isArray()) {
            emit checkFailed(
                QString::fromUtf8("Phản hồi không hợp lệ từ máy chủ."));
            return;
        }
        const QJsonArray releases = doc.array();
        for (const QJsonValue &v : releases) {
            const QJsonObject rel = v.toObject();
            if (rel.value("draft").toBool(false))
                continue;
            QString tag = rel.value("tag_name").toString();
            if (tag.startsWith('v') || tag.startsWith('V'))
                tag = tag.mid(1);

            const int remoteCounter = devCounterOf(tag);
            if (remoteCounter < 0)
                continue; // not a dev release — keep scanning

            const int currentCounter = devCounterOf(currentVersion_);
            const QVersionNumber remoteBase =
                QVersionNumber::fromString(devBaseOf(tag));
            const QVersionNumber currentBase =
                QVersionNumber::fromString(devBaseOf(currentVersion_));

            bool newer;
            if (remoteBase != currentBase) {
                newer = remoteBase > currentBase;
            } else {
                // Same base: compare build counters.  A stable build has no
                // counter and counts as 0, so a same-base dev build is an
                // offer to a stable user who opts into the dev channel.
                newer = remoteCounter >
                        (currentCounter >= 0 ? currentCounter : 0);
            }
            if (!newer) {
                emit noUpdateAvailable();
                return;
            }

            const QJsonObject best =
                pickBestAsset(rel.value("assets").toArray(), distro_);
            QString downloadUrl;
            if (!best.isEmpty())
                downloadUrl = best.value("browser_download_url").toString();

            emit updateAvailable(tag, downloadUrl,
                                 rel.value("body").toString());
            return;
        }
        emit noUpdateAvailable(); // no dev release published yet
        return;
    }

    // ── Stable channel (releases/latest, prereleases excluded by GitHub) ──
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
        // apt-get resolves dependencies from the repos (e.g. libqt6svg6);
        // plain `dpkg -i` fails with "dependency problems" whenever the
        // new package adds a dependency the system doesn't have yet.
        if (!QStandardPaths::findExecutable("apt-get").isEmpty()) {
            args = {"apt-get", "install", "-y", "--allow-downgrades", pendingPackagePath_};
        } else {
            args = {"dpkg", "-i", pendingPackagePath_};
        }
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
