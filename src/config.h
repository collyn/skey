#ifndef FCITX5_SKEY_CONFIG_H
#define FCITX5_SKEY_CONFIG_H

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-utils/i18n.h>

namespace fcitx {

// Input method type
enum class SKeyInputMethod { Telex, VNI };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(SKeyInputMethod, N_("Telex"), N_("VNI"));

// Output mode
enum class SKeyOutputMode { Uinput, SurroundingText, Preedit, Auto };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(SKeyOutputMode, N_("Uinput"),
                                 N_("Surrounding Text"),
                                 N_("Preedit"),
                                 N_("Auto"));

// Output mode used only in Chromium-family browser address bars.
enum class SKeyChromiumAddressBarMode {
    Auto, Uinput, SurroundingText, Preedit, NoVietnamese
};
FCITX_CONFIG_ENUM_NAME_WITH_I18N(SKeyChromiumAddressBarMode, N_("Auto"),
                                 N_("Uinput"),
                                 N_("Surrounding Text"),
                                 N_("Preedit"),
                                 N_("No Vietnamese"));

// Character set / encoding
enum class SKeyCharset { Unicode, TCVN3, VNIWindows };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(SKeyCharset, N_("Unicode"),
                                 N_("TCVN3 (ABC)"), N_("VNI Windows"));


FCITX_CONFIGURATION(
    SKeyConfig,
    // Input method: Telex or VNI
    Option<SKeyInputMethod> inputMethod{this, "InputMethod",
                                        _("Input Method"),
                                        SKeyInputMethod::Telex};
    // Telex only: type bare 'w' → 'ư' (uses bamboo telex_w)
    Option<bool> shortW{this, "ShortW", _("Gõ w thành ư"), false};
    // Telex only: type '[' → 'ơ' and ']' → 'ư' (UniKey-style)
    Option<bool> bracketUO{this, "BracketUO", _("Gõ ][ thành ư ơ"), false};
    // Character set / encoding
    Option<SKeyCharset> charset{this, "Charset", _("Bảng mã"),
                                SKeyCharset::Unicode};
    // Output mode: uinput (default), surrounding text, or preedit
    Option<SKeyOutputMode> outputMode{this, "OutputMode", _("Output Mode"),
                                      SKeyOutputMode::Auto};
    // Allow free tone/mark placement
    Option<bool> freeMarking{this, "FreeMarking", _("Free marking"), false};
    // Auto restore non-Vietnamese text
    Option<bool> autoRestore{this, "AutoRestore",
                             _("Auto restore non-Vietnamese"), true};
    // Show preedit text
    Option<bool> showPreedit{this, "ShowPreedit", _("Show preedit"), true};
    // Output mode used in Chromium-family browser address bars
    Option<SKeyChromiumAddressBarMode> chromiumAddressBarMode{
        this, "ChromiumAddressBarMode",
        _("Chromium address bar"), SKeyChromiumAddressBarMode::Auto};
    // Enable debug logging
    Option<bool> debug{this, "Debug", _("Enable debug logging"), false};);

} // namespace fcitx

#endif // FCITX5_SKEY_CONFIG_H
