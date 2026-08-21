#include "info_tab.h"
#include "config_io.h"
#include "updater.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
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
  // Config-driven icon with fallback to default 128px PNG
  QIcon icon(QString::fromStdString(effectiveIconPath(readSkeyConfig())));
  if (icon.isNull())
    icon = QIcon("/usr/share/icons/hicolor/128x128/apps/fcitx-skey.png");
  if (!icon.isNull()) {
    qreal dpr = iconLabel->devicePixelRatioF();
    int pxSize = static_cast<int>(80 * dpr);
    QPixmap src = icon.pixmap(pxSize, pxSize);
    src.setDevicePixelRatio(dpr);
    // Render with rounded corners via a clipped painter
    QPixmap rounded(pxSize, pxSize);
    rounded.setDevicePixelRatio(dpr);
    rounded.fill(Qt::transparent);
    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, 80, 80), 12, 12);
    painter.setClipPath(path);
    painter.drawPixmap(QRectF(0, 0, 80, 80), src, QRectF(0, 0, 80, 80));
    painter.end();
    iconLabel->setPixmap(rounded);
  } else {
    iconLabel->setText(QString::fromUtf8("🇻🇳"));
    iconLabel->setStyleSheet("font-size: 48px;");
  }
  mainLayout->addWidget(iconLabel, 0, Qt::AlignHCenter);

  // ── Version ──
  versionLabel_ =
      new QLabel(QString::fromUtf8("SKey - Phiên bản: ") + SKEY_VERSION, this);
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
      QString::fromUtf8("<a href=\"%1\">%1</a>").arg(kGitHubUrl));
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
  auto *channelLabel = new QLabel(QString::fromUtf8("Kênh cập nhật:"), this);
  channelCombo_ = new QComboBox(this);
  channelCombo_->addItem(QString::fromUtf8("Ổn định (Stable)"),
                         static_cast<int>(UpdateChannel::Stable));
  channelCombo_->addItem(QString::fromUtf8("Thử nghiệm (Dev)"),
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

  updateBtn_ = new QPushButton(QString::fromUtf8("Check Update"), this);
  connect(updateBtn_, &QPushButton::clicked, this, &InfoTab::onCheckUpdate);

  restartBtn_ = new QPushButton(QString::fromUtf8("Restart Fcitx5"), this);
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
  auto *contactTitle = new QLabel(QString::fromUtf8("Liên hệ"), this);
  contactTitle->setAlignment(Qt::AlignCenter);
  contactTitle->setStyleSheet("font-weight: bold; font-size: 13px;");
  mainLayout->addWidget(contactTitle);

  auto *authorLabel = new QLabel(QString::fromUtf8("Nguyễn Tiến Huy"), this);
  authorLabel->setAlignment(Qt::AlignCenter);
  mainLayout->addWidget(authorLabel);

  auto *telegramLabel = new QLabel(this);
  telegramLabel->setText(QString::fromUtf8(
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
      new QLabel(QString::fromUtf8("Backup / Restore:"), backupFrame);
  backupLayout->addWidget(backupLabel);
  backupLayout->addStretch();

  backupButton_ = new QPushButton(QString::fromUtf8("Sao lưu"), backupFrame);
  backupButton_->setMinimumWidth(90);
  connect(backupButton_, &QPushButton::clicked, this, &InfoTab::onBackup);
  backupLayout->addWidget(backupButton_);

  restoreButton_ = new QPushButton(QString::fromUtf8("Khôi phục"), backupFrame);
  restoreButton_->setMinimumWidth(90);
  connect(restoreButton_, &QPushButton::clicked, this, &InfoTab::onRestore);
  backupLayout->addWidget(restoreButton_);

  mainLayout->addWidget(backupFrame);

  mainLayout->addStretch();
}

// ── Button handlers ─────────────────────────────────────────────────────

void InfoTab::onCheckUpdate() {
  updateBtn_->setEnabled(false);
  updateBtn_->setText(QString::fromUtf8("Đang kiểm tra..."));
  statusLabel_->setText(QString::fromUtf8("Đang kết nối tới GitHub..."));
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
  restartBtn_->setText(QString::fromUtf8("Đang khởi động lại..."));
  statusLabel_->setText(QString::fromUtf8("Đang khởi động lại Fcitx5..."));
  statusLabel_->setStyleSheet("font-size: 12px; color: #666;");
  statusLabel_->show();

  // Force UI update before blocking
  QApplication::processEvents();

  restartFcitx5();

  restartBtn_->setEnabled(true);
  restartBtn_->setText(QString::fromUtf8("Khởi động lại Fcitx5"));
  statusLabel_->setText(QString::fromUtf8("✓ Fcitx5 đã được khởi động lại"));
  statusLabel_->setStyleSheet("font-size: 12px; color: green;");
}

// ── Updater: check result slots ─────────────────────────────────────────

void InfoTab::onUpdateAvailable(const QString &newVersion,
                                const QString &downloadUrl,
                                const QString &releaseNotes) {
  updateBtn_->setEnabled(true);
  updateBtn_->setText(QString::fromUtf8("Kiểm tra cập nhật"));
  statusLabel_->hide();

  pendingDownloadUrl_ = downloadUrl;
  pendingVersion_ = newVersion;

  QString msg = QString::fromUtf8("Có phiên bản mới: v%1\n"
                                  "(Phiên bản hiện tại: %2)\n")
                    .arg(newVersion, SKEY_VERSION);

  if (!releaseNotes.isEmpty()) {
    msg += QString::fromUtf8("\nGhi chú:\n%1").arg(releaseNotes);
  }

  if (downloadUrl.isEmpty()) {
    msg += QString::fromUtf8("\n\nKhông tìm thấy file cài đặt phù hợp. "
                             "Vui lòng tải thủ công từ GitHub.");
    QMessageBox::information(this, QString::fromUtf8("Có bản cập nhật"), msg);
    return;
  }

  QMessageBox msgBox(this);
  msgBox.setWindowTitle(QString::fromUtf8("Có bản cập nhật"));
  msgBox.setText(msg);
  msgBox.setIcon(QMessageBox::Information);
  auto *yesBtn = msgBox.addButton(QString::fromUtf8("Cập nhật ngay"),
                                  QMessageBox::AcceptRole);
  msgBox.addButton(QString::fromUtf8("Bỏ qua"), QMessageBox::RejectRole);
  msgBox.exec();

  if (msgBox.clickedButton() == yesBtn) {
    // User chose "Cập nhật ngay"
    updateBtn_->setEnabled(false);
    updateBtn_->setText(QString::fromUtf8("Đang tải..."));
    statusLabel_->setText(QString::fromUtf8("Đang tải bản cập nhật..."));
    statusLabel_->setStyleSheet("font-size: 12px; color: #666;");
    statusLabel_->show();
    progressBar_->setValue(0);
    progressBar_->show();

    updater_->downloadAndInstall(downloadUrl, newVersion);
  }
}

void InfoTab::onNoUpdate() {
  updateBtn_->setEnabled(true);
  updateBtn_->setText(QString::fromUtf8("Kiểm tra cập nhật"));
#if SKEY_DEV_BUILD > 0
  // This build is a dev build checking the stable channel: "no update"
  // usually means stable is at the same or lower version than this dev
  // build (no same-base downgrade is ever offered).  Say so honestly.
  if (channelCombo_->currentIndex() == 0) {
    statusLabel_->setText(QString::fromUtf8(
        "✓ Bạn đang dùng bản Dev mới hơn hoặc bằng bản Stable hiện tại."));
    statusLabel_->setStyleSheet("font-size: 12px; color: green;");
    statusLabel_->show();
    return;
  }
#endif
  statusLabel_->setText(
      QString::fromUtf8("✓ Bạn đang dùng phiên bản mới nhất."));
  statusLabel_->setStyleSheet("font-size: 12px; color: green;");
  statusLabel_->show();
}

void InfoTab::onCheckFailed(const QString &errorMessage) {
  updateBtn_->setEnabled(true);
  updateBtn_->setText(QString::fromUtf8("Kiểm tra cập nhật"));
  statusLabel_->setText(
      QString::fromUtf8("✗ Lỗi kiểm tra: %1").arg(errorMessage));
  statusLabel_->setStyleSheet("font-size: 12px; color: red;");
  statusLabel_->show();
}

// ── Updater: download slots ─────────────────────────────────────────────

void InfoTab::onDownloadProgress(int percent) {
  progressBar_->setValue(percent);
  statusLabel_->setText(QString::fromUtf8("Đang tải... %1%").arg(percent));
}

void InfoTab::onDownloadFinished(const QString & /*packagePath*/) {
  progressBar_->setValue(100);
  statusLabel_->setText(QString::fromUtf8("Tải xong. Đang cài đặt..."));
}

void InfoTab::onDownloadFailed(const QString &errorMessage) {
  updateBtn_->setEnabled(true);
  updateBtn_->setText(QString::fromUtf8("Kiểm tra cập nhật"));
  progressBar_->hide();
  statusLabel_->setText(QString::fromUtf8("✗ Lỗi tải: %1").arg(errorMessage));
  statusLabel_->setStyleSheet("font-size: 12px; color: red;");
  statusLabel_->show();
}

// ── Updater: install slots ──────────────────────────────────────────────

void InfoTab::onInstallStarted() {
  statusLabel_->setText(QString::fromUtf8("Đang cài đặt... (cần quyền root)"));
  progressBar_->setRange(0, 0); // indeterminate
}

void InfoTab::onInstallFinished(bool success, const QString &message) {
  updateBtn_->setEnabled(true);
  updateBtn_->setText(QString::fromUtf8("Kiểm tra cập nhật"));
  progressBar_->setRange(0, 100);
  progressBar_->hide();

  if (success) {
    statusLabel_->setText(QString::fromUtf8("✓ %1").arg(message));
    statusLabel_->setStyleSheet("font-size: 12px; color: green;");
    versionLabel_->setText(
        QString::fromUtf8("SKey - Phiên bản: %1").arg(pendingVersion_));

    // Close and reopen the settings GUI so the user is running
    // the freshly-installed version.  Brief delay lets the user
    // see the success message before the window closes.
    QTimer::singleShot(1500, this, [this]() {
      QWidget *win = window();
      QProcess::startDetached(QApplication::applicationFilePath(), {});
      if (win)
        win->close();
    });
  } else {
    statusLabel_->setText(QString::fromUtf8("✗ %1").arg(message));
    statusLabel_->setStyleSheet("font-size: 12px; color: red;");
  }
  statusLabel_->show();
}

// ── Backup / Restore ────────────────────────────────────────────────────

void InfoTab::onBackup() {
  QString defaultName =
      QString::fromUtf8("skey-backup-%1.tar.gz")
          .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
  QString savePath = QFileDialog::getSaveFileName(
      this, QString::fromUtf8("Lưu bản sao lưu cấu hình"),
      QDir::homePath() + "/" + defaultName,
      QString::fromUtf8("Tarball (*.tar.gz)"));
  if (savePath.isEmpty())
    return;

  // Copy all config files into a temp dir, then tar the dir.
  // This avoids path complexity — all files land as flat names in the archive.
  QTemporaryDir tmpDir;
  if (!tmpDir.isValid()) {
    QMessageBox::warning(this, QString::fromUtf8("Lỗi"),
                         QString::fromUtf8("Không thể tạo thư mục tạm."));
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
        this, QString::fromUtf8("Lỗi"),
        QString::fromUtf8("Không thể tạo tệp sao lưu:\n%1").arg(savePath));
    return;
  }

  QString msg =
      QString::fromUtf8("Cấu hình đã được lưu vào:\n%1").arg(savePath);
  if (!missing.isEmpty()) {
    QMessageBox::warning(
        this, QString::fromUtf8("Đã sao lưu (thiếu tệp)"),
        msg + QString::fromUtf8("\n\nKhông tìm thấy (bỏ qua): %1")
                  .arg(missing.join(", ")));
  } else {
    QMessageBox::information(this, QString::fromUtf8("Đã sao lưu"), msg);
  }
}

void InfoTab::onRestore() {
  auto answer = QMessageBox::question(
      this, QString::fromUtf8("Khôi phục cấu hình"),
      QString::fromUtf8("Khôi phục sẽ ghi đè toàn bộ cấu hình hiện tại.\n"
                        "Bạn có chắc muốn tiếp tục?"),
      QMessageBox::Yes | QMessageBox::No);
  if (answer != QMessageBox::Yes)
    return;

  QString openPath = QFileDialog::getOpenFileName(
      this, QString::fromUtf8("Chọn tệp sao lưu để khôi phục"),
      QDir::homePath(), QString::fromUtf8("Tarball (*.tar.gz)"));
  if (openPath.isEmpty())
    return;

  QTemporaryDir tmpDir;
  if (!tmpDir.isValid()) {
    QMessageBox::warning(this, QString::fromUtf8("Lỗi"),
                         QString::fromUtf8("Không thể tạo thư mục tạm."));
    return;
  }

  QProcess tar;
  tar.start("tar", {"-xzf", openPath, "-C", tmpDir.path()});
  tar.waitForFinished(10000);
  if (tar.exitCode() != 0) {
    QMessageBox::warning(
        this, QString::fromUtf8("Lỗi"),
        QString::fromUtf8("Không thể giải nén tệp sao lưu:\n%1").arg(openPath));
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
    QMessageBox::warning(this, QString::fromUtf8("Cảnh báo"),
                         QString::fromUtf8("Một số tệp không thể khôi phục.\n"
                                           "Kiểm tra lại tệp sao lưu."));
  }

  reloadFcitx5();
  emit configRestored();
  QMessageBox::information(
      this, QString::fromUtf8("Đã khôi phục"),
      QString::fromUtf8("Cấu hình đã được khôi phục và áp dụng."));
}
