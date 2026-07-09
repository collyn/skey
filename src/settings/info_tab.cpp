#include "info_tab.h"
#include "updater.h"

#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#ifndef SKEY_VERSION
#define SKEY_VERSION "0.1.0"
#endif

static const char *kGitHubUrl  = "https://github.com/collyn/skey";

InfoTab::InfoTab(QWidget *parent) : QWidget(parent) {
    updater_ = new Updater(SKEY_VERSION, this);

    // Connect updater signals
    connect(updater_, &Updater::updateAvailable,
            this, &InfoTab::onUpdateAvailable);
    connect(updater_, &Updater::noUpdateAvailable,
            this, &InfoTab::onNoUpdate);
    connect(updater_, &Updater::checkFailed,
            this, &InfoTab::onCheckFailed);
    connect(updater_, &Updater::downloadProgress,
            this, &InfoTab::onDownloadProgress);
    connect(updater_, &Updater::downloadFinished,
            this, &InfoTab::onDownloadFinished);
    connect(updater_, &Updater::downloadFailed,
            this, &InfoTab::onDownloadFailed);
    connect(updater_, &Updater::installStarted,
            this, &InfoTab::onInstallStarted);
    connect(updater_, &Updater::installFinished,
            this, &InfoTab::onInstallFinished);

    setupUI();
}

void InfoTab::setupUI() {
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // ── Icon ──
    auto *iconLabel = new QLabel(this);
    QIcon icon = QIcon::fromTheme("fcitx-skey");
    if (!icon.isNull()) {
        iconLabel->setPixmap(icon.pixmap(64, 64));
    } else {
        iconLabel->setText(QString::fromUtf8("🇻🇳"));
        iconLabel->setStyleSheet("font-size: 48px;");
    }
    iconLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(iconLabel);

    // ── Name & description ──
    auto *nameLabel = new QLabel("Skey", this);
    nameLabel->setAlignment(Qt::AlignCenter);
    nameLabel->setStyleSheet("font-size: 18px; font-weight: bold;");
    mainLayout->addWidget(nameLabel);

    auto *descLabel = new QLabel(
        QString::fromUtf8("Bộ gõ tiếng Việt cho Fcitx5\n"
                          "Hỗ trợ Telex, VNI"),
        this);
    descLabel->setAlignment(Qt::AlignCenter);
    descLabel->setWordWrap(true);
    mainLayout->addWidget(descLabel);

    // ── Version ──
    versionLabel_ = new QLabel(
        QString::fromUtf8("Phiên bản: ") + SKEY_VERSION, this);
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

    // ── Buttons row ──
    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(8);

    auto *githubBtn = new QPushButton(
        QString::fromUtf8("GitHub"), this);
    connect(githubBtn, &QPushButton::clicked,
            this, &InfoTab::onOpenGitHub);

    updateBtn_ = new QPushButton(
        QString::fromUtf8("Kiểm tra cập nhật"), this);
    connect(updateBtn_, &QPushButton::clicked,
            this, &InfoTab::onCheckUpdate);

    btnRow->addStretch();
    btnRow->addWidget(githubBtn);
    btnRow->addWidget(updateBtn_);
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
    auto *contactTitle = new QLabel(
        QString::fromUtf8("Liên hệ"), this);
    contactTitle->setAlignment(Qt::AlignCenter);
    contactTitle->setStyleSheet("font-weight: bold; font-size: 13px;");
    mainLayout->addWidget(contactTitle);

    auto *authorLabel = new QLabel(
        QString::fromUtf8("Nguyễn Tiến Huy"), this);
    authorLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(authorLabel);

    auto *emailLabel = new QLabel(this);
    emailLabel->setText(
        QString::fromUtf8("<a href=\"mailto:collyn094@gmail.com\">collyn094@gmail.com</a>"));
    emailLabel->setTextFormat(Qt::RichText);
    emailLabel->setOpenExternalLinks(true);
    emailLabel->setAlignment(Qt::AlignCenter);
    emailLabel->setCursor(Qt::PointingHandCursor);
    mainLayout->addWidget(emailLabel);

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

void InfoTab::onOpenGitHub() {
    QDesktopServices::openUrl(QUrl(kGitHubUrl));
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

    QString msg = QString::fromUtf8(
        "Có phiên bản mới: v%1\n"
        "(Phiên bản hiện tại: %2)\n")
        .arg(newVersion, SKEY_VERSION);

    if (!releaseNotes.isEmpty()) {
        msg += QString::fromUtf8("\nGhi chú:\n%1").arg(releaseNotes);
    }

    if (downloadUrl.isEmpty()) {
        msg += QString::fromUtf8(
            "\n\nKhông tìm thấy file .deb. "
            "Vui lòng tải thủ công từ GitHub.");
        QMessageBox::information(this,
                                 QString::fromUtf8("Có bản cập nhật"), msg);
        return;
    }

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(QString::fromUtf8("Có bản cập nhật"));
    msgBox.setText(msg);
    msgBox.setIcon(QMessageBox::Information);
    auto *yesBtn = msgBox.addButton(
        QString::fromUtf8("Cập nhật ngay"), QMessageBox::AcceptRole);
    msgBox.addButton(
        QString::fromUtf8("Bỏ qua"), QMessageBox::RejectRole);
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
    statusLabel_->setText(
        QString::fromUtf8("Đang tải... %1%").arg(percent));
}

void InfoTab::onDownloadFinished(const QString & /*debPath*/) {
    progressBar_->setValue(100);
    statusLabel_->setText(QString::fromUtf8("Tải xong. Đang cài đặt..."));
}

void InfoTab::onDownloadFailed(const QString &errorMessage) {
    updateBtn_->setEnabled(true);
    updateBtn_->setText(QString::fromUtf8("Kiểm tra cập nhật"));
    progressBar_->hide();
    statusLabel_->setText(
        QString::fromUtf8("✗ Lỗi tải: %1").arg(errorMessage));
    statusLabel_->setStyleSheet("font-size: 12px; color: red;");
    statusLabel_->show();
}

// ── Updater: install slots ──────────────────────────────────────────────

void InfoTab::onInstallStarted() {
    statusLabel_->setText(
        QString::fromUtf8("Đang cài đặt... (cần quyền root)"));
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
            QString::fromUtf8("Phiên bản: %1 (cần khởi động lại)")
                .arg(pendingVersion_));
    } else {
        statusLabel_->setText(QString::fromUtf8("✗ %1").arg(message));
        statusLabel_->setStyleSheet("font-size: 12px; color: red;");
    }
    statusLabel_->show();
}
