cmake_minimum_required(VERSION 3.10)
# extras/ holds components copied from other projects (gl_input from
# tanmatsu-graceloader). Variants with patched IDF components pass
# -DIDF6TEST_EXTRA_COMPONENTS=<dir>, whose copies override the IDF's own.
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/extras)
if(DEFINED IDF6TEST_EXTRA_COMPONENTS)
    list(APPEND EXTRA_COMPONENT_DIRS ${IDF6TEST_EXTRA_COMPONENTS})
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(application)
