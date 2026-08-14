# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.4.14")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_nat_router)

# espressif/esp-modbus is an unconditional transitive dependency of
# espressif/arduino-esp32 (eink_display/freeink_hw/microreticulum's Arduino
# dependency, esp32c3 only) -- pulled in even though this firmware never
# touches Modbus. Its port_tcp_utils.c fails this toolchain's default
# -Werror=stringop-truncation (a strncpy(dst, src, strlen(src)) pattern GCC
# flags as suspicious). Relax that one warning for that one third-party
# component only, rather than weakening -Werror project-wide.
if(TARGET __idf_espressif__esp-modbus)
    target_compile_options(__idf_espressif__esp-modbus PRIVATE -Wno-error=stringop-truncation)
endif()

# Force esp_app_desc.c to recompile on every build so that
# __DATE__/__TIME__ in the app descriptor stay current.
set(APP_DESC_OBJ "${CMAKE_BINARY_DIR}/esp-idf/esp_app_format/CMakeFiles/__idf_esp_app_format.dir/esp_app_desc.c.obj")
add_custom_target(force_app_desc_rebuild ALL
    COMMAND ${CMAKE_COMMAND} -E remove -f "${APP_DESC_OBJ}"
    COMMENT "Ensuring app descriptor timestamp is current"
)
add_dependencies(${PROJECT_NAME}.elf force_app_desc_rebuild)
