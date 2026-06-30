install(FILES icons/fcitx-skey.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/fcitx5-skey-uinput-server@.service.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-skey-uinput-server@.service"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-skey-uinput-server@.service"
    DESTINATION "lib/systemd/system")
