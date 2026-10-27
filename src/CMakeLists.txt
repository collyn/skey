# These targets are pure C++ (no Qt).  KDECompilerSettings enables
# AUTOMOC globally; turn it off here to suppress dev warnings.
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)

set(SKEY_SOURCES
    engine.cpp
    native_injector.cpp
    uinput_injector.cpp
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
pkg_check_modules(NATIVE_X11 REQUIRED IMPORTED_TARGET x11 xtst)

add_library(skey MODULE ${SKEY_SOURCES})
target_link_libraries(skey
    Fcitx5::Core
    skey_engine
    ${DBUS_LIBRARIES}
    ${XCB_LIBRARIES}
    PkgConfig::NATIVE_X11
    pthread
    dl
    m
)
target_include_directories(skey PRIVATE ${DBUS_INCLUDE_DIRS} ${XCB_INCLUDE_DIRS})
target_compile_features(skey PRIVATE cxx_std_17)
target_compile_definitions(skey PRIVATE
    FCITX_SKEY_ICON_PATH="${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/scalable/apps/fcitx-skey.svg"
    FCITX_SKEY_ICON_DIR="${CMAKE_INSTALL_PREFIX}/share/icons"
)

# Include path for skey_engine.h
target_include_directories(skey PRIVATE "${SKEY_ENGINE_DIR}")

add_executable(test_uinput_injector test_uinput_injector.cpp uinput_injector.cpp)
target_compile_features(test_uinput_injector PRIVATE cxx_std_17)
target_link_libraries(test_uinput_injector pthread)
add_test(NAME UinputInjector COMMAND test_uinput_injector)
set_tests_properties(UinputInjector PROPERTIES TIMEOUT 10)
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME UinputMigration COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/scripts/test_uinput_migration.py")
    add_executable(test_setup_profile_reader test_setup_profile_reader.cpp)
    target_compile_features(test_setup_profile_reader PRIVATE cxx_std_17)
    target_link_libraries(test_setup_profile_reader Fcitx5::Config)
    add_test(NAME KdeEnvironment COMMAND ${CMAKE_COMMAND} -E env
        "SKEY_TEST_PROFILE_READER=$<TARGET_FILE:test_setup_profile_reader>"
        ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/scripts/test_kde_environment.py")
endif()

# ── Test executable: Vietnamese typing tests ─────────────────────────
add_executable(test_vietnamese test_vietnamese.cpp vietnamese.cpp viet_recompose.cpp viet_undo.cpp viet_restore.cpp charset.cpp)
target_link_libraries(test_vietnamese skey_engine pthread dl)
target_compile_features(test_vietnamese PRIVATE cxx_std_17)
target_include_directories(test_vietnamese PRIVATE "${SKEY_ENGINE_DIR}")
# Enable CTest integration
add_test(NAME VietnameseTyping COMMAND test_vietnamese)
add_executable(test_charset test_charset.cpp charset.cpp)
target_link_libraries(test_charset skey_engine pthread dl)
target_compile_features(test_charset PRIVATE cxx_std_17)
target_include_directories(test_charset PRIVATE "${SKEY_ENGINE_DIR}")
add_test(NAME CharsetConversion COMMAND test_charset)
add_executable(test_sheets_cell_tracker test_sheets_cell_tracker.cpp vietnamese.cpp viet_recompose.cpp viet_undo.cpp viet_restore.cpp charset.cpp)
target_link_libraries(test_sheets_cell_tracker skey_engine pthread dl)
target_include_directories(test_sheets_cell_tracker PRIVATE "${SKEY_ENGINE_DIR}")
target_compile_features(test_sheets_cell_tracker PRIVATE cxx_std_17)
add_test(NAME SheetsCellTracking COMMAND test_sheets_cell_tracker)

add_executable(test_app_delay_key test_app_delay_key.cpp)
target_compile_features(test_app_delay_key PRIVATE cxx_std_17)
add_test(NAME AppDelayKey COMMAND test_app_delay_key)
add_executable(test_a11y_monitor test_a11y_monitor.cpp)
target_link_libraries(test_a11y_monitor ${DBUS_LIBRARIES} pthread)
target_include_directories(test_a11y_monitor PRIVATE ${DBUS_INCLUDE_DIRS})
target_compile_features(test_a11y_monitor PRIVATE cxx_std_17)
add_test(NAME A11yMonitor COMMAND test_a11y_monitor)
add_executable(test_engine_performance test_engine_performance.cpp ${SKEY_SOURCES})
target_link_libraries(test_engine_performance Fcitx5::Core skey_engine ${DBUS_LIBRARIES} ${XCB_LIBRARIES} PkgConfig::NATIVE_X11 pthread dl m)
target_include_directories(test_engine_performance PRIVATE ${SKEY_ENGINE_DIR} ${DBUS_INCLUDE_DIRS} ${XCB_INCLUDE_DIRS})
target_compile_features(test_engine_performance PRIVATE cxx_std_17)
target_compile_definitions(test_engine_performance PRIVATE SKEY_TESTING=1 FCITX_SKEY_ICON_PATH="/tmp/skey-test.svg" FCITX_SKEY_ICON_DIR="/tmp")
add_test(NAME EnginePerformance COMMAND test_engine_performance)
add_test(NAME GnomeAddressBarMenu COMMAND ${CMAKE_COMMAND} -E env
    SKEY_TEST_GNOME_MENU_ONLY=1 $<TARGET_FILE:test_engine_performance>)
add_test(NAME GnomeX11AddressBarAuto COMMAND ${CMAKE_COMMAND} -E env
    SKEY_TEST_X11_AUTO_MENU_ONLY=1 SKEY_TEST_EXPECT_GNOME=1 $<TARGET_FILE:test_engine_performance>)
add_test(NAME KdeX11AddressBarAuto COMMAND ${CMAKE_COMMAND} -E env
    --unset=SKEY_TEST_EXPECT_GNOME SKEY_TEST_X11_AUTO_MENU_ONLY=1 $<TARGET_FILE:test_engine_performance>)
add_test(NAME GnomeOmniboxUinput COMMAND ${CMAKE_COMMAND} -E env
    SKEY_TEST_GNOME_UINPUT_ONLY=1 $<TARGET_FILE:test_engine_performance>)
add_test(NAME GnomeOmniboxEditing COMMAND ${CMAKE_COMMAND} -E env
    SKEY_TEST_GNOME_EDIT_ONLY=1 $<TARGET_FILE:test_engine_performance>)
add_test(NAME GnomeOmniboxNative COMMAND ${CMAKE_COMMAND} -E env
    SKEY_TEST_GNOME_NATIVE_ONLY=1 $<TARGET_FILE:test_engine_performance>)
set_tests_properties(GnomeOmniboxUinput GnomeOmniboxEditing GnomeOmniboxNative PROPERTIES TIMEOUT 15)
add_test(NAME X11SelectReplacement COMMAND ${CMAKE_COMMAND} -E env
    SKEY_TEST_X11_SELECT_ONLY=1 $<TARGET_FILE:test_engine_performance>)
add_test(NAME InjectedBackspaceContract COMMAND ${CMAKE_COMMAND} -E env
    SKEY_TEST_BACKSPACE_ONLY=1 $<TARGET_FILE:test_engine_performance>)
add_test(NAME WebKitSurroundingText COMMAND ${CMAKE_COMMAND} -E env
    SKEY_TEST_WEBKIT_ONLY=1 $<TARGET_FILE:test_engine_performance>)
add_test(NAME WaylandOmniboxSurroundingText COMMAND ${CMAKE_COMMAND} -E env
    SKEY_TEST_WAYLAND_OMNIBOX_ONLY=1 $<TARGET_FILE:test_engine_performance>)
add_test(NAME X11OmniboxSurroundingText COMMAND ${CMAKE_COMMAND} -E env
    SKEY_TEST_X11_OMNIBOX_ONLY=1 $<TARGET_FILE:test_engine_performance>)
add_test(NAME X11SurroundingFallback COMMAND ${CMAKE_COMMAND} -E env
    SKEY_TEST_X11_SURR_FALLBACK_ONLY=1 $<TARGET_FILE:test_engine_performance>)
set_tests_properties(EnginePerformance PROPERTIES TIMEOUT 15)
find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    add_executable(test_a11y_integration test_a11y_integration.cpp a11y_monitor.cpp)
    target_link_libraries(test_a11y_integration ${DBUS_LIBRARIES} pthread)
    target_include_directories(test_a11y_integration PRIVATE ${DBUS_INCLUDE_DIRS})
    target_compile_features(test_a11y_integration PRIVATE cxx_std_17)
    add_test(NAME A11yIntegration COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_a11y_integration>)
    set_tests_properties(A11yIntegration PROPERTIES TIMEOUT 15)
endif()
enable_testing()

install(TARGETS skey DESTINATION "${FCITX_INSTALL_ADDONDIR}")

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

# Optional experimental portal keyboard backend. Ordinary builds keep working.
pkg_check_modules(LIBEI QUIET IMPORTED_TARGET libei-1.0>=1.0)
pkg_check_modules(LIBPORTAL QUIET IMPORTED_TARGET libportal>=0.7.1)
if(LIBEI_FOUND AND LIBPORTAL_FOUND)
    message(STATUS "Libei backend enabled (libei ${LIBEI_VERSION}, libportal ${LIBPORTAL_VERSION})")
    foreach(target skey test_engine_performance)
        target_compile_definitions(${target} PRIVATE SKEY_HAVE_LIBEI=1)
        if(LIBPORTAL_VERSION VERSION_GREATER_EQUAL 0.8)
            target_compile_definitions(${target} PRIVATE SKEY_LIBPORTAL_PERSIST=1)
        endif()
        target_link_libraries(${target} PkgConfig::LIBEI PkgConfig::LIBPORTAL)
    endforeach()
else()
    message(WARNING "Libei backend disabled: requires libei >= 1.0 and libportal >= 0.7.1")
endif()
