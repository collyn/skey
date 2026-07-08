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

// Tone mark position style
enum class TonePosition { Modern, Traditional };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(TonePosition, N_("Modern (hoà)"),
                                 N_("Traditional (hòa)"));

FCITX_CONFIGURATION(
    SKeyConfig,
    // Input method: Telex or VNI
    Option<SKeyInputMethod> inputMethod{this, "InputMethod",
                                        _("Input Method"),
                                        SKeyInputMethod::Telex};
    // Output mode: uinput (default), surrounding text, or preedit
    Option<SKeyOutputMode> outputMode{this, "OutputMode", _("Output Mode"),
                                      SKeyOutputMode::Uinput};
    // Tone position style
    Option<TonePosition> tonePosition{this, "TonePosition",
                                      _("Tone Mark Position"),
                                      TonePosition::Modern};
    // Allow free tone/mark placement
    Option<bool> freeMarking{this, "FreeMarking", _("Free marking"), true};
    // Auto restore non-Vietnamese text
    Option<bool> autoRestore{this, "AutoRestore",
                             _("Auto restore non-Vietnamese"), true};
    // Show preedit text
    Option<bool> showPreedit{this, "ShowPreedit", _("Show preedit"), true};
    // Auto switch to Preedit in URL fields (e.g. Chromium address bar)
    Option<bool> chromiumAddressBarPreedit{
        this, "ChromiumAddressBarPreedit",
        _("Auto Preedit for address bar"), true};
    // Enable debug logging
    Option<bool> debug{this, "Debug", _("Enable debug logging"), false};);

} // namespace fcitx

#endif // FCITX5_SKEY_CONFIG_H
