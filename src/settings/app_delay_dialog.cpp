#include "app_delay_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "config_io.h"
#include "tr.h"

namespace {

bool isWaylandSession() {
    return !qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty() ||
           QGuiApplication::platformName().contains("wayland");
}

QSpinBox *makeSpin(int maxMs, QWidget *parent) {
    auto *spin = new QSpinBox(parent);
    spin->setRange(0, maxMs);
    spin->setSuffix(" ms");
    return spin;
}

} // namespace

AppDelayDialog::AppDelayDialog(const QString &appName,
                               const skey::AppDelayOverride &x11,
                               const skey::AppDelayOverride &wayland,
                               QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(T("Độ trễ nâng cao — %1").arg(appName));
    setMinimumWidth(340);

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        T("Bỏ tích \"Tùy chỉnh\" = dùng giá trị tự động (các ô chỉ hiển thị "
          "mức đang dùng). Tích vào để ghi đè bằng các giá trị bên dưới. Có "
          "hiệu lực cho cả thanh địa chỉ Chromium."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    const std::string rawName = appName.toStdString();
    const auto learned = readLearnedDelays();
    waylandSession_ = isWaylandSession();
    inactiveInitial_ = waylandSession_ ? x11 : wayland;
    const skey::AppDelayOverride &initial =
        waylandSession_ ? wayland : x11;
    const std::string appKey =
        skey::appDelayKey(rawName, waylandSession_);

    // Effective values: existing override wins, otherwise the engine's
    // defaults (1ms pacing, 0ms post) and the last applied sleep for the
    // pre-commit wait (15ms fallback when the app has no data yet).
    effPaceMs_ = initial.paceMs >= 0 ? initial.paceMs : 1;
    effPostMs_ = initial.postCommitMs >= 0 ? initial.postCommitMs : 0;
    const auto it = learned.find(appKey);
    uint64_t lastSleepMs =
        (it != learned.end()) ? it->second.lastSleepUsec / 1000 : 0;
    effPreMs_ = initial.preCommitMs >= 0
                    ? initial.preCommitMs
                    : static_cast<int>(lastSleepMs > 0 ? lastSleepMs : 15);

    active_ = buildGroup(appKey, learned);
    active_.customCheck->setChecked(initial.any());
    layout->addWidget(active_.paceSpin->parentWidget());

    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(T("Lưu"), QDialogButtonBox::AcceptRole);
    buttons->addButton(T("Hủy"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

AppDelayDialog::Group AppDelayDialog::buildGroup(
    const std::string &appKey,
    const std::map<std::string, LearnedDelay> &learned) {
    Group g;
    auto *box = new QGroupBox(waylandSession_ ? T("Wayland") : T("X11"),
                              this);
    auto *form = new QFormLayout(box);
    form->setContentsMargins(12, 12, 12, 12);
    form->setHorizontalSpacing(10);

    // The Custom checkbox is the explicit override switch — without it,
    // the spinboxes are informational (disabled) and the engine keeps its
    // automatic values.
    g.customCheck = new QCheckBox(T("Tùy chỉnh"), box);
    g.customCheck->setToolTip(
        T("Tích vào để ghi đè bằng các giá trị bên dưới."));
    form->addRow(g.customCheck);

    g.paceSpin = makeSpin(skey::kMaxPaceMs, box);
    g.paceSpin->setToolTip(T(
        "Khoảng cách giữa các phím Backspace bơm vào khi sửa dấu. "
        "0ms có thể mất chữ trên Wayland."));
    form->addRow(T("Nhịp giữa phím BS:"), g.paceSpin);

    g.preSpin = makeSpin(skey::kMaxPreCommitMs, box);
    g.preSpin->setToolTip(T("Chờ sau khi xóa, trước khi chèn chữ mới."));
    form->addRow(T("Chờ trước commit:"), g.preSpin);

    g.postSpin = makeSpin(skey::kMaxPostCommitMs, box);
    g.postSpin->setToolTip(
        T("Chờ sau khi chèn chữ, trước khi các phím gõ trong lúc chờ được "
          "phát lại."));
    form->addRow(T("Chờ sau commit:"), g.postSpin);

    // Learned-RT hint from the AutoDelay statistics file.
    const auto it = learned.find(appKey);
    g.hintLabel = new QLabel(box);
    if (it != learned.end()) {
        g.hintLabel->setText(T("RT học được: %1 ms (%2 mẫu)")
                                 .arg(it->second.rtUsec / 1000)
                                 .arg(it->second.samples));
    } else {
        g.hintLabel->setText(T("RT học được: chưa có dữ liệu"));
    }
    g.hintLabel->setStyleSheet("color: #555;");
    form->addRow(g.hintLabel);

    // "Auto" is a universal term — same string in both languages.
    auto *resetButton = new QPushButton(QStringLiteral("Auto"), box);
    resetButton->setToolTip(
        T("Bỏ ghi đè, trở về giá trị tự động hiện tại."));
    connect(resetButton, &QPushButton::clicked, this, [this, &g]() {
        g.customCheck->setChecked(false);
        g.paceSpin->setValue(effPaceMs_);
        g.preSpin->setValue(effPreMs_);
        g.postSpin->setValue(effPostMs_);
    });
    form->addRow(resetButton);

    g.warningLabel = new QLabel(box);
    g.warningLabel->setStyleSheet("color: #c0392b;");
    g.warningLabel->setWordWrap(true);
    form->addRow(g.warningLabel);

    // Effective values first; the Custom switch controls editability.
    g.paceSpin->setValue(effPaceMs_);
    g.preSpin->setValue(effPreMs_);
    g.postSpin->setValue(effPostMs_);
    g.paceSpin->setEnabled(false);
    g.preSpin->setEnabled(false);
    g.postSpin->setEnabled(false);

    auto refreshWarning = [this, &g]() {
        if (!g.customCheck->isChecked()) {
            g.warningLabel->clear();
            return;
        }
        QStringList warnings;
        if (g.preSpin->value() < 10) {
            warnings << T("Cảnh báo: chờ trước commit dưới 10ms có thể mất "
                          "chữ.");
        }
        if (g.paceSpin->value() == 0 && waylandSession_) {
            warnings << T("Nhịp 0ms có thể mất chữ trên Wayland.");
        }
        g.warningLabel->setText(warnings.join(' '));
    };
    connect(g.customCheck, &QCheckBox::toggled, this,
            [this, &g, refreshWarning](bool on) {
                g.paceSpin->setEnabled(on);
                g.preSpin->setEnabled(on);
                g.postSpin->setEnabled(on);
                refreshWarning();
            });
    connect(g.paceSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [refreshWarning](int) { refreshWarning(); });
    connect(g.preSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [refreshWarning](int) { refreshWarning(); });
    connect(g.postSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [refreshWarning](int) { refreshWarning(); });
    refreshWarning();

    box->setLayout(form);
    return g;
}

skey::AppDelayOverride AppDelayDialog::x11Override() const {
    if (waylandSession_) {
        return inactiveInitial_;
    }
    if (!active_.customCheck->isChecked()) {
        return skey::AppDelayOverride{};
    }
    return skey::AppDelayOverride{active_.paceSpin->value(),
                                  active_.preSpin->value(),
                                  active_.postSpin->value()};
}

skey::AppDelayOverride AppDelayDialog::waylandOverride() const {
    if (!waylandSession_) {
        return inactiveInitial_;
    }
    if (!active_.customCheck->isChecked()) {
        return skey::AppDelayOverride{};
    }
    return skey::AppDelayOverride{active_.paceSpin->value(),
                                  active_.preSpin->value(),
                                  active_.postSpin->value()};
}
