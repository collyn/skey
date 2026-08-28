#include "info_tab.h"
#include "config_io.h"
#include "tr.h"
#include "updater.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#ifndef SKEY_VERSION
#define SKEY_VERSION "0.1.1"
#endif

static const char *kGitHubUrl = "https://github.com/collyn/skey";

InfoTab::InfoTab(QWidget *parent) : QWidget(parent) {
  updater_ = new Updater(SKEY_VERSION, this);
  updater_->setChannel(readSkeyConfig().updateChannel);

  // Connect updater signals
  connect(updater_, &Updater::updateAvailable, this,
          &InfoTab::onUpdateAvailable);
  connect(updater_, &Updater::noUpdateAvailable, this, &InfoTab::onNoUpdate);
  connect(updater_, &Updater::checkFailed, this, &InfoTab::onCheckFailed);
  connect(updater_, &Updater::downloadProgress, this,
          &InfoTab::onDownloadProgress);
  connect(updater_, &Updater::downloadFinished, this,
          &InfoTab::onDownloadFinished);
  connect(updater_, &Updater::downloadFailed, this, &InfoTab::onDownloadFailed);
  connect(updater_, &Updater::installStarted, this, &InfoTab::onInstallStarted);
  connect(updater_, &Updater::installFinished, this,
          &InfoTab::onInstallFinished);
  connect(updater_, &Updater::nixosManualUpdateRequired, this,
          &InfoTab::onNixosManualUpdateRequired);

  setupUI();
}

