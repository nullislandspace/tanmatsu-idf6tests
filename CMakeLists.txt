cmake_minimum_required(VERSION 3.10)
# The "ringfix" variant passes -DIDF6TEST_EXTRA_COMPONENTS=<dir> holding a
# patched copy of esp_driver_sdmmc, which then overrides the IDF's own.
if(DEFINED IDF6TEST_EXTRA_COMPONENTS)
    set(EXTRA_COMPONENT_DIRS ${IDF6TEST_EXTRA_COMPONENTS})
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(application)
