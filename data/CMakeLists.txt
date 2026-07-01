# SVG icon (scalable)
install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")

# Generate PNG icons at standard sizes for Wayland/KDE system tray
# rsvg-convert from librsvg2-bin is required
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
