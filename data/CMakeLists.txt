# Install destinations for distro-specific files.  Overridable at configure
# time so NixOS (whose sandbox forbids writing to /lib and /etc) can point
# them inside the install prefix.  Defaults match Debian/Ubuntu/Fedora.
set(SKEY_SYSTEMD_UNIT_DIR "/lib/systemd/system" CACHE PATH
    "Systemd system unit install directory")
set(SKEY_POLKIT_RULES_DIR "/etc/polkit-1/rules.d" CACHE PATH
    "Polkit rules install directory")
set(SKEY_PROFILE_D_DIR "/etc/profile.d" CACHE PATH
    "profile.d snippet install directory")

# SVG icons
# apps/ — app launcher and IM list popup
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
# pixmaps/ — last-resort fallback for all DEs, Qt, GTK, KWin.
# KDE checks pixmaps when the current icon theme lacks an index.theme
# (e.g. third-party themes like Vivid-Glassy-Dark that don't inherit hicolor).
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps"
    RENAME "fcitx-skey.svg")
# Generate a 128px PNG for pixmaps (fallback for environments without QtSvg plugin)
find_program(RSVG_CONVERT rsvg-convert)
if(RSVG_CONVERT)
    set(SVG_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icons/fcitx-skey.svg")
    set(PIXMAPS_PNG "${CMAKE_CURRENT_BINARY_DIR}/pixmaps/fcitx-skey.png")
    add_custom_command(
        OUTPUT ${PIXMAPS_PNG}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/pixmaps"
        COMMAND ${RSVG_CONVERT} -w 128 -h 128 "${SVG_ICON}" -o ${PIXMAPS_PNG}
        DEPENDS "${SVG_ICON}"
        COMMENT "Generating 128x128 pixmaps icon"
    )
    install(FILES ${PIXMAPS_PNG}
        DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps"
        RENAME "fcitx-skey.png")
    add_custom_target(skey_icons ALL DEPENDS ${PIXMAPS_PNG})
else()
    message(WARNING "rsvg-convert not found — PNG fallback icon will not be generated")
endif()

# status/ — KDE system tray (system tray uses 'status' icon context, not 'apps')
# Hicolor fallback (works for all themes)
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/status"
    RENAME "fcitx-skey.svg")
# KDE Breeze theme — status (system tray)
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/breeze/status/22"
    RENAME "fcitx-skey.svg")
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/breeze/status/24"
    RENAME "fcitx-skey.svg")
# KDE Breeze theme — apps (taskbar via .desktop Icon= lookup)
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/breeze/apps/48"
    RENAME "fcitx-skey.svg")
# KDE Breeze Dark theme — status (system tray)
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/breeze-dark/status/22"
    RENAME "fcitx-skey.svg")
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/breeze-dark/status/24"
    RENAME "fcitx-skey.svg")
# KDE Breeze Dark theme — apps (taskbar via .desktop Icon= lookup)
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/breeze-dark/apps/48"
    RENAME "fcitx-skey.svg")

# ── Preset icons (v-blue, v-dark, v-light, v-red, vn-flag) — SVG installs (always, no rsvg-convert needed) ──
foreach(PRESET fcitx-skey-v-blue fcitx-skey-v-dark fcitx-skey-v-light fcitx-skey-v-red fcitx-skey-vn-flag)
    # apps/ — app launcher and IM list popup
    install(FILES icons/${PRESET}.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
    # pixmaps/ — last-resort fallback for all DEs, Qt, GTK, KWin
    install(FILES icons/${PRESET}.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps" RENAME "${PRESET}.svg")
    # status/ — system tray
    install(FILES icons/${PRESET}.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/status" RENAME "${PRESET}.svg")
    foreach(CTX breeze breeze-dark)
        install(FILES icons/${PRESET}.svg
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/${CTX}/status/22" RENAME "${PRESET}.svg")
        install(FILES icons/${PRESET}.svg
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/${CTX}/status/24" RENAME "${PRESET}.svg")
        install(FILES icons/${PRESET}.svg
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/${CTX}/apps/48" RENAME "${PRESET}.svg")
    endforeach()
endforeach()



configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/fcitx5-skey-uinput-server@.service.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-skey-uinput-server@.service"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-skey-uinput-server@.service"
    DESTINATION "${SKEY_SYSTEMD_UNIT_DIR}")

# Polkit rule: lets each user restart their own uinput server instance
# (systemd manage-units for fcitx5-skey-uinput-server@<user>.service only).
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/60-fcitx5-skey-uinput.rules"
    DESTINATION "${SKEY_POLKIT_RULES_DIR}")

# ── Settings GUI .desktop file ──
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/fcitx5-skey-settings.desktop.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-skey-settings.desktop"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-skey-settings.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

# Environment variables for X11/Wayland (sourced by login shells and display managers)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/fcitx5-skey.sh"
    DESTINATION "${SKEY_PROFILE_D_DIR}")
