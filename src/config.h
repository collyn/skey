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

// Character set / encoding — IDs match skey-engine VietCharset enum
enum class SKeyCharset {
    Unicode, TCVN3, VNIWindows, WinCP1258, VIQR,
    VPS, VISCII, BKHCM1, VietwareF, ISC,
    BKHCM2, VietwareX, VNIMac, UnicodeDecomposed
};
FCITX_CONFIG_ENUM_NAME_WITH_I18N(SKeyCharset,
    N_("Unicode"), N_("TCVN3 (ABC)"), N_("VNI Windows"),
    N_("Windows CP1258"), N_("VIQR"),
    N_("VPS"), N_("VISCII"), N_("BKHCM1"), N_("Vietware-F"), N_("ISC"),
    N_("BKHCM2"), N_("Vietware-X"), N_("VNI-MAC"), N_("Unicode NFD"));


FCITX_CONFIGURATION(
    SKeyConfig,
    Option<SKeyInputMethod> inputMethod{this, "InputMethod",
                                        _("Kiểu gõ"),
                                        SKeyInputMethod::Telex};
    Option<bool> shortW{this, "ShortW", _("Gõ w thành ư"), false};
    Option<bool> bracketUO{this, "BracketUO", _("Gõ ][ thành ư ơ"), false};
    Option<SKeyCharset> charset{this, "Charset", _("Bảng mã"),
                                SKeyCharset::Unicode};
    Option<SKeyOutputMode> outputMode{this, "OutputMode", _("Chế độ xuất"),
                                      SKeyOutputMode::Auto};
    Option<bool> freeMarking{this, "FreeMarking", _("Đánh dấu tự do"), false};
    Option<bool> autoRestore{this, "AutoRestore",
                             _("Tự động khôi phục"), true};
    Option<bool> showPreedit{this, "ShowPreedit", _("Hiện preedit"), true};
    Option<SKeyChromiumAddressBarMode> chromiumAddressBarMode{
        this, "ChromiumAddressBarMode",
        _("Thanh địa chỉ Chromium"), SKeyChromiumAddressBarMode::Auto};
    Option<bool> debug{this, "Debug", _("Ghi log debug"), false};

    Option<bool> enableMacro{this, "EnableMacro", _("Bật gõ tắt"), true};
    Option<bool> capitalizeMacro{this, "CapitalizeMacro",
                                 _("Viết hoa macro"), true};
    Option<bool> macroInOffMode{this, "MacroInOffMode",
                                _("Gõ tắt cả khi tắt tiếng Việt"), false};
    SubConfigOption macroEditor{this, "MacroEditor", _("Gõ tắt"),
                                "fcitx://config/addon/skey/skey-macro"};
    Option<std::string> modeMenuKey{this, "ModeMenuKey",
                                    _("Phím menu chế độ"), "grave"};
    Option<std::string> iconTheme{this, "IconTheme",
                                   _("Biểu tượng"), "default"};
    Option<std::string> customIconPath{this, "CustomIconPath",
                                        _("Đường dẫn biểu tượng tùy chỉnh"), ""};);

FCITX_CONFIGURATION(
    skeyMacroEntry,
    Option<std::string> key{this, "Key", _("Từ viết tắt"), ""};
    Option<std::string> value{this, "Value", _("Thành"), ""};);

FCITX_CONFIGURATION(
    skeyMacroTableConfig,
    OptionWithAnnotation<std::vector<skeyMacroEntry>, ListDisplayOptionAnnotation>
        entries{this, "Entries", _("Danh sách"), {}, {}, {},
                ListDisplayOptionAnnotation("Key")};);

} // namespace fcitx

#endif // FCITX5_SKEY_CONFIG_H
