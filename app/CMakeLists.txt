cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(edison)

target_sources(app PRIVATE
    src/main.c
    src/ethernet.c
    src/web_server.c
)

zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(NAME http_resource_desc_edison_http_service
    KVMA RAM_REGION
    GROUP RODATA_REGION
)