void InfoTab::setupUI() {
  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(16, 16, 16, 16);
  mainLayout->setSpacing(12);

  // ── Icon ──
  auto *iconLabel = new QLabel(this);
  iconLabel->setFixedSize(80, 80);
  iconLabel->setAlignment(Qt::AlignCenter);
  iconLabel->setStyleSheet(
      "QLabel { border-radius: 12px; background: transparent; }");
  // Config-driven icon with fallback to the packaged default.
  QIcon icon(QString::fromStdString(effectiveIconPath(readSkeyConfig())));
  if (icon.isNull())
    icon = QIcon(FCITX_SKEY_ICON_PATH);
  if (!icon.isNull()) {
    // QIcon::pixmap() treats its size argument as device-independent and
    // returns a pixmap already scaled by the app's devicePixelRatio, so ask
    // for 80×80 and let Qt do the scaling.  Start `rounded` from src so
    // both share physical size and dpr — the draw below is then identity.
    // The old code multiplied by dpr by hand (80 * dpr + an explicit
    // source rect), double-scaling the icon so it overflowed the rounded
    // tile at fractional scaling (e.g. 125%).
    QPixmap src = icon.pixmap(80, 80);
    QPixmap rounded = src;
    rounded.fill(Qt::transparent);
    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(QPointF(0, 0), rounded.deviceIndependentSize()),
                        12, 12);
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, src);
    painter.end();
    iconLabel->setPixmap(rounded);
  } else {
    iconLabel->setText(QString::fromUtf8("🇻🇳"));
    iconLabel->setStyleSheet("font-size: 48px;");
  }
  mainLayout->addWidget(iconLabel, 0, Qt::AlignHCenter);

  // ── Version ──
  versionLabel_ =
      new QLabel(T("SKey - Phiên bản: ") + SKEY_VERSION, this);
  versionLabel_->setAlignment(Qt::AlignCenter);
  versionLabel_->setStyleSheet("font-size: 13px;");
  mainLayout->addWidget(versionLabel_);

  // ── Separator ──
  auto *sep1 = new QFrame(this);
  sep1->setFrameShape(QFrame::HLine);
  sep1->setFrameShadow(QFrame::Sunken);
  mainLayout->addWidget(sep1);

  // ── GitHub link ──
  auto *linkLabel = new QLabel(this);
  linkLabel->setText(
      T("<a href=\"%1\">%1</a>").arg(kGitHubUrl));
  linkLabel->setTextFormat(Qt::RichText);
  linkLabel->setOpenExternalLinks(true);
  linkLabel->setAlignment(Qt::AlignCenter);
  linkLabel->setCursor(Qt::PointingHandCursor);
  mainLayout->addWidget(linkLabel);

  // ── Update channel ──
  // Switching the channel takes effect immediately, then Check Update
  // queries the selected channel.
  auto *channelRow = new QHBoxLayout();
  channelRow->setSpacing(8);
  auto *channelLabel = new QLabel(T("Kênh cập nhật:"), this);
  channelCombo_ = new QComboBox(this);
  channelCombo_->addItem(T("Ổn định (Stable)"),
                         static_cast<int>(UpdateChannel::Stable));
  channelCombo_->addItem(T("Thử nghiệm (Dev)"),
                         static_cast<int>(UpdateChannel::Dev));
  channelCombo_->setCurrentIndex(
      readSkeyConfig().updateChannel == UpdateChannel::Dev ? 1 : 0);
  connect(channelCombo_, &QComboBox::currentIndexChanged, this,
          &InfoTab::onChannelChanged);
  channelRow->addStretch();
  channelRow->addWidget(channelLabel);
  channelRow->addWidget(channelCombo_);
  channelRow->addStretch();
  mainLayout->addLayout(channelRow);

  // ── Buttons row ──
  auto *btnRow = new QHBoxLayout();
  btnRow->setSpacing(8);

  updateBtn_ = new QPushButton(T("Check Update"), this);
  connect(updateBtn_, &QPushButton::clicked, this, &InfoTab::onCheckUpdate);

  restartBtn_ = new QPushButton(T("Restart Fcitx5"), this);
  connect(restartBtn_, &QPushButton::clicked, this, &InfoTab::onRestartFcitx5);

  btnRow->addStretch();
  btnRow->addWidget(updateBtn_);
  btnRow->addWidget(restartBtn_);
  btnRow->addStretch();
  mainLayout->addLayout(btnRow);

  // ── Status label (hidden by default) ──
  statusLabel_ = new QLabel(this);
  statusLabel_->setAlignment(Qt::AlignCenter);
  statusLabel_->setStyleSheet("font-size: 12px; color: #666;");
  statusLabel_->hide();
  mainLayout->addWidget(statusLabel_);

  // ── Progress bar (hidden by default) ──
  progressBar_ = new QProgressBar(this);
  progressBar_->setRange(0, 100);
  progressBar_->setValue(0);
  progressBar_->setTextVisible(true);
  progressBar_->hide();
  mainLayout->addWidget(progressBar_);

  // ── Separator ──
  auto *sep2 = new QFrame(this);
  sep2->setFrameShape(QFrame::HLine);
  sep2->setFrameShadow(QFrame::Sunken);
  mainLayout->addWidget(sep2);

  // ── Contact info ──
  auto *contactTitle = new QLabel(T("Liên hệ"), this);
  contactTitle->setAlignment(Qt::AlignCenter);
  contactTitle->setStyleSheet("font-weight: bold; font-size: 13px;");
  mainLayout->addWidget(contactTitle);

  auto *authorLabel = new QLabel(T("Nguyễn Tiến Huy"), this);
  authorLabel->setAlignment(Qt::AlignCenter);
  mainLayout->addWidget(authorLabel);

  auto *telegramLabel = new QLabel(this);
  telegramLabel->setText(T(
      "<a href=\"https://t.me/+irlw1EnOtAkxNDc1\">Telegram Group</a>"));
  telegramLabel->setTextFormat(Qt::RichText);
  telegramLabel->setOpenExternalLinks(true);
  telegramLabel->setAlignment(Qt::AlignCenter);
  telegramLabel->setCursor(Qt::PointingHandCursor);
  mainLayout->addWidget(telegramLabel);

  // ── Backup / Restore ──
  auto *sep3 = new QFrame(this);
  sep3->setFrameShape(QFrame::HLine);
  sep3->setFrameShadow(QFrame::Sunken);
  mainLayout->addWidget(sep3);

  auto *backupFrame = new QFrame(this);
  backupFrame->setFrameStyle(QFrame::StyledPanel);
  auto *backupLayout = new QHBoxLayout(backupFrame);
  backupLayout->setContentsMargins(12, 10, 12, 10);
  backupLayout->setSpacing(8);

  auto *backupLabel =
      new QLabel(T("Backup / Restore:"), backupFrame);
  backupLayout->addWidget(backupLabel);
  backupLayout->addStretch();

  backupButton_ = new QPushButton(T("Sao lưu"), backupFrame);
  backupButton_->setMinimumWidth(90);
  connect(backupButton_, &QPushButton::clicked, this, &InfoTab::onBackup);
  backupLayout->addWidget(backupButton_);

  restoreButton_ = new QPushButton(T("Khôi phục"), backupFrame);
  restoreButton_->setMinimumWidth(90);
  connect(restoreButton_, &QPushButton::clicked, this, &InfoTab::onRestore);
  backupLayout->addWidget(restoreButton_);

  mainLayout->addWidget(backupFrame);

  mainLayout->addStretch();
}

