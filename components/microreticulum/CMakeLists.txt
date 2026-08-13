# Reticulum Network Stack transport node (github.com/attermann/microReticulum),
# vendored as git submodules under third_party/. Scoped to ESP32-C3 only for
# now, same as eink_display -- it reuses that component's arduino-esp32
# dependency (microReticulum's ESP32 path is Arduino-only; see the .cpp files
# for why) rather than adding a second, untested build configuration.
#
# EXPERIMENTAL: this wiring has not been compiled or run on real hardware.
# See include/microreticulum.h for the current known gaps before enabling it.
if(IDF_TARGET STREQUAL "esp32c3")
    set(TP_DIR              "${CMAKE_CURRENT_LIST_DIR}/../../third_party")
    set(RNS_DIR              "${TP_DIR}/microReticulum")
    set(MICROSTORE_DIR       "${TP_DIR}/microstore")
    set(CRYPTO_DIR           "${TP_DIR}/Crypto")
    set(MSGPACK_DIR          "${TP_DIR}/MsgPack")
    set(ARXCONTAINER_DIR     "${TP_DIR}/ArxContainer")
    set(ARXTYPETRAITS_DIR    "${TP_DIR}/ArxTypeTraits")
    set(DEBUGLOG_DIR         "${TP_DIR}/DebugLog")

    if(NOT EXISTS "${RNS_DIR}/src/microReticulum/Reticulum.h")
        message(FATAL_ERROR
            "microreticulum: one or more third_party submodules are missing. Run: "
            "git submodule update --init third_party/microReticulum third_party/microstore "
            "third_party/Crypto third_party/MsgPack third_party/ArxContainer "
            "third_party/ArxTypeTraits third_party/DebugLog")
    endif()

    # No CONFIGURE_DEPENDS: ESP-IDF evaluates component CMakeLists.txt files
    # once in a restricted "script mode" pass (just to gather REQUIRES) before
    # the real configure pass, and CONFIGURE_DEPENDS is invalid there. These
    # are pinned submodules, so the file list won't change without a
    # deliberate re-vendor anyway (which already means re-running cmake).
    file(GLOB_RECURSE RNS_SRCS "${RNS_DIR}/src/microReticulum/*.cpp")
    file(GLOB CRYPTO_SRCS "${CRYPTO_DIR}/*.cpp")

    set(MRNS_SRCS
        ${RNS_SRCS}
        ${CRYPTO_SRCS}
        "RouterUdpInterface.cpp"
        "EspNowInterface.cpp"
        "FatFsFileSystem.cpp"
        "MeshUplink.cpp"
        "microreticulum.cpp"
    )
    set(MRNS_PRIV_INCLUDE_DIRS
        "${RNS_DIR}/src"
        "${MICROSTORE_DIR}/include"
        "${CRYPTO_DIR}"
        "${MSGPACK_DIR}"
        "${ARXCONTAINER_DIR}"
        "${ARXTYPETRAITS_DIR}"
        "${DEBUGLOG_DIR}"
    )
    set(MRNS_REQUIRES nvs_flash freertos log esp_wifi esp_netif lwip)
    set(MRNS_PRIV_REQUIRES espressif__arduino-esp32 bblanchon__arduinojson)
else()
    set(MRNS_SRCS "")
    set(MRNS_PRIV_INCLUDE_DIRS "")
    set(MRNS_REQUIRES "")
    set(MRNS_PRIV_REQUIRES "")
endif()

idf_component_register(
    SRCS ${MRNS_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS ${MRNS_PRIV_INCLUDE_DIRS}
    REQUIRES ${MRNS_REQUIRES}
    PRIV_REQUIRES ${MRNS_PRIV_REQUIRES}
)

if(MRNS_SRCS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        RNS_USE_FS
        MSGPACK_USE_BOOST=OFF
    )
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Os
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
        -Wno-missing-field-initializers
        -Wno-format
        -Wno-unused-parameter
        # Below: this toolchain's -Werror is stricter than what
        # microReticulum/microStore were developed against upstream. All of
        # these are inside vendored third-party source (Link.cpp's switch
        # over RNS::Type::Packet::context_types and shift/mask expression,
        # FileStore.h's member-initializer-list order), not this component's
        # own bridge code -- relaxed here rather than patching the submodule.
        -Wno-error=switch
        -Wno-error=parentheses
        -Wno-error=reorder
    )
endif()
