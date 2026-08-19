# Standard Pico SDK import helper. PICO_SDK_PATH may override the local default.
if (DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()

if (NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "/home/henrik/sdk/pico-sdk")
endif()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK")
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
