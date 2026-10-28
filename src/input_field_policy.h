#ifndef SKEY_INPUT_FIELD_POLICY_H
#define SKEY_INPUT_FIELD_POLICY_H

#include <string_view>

namespace skey {

struct InputField {
  std::string_view frontend;
  bool keyEventOrderFix = false;
  bool formattedPreedit = false;
  bool surroundingText = false;
  bool preedit = false;
  bool chromiumRuntime = false;
  bool nativeX11 = false;
  bool gtk4Client = false;
  bool commitsOnlyClient = false;
  bool ptyMaster = false;
};

struct ForwardKeySupport {
  bool supported;
  const char *reason;
};

inline ForwardKeySupport forwardedKeySupport(const InputField &field) {
  if (field.commitsOnlyClient)
    return {false, field.frontend == "ibus" ? "ibus-unsupported-client" : "commit-only-client"};
  if (field.frontend == "ibus") {
    if (field.gtk4Client) return {false, "ibus-unsupported-client"};
    return {true, "ibus-forward-key"};
  }
  if (field.frontend == "dbus") {
    if (!field.keyEventOrderFix) {
      if (field.nativeX11 && field.preedit && field.formattedPreedit &&
          !field.surroundingText && field.ptyMaster)
        return {true, "terminal-commit-forward"};
      return {false, "dbus-missing-ordering"};
    }
    return {true, "dbus-ordered-forward-key"};
  }
  if (field.frontend == "fcitx4") {
    if (!field.formattedPreedit && !field.surroundingText)
      return {false, "fcitx4-missing-capability"};
    return {true, "fcitx4-forward-key"};
  }
  if (field.frontend == "xim") return {true, "xim-forward-key"};
  return {false, "unsupported-frontend"};
}

inline bool usesCompositionCommits(const InputField &field) {
  return field.nativeX11 && field.chromiumRuntime && field.preedit &&
         !field.commitsOnlyClient &&
         (field.frontend == "dbus" || field.frontend == "ibus" || field.frontend == "fcitx4");
}

} // namespace skey

#endif