// ── Button handlers ─────────────────────────────────────────────────────

void InfoTab::onCheckUpdate() {
  updateBtn_->setEnabled(false);
  updateBtn_->setText(T("Đang kiểm tra..."));
  statusLabel_->setText(T("Đang kết nối tới GitHub..."));
  statusLabel_->setStyleSheet("font-size: 12px; color: #666;");
  statusLabel_->show();
  progressBar_->hide();

  updater_->checkForUpdate();
}

UpdateChannel InfoTab::updateChannel() const {
  return channelCombo_->currentIndex() == 1 ? UpdateChannel::Dev
                                            : UpdateChannel::Stable;
}

void InfoTab::setUpdateChannel(UpdateChannel channel) {
  // Block signals so syncing from outside (loadSettings / onDefaults)
  // doesn't trigger onChannelChanged's write + updater round-trip.
  const QSignalBlocker blocker(channelCombo_);
  channelCombo_->setCurrentIndex(channel == UpdateChannel::Dev ? 1 : 0);
}

void InfoTab::onChannelChanged(int /*index*/) {
  // Read-modify-write: writeSkeyConfig rewrites the whole file, so start
  // from what's on disk to avoid clobbering fields edited in other tabs.
  SKeyConfig cfg = readSkeyConfig();
  cfg.updateChannel = channelCombo_->currentIndex() == 1
                          ? UpdateChannel::Dev
                          : UpdateChannel::Stable;
  writeSkeyConfig(cfg);
  updater_->setChannel(cfg.updateChannel);
}

void InfoTab::onRestartFcitx5() {
  restartBtn_->setEnabled(false);
  restartBtn_->setText(T("Đang khởi động lại..."));
  statusLabel_->setText(T("Đang khởi động lại Fcitx5..."));
  statusLabel_->setStyleSheet("font-size: 12px; color: #666;");
  statusLabel_->show();

  // Force UI update before blocking
  QApplication::processEvents();

  restartFcitx5();

  restartBtn_->setEnabled(true);
  restartBtn_->setText(T("Khởi động lại Fcitx5"));
  statusLabel_->setText(T("✓ Fcitx5 đã được khởi động lại"));
  statusLabel_->setStyleSheet("font-size: 12px; color: green;");
}

// ── Updater: check result slots ─────────────────────────────────────────

void InfoTab::onUpdateAvailable(const QString &newVersion,
                                const QString &downloadUrl,
                                const QString &releaseNotes) {
  updateBtn_->setEnabled(true);
  updateBtn_->setText(T("Kiểm tra cập nhật"));
  statusLabel_->hide();

  pendingDownloadUrl_ = downloadUrl;
  pendingVersion_ = newVersion;

  QString msg = T("Có phiên bản mới: v%1\n"
                                  "(Phiên bản hiện tại: %2)\n")
                    .arg(newVersion, SKEY_VERSION);

  if (!releaseNotes.isEmpty()) {
    msg += T("\nGhi chú:\n%1").arg(releaseNotes);
  }

  if (downloadUrl.isEmpty()) {
    if (updater_->distro() == Distro::NixOS) {
      // No asset expected on NixOS: the update is a system rebuild.
      msg += T(
          "\n\nBạn đang dùng NixOS: bản cập nhật sẽ được cài qua "
          "nixos-rebuild (không tải file từ GitHub).");
      if (updateChannel() == UpdateChannel::Dev) {
        msg += T(
            "\nBản Dev sẽ pin flake input vào tag v%1 và ghi "
            "services.fcitx5-skey.devVersion vào configuration.nix.")
                   .arg(newVersion);
      }
    } else {
      msg += T("\n\nKhông tìm thấy file cài đặt phù hợp. "
                               "Vui lòng tải thủ công từ GitHub.");
      QMessageBox::information(this, T("Có bản cập nhật"), msg);
      return;
    }
  }

  QMessageBox msgBox(this);
  msgBox.setWindowTitle(T("Có bản cập nhật"));
  msgBox.setText(msg);
  msgBox.setIcon(QMessageBox::Information);
  auto *yesBtn = msgBox.addButton(T("Cập nhật ngay"),
                                  QMessageBox::AcceptRole);
  msgBox.addButton(T("Bỏ qua"), QMessageBox::RejectRole);
  msgBox.exec();

  if (msgBox.clickedButton() == yesBtn) {
    // User chose "Cập nhật ngay"
    updateBtn_->setEnabled(false);
    updateBtn_->setText(T("Đang cập nhật..."));

    if (updater_->distro() == Distro::NixOS) {
      if (!QFile::exists("/etc/nixos/flake.nix")) {
        showNixosManualInstructions();
        updateBtn_->setEnabled(true);
        updateBtn_->setText(T("Kiểm tra cập nhật"));
        return;
      }
      progressBar_->setValue(0);
      progressBar_->setRange(0, 0); // indeterminate — rebuild can take minutes
      progressBar_->show();
      updater_->rebuildNixos(pendingVersion_);
      return;
    }

    statusLabel_->setText(T("Đang tải bản cập nhật..."));
    statusLabel_->setStyleSheet("font-size: 12px; color: #666;");
    statusLabel_->show();
    progressBar_->setValue(0);
    progressBar_->show();

    updater_->downloadAndInstall(downloadUrl, newVersion);
  }
}

