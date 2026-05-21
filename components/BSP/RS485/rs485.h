/**
 ****************************************************************************************************
 * @file        rs485.h
 * @author      sjwu
 * @version     V1.0
 * @date        2025-01-01
 * @brief       RS485驱动代码
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
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

#ifndef __RS485_H
#define __RS485_H

#include "driver/uart.h"
#include "driver/gpio.h"
#include "xl9555.h"
#include "esp_err.h"
#include <stdint.h>

/* RS485通信引脚 和 串口定义 */
#define RS485_UART_PORT         UART_NUM_1
#define RS485_TX_GPIO_PIN       GPIO_NUM_26
#define RS485_RX_GPIO_PIN       GPIO_NUM_27

/* 定义RS485接收大小及接收使能配置 */
#define RS485_REC_LEN   1024  /* 定义最大接收字节数1024 */

/* ================================================================== */
/*                    HEX 单字节指令协议定义                            */
/* ================================================================== */
/* 帧格式: [1字节指令], 无CRC, 无从机地址, 无帧尾                    */
/* 应答: 收到有效指令后回复 "OK\r\n"                                   */

/* ---- A组: 触发命令 (0xA1 ~ 0xAA) ---- */
#define CMD_BOOT           0xA1    /* 开机 */
#define CMD_POUR_BOX1      0xA3    /* 倒一号菜 */
#define CMD_POUR_BOX2      0xA4    /* 倒二号菜 */
#define CMD_POUR_BOX3      0xA5    /* 倒三号菜 */
#define CMD_BOX_RETURN     0xA6    /* 归位(用三号归位图) */
#define CMD_START_COOK     0xA7    /* 开始炒菜 */
#define CMD_FINISH         0xA8    /* 炒菜完成 */
#define CMD_ALARM_TEMP     0xA9    /* 温度异常(循环) */
#define CMD_ALARM_FIRE     0xAA    /* 火警(循环) */

/* ---- B组: 音量控制 (0xB0 ~ 0xB5, 共6档) ---- */
#define CMD_VOLUME_MUTE    0xB0    /* 静音(0) */
#define CMD_VOLUME_LV2     0xB1    /* 音量档位2 */
#define CMD_VOLUME_LV3     0xB2    /* 音量档位3 */
#define CMD_VOLUME_LV4     0xB3    /* 音量档位4 */
#define CMD_VOLUME_LV5     0xB4    /* 音量档位5 */
#define CMD_VOLUME_MAX     0xB5    /* 最大音量(33) */

/* 函数声明 */
esp_err_t rs485_init(uint32_t baudrate);                    /* RS485初始化 */
esp_err_t rs485_send_data(uint8_t *buf, uint8_t len);       /* RS485发送len个字节 */
esp_err_t rs485_receive_data(uint8_t *buf, uint8_t *len);   /* RS485查询接收到的数据 */

/**
 * @brief 解析HEX单字节指令
 * @param buf      接收缓冲区 (期望: 1字节hex指令)
 * @param len      接收长度
 * @return  >0: 有效命令ID (用于switch分发), -1: 无效/不回应
 */
int cmd_parse(const uint8_t *buf, uint8_t len);

#endif
