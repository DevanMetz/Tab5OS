idf_component_register(SRCS "main.c" "storage_io.c" "capture_data.c" "capture_viewer.c" "serial_log_data.c" "serial_log_viewer.c" "uart_tool.c" "spi_tool.c" "signal_tool.c" "network_tool.c" "http_tool.c" "mqtt_tool.c" "ble_tool.c" "ota_manifest.c" "ender3_tool.c"
                       INCLUDE_DIRS ".")

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/chat_secrets.h")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CHAT_HAS_SECRETS=1)
    set_property(SOURCE "main.c" APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/chat_secrets.h")
endif()
