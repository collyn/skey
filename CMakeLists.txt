cmake_minimum_required(VERSION 3.16)
project(fcitx5-skey VERSION 0.5.12)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# ── Build skey-engine Rust static library ────────────────────────────────
set(SKEY_ENGINE_VERSION "v0.1.14")
set(SKEY_ENGINE_DIR "${CMAKE_CURRENT_BINARY_DIR}/skey-engine")
set(SKEY_ENGINE_LIB "${SKEY_ENGINE_DIR}/target/release/libskey_engine.a")

# Populate skey-engine source into build dir.
# - PPA / offline: pass -DSKEY_ENGINE_SOURCE_DIR=/path/to/skey-engine
# - Normal builds: auto-clone from GitHub (requires internet)
if(DEFINED SKEY_ENGINE_SOURCE_DIR)
    if(NOT EXISTS "${SKEY_ENGINE_SOURCE_DIR}/Cargo.toml")
        message(FATAL_ERROR "SKEY_ENGINE_SOURCE_DIR=${SKEY_ENGINE_SOURCE_DIR} "
                            "but Cargo.toml not found there")
    endif()
    message(STATUS "Using skey-engine from ${SKEY_ENGINE_SOURCE_DIR} (offline mode)")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SKEY_ENGINE_SOURCE_DIR}" "${SKEY_ENGINE_DIR}"
    )
elseif(NOT EXISTS "${SKEY_ENGINE_DIR}/Cargo.toml")
    find_package(Git REQUIRED)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} clone --branch ${SKEY_ENGINE_VERSION} --depth 1
            https://github.com/collyn/skey-engine.git ${SKEY_ENGINE_DIR}
        RESULT_VARIABLE CLONE_RESULT
    )
    if(NOT CLONE_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to clone skey-engine ${SKEY_ENGINE_VERSION}")
    endif()
endif()

add_custom_command(
    OUTPUT ${SKEY_ENGINE_LIB}
    COMMAND cargo build --release
    WORKING_DIRECTORY ${SKEY_ENGINE_DIR}
    COMMENT "Building skey-engine ${SKEY_ENGINE_VERSION}"
    DEPENDS
        ${SKEY_ENGINE_DIR}/Cargo.toml
        ${SKEY_ENGINE_DIR}/src/lib.rs
        ${SKEY_ENGINE_DIR}/src/engine.rs
        ${SKEY_ENGINE_DIR}/src/tables.rs
        ${SKEY_ENGINE_DIR}/src/spelling.rs
        ${SKEY_ENGINE_DIR}/src/charset.rs
)

add_custom_target(skey_engine_build DEPENDS ${SKEY_ENGINE_LIB})

# Create an imported static library target for skey-engine
add_library(skey_engine STATIC IMPORTED)
set_target_properties(skey_engine PROPERTIES
    IMPORTED_LOCATION ${SKEY_ENGINE_LIB}
)
add_dependencies(skey_engine skey_engine_build)
# ─────────────────────────────────────────────────────────────────────────

add_subdirectory(src)
add_subdirectory(data)

# ── Qt6 settings application (optional) ─────────────────────────────────
find_package(Qt6 QUIET COMPONENTS Widgets Core)
if(Qt6_FOUND)
    message(STATUS "Qt6 found — building skey settings GUI")
    add_subdirectory(src/settings)
else()
    message(STATUS "Qt6 not found — skey settings GUI will not be built")
endif()

# ── Install setup script ──
install(PROGRAMS scripts/skey-setup DESTINATION ${CMAKE_INSTALL_BINDIR})
