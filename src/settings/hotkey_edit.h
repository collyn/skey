#ifndef SKEY_SETTINGS_HOTKEY_EDIT_H
#define SKEY_SETTINGS_HOTKEY_EDIT_H

#include <QLineEdit>

/// A read-only QLineEdit that captures a single key combination (including
/// modifier-only combos like Ctrl+Shift which QKeySequenceEdit rejects).
/// The internal value is stored in fcitx5 format ("Control+Shift_L").
class HotkeyEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit HotkeyEdit(QWidget *parent = nullptr);

    /// Get the current hotkey in fcitx5 format (e.g. "Control+space")
    std::string fcitx5Value() const;
    /// Set from fcitx5 format
    void setFcitx5Value(const std::string &val);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;

private:
    QString formatModifiers(Qt::KeyboardModifiers mods, int keyCode);
    std::string fcitx5Value_;
};

#endif // SKEY_SETTINGS_HOTKEY_EDIT_H
