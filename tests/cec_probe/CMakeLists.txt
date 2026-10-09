# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

cmake_minimum_required(VERSION 3.16)
set(COMPONENTS main)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/cec"
                        "${CMAKE_CURRENT_LIST_DIR}/../../components/amity_hub")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(amity_cec_probe)
