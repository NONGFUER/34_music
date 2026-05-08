/**
 ****************************************************************************************************
 * @file        xl9555.h
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       XL9555驱动代码
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

#ifndef __XL9555_H
#define __XL9555_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "myiic.h"
#include "string.h"


/* 引脚与相关参数定义 */
#define XL9555_INT_IO               GPIO_NUM_36                     /* XL9555_INT引脚 */

/* XL9555寄存器宏 */
#define XL9555_INPUT_PORT0_REG      0                               /* 输入寄存器0地址 */
#define XL9555_INPUT_PORT1_REG      1                               /* 输入寄存器1地址 */
#define XL9555_OUTPUT_PORT0_REG     2                               /* 输出寄存器0地址 */
#define XL9555_OUTPUT_PORT1_REG     3                               /* 输出寄存器1地址 */
#define XL9555_INVERSION_PORT0_REG  4                               /* 极性反转寄存器0地址 */
#define XL9555_INVERSION_PORT1_REG  5                               /* 极性反转寄存器1地址 */
#define XL9555_CONFIG_PORT0_REG     6                               /* 方向配置寄存器0地址 */
#define XL9555_CONFIG_PORT1_REG     7                               /* 方向配置寄存器1地址 */

#define XL9555_ADDR                 0X24                            /* XL9555器件7位地址-->请看手册（9.1. Device Address） */

/* XL9555各个IO的功能 */
#define BEEP_IO                     0x0001                          /* 蜂鸣器控制引脚 */
#define SPK_EN_IO                   0x0002                          /* 功放使能引脚 */
#define GBC_LED_IO                  0x0004                          /* ATK_MODULE接口LED引脚 */
#define GBC_KEY_IO                  0x0008                          /* ATK_MODULE接口KEY引脚 */
#define RS485_RE_IO                 0x0010                          /* 485切换发送/接收引脚 */ 
#define SLCD_PWR_IO                 0x0020                          /* SPI_LCD控制背光引脚 */
#define EXIO_6_IO                   0x0040                          /* 未使用引脚 */
#define SLCD_RST_IO                 0x0080                          /* SPI_LCD复位引脚 */
#define KEY_0_IO                    0x0100                          /* 按键0引脚 */
#define KEY_1_IO                    0x0200                          /* 按键1引脚 */
#define KEY_2_IO                    0x0400                          /* 按键2引脚 */
#define AP_INT_IO                   0x0800                          /* AP3216C中断引脚 */
#define QMI_INT_IO                  0x1000                          /* 六轴传感器中断引脚 */
#define LED_1_IO                    0x2000                          /* LED1引脚 */
#define EXIO_14_IO                  0x4000                          /* 未使用引脚 */
#define EXIO_15_IO                  0x8000                          /* 未使用引脚 */

#define KEY0                        xl9555_pin_read(KEY_0_IO)
#define KEY1                        xl9555_pin_read(KEY_1_IO)
#define KEY2                        xl9555_pin_read(KEY_2_IO)

#define KEY0_PRES                   1
#define KEY1_PRES                   2
#define KEY2_PRES                   3

/* 函数声明 */
esp_err_t xl9555_init(void);                                            /* 初始化XL9555 */
int xl9555_pin_read(uint16_t pin);                                      /* 获取某个IO状态 */
uint16_t xl9555_pin_write(uint16_t pin, int val);                       /* 控制某个IO的电平 */
esp_err_t xl9555_read_byte(uint8_t* data, size_t len);                  /* 读取XL9555的IO值 */
esp_err_t xl9555_write_byte(uint8_t reg, uint8_t *data, size_t len);    /* 向XL9555寄存器写入数据 */
uint8_t xl9555_key_scan(uint8_t mode);                                  /* 扫描扩展按键 */
void xl9555_int_init(void);                                             /* 初始化XL9555的中断引脚 */

#endif
