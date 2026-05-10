/**
 ******************************************************************************
 * @file        main.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       音乐播放器实验
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ******************************************************************************
 * @attention
 * 
 * 实验平台:正点原子 ESP32-P4 开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 ******************************************************************************
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "driver/adc.h"
#include "esp_adc/adc_oneshot.h"
#include "key.h"
#include "myiic.h"
#include "xl9555.h"
#include "lcd.h"
#include "es8388.h"
#include "myi2s.h"
#include "sdmmc.h"
#include "text.h"
#include "fonts.h"
#include "exfuns.h"
#include "audioplay.h"
#include "rs485.h"
#include "piclib.h"
#include <stdio.h>

/* ADC句柄（用于电位器音量控制） */
adc_oneshot_unit_handle_t adc1_handle = NULL;

/* RS485接收任务配置 */
#define RS485_TASK_PRIO      3               /* 任务优先级(低于MUSIC的4-5) */
#define RS485_TASK_STK_SIZE  2*1024          /* 任务堆栈 */

/**
 * @brief   RS485接收任务: 接收PC发来的切歌指令并设置全局控制变量
 * @param   arg: 未使用
 */
static void rs485_task(void *arg)
{
    uint8_t rx_buf[16];
    uint8_t rx_len = 0;

    while (1)
    {
        rs485_receive_data(rx_buf, &rx_len);

        if (rx_len > 0)
        {
            /* 简单协议: 收到的第一个字节就是曲目编号(十六进制)
             * 例: PC发送 0x01 -> 播放第1首, 0x0A -> 播放第10首 */
            uint8_t cmd = rx_buf[0];

            if (cmd >= 1 && cmd <= 255)                              /* 有效范围 */
            {
                rs485_target_index = cmd;                            /* 设置目标曲目(1-based) */
                rs485_cmd_flag = 1;                                  /* 置位命令标志 */
                i2s_play_next_prev = ESP_OK;                         /* 触发当前歌曲停止 */

                printf("[RS485] RX: 0x%02X -> song #%d\r\n", cmd, cmd);
            }

            rx_len = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(20));                               /* 20ms轮询间隔 */
    }
}


/**
 * @brief       程序入口
 * @param       无
 * @retval      无
 */
void app_main(void)
{
    esp_err_t ret;
    uint8_t key = 0;

    ret = nvs_flash_init();     /* 初始化NVS */

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    key_init();                 /* 初始化KEY */
    myiic_init();               /* IIC0初始化 */
    xl9555_init();              /* XL9555初始化 */
    lcd_init();                 /* LCD屏初始化 */

    while (es8388_init())       /* ES8388初始化 */
    {
        lcd_show_string(30, 110, 200, 16, 16, "ES8388 Error", RED);
        vTaskDelay(pdMS_TO_TICKS(200));
        lcd_fill(30, 110, 239, 126, WHITE);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    xl9555_pin_write(SPK_EN_IO, 1);     /* 关闭喇叭功放(使用耳机模式) */

    /* ADC初始化，用于电位器音量控制 */
    adc_oneshot_unit_init_cfg_t adc_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_new_unit(&adc_config, &adc1_handle);
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &chan_cfg);

    while (sdmmc_init())        /* 检测不到SD卡 */
    {
        lcd_show_string(30, 110, 200, 16, 16, "SD Card Error!", RED);
        vTaskDelay(pdMS_TO_TICKS(200));
        lcd_fill(30, 110, 239, 126, WHITE);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ret = exfuns_init();    /* 为fatfs相关变量申请内存 */

    while (fonts_init())    /* 检查字库 */
    {
        lcd_clear(WHITE);
        lcd_show_string(30, 30, 200, 16, 16, "ESP32-P4", RED);
        
        key = fonts_update_font(30, 50, 16, (uint8_t *)"0:", RED);  /* 更新字库 */
        
        while (key)         /* 更新失败 */
        {
            lcd_show_string(30, 50, 200, 16, 16, "Font Update Failed!", RED);
            vTaskDelay(pdMS_TO_TICKS(200));
            lcd_fill(20, 50, 200 + 20, 90 + 16, WHITE);
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        lcd_show_string(30, 50, 200, 16, 16, "Font Update Success!   ", RED);
        vTaskDelay(pdMS_TO_TICKS(1000));
        lcd_clear(WHITE);   
    }

    /* 开机显示待机画面 */
    piclib_ai_load_picfile("0:/PICTURE/standby.png", 0, 0, lcddev.width, lcddev.height);
    printf("[PIC] Standby image loaded\r\n");

    /* ★ 初始化图片解码库 */
    piclib_init();
    printf("[PIC] piclib init done\r\n");

    /* ★ 初始化RS485 (UART1, 波特率9600) */
    rs485_init(9600);
    printf("[RS485] init done @9600bps\r\n");

    /* ★ 创建RS485接收任务 */
    xTaskCreate(rs485_task, "rs485", RS485_TASK_STK_SIZE, NULL, RS485_TASK_PRIO, NULL);
    printf("[RS485] task started\r\n");

    /* ★ 主循环: 等待RS485指令才播放，不自动播放 */
    while (1)
    {
        if (rs485_cmd_flag && rs485_target_index > 0)
        {
            printf("[MAIN] Playing song #%d via RS485\r\n", rs485_target_index);
            audio_play();       /* 收到RS485指令后播放音乐 */
            rs485_cmd_flag = 0; /* 播放结束,清除标志 */

            /* 播放完回到待机画面 */
            piclib_ai_load_picfile("0:/PICTURE/standby.png", 0, 0, lcddev.width, lcddev.height);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
