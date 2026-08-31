#include "hotkey_edit.h"
#include "tr.h"

#include <QKeyEvent>
#include <QKeySequence>

namespace {
// Flash styles used while the field has focus.  The :focus rule mirrors the
// base rule so the style stays stable instead of flickering on focus loss.
QString captureStyle() {
    return "QLineEdit { background: #c8e6c9; color: #1b5e20; font-weight: bold;"
           "  border: 2px solid #4caf50; border-radius: 3px; }"
           "QLineEdit:focus { background: #c8e6c9; color: #1b5e20; font-weight: bold;"
           "  border: 2px solid #4caf50; border-radius: 3px; }";
}
QString focusStyle() {
    return "QLineEdit { background: #bbdefb; color: #0d47a1; font-weight: bold;"
           "  border: 2px solid #2196f3; border-radius: 3px; }"
           "QLineEdit:focus { background: #bbdefb; color: #0d47a1; font-weight: bold;"
           "  border: 2px solid #2196f3; border-radius: 3px; }";
}
QString clearStyle() {
    return "QLineEdit { background: #eeeeee; color: #424242; font-weight: bold;"
           "  border: 2px solid #9e9e9e; border-radius: 3px; }"
           "QLineEdit:focus { background: #eeeeee; color: #424242; font-weight: bold;"
           "  border: 2px solid #9e9e9e; border-radius: 3px; }";
}
}  // namespace

HotkeyEdit::HotkeyEdit(QWidget *parent) : QLineEdit(parent) {
    setReadOnly(true);
    setPlaceholderText(T("Nhấn tổ hợp phím..."));
    setFcitx5Value("Control+space");
}

/// Convert a fcitx5 key name to a human-readable display form.
/// Handles special keysym names that QKeySequence can't convert.
static QString fcitx5KeyToDisplay(const QString &fcitx5Name) {
    // fcitx5 keysym → human-readable display name
    static const std::unordered_map<std::string, QString> kMap = {
        {"grave",    "`"},
        {"space",    "Space"},
        {"BackSpace","Backspace"},
        {"Escape",   "Esc"},
        {"Return",   "Enter"},
        {"Tab",      "Tab"},
        {"Delete",   "Del"},
        {"Insert",   "Ins"},
        {"Page_Up",  "PgUp"},
        {"Page_Down","PgDn"},
        {"Left",     "←"},
        {"Right",    "→"},
        {"Up",       "↑"},
        {"Down",     "↓"},
        {"minus",    "-"},
        {"equal",    "="},
        {"bracketleft",  "["},
        {"bracketright", "]"},
        {"backslash",    "\\"},
        {"semicolon",    ";"},
        {"apostrophe",   "'"},
        {"comma",    ","},
        {"period",   "."},
        {"slash",    "/"},
    };
    auto it = kMap.find(fcitx5Name.toStdString());
    if (it != kMap.end())
        return it->second;
    // For simple keys like "a", "F12", capitalize the first letter
    if (!fcitx5Name.isEmpty()) {
        QString s = fcitx5Name;
        s[0] = s[0].toUpper();
        return s;
    }
    return fcitx5Name;
}

QString HotkeyEdit::formatModifiers(Qt::KeyboardModifiers mods, int keyCode) {
    QStringList parts;

    if (mods & Qt::ControlModifier)  parts << "Control";
    if (mods & Qt::ShiftModifier)    parts << "Shift";
    if (mods & Qt::AltModifier)      parts << "Alt";
    if (mods & Qt::MetaModifier)     parts << "Super";

    // Get the key name (only if a non-modifier key was pressed)
    if (keyCode != 0 && keyCode != Qt::Key_Control &&
        keyCode != Qt::Key_Shift && keyCode != Qt::Key_Alt &&
        keyCode != Qt::Key_Meta && keyCode != Qt::Key_unknown) {
        QKeySequence seq(keyCode);
        QString keyName = seq.toString();
        // QKeySequence capitalizes — lowercase common keys for fcitx5 format
        if (keyName.size() == 1 && keyName[0].isUpper())
            keyName = keyName.toLower();
        else if (keyName.compare("Space", Qt::CaseInsensitive) == 0)
            keyName = "space";
        else if (keyName.compare("Tab", Qt::CaseInsensitive) == 0)
            keyName = "Tab";
        else if (keyName.compare("Backspace", Qt::CaseInsensitive) == 0)
            keyName = "BackSpace";
        else if (keyName.compare("Escape", Qt::CaseInsensitive) == 0)
            keyName = "Escape";
        parts << keyName;
    }

    // If only modifiers (no regular key), use the modifier key itself as the key
    if (parts.isEmpty())
        return "";

    return parts.join("+");
}

void HotkeyEdit::keyPressEvent(QKeyEvent *event) {
    int keyCode = event->key();
    Qt::KeyboardModifiers mods = event->modifiers();
    bool bareKey = (mods == Qt::NoModifier);

    // Backspace alone unbinds; Esc alone cancels and restores the previous
    // value.  Both remain capturable WITH a modifier (e.g. Ctrl+Backspace).
    if (bareKey && keyCode == Qt::Key_Backspace) {
        fcitx5Value_ = "";
        refreshDisplay();
        setStyleSheet(clearStyle());  // gray flash: binding removed
        event->accept();
        return;
    }
    if (bareKey && keyCode == Qt::Key_Escape) {
        refreshDisplay();  // keep the old value
        setStyleSheet(focusStyle());
        event->accept();
        return;
    }

    QString combo = formatModifiers(mods, keyCode);

    if (!combo.isEmpty()) {
        fcitx5Value_ = combo.toStdString();
        // Convert fcitx5 format to display format: "Control+grave" → "Ctrl+`"
        setText(toDisplayString(combo));
        setStyleSheet(captureStyle());  // green flash to confirm capture
    }

    event->accept();
}

void HotkeyEdit::focusInEvent(QFocusEvent *event) {
    QLineEdit::focusInEvent(event);
    // Replace text with instruction — much more visible than placeholder
    setText(T("⌨ Nhấn phím mới... (Backspace: gỡ, Esc: hủy)"));
    setStyleSheet(focusStyle());  // blue highlight, clearly distinct
}

void HotkeyEdit::focusOutEvent(QFocusEvent *event) {
    QLineEdit::focusOutEvent(event);
    // If user clicked away without capturing a key, restore the original value
    if (text() == T("⌨ Nhấn phím mới... (Backspace: gỡ, Esc: hủy)") ||
        text().isEmpty()) {
        refreshDisplay();
    }
    setStyleSheet("");  // reset to default QLineEdit appearance
}

std::string HotkeyEdit::fcitx5Value() const {
    return fcitx5Value_;
}

void HotkeyEdit::setFcitx5Value(const std::string &val) {
    fcitx5Value_ = val;
    refreshDisplay();
}

void HotkeyEdit::refreshDisplay() {
    if (fcitx5Value_.empty())
        setText(T("(không có)"));
    else
        setText(toDisplayString(QString::fromStdString(fcitx5Value_)));
}

QString HotkeyEdit::toDisplayString(const QString &fcitx5Combo) const {
    // Split "Control+grave" → modifiers + key, convert each part
    QStringList parts = fcitx5Combo.split('+');
    QStringList displayParts;
    for (const QString &part : parts) {
        QString p = part;
        if (p == "Control")
            displayParts << "Ctrl";
        else if (p == "Super")
            displayParts << "Super";
        else if (p == "Shift")
            displayParts << "Shift";
        else if (p == "Alt")
            displayParts << "Alt";
        else
            displayParts << fcitx5KeyToDisplay(p);
    }
    return displayParts.join("+");
}
