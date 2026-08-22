#ifndef SKEY_SETTINGS_UPDATER_H
#define SKEY_SETTINGS_UPDATER_H

#include <QObject>
#include <QString>

// Defines UpdateChannel (used by the member initializers below).
#include "config_io.h"

class QNetworkAccessManager;
class QNetworkReply;

/// Supported Linux distro families for package management.
enum class Distro { Debian, Fedora, Arch, NixOS, Unknown };

class Updater : public QObject {
    Q_OBJECT
public:
    explicit Updater(const QString &currentVersion, QObject *parent = nullptr);

    void checkForUpdate();
    /// Select which channel checkForUpdate() queries.
    void setChannel(UpdateChannel channel);
    void downloadAndInstall(const QString &downloadUrl, const QString &version);

    /// NixOS: run `nix flake update skey` + `nixos-rebuild switch` via
    /// pkexec (no download).  Stable channel follows the default branch;
    /// dev channel pins the input to the dev prerelease tag and records
    /// `services.fcitx5-skey.devVersion` in configuration.nix so the
    /// rebuilt package carries the dev version string.  Emits
    /// installStarted() then installFinished(), or
    /// nixosManualUpdateRequired() when manual action is needed.
    void rebuildNixos(const QString &version);

    /// Detect which distro family we're running on.
    static Distro detectDistro();
    /// Distro detected when this Updater was constructed.
    Distro distro() const { return distro_; }

signals:
    void updateAvailable(const QString &newVersion,
                         const QString &downloadUrl,
                         const QString &releaseNotes);
    void noUpdateAvailable();
    void checkFailed(const QString &errorMessage);

    void downloadProgress(int percent);
    void downloadFinished(const QString &packagePath);
    void downloadFailed(const QString &errorMessage);

    void installStarted();
    void installFinished(bool success, const QString &message);

    /// NixOS: auto-update cannot proceed (no /etc/nixos/flake.nix, input
    /// not named `skey`, or the nix CLI is missing).  The UI must show
    /// copyable manual commands.
    void nixosManualUpdateRequired();

private slots:
    void onCheckReplyFinished();
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadFinished();

private:
    QNetworkAccessManager *nam_;
    QString currentVersion_;

    UpdateChannel channel_ = UpdateChannel::Stable;
    // Snapshot taken when the check request starts; a channel switch while
    // the reply is in flight must not change how this reply is parsed.
    UpdateChannel activeChannel_ = UpdateChannel::Stable;

    QNetworkReply *checkReply_ = nullptr;
    QNetworkReply *downloadReply_ = nullptr;
    QString pendingPackagePath_;
    Distro distro_ = Distro::Unknown;
};

#endif // SKEY_SETTINGS_UPDATER_H
