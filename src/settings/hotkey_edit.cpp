#include "hotkey_edit.h"

#include <QKeyEvent>
#include <QKeySequence>

HotkeyEdit::HotkeyEdit(QWidget *parent) : QLineEdit(parent) {
    setReadOnly(true);
    setPlaceholderText(QString::fromUtf8("Nhấn tổ hợp phím..."));
    setFcitx5Value("Control+space");
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

    // Combine: the pressed modifier keys + the actual key
    // Qt sets modifiers based on which mod keys are held, so for Ctrl+Shift
    // we get ControlModifier | ShiftModifier and key() might be Key_Shift
    QString combo = formatModifiers(mods, keyCode);

    if (!combo.isEmpty()) {
        fcitx5Value_ = combo.toStdString();
        setText(combo);
        setStyleSheet(""); // clear any error styling
    }

    // Don't call base class — we consume the event
    event->accept();
}

void HotkeyEdit::focusInEvent(QFocusEvent *event) {
    QLineEdit::focusInEvent(event);
    // Select all to give visual feedback that it's ready to capture
    selectAll();
}

std::string HotkeyEdit::fcitx5Value() const {
    return fcitx5Value_;
}

void HotkeyEdit::setFcitx5Value(const std::string &val) {
    fcitx5Value_ = val;
    // Convert "Control+space" style to "Control+Space" for display
    QString display = QString::fromStdString(val);
    // Capitalize the last segment (the key name) for display
    int lastPlus = display.lastIndexOf('+');
    if (lastPlus >= 0 && lastPlus + 1 < display.size()) {
        display[lastPlus + 1] = display[lastPlus + 1].toUpper();
    } else if (!display.isEmpty()) {
        display[0] = display[0].toUpper();
    }
    setText(display);
}