void InfoTab::onNoUpdate() {
  updateBtn_->setEnabled(true);
  updateBtn_->setText(T("Kiểm tra cập nhật"));
#if SKEY_DEV_BUILD > 0
  // This build is a dev build checking the stable channel: "no update"
  // usually means stable is at the same or lower version than this dev
  // build (no same-base downgrade is ever offered).  Say so honestly.
  if (channelCombo_->currentIndex() == 0) {
    statusLabel_->setText(T(
        "✓ Bạn đang dùng bản Dev mới hơn hoặc bằng bản Stable hiện tại."));
    statusLabel_->setStyleSheet("font-size: 12px; color: green;");
    statusLabel_->show();
    return;
  }
#endif
  statusLabel_->setText(
      T("✓ Bạn đang dùng phiên bản mới nhất."));
  statusLabel_->setStyleSheet("font-size: 12px; color: green;");
  statusLabel_->show();
}

void InfoTab::onCheckFailed(const QString &errorMessage) {
  updateBtn_->setEnabled(true);
  updateBtn_->setText(T("Kiểm tra cập nhật"));
  statusLabel_->setText(
      T("✗ Lỗi kiểm tra: %1").arg(errorMessage));
  statusLabel_->setStyleSheet("font-size: 12px; color: red;");
  statusLabel_->show();
}

// ── Updater: download slots ─────────────────────────────────────────────

void InfoTab::onDownloadProgress(int percent) {
  progressBar_->setValue(percent);
  statusLabel_->setText(T("Đang tải... %1%").arg(percent));
}

void InfoTab::onDownloadFinished(const QString & /*packagePath*/) {
  progressBar_->setValue(100);
  statusLabel_->setText(T("Tải xong. Đang cài đặt..."));
}

void InfoTab::onDownloadFailed(const QString &errorMessage) {
  updateBtn_->setEnabled(true);
  updateBtn_->setText(T("Kiểm tra cập nhật"));
  progressBar_->hide();
  statusLabel_->setText(T("✗ Lỗi tải: %1").arg(errorMessage));
  statusLabel_->setStyleSheet("font-size: 12px; color: red;");
  statusLabel_->show();
}

// ── Updater: install slots ──────────────────────────────────────────────

void InfoTab::onInstallStarted() {
  if (updater_->distro() == Distro::NixOS) {
    if (updateChannel() == UpdateChannel::Dev) {
      statusLabel_->setText(T(
          "Đang pin bản Dev và build lại hệ thống bằng nixos-rebuild... "
          "(cần quyền root)"));
    } else {
      statusLabel_->setText(T(
          "Đang build lại hệ thống bằng nixos-rebuild... (cần quyền root)"));
    }
    statusLabel_->setStyleSheet("font-size: 12px; color: #666;");
    statusLabel_->show();
  } else {
    statusLabel_->setText(
        T("Đang cài đặt... (cần quyền root)"));
  }
  progressBar_->setRange(0, 0); // indeterminate
}

