#include "info_tab.h"

#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#ifndef SKEY_VERSION
#define SKEY_VERSION "0.1.0"
#endif

static const char *kGitHubUrl  = "https://github.com/collyn/skey";
static const char *kReleasesUrl = "https://github.com/collyn/skey/releases";

InfoTab::InfoTab(QWidget *parent) : QWidget(parent) {
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
                          "Hỗ trợ Telex, Telex W, VNI"),
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

    auto *updateBtn = new QPushButton(
        QString::fromUtf8("Kiểm tra cập nhật"), this);
    connect(updateBtn, &QPushButton::clicked,
            this, &InfoTab::onCheckUpdate);

    btnRow->addStretch();
    btnRow->addWidget(githubBtn);
    btnRow->addWidget(updateBtn);
    btnRow->addStretch();
    mainLayout->addLayout(btnRow);

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

void InfoTab::onCheckUpdate() {
    QDesktopServices::openUrl(QUrl(kReleasesUrl));
}

void InfoTab::onOpenGitHub() {
    QDesktopServices::openUrl(QUrl(kGitHubUrl));
}
