cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(sdc41 C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(sdc41
    src/main.c
    src/sdc41.c
    src/sdc41_crc.c
)

target_include_directories(sdc41 PRIVATE src)
target_compile_options(sdc41 PRIVATE -Wall -Wextra)
target_link_libraries(sdc41 PRIVATE pico_stdlib hardware_i2c hardware_uart)

pico_enable_stdio_uart(sdc41 0)
pico_enable_stdio_usb(sdc41 0)
pico_set_binary_type(sdc41 copy_to_ram)
pico_add_extra_outputs(sdc41)

add_executable(sdc41_ram
    src/main.c
    src/sdc41.c
    src/sdc41_crc.c
)

target_include_directories(sdc41_ram PRIVATE src)
target_compile_options(sdc41_ram PRIVATE -Wall -Wextra)
target_link_libraries(sdc41_ram PRIVATE pico_stdlib hardware_i2c hardware_uart)

pico_enable_stdio_uart(sdc41_ram 0)
pico_enable_stdio_usb(sdc41_ram 0)
pico_set_binary_type(sdc41_ram no_flash)
pico_add_extra_outputs(sdc41_ram)
