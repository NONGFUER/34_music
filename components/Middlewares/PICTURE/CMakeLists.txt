list(APPEND srcs    bmp.c
                    gif.c
                    jpeg.c
                    piclib.c
                    png.c
                    pngle.c
                    )

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "."
                    REQUIRES BSP
                             MYFATFS
                             esp_driver_jpeg
                             fatfs
                             )