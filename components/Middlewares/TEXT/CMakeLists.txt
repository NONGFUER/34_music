list(APPEND srcs    fonts.c
                    text.c)

idf_component_register(SRCS "text.c" "${srcs}"
                    INCLUDE_DIRS "."
                    REQUIRES esp_partition
                             spi_flash
                             fatfs
                             BSP)