find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network DBus)

set(SETTINGS_SOURCES
    main.cpp
    config_io.cpp
    hotkey_edit.cpp
    settings_window.cpp
    general_tab.cpp
    app_modes_tab.cpp
    macro_tab.cpp
    dict_tab.cpp
    dict_view_dialog.cpp
    appearance_tab.cpp
    info_tab.cpp
    updater.cpp
    ../icon_resolver.cpp
)

add_executable(fcitx5-skey-settings ${SETTINGS_SOURCES})
# skey_engine: embedded dictionary word list for the full-dictionary viewer.
target_include_directories(fcitx5-skey-settings PRIVATE "${SKEY_ENGINE_DIR}")
target_link_libraries(fcitx5-skey-settings PRIVATE Qt6::Widgets Qt6::Core Qt6::Network Qt6::DBus skey_engine pthread dl)
target_compile_features(fcitx5-skey-settings PRIVATE cxx_std_17)
target_compile_definitions(fcitx5-skey-settings PRIVATE
    SKEY_VERSION="${SKEY_APP_VERSION}"
    SKEY_DEV_BUILD=${SKEY_DEV_BUILD}
    # Same compile-time icon paths as the addon target (src/CMakeLists.txt):
    # the packaged icons live under the install prefix, and distros without
    # /usr/share (NixOS) resolve them through these.
    FCITX_SKEY_ICON_PATH="${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/scalable/apps/fcitx-skey.svg"
    FCITX_SKEY_ICON_DIR="${CMAKE_INSTALL_PREFIX}/share/icons")

# Optional Qt6 SVG for native SVG icon rendering (custom icon uploads + presets).
# Gracefully degrades to PNG fallback when absent.
find_package(Qt6 QUIET COMPONENTS Svg)
if(Qt6Svg_FOUND)
    target_link_libraries(fcitx5-skey-settings PRIVATE Qt6::Svg)
endif()

install(TARGETS fcitx5-skey-settings
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
