/**
 ****************************************************************************************************
 * @file        jpeg.h
 * @author      sjwu
 * @version     V1.0
 * @date        2025-01-01
 * @brief       图片解码-jpeg解码 代码
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

#ifndef __JPEG_H_
#define __JPEG_H_

#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "piclib.h"
#include "esp_vfs_fat.h"
#include "driver/jpeg_decode.h"


/* 函数声明 */
TickType_t jpeg_decode(const char *filename, int width, int height); /* JPEG解码+绘制 */

/**
 * @brief  仅将JPEG解码为RGB565像素数据到动态缓冲区(不绘制到LCD)
 * @param  filename   JPEG文件路径
 * @param  width      显示宽度(用于对齐计算)
 * @param  height     显示高度(用于对齐计算)
 * @param  out_buf    [输出] 动态分配的RGB565缓冲区指针, 调用者需 free()
 * @param  out_size   [输出] 缓冲区有效字节数
 * @param  out_w      [输出] 解码后图像实际宽度(像素)
 * @param  out_h      [输出] 解码后图像实际高度(像素)
 * @return 0=成功, 非0=错误码
 */
int8_t jpeg_decode_to_buffer(const char *filename, int width, int height,
                             uint8_t **out_buf, size_t *out_size,
                             int32_t *out_w, int32_t *out_h);

#endif
