# Xteink X4 hardware beyond the display: buttons, battery, power-button deep
# sleep, from the FreeInk SDK (third_party/freeink-sdk submodule, same one
# eink_display uses). ESP32-C3 only, reuses eink_display's arduino-esp32
# dependency -- these libraries are Arduino-based like the display driver.
if(IDF_TARGET STREQUAL "esp32c3")
    set(FREEINK_SDK_DIR      "${CMAKE_CURRENT_LIST_DIR}/../../third_party/freeink-sdk")
    set(INPUT_DIR            "${FREEINK_SDK_DIR}/libs/hardware/InputManager")
    set(BATTERY_DIR          "${FREEINK_SDK_DIR}/libs/hardware/BatteryMonitor")
    set(POWER_DIR            "${FREEINK_SDK_DIR}/libs/hardware/PowerManager")
    set(FREEINK_BOARDCFG_DIR "${FREEINK_SDK_DIR}/libs/hardware/BoardConfig")

    if(NOT EXISTS "${INPUT_DIR}/include/InputManager.h")
        message(FATAL_ERROR
            "freeink_hw: third_party/freeink-sdk submodule is missing or not "
            "checked out. Run: git submodule update --init third_party/freeink-sdk")
    endif()

    set(FKHW_SRCS
        "freeink_hw.cpp"
        "${INPUT_DIR}/src/InputManager.cpp"
        "${BATTERY_DIR}/src/BatteryMonitor.cpp"
        "${POWER_DIR}/src/PowerManager.cpp"
    )
    set(FKHW_PRIV_INCLUDE_DIRS
        "${INPUT_DIR}/include"
        "${BATTERY_DIR}/include"
        "${POWER_DIR}/include"
        "${FREEINK_BOARDCFG_DIR}/include"
    )
    set(FKHW_REQUIRES nvs_flash freertos log driver esp_adc)
    set(FKHW_PRIV_REQUIRES espressif__arduino-esp32)
else()
    set(FKHW_SRCS "")
    set(FKHW_PRIV_INCLUDE_DIRS "")
    set(FKHW_REQUIRES "")
    set(FKHW_PRIV_REQUIRES "")
endif()

idf_component_register(
    SRCS ${FKHW_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS ${FKHW_PRIV_INCLUDE_DIRS}
    REQUIRES ${FKHW_REQUIRES}
    PRIV_REQUIRES ${FKHW_PRIV_REQUIRES}
)

if(FKHW_SRCS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        FREEINK_DEVICE_X4=1
    )
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Os
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    )
endif()
