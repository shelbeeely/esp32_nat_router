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
    set(SDFAT_DIR            "${TP_DIR}/SdFat")
    set(FREEINK_SDK_DIR      "${TP_DIR}/freeink-sdk")
    set(SDCARDMGR_DIR        "${FREEINK_SDK_DIR}/libs/hardware/SDCardManager")
    set(FREEINK_BOARDCFG_DIR "${FREEINK_SDK_DIR}/libs/hardware/BoardConfig")

    if(NOT EXISTS "${RNS_DIR}/src/microReticulum/Reticulum.h" OR NOT EXISTS "${SDFAT_DIR}/src/SdFat.h")
        message(FATAL_ERROR
            "microreticulum: one or more third_party submodules are missing. Run: "
            "git submodule update --init third_party/microReticulum third_party/microstore "
            "third_party/Crypto third_party/MsgPack third_party/ArxContainer "
            "third_party/ArxTypeTraits third_party/DebugLog third_party/SdFat")
    endif()

    # No CONFIGURE_DEPENDS: ESP-IDF evaluates component CMakeLists.txt files
    # once in a restricted "script mode" pass (just to gather REQUIRES) before
    # the real configure pass, and CONFIGURE_DEPENDS is invalid there. These
    # are pinned submodules, so the file list won't change without a
    # deliberate re-vendor anyway (which already means re-running cmake).
    file(GLOB_RECURSE RNS_SRCS "${RNS_DIR}/src/microReticulum/*.cpp")
    file(GLOB CRYPTO_SRCS "${CRYPTO_DIR}/*.cpp")
    # All of SdFat's own .cpp files, not just the ones our SDCardFileSystem
    # adapter calls directly -- FatVolume/ExFatVolume/FsVolume/FatFile/
    # ExFatFile/etc. implementations live in these and were previously only
    # pulled in via the include dir, leaving every SdFat symbol we actually
    # call an undefined reference at link time. The handful of platform-
    # specific SPI/SDIO driver files under SdCard/ (Due, STM32, Teensy,
    # RP2040) are each self-guarded by their own platform macro (e.g.
    # ARDUINO_SAM_DUE, STM32_CORE_VERSION, CORE_TEENSY) and compile to empty
    # translation units here, so including them all is safe.
    file(GLOB_RECURSE SDFAT_SRCS "${SDFAT_DIR}/src/*.cpp")

    set(MRNS_SRCS
        ${RNS_SRCS}
        ${CRYPTO_SRCS}
        ${SDFAT_SRCS}
        "RouterUdpInterface.cpp"
        "EspNowInterface.cpp"
        "FatFsFileSystem.cpp"
        "SdCardFileSystem.cpp"
        "${SDCARDMGR_DIR}/src/SDCardManager.cpp"
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
        "${SDFAT_DIR}/src"
        "${SDCARDMGR_DIR}/include"
        "${FREEINK_BOARDCFG_DIR}/include"
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
        FREEINK_DEVICE_X4=1
        # Without this, SdFat mangles any non-ASCII long filename into an
        # unopenable path -- see SDCardManager.h's own comment (its upstream
        # PlatformIO build hook sets this the same way).
        USE_UTF8_LONG_NAMES=1
    )
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Os
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
        -Wno-missing-field-initializers
        -Wno-format
        -Wno-unused-parameter
        # This component's sources are almost entirely vendored third-party
        # code (microReticulum, microStore, Crypto, MsgPack, ArxContainer,
        # ArxTypeTraits, DebugLog) developed against a less strict warning
        # set than this toolchain's -Werror=all default -- none of these are
        # in this component's own ~5 bridge files (RouterUdp/EspNow/
        # FatFsFileSystem/MeshUplink/microreticulum.cpp). A bare -Wno-error
        # does NOT retroactively un-promote warnings -Werror=all already
        # turned into errors (tested: it's a no-op against them), so each
        # category has to be named explicitly.
        -Wno-error=switch
        -Wno-error=parentheses
        -Wno-error=reorder
        -Wno-error=class-memaccess
        -Wno-error
    )
endif()
