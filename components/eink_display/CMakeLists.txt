# Xteink X4 e-ink status display. FreeInk SDK is vendored as a git submodule
# (third_party/freeink-sdk); only its X4 driver stack (FreeInkDisplay + EpdBus
# + Ssd1677Driver) is compiled in, on the ESP32-C3 target the X4 ships with.
if(IDF_TARGET STREQUAL "esp32c3")
    set(FREEINK_SDK_DIR      "${CMAKE_CURRENT_LIST_DIR}/../../third_party/freeink-sdk")
    set(FREEINK_DISPLAY_DIR  "${FREEINK_SDK_DIR}/libs/display/FreeInkDisplay")
    set(FREEINK_BOARDCFG_DIR "${FREEINK_SDK_DIR}/libs/hardware/BoardConfig")

    if(NOT EXISTS "${FREEINK_DISPLAY_DIR}/include/FreeInkDisplay.h")
        message(FATAL_ERROR
            "eink_display: third_party/freeink-sdk submodule is missing or not "
            "checked out. Run: git submodule update --init third_party/freeink-sdk")
    endif()

    set(EINK_SRCS
        "eink_display.cpp"
        "${FREEINK_DISPLAY_DIR}/src/FreeInkDisplay.cpp"
        "${FREEINK_DISPLAY_DIR}/src/bus/EpdBus.cpp"
        "${FREEINK_DISPLAY_DIR}/src/driver/Ssd1677Driver.cpp"
    )
    set(EINK_PRIV_INCLUDE_DIRS
        "${FREEINK_DISPLAY_DIR}/include"
        "${FREEINK_DISPLAY_DIR}/src"
        "${FREEINK_BOARDCFG_DIR}/include"
    )
    set(EINK_REQUIRES nvs_flash freertos log driver esp_wifi esp_netif lwip)
    set(EINK_PRIV_REQUIRES espressif__arduino-esp32 freeink_hw)
else()
    set(EINK_SRCS "")
    set(EINK_PRIV_INCLUDE_DIRS "")
    set(EINK_REQUIRES "")
    set(EINK_PRIV_REQUIRES "")
endif()

idf_component_register(
    SRCS ${EINK_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS ${EINK_PRIV_INCLUDE_DIRS}
    REQUIRES ${EINK_REQUIRES}
    PRIV_REQUIRES ${EINK_PRIV_REQUIRES}
)

if(EINK_SRCS)
    # FreeInk targets C++20. Single-framebuffer mode trades the SSD1677's
    # dual-RAM differential support for skipping a second ~48KB host-side
    # framebuffer copy -- the tradeoff FreeInk's own X4-only sample build
    # makes for the C3's tight RAM (see platformio.sample.ini upstream).
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        FREEINK_DEVICE_X4=1
        EINK_DISPLAY_SINGLE_BUFFER_MODE=1
    )
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Os
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++2a>
    )
endif()