void InfoTab::onInstallFinished(bool success, const QString &message) {
  updateBtn_->setEnabled(true);
  updateBtn_->setText(T("Kiểm tra cập nhật"));
  progressBar_->setRange(0, 100);
  progressBar_->hide();

  if (success) {
    if (updater_->distro() == Distro::NixOS) {
      // No package postinst on NixOS: restart fcitx5 + the user's uinput
      // server (polkit rule in the NixOS module permits it without a
      // password) so the newly-built skey.so takes effect.
      restartFcitx5();
    }
    statusLabel_->setText(T("✓ %1").arg(message));
    statusLabel_->setStyleSheet("font-size: 12px; color: green;");
    versionLabel_->setText(
        T("SKey - Phiên bản: %1").arg(pendingVersion_));

    // Close and reopen the settings GUI so the user is running
    // the freshly-installed version.  Brief delay lets the user
    // see the success message before the window closes.
    // On NixOS, applicationFilePath() is the OLD store path (alive until
    // GC); findExecutable resolves through PATH → /run/current-system/sw
    // → the new generation after the rebuild.
    QString exe = QStandardPaths::findExecutable("fcitx5-skey-settings");
    if (exe.isEmpty())
      exe = QApplication::applicationFilePath();
    QTimer::singleShot(1500, this, [this, exe]() {
      QWidget *win = window();
      QProcess::startDetached(exe, {});
      if (win)
        win->close();
    });
  } else {
    statusLabel_->setText(T("✗ %1").arg(message));
    statusLabel_->setStyleSheet("font-size: 12px; color: red;");
  }
  statusLabel_->show();
}

void InfoTab::onNixosManualUpdateRequired() {
  updateBtn_->setEnabled(true);
  updateBtn_->setText(T("Kiểm tra cập nhật"));
  progressBar_->setRange(0, 100);
  progressBar_->hide();
  statusLabel_->setText(
      T("Không thể cập nhật tự động — xem hướng dẫn."));
  statusLabel_->setStyleSheet("font-size: 12px; color: #666;");
  showNixosManualInstructions();
}

void InfoTab::showNixosManualInstructions() {
  const bool dev = updateChannel() == UpdateChannel::Dev;
  QString instructions = T(
      "Không thể tự động cập nhật SKey trên NixOS.\n\n"
      "Hệ thống cần dùng flake với input tên là \"skey\".\n"
      "Thêm vào /etc/nixos/flake.nix:\n\n"
      "  inputs.skey = {\n"
      "    url = \"github:collyn/skey\";\n"
      "    inputs.nixpkgs.follows = \"nixpkgs\";\n"
      "  };\n\n"
      "Rồi chạy (cần quyền root):\n\n"
      "  cd /etc/nixos\n");
  if (dev && !pendingVersion_.isEmpty()) {
    // Dev channel: pin the input to the dev tag and record the dev
    // version so the build carries the "-dev.N" suffix.
    instructions += T(
        "  # thêm vào configuration.nix:\n"
        "  # services.fcitx5-skey.devVersion = \"%1\";\n"
        "  sudo nix flake lock --override-input skey "
        "github:collyn/skey/v%1\n"
        "  sudo nixos-rebuild switch --flake /etc/nixos\n"
        "  fcitx5 -r -d\n\n")
        .arg(pendingVersion_);
  } else {
    instructions += T(
        "  sudo nix flake update skey\n"
        "  sudo nixos-rebuild switch --flake /etc/nixos\n"
        "  fcitx5 -r -d\n\n");
  }
  instructions += T(
      "Xem chi tiết: https://github.com/collyn/skey/blob/main/"
      "packaging/nixos/README.md");

  QMessageBox::information(this,
                           T("Cần cập nhật thủ công"),
                           instructions);
}

// ── Backup / Restore ────────────────────────────────────────────────────

