/**
 ****************************************************************************************************
 * @file        jpeg.h
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       图片解码-jpeg解码 代码
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

#ifndef __JPEG_H_
#define __JPEG_H_

#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "piclib.h"
#include "esp_vfs_fat.h"
#include "driver/jpeg_decode.h"


/* 函数声明 */
TickType_t jpeg_decode(const char *filename, int width, int height); /* JPEG解码 */

#endif
