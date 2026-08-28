#ifndef SKEY_SETTINGS_TR_H
#define SKEY_SETTINGS_TR_H

// Gettext i18n cho settings GUI. Ngôn ngữ nguồn là tiếng Việt: mọi chuỗi
// hiển thị là msgid tiếng Việt bọc trong T(). Domain "fcitx5-skey"
// (FCITX_GETTEXT_DOMAIN) được đăng ký ở main.cpp theo recipe ABI-safe ghi
// tại src/engine.h:416-423. xgettext trích xuất T() qua -kT (không
// c-format: placeholder %1 do QString::arg() xử lý sau).

#include <QString>
#include <fcitx-utils/i18n.h>

inline QString T(const char *s) {
    // _() → fcitx::translateDomain(FCITX_GETTEXT_DOMAIN, s); overload
    // const char* trả về const char* (không bao giờ null).
    return QString::fromUtf8(_(s));
}

#endif // SKEY_SETTINGS_TR_H
