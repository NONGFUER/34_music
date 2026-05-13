/**
 ******************************************************************************
 * @file        main.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V2.0
 * @date        2025-01-01
 * @brief       ESP32-P4 音乐播放器 - 主程序入口与系统初始化
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 *
 * @note        V2.0优化内容:
 *              1. 精简主循环逻辑, 移除冗余代码
 *              2. 完善初始化错误处理流程
 *              3. 统一注释风格, 增强可读性
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

/* ================================================================== */
/*                         全局变量                                   */
/* ================================================================== */

/** ADC1句柄(用于电位器模拟音量控制, 在app_main中初始化) */
adc_oneshot_unit_handle_t adc1_handle = NULL;

/* ================================================================== */
/*                      RS485接收任务配置                              */
/* ================================================================== */

#define RS485_TASK_PRIO      3               /* RS485任务优先级(低于MUSIC任务的4) */
#define RS485_TASK_STK_SIZE  (2 * 1024)      /* RS485任务堆栈大小 */

/**
 * @brief RS485接收任务 - 解析MODBUS RTU请求帧, 发送响应, 设置全局命令变量
 *
 * ★ MODBUS RTU 协议 (从机地址 0x11, 功能码 0x06) ★
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │ 帧格式: [0x11][0x06][REG_HI][REG_LO][DATA_HI][DATA_LO][CRC_LO][CRC_HI] │
 * │                                                                     │
 * │ 寄存器 0x0001~0x00FF → 一首歌一个地址:                             │
 * │   写入 0x0001 = 播放 / 0x0000 = 停止                                │
 * │                                                                     │
 * │ 寄存器 0x0100 → 音量:                                               │
 * │   写入 0x0000~0x0021 = 音量0~33                                     │
 * └──────────────────────────────────────────────────────────────────────┘
 *
 * @param arg  未使用(FreeRTOS任务函数签名要求)
 */
static void rs485_task(void *arg)
{
    uint8_t rx_buf[16];
    uint8_t rx_len = 0;
    (void)arg;

    while (1) {
        rs485_receive_data(rx_buf, &rx_len);

        if (rx_len > 0) {
            uint8_t  tx_buf[8];
            uint8_t  tx_len = 0;
            uint16_t reg_addr, reg_data;

            int ret = modbus_parse_frame(rx_buf, rx_len,
                                         tx_buf, &tx_len,
                                         &reg_addr, &reg_data);

            /* 发送MODBUS响应帧 (正常回显或异常响应) */
            if (tx_len > 0) {
                rs485_send_data(tx_buf, tx_len);
            }

            /* 命令分派: 有效命令由主任务在主循环中执行 */
            if (ret == 0) {
                if (reg_addr == MODBUS_REG_VOLUME) {
                    /* 音量控制 */
                    rs485_volume_val  = (uint8_t)reg_data;
                    rs485_volume_flag = 1;
                    printf("[RS485] VOL: %d/33\r\n", (uint8_t)reg_data);
                }
                else if (reg_addr >= MODBUS_REG_SONG_FIRST && reg_addr <= MODBUS_REG_SONG_LAST) {
                    if (reg_data == MODBUS_VAL_PLAY) {
                        /* 播放某首歌: 中断当前播放 + 标记新命令 */
                        rs485_target_index = (uint8_t)reg_addr;
                        rs485_cmd_flag = 1;
                        i2s_play_next_prev = ESP_OK;
                        printf("[RS485] PLAY: song #%d\r\n", (uint8_t)reg_addr);
                    }
                    else /* MODBUS_VAL_STOP */ {
                        /* 停止: 仅中断当前播放, 不标记新命令(避免触发新一轮播放) */
                        i2s_play_next_prev = ESP_OK;
                        printf("[RS485] STOP\r\n");
                    }
                }
            }

            rx_len = 0;   /* 重置接收长度 */
        }

        vTaskDelay(pdMS_TO_TICKS(10));   /* 10ms轮询间隔 */
    }
}

/* ================================================================== */
/*                     系统初始化辅助函数                              */
/* ================================================================== */

