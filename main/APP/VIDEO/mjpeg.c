/**
 ****************************************************************************************************
 * @file        mjpeg.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       MJPEG视频处理 代码
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

#include "mjpeg.h"


jpeg_decoder_handle_t jpgd_handle;                      /* JPEG解码器句柄 */

static jpeg_decode_cfg_t decode_cfg_rgb = {                    /* JPEG解码器配置参数 */
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,     /* 解码输出的颜色格式RGB565 */
    .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,    /* 解码输出的RGB顺序 */
};

/**
 * @brief       初始化JPEG解码器
 * @param       width: 显示图像的宽度
 * @param       height: 显示图像的高度
 * @param       malloc_size: 申请到BUF大小
 * @retval      JPEG解码器申请到内存的地址
 */
uint8_t *mjpegdec_init(uint32_t width, uint32_t height, uint32_t *malloc_size)
{
    jpeg_decode_engine_cfg_t decode_eng_cfg = {     /* JPEG解码器引擎配置 */
        .intr_priority = 0,                         /* 中断优先级,0选择默认 */
        .timeout_ms    = -1,                        /* 超时时间 */
    };
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&decode_eng_cfg, &jpgd_handle));    /* 安装JPEG解码器驱动 */

    jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {   /* JPEG解码器内存申请配置 */
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,   /* 图像输出BUF */
    };

    return (uint8_t *)jpeg_alloc_decoder_mem(width * height * 2 * 4, &rx_mem_cfg, (size_t *)malloc_size);   /* 分配缓冲区 */
}

/**
 * @brief       解码一副JPEG图片
 * @param       inbuf: jpeg数据流数组
 * @param       inbuf_size: jpeg数据流大小
 * @param       outbuf: jpeg解码器申请的内存缓冲区
 * @param       outbuf_size: jpeg解码器申请到内存大小
 * @param       width: 显示图像的宽度
 * @param       height: 显示图像的高度  
 * @retval      0,成功; 1,错误帧/解码错误
 */
uint8_t mjpegdec_decode(uint8_t *inbuf, uint32_t inbuf_size,  uint8_t *outbuf, uint32_t outbuf_size, uint32_t width, uint32_t height)
{
    if (inbuf_size == 0)    /* 帧错误,跳过解码 */
    {
        return 1;
    }

    static uint32_t out_size = 0;
    ESP_ERROR_CHECK(jpeg_decoder_process(jpgd_handle, &decode_cfg_rgb, inbuf, inbuf_size, outbuf, outbuf_size, &out_size));     /* 解码JPEG图片 */
    if (out_size == 0)      /* 解码长度错误 */
    {
        return 1;
    }

    /* 计算居中绘制的起始坐标 */
    int x_offset = (lcddev.width - width) / 2;
    int y_offset = (lcddev.height  - height) / 2;

    /* 确保坐标合法性 */
    x_offset = x_offset < 0 ? 0 : x_offset;
    y_offset = y_offset < 0 ? 0 : y_offset;

    /* LCD draw */
    esp_lcd_panel_draw_bitmap(lcddev.lcd_panel_handle, x_offset, y_offset, width + x_offset, height + y_offset, (uint8_t *)outbuf);

    return 0;
}

/**
 * @brief       卸载JPEG解码器并释放内存
 * @param       buf: JPEG解码器申请的内存
 * @retval      无
 */
void mjpegdec_free(uint8_t *buf)
{
    heap_caps_free(buf);                                    /* 释放JPEG解码器申请的内存 */
    ESP_ERROR_CHECK(jpeg_del_decoder_engine(jpgd_handle));  /* 卸载JPEG解码器引擎 */
}