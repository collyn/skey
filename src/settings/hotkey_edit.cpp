#include "hotkey_edit.h"
#include "tr.h"

#include <QKeyEvent>
#include <QKeySequence>

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

    QString combo = formatModifiers(mods, keyCode);

    if (!combo.isEmpty()) {
        fcitx5Value_ = combo.toStdString();
        // Convert fcitx5 format to display format: "Control+grave" → "Ctrl+`"
        setText(toDisplayString(combo));
        // Green flash to confirm capture, uses the same bg for :focus to avoid flicker
        setStyleSheet(
            "QLineEdit { background: #c8e6c9; color: #1b5e20; font-weight: bold;"
            "  border: 2px solid #4caf50; border-radius: 3px; }"
            "QLineEdit:focus { background: #c8e6c9; color: #1b5e20; font-weight: bold;"
            "  border: 2px solid #4caf50; border-radius: 3px; }");
    }

    event->accept();
}

void HotkeyEdit::focusInEvent(QFocusEvent *event) {
    QLineEdit::focusInEvent(event);
    // Replace text with instruction — much more visible than placeholder
    setText(T("⌨ Nhấn phím mới..."));
    // Blue highlight: clearly distinct from normal state, good contrast with text
    setStyleSheet(
        "QLineEdit { background: #bbdefb; color: #0d47a1; font-weight: bold;"
        "  border: 2px solid #2196f3; border-radius: 3px; }"
        "QLineEdit:focus { background: #bbdefb; color: #0d47a1; font-weight: bold;"
        "  border: 2px solid #2196f3; border-radius: 3px; }");
}

void HotkeyEdit::focusOutEvent(QFocusEvent *event) {
    QLineEdit::focusOutEvent(event);
    // If user clicked away without capturing a key, restore the original value
    if (text() == T("⌨ Nhấn phím mới...") || text().isEmpty()) {
        setText(toDisplayString(QString::fromStdString(fcitx5Value_)));
    }
    // Also restore if the text is still the instruction (edge case)
    setStyleSheet("");  // reset to default QLineEdit appearance
}

std::string HotkeyEdit::fcitx5Value() const {
    return fcitx5Value_;
}

void HotkeyEdit::setFcitx5Value(const std::string &val) {
    fcitx5Value_ = val;
    setText(toDisplayString(QString::fromStdString(val)));
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