/**
 * @brief 初始化NVS (Non-Volatile Storage) 分区
 * @note  NVS是ESP-IDF的键值存储系统, 被WiFi/BLE等组件依赖,
 *        即使本应用不直接使用也必须初始化
 */
static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS分区损坏或格式不匹配 → 擦除后重新初始化 */
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

/**
 * @brief 初始化ES8388音频编解码器 (带重试和LCD状态提示)
 */
static void init_es8388(void)
{
    while (es8388_init()) {
        lcd_show_string(30, 110, 200, 16, 16, "ES8388 Error!", RED);
        vTaskDelay(pdMS_TO_TICKS(200));
        lcd_fill(30, 110, 239, 126, WHITE);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/**
 * @brief 初始化SD卡 (带重试和LCD状态提示)
 */
static void init_sdcard(void)
{
    while (sdmmc_init()) {
        lcd_show_string(30, 110, 200, 16, 16, "SD Card Error!", RED);
        vTaskDelay(pdMS_TO_TICKS(200));
        lcd_fill(30, 110, 239, 126, WHITE);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    printf("[SD] Card initialized OK\r\n");
}

/**
 * @brief 初始化字库 (首次运行时从SD卡更新到SPI Flash)
 */
static void init_fonts(void)
{
    uint8_t key;

    while (fonts_init()) {
        lcd_clear(WHITE);
        lcd_show_string(30, 30, 200, 16, 16, "ESP32-P4 MusicPlayer", RED);

        key = fonts_update_font(30, 50, 16, (uint8_t *)"0:", RED);

        while (key) {
            lcd_show_string(30, 50, 200, 16, 16, "Font Update Failed!", RED);
            vTaskDelay(pdMS_TO_TICKS(200));
            lcd_fill(20, 50, 220, 90 + 16, WHITE);
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        lcd_show_string(30, 50, 200, 16, 16, "Font Update Success!   ", RED);
        vTaskDelay(pdMS_TO_TICKS(1000));
        lcd_clear(WHITE);
    }
    printf("[FONTS] Font library ready\r\n");
}

/**
 * @brief 初始化ADC1用于电位器音量控制 (通道0, 12bit精度, 12dB衰减)
 */
static void init_adc_volume(void)
{
    adc_oneshot_unit_init_cfg_t adc_config = {
        .unit_id  = ADC_UNIT_1,
        .clk_src  = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_config, &adc1_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,       /* 12dB衰减(测量范围~0~3.3V) */
        .bitwidth = ADC_BITWIDTH_12,       /* 12位精度(0~4095) */
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &chan_cfg));
}

/* ================================================================== */
/*                        主程序入口                                  */
/**
 * @note  启动顺序:
 *        1. 基础系统(NVS)
 *        2. 外设硬件(KEY/IIC/XL9555/LCD/ES8388/SD卡/ADC)
 *        3. 文件系统与字库(FatFS/字体)
 *        4. 多媒体组件(图片库/待机画面)
 *        5. 通信(RS485初始化+启动接收任务)
 *        6. 进入主循环(按键扫描+RS485命令分发+播放调度)
 *
 * @note  V2.0后audio_play()内部已实现索引缓存,
 *        RS485切歌不再触发目录全扫描, 延迟从~5s降至<100ms
 * ================================================================== */
void app_main(void)
{
    static uint8_t local_song_index = 1;   /* 本地曲目索引(1-based), 按键控制用 */
    uint8_t key;

    /* ===== 阶段1: 基础系统初始化 ===== */
    init_nvs();

    /* ===== 阶段2: 外设硬件初始化 ===== */
    key_init();                 /* GPIO按键 */
    myiic_init();               /* I2C总线 (用于ES8388通信) */
    xl9555_init();              /* IO扩展器 (按键/功放使能) */
    lcd_init();                 /* LCD显示屏 */
    init_es8388();              /* ES8388音频编解码器 (带重试) */

    /* 默认关闭喇叭功放(使用耳机模式, 避免上电POP声) */
    xl9555_pin_write(SPK_EN_IO, 1);         /* 高电平=关闭功放 */

    init_adc_volume();          /* ADC电位器音量输入 */
    init_sdcard();              /* SD卡文件系统 */

    /* ===== 阶段3: 文件系统与字库 ===== */
    exfuns_init();              /* FatFS工作区分配 */
    init_fonts();               /* 字库加载/更新 */

    /* ===== 阶段4: 多媒体组件 ===== */
    piclib_init();              /* 图片解码库初始化(JPG/PNG/BMP解码引擎) */
    printf("[SYS] Picture decoder initialized\r\n");

    audio_init_cover_task();    /* 封面异步加载任务(prio=2, 不抢音频DMA带宽) */
    printf("[SYS] Cover loader task initialized\r\n");

    /* 显示开机待机画面 */
    piclib_ai_load_picfile("0:/PICTURE/standby.png", 0, 0, lcddev.width, lcddev.height);
    printf("[SYS] Standby image displayed\r\n");

    /* ===== 阶段5: 通信子系统 ===== */
    rs485_init(9600);           /* UART1 @9600bps (RS485总线) */
    printf("[RS485] Initialized @9600bps\r\n");

    xTaskCreate(rs485_task, "rs485", RS485_TASK_STK_SIZE, NULL, RS485_TASK_PRIO, NULL);
    printf("[RS485] Receive task started\r\n");

    /* ===== 阶段6: 主事件循环 ===== */
    /**
     * @note  双模式操作:
     *        - RS485远程模式: PC发送指令 → 设置rs485_cmd_flag → 触发audio_play()
     *        - 本地按键模式: KEY0/KEY1/KEY2 → 同样设置rs485_cmd_flag → 触发audio_play()
     *        两种模式最终汇聚到同一播放路径, 保证行为一致
     *
     * @note  循环周期10ms:
     *        - RS485命令响应延迟 ≈ 10ms(轮询间隔) + audio_play内部耗时
     *        - V2.0索引缓存后, audio_play在非首次调用时 <100ms 即可进入播放
     */
    while (1) {
        /* ---- A. 音量控制 (最高优先级, 无需等待播放状态) ---- */
        if (rs485_volume_flag && rs485_volume_val <= 33) {
            es8388_hpvol_set(rs485_volume_val);
            es8388_spkvol_set(rs485_volume_val);
            printf("[MAIN] Volume -> %d/33\r\n", rs485_volume_val);
            rs485_volume_flag = 0;
            rs485_volume_val  = 0xFF;       /* 标记无效, 防止重复处理 */
        }

        /* ---- B. RS485远程切歌 / 挮键切歌 统一处理 ---- */
        if (rs485_cmd_flag && rs485_target_index > 0) {
            printf("[MAIN] Playing song #%d\r\n", rs485_target_index);

            /* ★ 调用播放入口(V2.0: 内部有索引缓存快速路径) */
            audio_play();

            /* 播放结束清理 */
            rs485_cmd_flag = 0;
            local_song_index = rs485_target_index;

            /* 回到待机画面 */
            piclib_ai_load_picfile("0:/PICTURE/standby.png", 0, 0, lcddev.width, lcddev.height);
        }

        /* ---- C. 本地按键扫描 ---- */
        key = xl9555_key_scan(0);

        switch (key) {
            case KEY0_PRES:           /* KEY0: 下一首 */
                local_song_index++;
                rs485_target_index = local_song_index;
                rs485_cmd_flag = 1;   /* 复用RS485标志统一处理 */
                printf("[KEY] Next -> #%d\r\n", local_song_index);
                break;

            case KEY1_PRES:           /* KEY1: 上一首 */
                if (local_song_index > 1) {
                    local_song_index--;
                }
                rs485_target_index = local_song_index;
                rs485_cmd_flag = 1;
                printf("[KEY] Prev -> #%d\r\n", local_song_index);
                break;

            case KEY2_PRES:           /* KEY2: 播放/暂停 当前曲目 */
                rs485_target_index = local_song_index;
                rs485_cmd_flag = 1;
                printf("[KEY] Play -> #%d\r\n", local_song_index);
                break;

            default:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));   /* 10ms主循环周期 */
    }
}
