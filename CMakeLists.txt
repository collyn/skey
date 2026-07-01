cmake_minimum_required(VERSION 3.16)
project(fcitx5-skey VERSION 0.1.6)

find_package(ECM 1.0.0 REQUIRED)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

# KDECompilerSettings adds -fno-operator-names and -fno-exceptions
# but fcitx5 headers use 'not' keyword and 'throw', so we must re-enable them
string(REPLACE "-fno-operator-names" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "-fno-exceptions" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

find_package(Fcitx5Core REQUIRED)

include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

add_definitions(-DFCITX_GETTEXT_DOMAIN=\"fcitx5-skey\" -D_GNU_SOURCE)
fcitx5_add_i18n_definition()

# ── Build bamboo-core Rust static library ──────────────────────────────
set(BAMBOO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bamboo")
set(BAMBOO_LIB "${BAMBOO_DIR}/target/release/libskey_bamboo.a")

add_custom_command(
    OUTPUT ${BAMBOO_LIB}
    COMMAND cargo build --release
    WORKING_DIRECTORY ${BAMBOO_DIR}
    COMMENT "Building bamboo-core Rust static library"
    DEPENDS
        ${BAMBOO_DIR}/Cargo.toml
        ${BAMBOO_DIR}/src/lib.rs
)

add_custom_target(bamboo_build DEPENDS ${BAMBOO_LIB})

# Create an imported static library target for bamboo-core
add_library(bamboo_core STATIC IMPORTED)
set_target_properties(bamboo_core PROPERTIES
    IMPORTED_LOCATION ${BAMBOO_LIB}
)
add_dependencies(bamboo_core bamboo_build)
# ───────────────────────────────────────────────────────────────────────

add_subdirectory(src)
add_subdirectory(data)

# ── Install setup script ──
install(PROGRAMS scripts/skey-setup DESTINATION ${CMAKE_INSTALL_BINDIR})
