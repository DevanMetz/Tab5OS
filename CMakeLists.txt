cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/version.cmake)
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.idf6")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_os)
