cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(edison)

target_sources(app PRIVATE
    src/device_control.c
    src/eeprom.c
    src/main.c
    src/isotp_service.c
    src/ethernet.c
    src/pmi_update.c
    src/psram.c
    src/qspi_nor.c
    src/web_server.c
)

# The MCUX FlexSPI controller API is currently internal to Zephyr's memc
# driver. Keep this dependency local to the PSRAM module.
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/drivers/memc)

zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(NAME http_resource_desc_edison_http_service
    KVMA RAM_REGION
    GROUP RODATA_REGION
)
