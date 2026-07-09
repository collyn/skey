#ifndef FCITX5_SKEY_CONFIG_H
#define FCITX5_SKEY_CONFIG_H

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-utils/i18n.h>

namespace fcitx {

// Input method type
enum class SKeyInputMethod { Telex, VNI, TelexW };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(SKeyInputMethod, N_("Telex"), N_("VNI"),
                                 N_("Telex W"));

// Output mode
enum class SKeyOutputMode { Uinput, SurroundingText, Preedit };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(SKeyOutputMode, N_("Uinput"),
                                 N_("Surrounding Text"),
                                 N_("Preedit"));

// Chromium address bar behavior: auto-Preedit vs. disable Vietnamese
enum class SKeyChromiumAddressBarMode { Preedit, NoVietnamese };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(SKeyChromiumAddressBarMode, N_("Preedit"),
                                 N_("No Vietnamese"));


FCITX_CONFIGURATION(
    SKeyConfig,
    // Input method: Telex or VNI
    Option<SKeyInputMethod> inputMethod{this, "InputMethod",
                                        _("Input Method"),
                                        SKeyInputMethod::Telex};
    // Output mode: uinput (default), surrounding text, or preedit
    Option<SKeyOutputMode> outputMode{this, "OutputMode", _("Output Mode"),
                                      SKeyOutputMode::Uinput};
    // Allow free tone/mark placement
    Option<bool> freeMarking{this, "FreeMarking", _("Free marking"), false};
    // Auto restore non-Vietnamese text
    Option<bool> autoRestore{this, "AutoRestore",
                             _("Auto restore non-Vietnamese"), true};
    // Show preedit text
    Option<bool> showPreedit{this, "ShowPreedit", _("Show preedit"), true};
    // Chromium address bar behavior (auto-Preedit or disable Vietnamese)
    Option<SKeyChromiumAddressBarMode> chromiumAddressBarMode{
        this, "ChromiumAddressBarMode",
        _("Chromium address bar"), SKeyChromiumAddressBarMode::Preedit};
    // Enable debug logging
    Option<bool> debug{this, "Debug", _("Enable debug logging"), false};);

} // namespace fcitx

#endif // FCITX5_SKEY_CONFIG_H
