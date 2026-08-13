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
# Generate a 128px PNG for pixmaps (no SVG renderer in KWin/plasmashell)
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
    list(APPEND PNG_ICONS ${PIXMAPS_PNG})
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

# PNG icons for apps/ (launcher, menus) and status/ (system tray)
find_program(RSVG_CONVERT rsvg-convert)
if(RSVG_CONVERT)
    set(ICON_SIZES 16 22 24 32 48 128)
    set(SVG_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icons/fcitx-skey.svg")
    foreach(SIZE ${ICON_SIZES})
        # apps/ context — launcher, menus, settings
        set(PNG_ICON_APP "${CMAKE_CURRENT_BINARY_DIR}/icons/hicolor/${SIZE}x${SIZE}/apps/fcitx-skey.png")
        add_custom_command(
            OUTPUT ${PNG_ICON_APP}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/icons/hicolor/${SIZE}x${SIZE}/apps"
            COMMAND ${RSVG_CONVERT} -w ${SIZE} -h ${SIZE} ${SVG_ICON} -o ${PNG_ICON_APP}
            DEPENDS ${SVG_ICON}
            COMMENT "Generating ${SIZE}x${SIZE} PNG app icon"
        )
        install(FILES ${PNG_ICON_APP}
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${SIZE}x${SIZE}/apps")
        list(APPEND PNG_ICONS ${PNG_ICON_APP})

        # status/ context — system tray (ensures all icon themes find the icon
        # via hicolor fallback, not just Breeze which has explicit installs)
        set(PNG_ICON_STATUS "${CMAKE_CURRENT_BINARY_DIR}/icons/hicolor/${SIZE}x${SIZE}/status/fcitx-skey.png")
        add_custom_command(
            OUTPUT ${PNG_ICON_STATUS}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/icons/hicolor/${SIZE}x${SIZE}/status"
            COMMAND ${RSVG_CONVERT} -w ${SIZE} -h ${SIZE} ${SVG_ICON} -o ${PNG_ICON_STATUS}
            DEPENDS ${SVG_ICON}
            COMMENT "Generating ${SIZE}x${SIZE} PNG status icon"
        )
        install(FILES ${PNG_ICON_STATUS}
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${SIZE}x${SIZE}/status")
        list(APPEND PNG_ICONS ${PNG_ICON_STATUS})
    endforeach()

    # ── Preset icon PNG generation (rsvg-convert required) ──
    foreach(PRESET fcitx-skey-v-blue fcitx-skey-v-dark fcitx-skey-v-light fcitx-skey-v-red fcitx-skey-vn-flag)
        set(PRESET_SVG "${CMAKE_CURRENT_SOURCE_DIR}/icons/${PRESET}.svg")
        # 128px pixmaps PNG (last-resort fallback for all DEs)
        set(PRESET_PIXMAPS_PNG "${CMAKE_CURRENT_BINARY_DIR}/pixmaps/${PRESET}.png")
        add_custom_command(
            OUTPUT ${PRESET_PIXMAPS_PNG}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/pixmaps"
            COMMAND ${RSVG_CONVERT} -w 128 -h 128 "${PRESET_SVG}" -o ${PRESET_PIXMAPS_PNG}
            DEPENDS "${PRESET_SVG}"
            COMMENT "Generating 128x128 pixmaps icon for ${PRESET}"
        )
        install(FILES ${PRESET_PIXMAPS_PNG}
            DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps")
        list(APPEND PNG_ICONS ${PRESET_PIXMAPS_PNG})

        # Hicolor PNGs for DEs that prefer raster icons
        foreach(SIZE 16 22 24 32 48 128)
            foreach(CTX apps status)
                set(PRESET_PNG_OUT "${CMAKE_CURRENT_BINARY_DIR}/icons/hicolor/${SIZE}x${SIZE}/${CTX}/${PRESET}.png")
                add_custom_command(
                    OUTPUT ${PRESET_PNG_OUT}
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${CMAKE_CURRENT_BINARY_DIR}/icons/hicolor/${SIZE}x${SIZE}/${CTX}"
                    COMMAND ${RSVG_CONVERT} -w ${SIZE} -h ${SIZE} "${PRESET_SVG}" -o ${PRESET_PNG_OUT}
                    DEPENDS "${PRESET_SVG}"
                    COMMENT "Generating ${SIZE}x${SIZE} PNG ${CTX} icon for ${PRESET}"
                )
                install(FILES ${PRESET_PNG_OUT}
                    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${SIZE}x${SIZE}/${CTX}")
                list(APPEND PNG_ICONS ${PRESET_PNG_OUT})
            endforeach()
        endforeach()
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
    DESTINATION "/etc/profile.d")