void InfoTab::onBackup() {
  QString defaultName =
      QString::fromUtf8("skey-backup-%1.tar.gz")
          .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
  QString savePath = QFileDialog::getSaveFileName(
      this, T("Lưu bản sao lưu cấu hình"),
      QDir::homePath() + "/" + defaultName,
      T("Tarball (*.tar.gz)"));
  if (savePath.isEmpty())
    return;

  // Copy all config files into a temp dir, then tar the dir.
  // This avoids path complexity — all files land as flat names in the archive.
  QTemporaryDir tmpDir;
  if (!tmpDir.isValid()) {
    QMessageBox::warning(this, T("Lỗi"),
                         T("Không thể tạo thư mục tạm."));
    return;
  }

  struct {
    std::string srcPath;
    const char *destName;
  } files[] = {
      {skeyConfPath(), "skey.conf"},
      {appModesPath(), "skey-app-modes.conf"},
      {macroPath(), "skey-macro.conf"},
      {fcitx5ConfigPath(), "fcitx5-config"},
      {userDictPath(), "user-dict.txt"},
  };

  QStringList missing;
  for (auto &f : files) {
    if (!QFile::copy(QString::fromStdString(f.srcPath),
                     tmpDir.path() + "/" + f.destName))
      missing << f.destName;
  }

  // Custom icons (imported via the Icons tab) — back up the whole dir so
  // a restore brings the icons back with the config that references them.
  const QDir iconsDir(QString::fromStdString(userIconDir()));
  if (iconsDir.exists()) {
    QDir dest(tmpDir.path() + "/icons");
    dest.mkpath(".");
    for (const QString &f :
         iconsDir.entryList(QDir::Files | QDir::NoDotAndDotDot)) {
      QFile::copy(iconsDir.filePath(f), dest.filePath(f));
    }
  }

  QProcess tar;
  tar.setWorkingDirectory(tmpDir.path());
  tar.start("tar", {"-czf", savePath, "."});
  tar.waitForFinished(10000);
  if (tar.exitCode() != 0) {
    QMessageBox::warning(
        this, T("Lỗi"),
        T("Không thể tạo tệp sao lưu:\n%1").arg(savePath));
    return;
  }

  QString msg =
      T("Cấu hình đã được lưu vào:\n%1").arg(savePath);
  if (!missing.isEmpty()) {
    QMessageBox::warning(
        this, T("Đã sao lưu (thiếu tệp)"),
        msg + T("\n\nKhông tìm thấy (bỏ qua): %1")
                  .arg(missing.join(", ")));
  } else {
    QMessageBox::information(this, T("Đã sao lưu"), msg);
  }
}

void InfoTab::onRestore() {
  auto answer = QMessageBox::question(
      this, T("Khôi phục cấu hình"),
      T("Khôi phục sẽ ghi đè toàn bộ cấu hình hiện tại.\n"
                        "Bạn có chắc muốn tiếp tục?"),
      QMessageBox::Yes | QMessageBox::No);
  if (answer != QMessageBox::Yes)
    return;

  QString openPath = QFileDialog::getOpenFileName(
      this, T("Chọn tệp sao lưu để khôi phục"),
      QDir::homePath(), T("Tarball (*.tar.gz)"));
  if (openPath.isEmpty())
    return;

  QTemporaryDir tmpDir;
  if (!tmpDir.isValid()) {
    QMessageBox::warning(this, T("Lỗi"),
                         T("Không thể tạo thư mục tạm."));
    return;
  }

  QProcess tar;
  tar.start("tar", {"-xzf", openPath, "-C", tmpDir.path()});
  tar.waitForFinished(10000);
  if (tar.exitCode() != 0) {
    QMessageBox::warning(
        this, T("Lỗi"),
        T("Không thể giải nén tệp sao lưu:\n%1").arg(openPath));
    return;
  }

  struct {
    const char *filename;
    std::string destPath;
  } mappings[] = {
      {"skey.conf", skeyConfPath()},
      {"skey-app-modes.conf", appModesPath()},
      {"skey-macro.conf", macroPath()},
      {"fcitx5-config", fcitx5ConfigPath()},
  };

  bool allOk = true;
  for (auto &m : mappings) {
    QString src = tmpDir.path() + "/" + m.filename;
    if (!QFile::exists(src)) {
      allOk = false;
      continue;
    }
    QFile::remove(QString::fromStdString(m.destPath));
    if (!QFile::copy(src, QString::fromStdString(m.destPath))) {
      allOk = false;
    }
  }

  // Optional user data — older backups don't contain them, so skip
  // silently when absent; report only real copy failures.
  const QString dictSrc = tmpDir.path() + "/user-dict.txt";
  if (QFile::exists(dictSrc)) {
    QFile::remove(QString::fromStdString(userDictPath()));
    if (!QFile::copy(dictSrc, QString::fromStdString(userDictPath())))
      allOk = false;
  }
  const QDir iconSrcDir(tmpDir.path() + "/icons");
  if (iconSrcDir.exists()) {
    QDir dest(QString::fromStdString(userIconDir()));
    dest.mkpath(".");
    for (const QString &f :
         iconSrcDir.entryList(QDir::Files | QDir::NoDotAndDotDot)) {
      QFile::remove(dest.filePath(f));
      if (!QFile::copy(iconSrcDir.filePath(f), dest.filePath(f)))
        allOk = false;
    }
  }

  if (!allOk) {
    QMessageBox::warning(this, T("Cảnh báo"),
                         T("Một số tệp không thể khôi phục.\n"
                                           "Kiểm tra lại tệp sao lưu."));
  }

  reloadFcitx5();
  emit configRestored();
  QMessageBox::information(
      this, T("Đã khôi phục"),
      T("Cấu hình đã được khôi phục và áp dụng."));
}
