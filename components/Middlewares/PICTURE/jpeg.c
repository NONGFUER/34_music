/**
 ****************************************************************************************************
 * @file        jpeg.c
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

#include "jpeg.h"


static const char *TAG = "jpeg";

uint8_t *rx_buf = NULL;
uint8_t *tx_buf = NULL;
/* Configuration parameters for the JPEG decoder engine */
jpeg_decode_engine_cfg_t decode_eng_cfg = {
    .intr_priority = 0,
    .timeout_ms = 40,
};

/* Configuration parameters for a JPEG decoder image process */
jpeg_decode_cfg_t decode_cfg_rgb = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565, /* RGB565 */
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
};

/* JPEG decoder memory allocation config */
jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
    .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,   /* Alloc the picture output buffer, (decompressed format in decoder) */
};
/* JPEG decoder memory allocation config */
jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
    .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,    /* Alloc the picture input buffer, (compressed format in decoder) */
};

TickType_t jpeg_decode(const char *filename, int width, int height)
{
    jpeg_decoder_handle_t jpgd_handle;
    TickType_t startTick, endTick, diffTick;
    FRESULT ret;

    startTick = xTaskGetTickCount();


    /* Acquire a JPEG decode engine with the specified configuration */
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&decode_eng_cfg, &jpgd_handle));

    FIL* fp = (FIL *)malloc(sizeof(FIL));

    if (fp == NULL)
    {
        ESP_LOGE(__FUNCTION__, "Failed to allocate memory for file handle");
        return PIC_MEM_ERR;
    }
    /* Open File */
    ret = f_open(fp, (const TCHAR *)filename, FA_READ);

    if (ret != FR_OK)
    {
        ESP_LOGW(__FUNCTION__, "Failed to open file [%s], err=%d", filename, ret);
        free(fp);
        return PIC_FORMAT_ERR;
    }

    UINT bytes_read;

    /* Get size */
    FILINFO fno;
    ret = f_stat(filename, &fno);

    if (ret != FR_OK)
    {
        ESP_LOGE(TAG, "f_stat error: %d", ret);
        f_close(fp);
        free(fp);
        return PIC_FORMAT_ERR;
    }
    /* File size */
    // size_t jpeg_size = fno.fsize;
    size_t jpeg_size = fno.fsize;

    /* allocate memory space for JPEG decoder */
    size_t tx_buffer_size = 0;
    tx_buf = (uint8_t*)jpeg_alloc_decoder_mem(jpeg_size, &tx_mem_cfg, &tx_buffer_size);
    if (tx_buf == NULL)
    {
        ESP_LOGE(__FUNCTION__, "alloc tx buffer error");
        f_close(fp);
        free(fp);
        return PIC_MEM_ERR;
    }
    f_lseek(fp,SEEK_SET);
    /* Read File */
    ret = f_read(fp, tx_buf, jpeg_size, &bytes_read);

    if (jpeg_size != bytes_read)
    {
        ESP_LOGE(TAG, "f_read incomplete: %d/%d", bytes_read, jpeg_size);
        f_close(fp);
        free(fp);
        return PIC_FORMAT_ERR;
    }

    /* Structure for jpeg decode header */
    jpeg_decode_picture_info_t header_info;
    ESP_ERROR_CHECK(jpeg_decoder_get_info(tx_buf, jpeg_size, &header_info));
    ESP_LOGI(__FUNCTION__, "header parsed, width is %" PRId32 ", height is %" PRId32, header_info.width, header_info.height);

    /* allocate memory space for JPEG decoder */
    size_t rx_buffer_size = 0;
    rx_buf = (uint8_t*)jpeg_alloc_decoder_mem(header_info.width * header_info.height * 2 * 2, &rx_mem_cfg, &rx_buffer_size);
    
    if (rx_buf == NULL)
    {
        ESP_LOGE(__FUNCTION__, "alloc rx buffer error");
        f_close(fp);
        free(fp);
        free(tx_buf);
        tx_buf = NULL;
        return PIC_MEM_ERR;
    }

    static uint32_t out_size = 0;
    ESP_ERROR_CHECK(jpeg_decoder_process(jpgd_handle, &decode_cfg_rgb, tx_buf, tx_buffer_size, rx_buf, rx_buffer_size, &out_size));
    
    /* 计算居中绘制的起始坐标 */
    int x_offset = (width - header_info.width) / 2;
    int y_offset = (height  - header_info.height) / 2;

    /* 确保坐标合法性 */
    x_offset = x_offset < 0 ? 0 : x_offset;
    y_offset = y_offset < 0 ? 0 : y_offset;

    /* LCD draw */
    pic_phy.multicolor(x_offset, y_offset, header_info.width + x_offset, header_info.height + y_offset, (uint16_t *)rx_buf);
    
    f_close(fp);
    ESP_ERROR_CHECK(jpeg_del_decoder_engine(jpgd_handle));

    free(rx_buf);
    free(tx_buf);

    endTick = xTaskGetTickCount();
    diffTick = endTick - startTick;
    ESP_LOGI(__FUNCTION__, "elapsed time[ms]:%"PRIu32,diffTick * portTICK_PERIOD_MS);

    return diffTick;
}
