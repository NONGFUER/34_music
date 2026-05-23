/**
 ****************************************************************************************************
 * @file        mjpeg.h
 *  
 * @version     V1.0
 * @date        2025-01-01
 * @brief       MJPEG视频处理 代码
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
 
#ifndef __MJPEG_H
#define __MJPEG_H 

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "piclib.h"


/* 函数声明 */
uint8_t *mjpegdec_init(uint32_t width, uint32_t height, uint32_t *malloc_size);     /* 初始化JPEG解码器 */
uint8_t mjpegdec_decode(uint8_t *buf, uint32_t bsize,  uint8_t *outbuf, uint32_t outbuf_size, uint32_t width, uint32_t height);     /* 解码一副JPEG图片 */
void mjpegdec_free(uint8_t *buf);                                                           /* 卸载JPEG解码器并释放内存 */

#endif
