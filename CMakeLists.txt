# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

cmake_minimum_required (VERSION 3.14)

idf_component_register()

project(az_iot_middleware_freertos LANGUAGES C)

set(CMAKE_C_STANDARD 99)

include_directories(${IDF_PATH}/components/freertos/FreeRTOS-Kernel/include/freertos)

option(WARNINGS_AS_ERRORS FALSE)

# Add the middleware library
add_subdirectory(source/)
target_compile_options(az_iot_middleware_freertos PRIVATE
        -Wno-pedantic
        -Wno-sign-conversion
        )

target_link_libraries(${COMPONENT_LIB} INTERFACE az_iot_middleware_freertos)
