/**
 ****************************************************************************************************
 * @file        mytimer.h
 *  
 * @version     V1.0
 * @date        2025-01-01
 * @brief       视频播放器实验定时器 驱动代码
 *  
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 ESP32-P4 开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 * 
 ****************************************************************************************************
 */

#ifndef __MYTIMER_H
#define __MYTIMER_H

#include "esp_timer.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "../src/gptimer_priv.h"


/* 函数声明 */
void frame_timer_init(uint64_t tps);        /* 初始化用于等待帧间隔时间定时器 */
void frame_timer_start(uint64_t tps);       /* 定时器重新开启 */
void frame_timer_stop(void);                /* 定时器暂停工作 */
void frame_rate_timer(void);                /* 用于帧率显示的定时器初始化函数 */
bool IRAM_ATTR frame_rate_print(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_data);   /* 定时器回调函数 */

#endif
