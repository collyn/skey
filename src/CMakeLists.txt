set(SKEY_SOURCES
    engine.cpp
    vietnamese.cpp
)

add_library(skey MODULE ${SKEY_SOURCES})
target_link_libraries(skey
    Fcitx5::Core
)
target_compile_features(skey PRIVATE cxx_std_17)

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
