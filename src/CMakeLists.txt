# These targets are pure C++ (no Qt).  KDECompilerSettings enables
# AUTOMOC globally; turn it off here to suppress dev warnings.
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)

set(SKEY_SOURCES
    engine.cpp
    vietnamese.cpp
    viet_recompose.cpp
    viet_undo.cpp
    viet_restore.cpp
    charset.cpp
    a11y_monitor.cpp
    icon_resolver.cpp
    x11_app_name.cpp
)

# Find dbus-1 for AT-SPI2 accessibility monitoring, and xcb for the
# focused-window WM_CLASS lookup (empty-program IBus-frontend apps).
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)
pkg_check_modules(XCB REQUIRED xcb)

add_library(skey MODULE ${SKEY_SOURCES})
target_link_libraries(skey
    Fcitx5::Core
    skey_engine
    ${DBUS_LIBRARIES}
    ${XCB_LIBRARIES}
    pthread
    dl
    m
)
target_include_directories(skey PRIVATE ${DBUS_INCLUDE_DIRS} ${XCB_INCLUDE_DIRS})
target_compile_features(skey PRIVATE cxx_std_17)
target_compile_definitions(skey PRIVATE
    FCITX_SKEY_ICON_PATH="${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/scalable/apps/fcitx-skey.svg"
)

# Include path for skey_engine.h
target_include_directories(skey PRIVATE "${SKEY_ENGINE_DIR}")

add_executable(fcitx5-skey-uinput-server uinput_server.cpp)
target_compile_features(fcitx5-skey-uinput-server PRIVATE cxx_std_17)

# ── Test executable: Vietnamese typing tests ─────────────────────────
add_executable(test_vietnamese test_vietnamese.cpp vietnamese.cpp viet_recompose.cpp viet_undo.cpp viet_restore.cpp charset.cpp)
target_link_libraries(test_vietnamese skey_engine pthread dl)
target_compile_features(test_vietnamese PRIVATE cxx_std_17)
target_include_directories(test_vietnamese PRIVATE "${SKEY_ENGINE_DIR}")
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
