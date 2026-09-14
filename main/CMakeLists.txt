idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4" AND CONFIG_IDF6TEST_WITH_HOSTED)
    list(APPEND extra_requires esp-hosted-tanmatsu tanmatsu-wifi bt)
endif()

idf_component_register(
    SRCS
        "main.c"
        "sdcard.c"
        "sdtest.c"
    PRIV_REQUIRES
        esp_lcd
        fatfs
        nvs_flash
        badge-bsp
        esp_driver_sdmmc
        sdmmc
        vfs
        esp_mm
        heap
        ${extra_requires}
    INCLUDE_DIRS
        "."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
