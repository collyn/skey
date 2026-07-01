# SVG icons
# apps/ — app launcher and IM list popup
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")

# status/ — KDE system tray (system tray uses 'status' icon context, not 'apps')
# Note: Lotus installs to breeze/status/ — we follow the same pattern
# Hicolor fallback (works for all themes)
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/status"
    RENAME "fcitx-skey.svg")
# KDE Breeze theme
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/breeze/status/22"
    RENAME "fcitx-skey.svg")
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/breeze/status/24"
    RENAME "fcitx-skey.svg")
# KDE Breeze Dark theme
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/breeze-dark/status/22"
    RENAME "fcitx-skey.svg")
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/breeze-dark/status/24"
    RENAME "fcitx-skey.svg")

# PNG icons for apps/ (launcher, menus)
find_program(RSVG_CONVERT rsvg-convert)
if(RSVG_CONVERT)
    set(ICON_SIZES 16 22 24 32 48 128)
    set(SVG_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icons/fcitx-skey.svg")
    foreach(SIZE ${ICON_SIZES})
        set(PNG_ICON "${CMAKE_CURRENT_BINARY_DIR}/icons/hicolor/${SIZE}x${SIZE}/apps/fcitx-skey.png")
        add_custom_command(
            OUTPUT ${PNG_ICON}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/icons/hicolor/${SIZE}x${SIZE}/apps"
            COMMAND ${RSVG_CONVERT} -w ${SIZE} -h ${SIZE} ${SVG_ICON} -o ${PNG_ICON}
            DEPENDS ${SVG_ICON}
            COMMENT "Generating ${SIZE}x${SIZE} PNG icon"
        )
        install(FILES ${PNG_ICON}
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${SIZE}x${SIZE}/apps")
        list(APPEND PNG_ICONS ${PNG_ICON})
    endforeach()
    add_custom_target(skey_icons ALL DEPENDS ${PNG_ICONS})
else()
    message(WARNING "rsvg-convert not found — PNG tray icons will not be generated")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/fcitx5-skey-uinput-server@.service.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-skey-uinput-server@.service"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-skey-uinput-server@.service"
    DESTINATION "/lib/systemd/system")

# Environment variables for X11/Wayland (sourced by login shells and display managers)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/fcitx5-skey.sh"
    DESTINATION "/etc/profile.d")
