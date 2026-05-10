/**
 ****************************************************************************************************
 * @file        rs485.h
 * @author      正点原子团队(ALIENTEK)
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


/* RS485通信引脚 和 串口定义 */
#define RS485_UART_PORT         UART_NUM_1
#define RS485_TX_GPIO_PIN       GPIO_NUM_26
#define RS485_RX_GPIO_PIN       GPIO_NUM_27

/* 定义RS485接收大小及接收使能配置 */
#define RS485_REC_LEN   1024  /* 定义最大接收字节数1024 */

/* 函数声明 */
esp_err_t rs485_init(uint32_t baudrate);                    /* RS485初始化 */
esp_err_t rs485_send_data(uint8_t *buf, uint8_t len);       /* RS485发送len个字节 */
esp_err_t rs485_receive_data(uint8_t *buf, uint8_t *len);   /* RS485查询接收到的数据 */

#endif
