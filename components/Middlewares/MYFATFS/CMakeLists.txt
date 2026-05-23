list(APPEND srcs    exfuns.c)

idf_component_register(SRCS "exfuns.c" "${srcs}"
                    INCLUDE_DIRS "."
                    REQUIRES fatfs
                    )