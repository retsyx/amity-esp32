cmake_minimum_required(VERSION 3.16)
set(COMPONENTS main)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/cec"
                        "${CMAKE_CURRENT_LIST_DIR}/../../components/amity_hub")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(amity_cec_probe)
