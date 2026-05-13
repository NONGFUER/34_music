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
#include <stdint.h>

/* RS485通信引脚 和 串口定义 */
#define RS485_UART_PORT         UART_NUM_1
#define RS485_TX_GPIO_PIN       GPIO_NUM_26
#define RS485_RX_GPIO_PIN       GPIO_NUM_27

/* 定义RS485接收大小及接收使能配置 */
#define RS485_REC_LEN   1024  /* 定义最大接收字节数1024 */

/* ================================================================== */
/*                    MODBUS RTU 从机协议定义                           */
/* ================================================================== */
#define MODBUS_SLAVE_ADDR        0x11    /* 从机地址 */
#define MODBUS_FUNC_WRITE_REG    0x06    /* 功能码: 写单个寄存器 */

/* 寄存器地址映射 */
#define MODBUS_REG_SONG_FIRST    0x0001  /* 第1首 */
#define MODBUS_REG_SONG_LAST     0x00FF  /* 第255首 */
#define MODBUS_REG_VOLUME        0x0100  /* 音量寄存器 */

/* 数据区有效值 (播放控制) */
#define MODBUS_VAL_STOP          0x0000  /* 停止/暂停 */
#define MODBUS_VAL_PLAY          0x0001  /* 播放/恢复播放 */

/* 异常码 */
#define MODBUS_EX_ILLEGAL_FUNC   0x01    /* 非法功能码 */
#define MODBUS_EX_ILLEGAL_ADDR   0x02    /* 非法数据地址 */
#define MODBUS_EX_ILLEGAL_DATA   0x03    /* 非法数据值 */

/* 函数声明 */
esp_err_t rs485_init(uint32_t baudrate);                    /* RS485初始化 */
esp_err_t rs485_send_data(uint8_t *buf, uint8_t len);       /* RS485发送len个字节 */
esp_err_t rs485_receive_data(uint8_t *buf, uint8_t *len);   /* RS485查询接收到的数据 */

/**
 * @brief MODBUS CRC16 计算 (多项式 0xA001)
 * @param buf  数据缓冲区
 * @param len  数据长度(字节)
 * @return     16位CRC值
 */
uint16_t modbus_crc16(const uint8_t *buf, uint8_t len);

/**
 * @brief 解析MODBUS RTU请求帧, 验证并构建响应帧
 * @param rx_buf    接收缓冲区
 * @param rx_len    接收数据长度
 * @param tx_buf    响应帧输出缓冲区 (至少8字节)
 * @param tx_len    输出的响应帧长度 (0=无需响应)
 * @param reg_addr  输出的寄存器地址 (仅在返回0时有效)
 * @param reg_data  输出的寄存器写入值 (仅在返回0时有效)
 * @return  0=有效命令, -1=不应回应(地址不匹配/CRC错误/帧太短), 1=异常响应已构建
 */
int modbus_parse_frame(const uint8_t *rx_buf, uint8_t rx_len,
                       uint8_t *tx_buf, uint8_t *tx_len,
                       uint16_t *reg_addr, uint16_t *reg_data);

#endif
