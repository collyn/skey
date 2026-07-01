find_package(Qt6 REQUIRED COMPONENTS Widgets Core)

set(SETTINGS_SOURCES
    main.cpp
    config_io.cpp
    settings_window.cpp
    general_tab.cpp
    app_modes_tab.cpp
)

add_executable(fcitx5-skey-settings ${SETTINGS_SOURCES})
target_link_libraries(fcitx5-skey-settings PRIVATE Qt6::Widgets Qt6::Core)
target_compile_features(fcitx5-skey-settings PRIVATE cxx_std_17)

install(TARGETS fcitx5-skey-settings
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
