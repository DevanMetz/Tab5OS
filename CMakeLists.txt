cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/version.cmake)
# Wi-Fi Remote selects its Kconfig sources using the SDK export's major.minor.
# Check this before project() can regenerate sdkconfig with those sources omitted.
set(TAB5_IDF_ENV_VERSION "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
if(NOT "$ENV{ESP_IDF_VERSION}" STREQUAL "${TAB5_IDF_ENV_VERSION}")
    message(FATAL_ERROR
        "Tab5 SDK environment mismatch: ESP_IDF_VERSION must be '${TAB5_IDF_ENV_VERSION}' for this IDF_PATH. "
        "Activate this SDK's export.ps1 or export.sh before configuring; the full patch version is not this variable's format.")
endif()
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.idf6")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_os)

if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "Tab5 requires IDF_TARGET=esp32p4. Select esp32p4 in the activated SDK environment.")
endif()
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(TAB5_C6_SELECTED "${CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6}")
else()
    set(TAB5_C6_SELECTED "${CONFIG_SLAVE_IDF_TARGET_ESP32C6}")
endif()
if(NOT TAB5_C6_SELECTED)
    message(FATAL_ERROR "Tab5 requires the ESP32-C6 hosted Wi-Fi target. Review sdkconfig against the checked-in defaults.")
endif()
if(NOT CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE)
    message(FATAL_ERROR "Tab5 requires the ESP-Hosted SDIO transport. Review sdkconfig against the checked-in defaults.")
endif()
