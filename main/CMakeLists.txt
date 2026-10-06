idf_component_register(SRCS "main.c" "storage_io.c" "capture_data.c" "capture_viewer.c" "serial_log_data.c" "serial_log_viewer.c" "uart_tool.c" "spi_tool.c" "signal_tool.c" "network_tool.c" "http_tool.c" "mqtt_tool.c" "ble_tool.c" "ota_manifest.c" "ender3_tool.c" "electronics_tool.c" "electronics_math.c" "byte_tool.c" "byte_data.c" "modbus_tool.c" "modbus_data.c" "ntp_tool.c" "ntp_data.c" "wol_tool.c" "wol_data.c" "subnet_tool.c" "subnet_data.c"
                       INCLUDE_DIRS ".")
target_sources(${COMPONENT_LIB} PRIVATE "udp_tool.c" "resistor_tool.c" "resistor_data.c" "ipv4_data.c" "payload_clipboard.c" "uart_data.c")
target_sources(${COMPONENT_LIB} PRIVATE "modbus_rtu_tool.c")
target_sources(${COMPONENT_LIB} PRIVATE "i2c_result.c" "i2c_register.c")

target_sources(${COMPONENT_LIB} PRIVATE "http_transport.c")
target_sources(${COMPONENT_LIB} PRIVATE "network_ping.c" "network_resolver.c")

# LVGL's default static pool consumes 96 KiB of internal SRAM needed by SDIO.
# Use its supported pool allocator hook without modifying managed sources.
idf_component_get_property(tab5_lvgl_library lvgl__lvgl COMPONENT_LIB)
target_include_directories(${tab5_lvgl_library} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(${tab5_lvgl_library} PRIVATE
    LV_MEM_POOL_INCLUDE="lvgl_psram_pool.h"
    LV_MEM_POOL_ALLOC=tab5_lvgl_pool_alloc)

target_sources(${COMPONENT_LIB} PRIVATE "psram_exec_compat.c")
# Keep this compatibility fix scoped to the pinned SDK and affected P4 layout.
if(CONFIG_IDF_TARGET_ESP32P4 AND CONFIG_SPIRAM_XIP_FROM_PSRAM
   AND IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR EQUAL 4 AND IDF_VERSION_PATCH EQUAL 2)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TAB5_PSRAM_EXEC_COMPAT=1)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_ptr_executable")
endif()

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/chat_secrets.h")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CHAT_HAS_SECRETS=1)
    set_property(SOURCE "main.c" APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/chat_secrets.h")
endif()
