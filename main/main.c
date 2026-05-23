/**
 ******************************************************************************
 * @file        main.c
 * @author      sjwu
 * @version     V2.0
 * @date        2025-01-01
 * @brief       ESP32-P4 音乐播放器 - 主程序入口与系统初始化
 *  
 *
 * @note        V2.0优化内容:
 *              1. 精简主循环逻辑, 移除冗余代码
 *              2. 完善初始化错误处理流程
 *              3. 统一注释风格, 增强可读性
 ******************************************************************************
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
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
#include "cook_ui.h"
#include <stdio.h>
#include "videoplay.h"
#include "mytimer.h"

/* ================================================================== */
/*                         全局变量                                   */
/* ================================================================== */

/** ADC1句柄(用于电位器模拟音量控制, 在app_main中初始化) */
adc_oneshot_unit_handle_t adc1_handle = NULL;

/** 语音播报跳过标志 (1=跳过本次语音, 0=正常播报) */
uint8_t g_skip_voice = 0;

/** 全局静音标志 (1=静音中, 0=正常) - B0设置, B1~B5清除, 主循环/music任务均需检查 */
uint8_t g_mute_flag = 0;

/* ================================================================== */
/*                      RS485接收任务配置                              */
/* ================================================================== */

#define RS485_TASK_PRIO      3               /* RS485任务优先级(低于MUSIC任务的4) */
#define RS485_TASK_STK_SIZE  (2 * 1024)      /* RS485任务堆栈大小 */

/* 音量5档 (0~33范围, 压缩低端无效区间) */
#define VOL_1      18      /* B1  ~48% 对标原50% */
#define VOL_2      22      /* B2  ~67% */
#define VOL_3      27      /* B3  ~82% */
#define VOL_4      30      /* B4  ~91% */
#define VOL_MAX    33      /* B5  100% 最大 */

/**
 * @brief RS485接收任务 - 解析HEX单字节指令, 发送应答, 分发命令
 *
 * ★ HEX 单字节指令协议 V2 ★
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │ A组(触发): A1=开机 A3~A5=倒菜 A6=开始炒菜 A7=炒菜完成               │
 * │           A8=温度异常 A9=火警                                       │
 * │ C组:      C1=显示归位界面                                            │
 * │ 应答格式: 0x51 (单字节)                                              │
 * └──────────────────────────────────────────────────────────────────────┘
 *
 * @param arg  未使用(FreeRTOS任务函数签名要求)
 */
