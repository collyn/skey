set(SKEY_SOURCES
    engine.cpp
    vietnamese.cpp
    a11y_monitor.cpp
)

# Find dbus-1 for AT-SPI2 accessibility monitoring
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)

add_library(skey MODULE ${SKEY_SOURCES})
target_link_libraries(skey
    Fcitx5::Core
    bamboo_core
    ${DBUS_LIBRARIES}
    pthread
    dl
    m
)
target_include_directories(skey PRIVATE ${DBUS_INCLUDE_DIRS})
target_compile_features(skey PRIVATE cxx_std_17)
target_compile_definitions(skey PRIVATE
    FCITX_SKEY_ICON_PATH="${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/scalable/apps/fcitx-skey.svg"
)

# Include path for bamboo_ffi.h
target_include_directories(skey PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../bamboo"
)

add_executable(fcitx5-skey-uinput-server uinput_server.cpp)
target_compile_features(fcitx5-skey-uinput-server PRIVATE cxx_std_17)

# ── Test executable: Vietnamese typing tests ─────────────────────────────
add_executable(test_vietnamese test_vietnamese.cpp vietnamese.cpp)
target_link_libraries(test_vietnamese bamboo_core pthread dl)
target_compile_features(test_vietnamese PRIVATE cxx_std_17)
target_include_directories(test_vietnamese PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../bamboo"
)
# Enable CTest integration
add_test(NAME VietnameseTyping COMMAND test_vietnamese)
enable_testing()

install(TARGETS skey DESTINATION "${FCITX_INSTALL_ADDONDIR}")
install(TARGETS fcitx5-skey-uinput-server DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Install addon config - use configure_file since we have no translations yet
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/skey-addon.conf.in"
    "${CMAKE_CURRENT_BINARY_DIR}/skey.conf"
    COPYONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/skey.conf"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon")

# Install input method config
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/skey-im.conf.in"
    "${CMAKE_CURRENT_BINARY_DIR}/skey-im.conf"
    COPYONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/skey-im.conf"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/inputmethod")
