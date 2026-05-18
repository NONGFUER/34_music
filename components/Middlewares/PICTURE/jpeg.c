/**
 ****************************************************************************************************
 * @file        jpeg.c
 * @author      sjwu
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

    printf("[JPEG] STEP0: start decode [%s] %dx%d\r\n", filename, width, height);

    /* Acquire a JPEG decode engine with the specified configuration */
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&decode_eng_cfg, &jpgd_handle));

    printf("[JPEG] STEP1: engine acquired\r\n");

    FIL* fp = (FIL *)malloc(sizeof(FIL));

    if (fp == NULL)
    {
        printf("[JPEG] FAIL-A: malloc FIL failed\r\n");
        return PIC_MEM_ERR;
    }
    /* Open File */
    ret = f_open(fp, (const TCHAR *)filename, FA_READ);

    if (ret != FR_OK)
    {
        printf("[JPEG] FAIL-B: f_open [%s] err=%d\r\n", filename, ret);
        free(fp);
        return PIC_FORMAT_ERR;
    }

    printf("[JPEG] STEP2: file opened\r\n");

    UINT bytes_read;

    /* Get size */
    FILINFO fno;
    ret = f_stat(filename, &fno);

    if (ret != FR_OK)
    {
        printf("[JPEG] FAIL-C: f_stat err=%d\r\n", ret);
        f_close(fp);
        free(fp);
        return PIC_FORMAT_ERR;
    }
    /* File size */
    size_t jpeg_size = fno.fsize;
    printf("[JPEG] STEP3: file size=%zu bytes\r\n", jpeg_size);

    /* allocate memory space for JPEG decoder */
    size_t tx_buffer_size = 0;
    tx_buf = (uint8_t*)jpeg_alloc_decoder_mem(jpeg_size, &tx_mem_cfg, &tx_buffer_size);
    if (tx_buf == NULL)
    {
        printf("[JPEG] FAIL-D: alloc tx buffer (%zu bytes) failed\r\n", jpeg_size);
        f_close(fp);
        free(fp);
        return PIC_MEM_ERR;
    }

    printf("[JPEG] STEP4: tx buffer alloc OK (%zu bytes)\r\n", tx_buffer_size);

    f_lseek(fp,SEEK_SET);
    /* Read File */
    ret = f_read(fp, tx_buf, jpeg_size, &bytes_read);

    if (jpeg_size != bytes_read)
    {
        printf("[JPEG] FAIL-E: f_read incomplete %u/%zu\r\n", bytes_read, jpeg_size);
        f_close(fp);
        free(fp);
        return PIC_FORMAT_ERR;
    }

    printf("[JPEG] STEP5: file read OK\r\n");

    /* Structure for jpeg decode header */
    jpeg_decode_picture_info_t header_info;
    ESP_ERROR_CHECK(jpeg_decoder_get_info(tx_buf, jpeg_size, &header_info));
    printf("[JPEG] STEP6: header parsed, %" PRId32 "x%" PRId32 " (display: %dx%d)\r\n",
           header_info.width, header_info.height, width, height);

    /* 图片尺寸过大警告 */
    if (header_info.width > width || header_info.height > height) {
        printf("[JPEG] WARN: image %" PRId32 "x%" PRId32 " > display %dx%d! "
               "Please resize JPG to match LCD resolution.\r\n",
               header_info.width, header_info.height, width, height);
    }

    /* ESP-IDF JPEG decoder 要求宽16字节对齐 */
    int32_t aligned_w = ((header_info.width + 15) / 16) * 16;
    size_t rx_alloc_size = (size_t)aligned_w * (size_t)header_info.height * 2;
    
    printf("[JPEG] STEP6b: aligned_w=%" PRId32 ", need %zu bytes\r\n", aligned_w, rx_alloc_size);
    
    if (rx_alloc_size == 0 || (int32_t)rx_alloc_size < 0) {
        printf("[JPEG] FAIL-F: invalid alloc size!\r\n");
        f_close(fp); free(fp); free(tx_buf); tx_buf = NULL;
        return PIC_MEM_ERR;
    }

    size_t rx_buffer_size = 0;
    printf("[JPEG] STEP7: try alloc rx buffer %zu bytes...\r\n", rx_alloc_size);
    rx_buf = (uint8_t*)jpeg_alloc_decoder_mem(rx_alloc_size, &rx_mem_cfg, &rx_buffer_size);
    
    if (rx_buf == NULL)
    {
        printf("[JPEG] FAIL-F: alloc rx buffer failed (need %zu bytes)!\r\n", rx_alloc_size);
        f_close(fp);
        free(fp);
        free(tx_buf);
        tx_buf = NULL;
        return PIC_MEM_ERR;
    }

    printf("[JPEG] STEP8: rx buffer alloc OK (%zu bytes avail)\r\n", rx_buffer_size);

    static uint32_t out_size = 0;
    ESP_ERROR_CHECK(jpeg_decoder_process(jpgd_handle, &decode_cfg_rgb, tx_buf, tx_buffer_size, rx_buf, rx_buffer_size, &out_size));
    printf("[JPEG] STEP9: decode done, out_size=%" PRIu32 " bytes\r\n", out_size);

    /* 计算居中绘制的起始坐标 */
    int x_offset = (width - header_info.width) / 2;
    int y_offset = (height  - header_info.height) / 2;

    /* 确保坐标合法性 */
    x_offset = x_offset < 0 ? 0 : x_offset;
    y_offset = y_offset < 0 ? 0 : y_offset;

    /* LCD draw */
    printf("[JPEG] STEP10: draw at (%d,%d) size %" PRId32 "x%" PRId32 "\r\n",
           x_offset, y_offset, header_info.width, header_info.height);
    pic_phy.multicolor(x_offset, y_offset, header_info.width + x_offset, header_info.height + y_offset, (uint16_t *)rx_buf);
    printf("[JPEG] STEP11: draw complete\r\n");
    
    f_close(fp);
    ESP_ERROR_CHECK(jpeg_del_decoder_engine(jpgd_handle));

    free(rx_buf);
    free(tx_buf);

    endTick = xTaskGetTickCount();
    diffTick = endTick - startTick;
    printf("[JPEG] DONE: elapsed %" PRIu32 " ms\r\n", diffTick * portTICK_PERIOD_MS);

    return 0;   /* 成功返回0, 耗时已在上方打印 */
}