static void rs485_task(void *arg)
{
    uint8_t rx_byte;
    uint8_t rx_len = 0;
    (void)arg;

    while (1) {
        rs485_receive_data(&rx_byte, &rx_len);

        if (rx_len > 0) {
            /* DEBUG: hex dump */
            printf("[RS485] RX: %02X\r\n", rx_byte);

            int cmd_id = cmd_parse(&rx_byte, 1);
            printf("[RS485] parse -> %d\r\n", cmd_id);

            if (cmd_id > 0) {
                rs485_send_data((uint8_t *)"\x51", 1);

                switch (cmd_id) {
                    case 1:  cook_cmd_boot();                  break;  /* 0xA1 开机 */
                    case 2:  cook_cmd_pour_box(1);             break;  /* 0xA3 倒一号菜 */
                    case 3:  cook_cmd_pour_box(2);             break;  /* 0xA4 倒二号菜 */
                    case 4:  cook_cmd_pour_box(3);             break;  /* 0xA5 倒三号菜 */
                    case 5:  cook_cmd_start();                 break;  /* 0xA6 开始炒菜 */
                    case 6:  cook_cmd_finish();                break;  /* 0xA7 炒菜完成 */
                    case 7:  cook_cmd_alarm_temp();            break;  /* 0xA8 温度异常 */
                    case 8:  cook_cmd_alarm_fire();            break;  /* 0xA9 火警 */
                    case 10: g_mute_flag = 1; es8388_soft_mute(1); rs485_volume_flag = 0; break;   /* B0: 静音 */
                    case 11: g_mute_flag = 0; es8388_soft_mute(0);rs485_volume_val=VOL_1;   rs485_volume_flag=1;break;
                    case 12: g_mute_flag = 0; es8388_soft_mute(0);rs485_volume_val=VOL_2;   rs485_volume_flag=1;break;
                    case 13: g_mute_flag = 0; es8388_soft_mute(0);rs485_volume_val=VOL_3;   rs485_volume_flag=1;break;
                    case 14: g_mute_flag = 0; es8388_soft_mute(0);rs485_volume_val=VOL_4;   rs485_volume_flag=1;break;
                    case 15: g_mute_flag = 0; es8388_soft_mute(0);rs485_volume_val=VOL_MAX; rs485_volume_flag=1;break;
                    case 16: cook_cmd_c1();                     break;  /* 0xC1 显示归位界面 */
                    default: break;
                }
            }

            rx_len = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
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
        /* NVS分区损坏或格式不匹配 -> 擦除后重新初始化 */
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

    es8388_output_cfg(1, 0);            /* 开启耳机输出 */
    es8388_adda_cfg(1, 0);             
    es8388_hpvol_set(0);               
    es8388_soft_mute(1); 
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

    cook_ui_init();              /* 炒菜机UI系统初始化(默认待机状态) */
    printf("[SYS] Cook UI initialized\r\n");

    audio_init_cover_task();    /* 封面异步加载任务(通知cook_ui刷新) */
    printf("[SYS] Cover loader task initialized\r\n");

    /* 开机画面由 cook_ui_init -> cook_render 首次渲染自动处理
       (bg_scene=1 对应 BG_FILE_STANDBY) */
    printf("[SYS] Standby image will be rendered by cook_ui\r\n");

    /* ===== 阶段5: 通信子系统 ===== */
    rs485_init(9600);           /* UART1 @9600bps (RS485总线) */
    printf("[RS485] Initialized @9600bps\r\n");

    xTaskCreate(rs485_task, "rs485", RS485_TASK_STK_SIZE, NULL, RS485_TASK_PRIO, NULL);
    printf("[RS485] Receive task started\r\n");

    /* ===== 阶段5.5: 开机自播(待机界面, 跳过语音避免I2S冲突) ===== */
    g_skip_voice = 1;                    /* 标记: 跳过001.wav, 视频会占用I2S */
    cook_cmd_boot();
    g_skip_voice = 0;                    /* 恢复: 后续命令正常播报 */

    /* ===== 阶段5.6: 开机播放指定AVI视频 ===== */
    printf("[SYS] Starting boot video...\r\n");
    video_play_single("0:/VIDEO/boot.avi");   /* 指定开机视频文件名 */
    printf("[SYS] Boot video done, entering main loop\r\n");

    /* ===== 阶段6: 主事件循环 ===== */
    /**
     * @note  双模式操作:
     *        - RS485远程模式: PC发送指令 -> 设置rs485_cmd_flag -> 触发audio_play()
     *        - 本地按键模式: KEY0/KEY1/KEY2 -> 同样设置rs485_cmd_flag -> 触发audio_play()
     *        两种模式最终汇聚到同一播放路径, 保证行为一致
     *
     * @note  循环周期10ms:
     *        - RS485命令响应延迟 ~= 10ms(轮询间隔) + audio_play内部耗时
     *        - V2.0索引缓存后, audio_play在非首次调用时 <100ms 即可进入播放
     */
    while (1) {
        /* ---- A. RS485远程切歌 / 按键切歌 (最高优先级: 音频先启动) ---- */
        if (rs485_cmd_flag && rs485_target_index > 0) {
            int64_t t_main = esp_timer_get_time();
            printf("[MAIN] CMD detected @%lld us\r\n", t_main);

            /* 调用播放入口(V3.0: 非阻塞, <100ms返回) */
            audio_play();

            rs485_cmd_flag = 0;
            local_song_index = rs485_target_index;
        }

        /* ---- B. 音量控制 (静音时跳过) ---- */
        if (!g_mute_flag && rs485_volume_flag && rs485_volume_val <= 33) {
            es8388_hpvol_set(rs485_volume_val);
            es8388_spkvol_set(rs485_volume_val);
            audio_set_last_volume(rs485_volume_val);
            printf("[MAIN] Volume -> %d/33\r\n", rs485_volume_val);
            rs485_volume_flag = 0;
            rs485_volume_val  = 0xFF;
        }

        /* ---- C. 炒菜机UI渲染 (含背景图加载, 可能阻塞~200ms, 放最后执行) ---- */
        cook_render();

        /* ---- D. 本地按键扫描 ---- */
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

        /* ---- E. 报警循环播报检测(温度异常/火警) ---- */
        if (cook_is_alarm_loop()) {
            static uint16_t alarm_cooldown = 0;
            /* 上次播放已结束 + 冷却归零 + 当前未播放 -> 重播 */
            if (!rs485_cmd_flag && g_audiodev.status != 0x03 && alarm_cooldown == 0) {
                uint8_t voice_idx = cook_alarm_voice_index();
                if (voice_idx > 0) {
                    rs485_target_index = voice_idx;
                    rs485_cmd_flag     = 1;
                    alarm_cooldown     = 50;   /* 500ms冷却(50x10ms), 避免连续触发 */
                    printf("[MAIN] Alarm re-play voice #%d\r\n", voice_idx);
                }
            }
            if (alarm_cooldown > 0) alarm_cooldown--;
        }

        vTaskDelay(pdMS_TO_TICKS(10));   /* 10ms主循环周期 */
    }
}
