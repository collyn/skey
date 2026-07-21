find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network DBus)

set(SETTINGS_SOURCES
    main.cpp
    config_io.cpp
    hotkey_edit.cpp
    settings_window.cpp
    general_tab.cpp
    app_modes_tab.cpp
    info_tab.cpp
    updater.cpp
)

add_executable(fcitx5-skey-settings ${SETTINGS_SOURCES})
target_link_libraries(fcitx5-skey-settings PRIVATE Qt6::Widgets Qt6::Core Qt6::Network Qt6::DBus)
target_compile_features(fcitx5-skey-settings PRIVATE cxx_std_17)
target_compile_definitions(fcitx5-skey-settings PRIVATE SKEY_VERSION="${PROJECT_VERSION}")

install(TARGETS fcitx5-skey-settings
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
