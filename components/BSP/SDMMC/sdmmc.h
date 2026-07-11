#ifndef __SDMMC_H
#define __SDMMC_H

#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_err.h"


/* SDMMC外设相关硬件引脚 */
#define SDMMC_PIN_CMD       GPIO_NUM_44
#define SDMMC_PIN_CLK       GPIO_NUM_43
#define SDMMC_PIN_D0        GPIO_NUM_39
#define SDMMC_PIN_D1        GPIO_NUM_40
#define SDMMC_PIN_D2        GPIO_NUM_41
#define SDMMC_PIN_D3        GPIO_NUM_42

/* 挂载名称 */
#define MOUNT_POINT         "/0:"

extern sdmmc_card_t         *card;

/* 函数声明 */
esp_err_t sdmmc_init(void);     /* SD卡初始化并挂载SD卡 */
esp_err_t sdmmc_unmount(void);  /* 取消挂载SD卡 */

#endif
