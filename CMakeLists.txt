# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

cmake_minimum_required(VERSION 3.14)

idf_component_register(REQUIRES mbedtls)

project(az_iot_middleware_freertos LANGUAGES C)

set(CMAKE_C_STANDARD 99)

include_directories(${IDF_PATH}/components/freertos/FreeRTOS-Kernel/include/freertos)

option(WARNINGS_AS_ERRORS FALSE)

# Add the middleware library
set(USE_COREHTTP TRUE)
add_subdirectory(source/)
target_compile_options(az_iot_middleware_freertos PRIVATE
        -Wno-pedantic
        -Wno-sign-conversion
        )

# suppress library header compiler warnings
get_target_property(az_iot_middleware_freertos_INCLUDE_DIRECTORIES az_iot_middleware_freertos INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(az_iot_middleware_freertos SYSTEM PUBLIC ${az_iot_middleware_freertos_INCLUDE_DIRECTORIES})

get_target_property(mbedtls_INCLUDE_DIRECTORIES __idf_mbedtls INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(az_iot_middleware_freertos PRIVATE ${mbedtls_INCLUDE_DIRECTORIES})

target_link_libraries(
        ${COMPONENT_LIB}
        INTERFACE
        az_iot_middleware_freertos
        azure_iot_core_http
)
