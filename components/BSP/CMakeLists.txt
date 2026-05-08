set(src_dirs
            LED
            KEY
            MYIIC
            XL9555
            LCD
            ES8388
            MYI2S
            SDMMC)

set(include_dirs
            LED
            KEY
            MYIIC
            XL9555
            LCD
            ES8388
            MYI2S
            SDMMC)

set(requires
            driver
            esp_lcd
            esp_common
            fatfs)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)