#include "jpeg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <inttypes.h>   /* PRIu32, PRId32 for printf format */


static const char *TAG = "jpeg";

/** JPEG解码互斥锁 (防止多任务并发操作全局 rx_buf/tx_buf 导致 double free) */
static SemaphoreHandle_t s_jpeg_mutex = NULL;

uint8_t *rx_buf = NULL;
uint8_t *tx_buf = NULL;
/* Configuration parameters for the JPEG decoder engine */
jpeg_decode_engine_cfg_t decode_eng_cfg = {
    .intr_priority = 0,
    .timeout_ms = 40,
};

/* Configuration parameters for a JPEG decoder image process */
static jpeg_decode_cfg_t decode_cfg_rgb = {
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
    /* 懒初始化: 首次调用时创建互斥锁 */
    if (s_jpeg_mutex == NULL) {
        s_jpeg_mutex = xSemaphoreCreateMutex();
    }
    /* ★ 获取锁: 确保同一时刻只有一个任务在解码(防止全局 rx_buf/tx_buf 并发冲突) */
    xSemaphoreTake(s_jpeg_mutex, portMAX_DELAY);

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
        xSemaphoreGive(s_jpeg_mutex);
        return PIC_MEM_ERR;
    }
    /* Open File */
    ret = f_open(fp, (const TCHAR *)filename, FA_READ);

    if (ret != FR_OK)
    {
        printf("[JPEG] FAIL-B: f_open [%s] err=%d\r\n", filename, ret);
        free(fp);
        xSemaphoreGive(s_jpeg_mutex);
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
        xSemaphoreGive(s_jpeg_mutex);
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
        xSemaphoreGive(s_jpeg_mutex);
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
        xSemaphoreGive(s_jpeg_mutex);
        return PIC_FORMAT_ERR;
    }

    printf("[JPEG] STEP5: file read OK\r\n");

    /* Structure for jpeg decode header */
    jpeg_decode_picture_info_t header_info;
    ESP_ERROR_CHECK(jpeg_decoder_get_info(tx_buf, jpeg_size, &header_info));
    printf("[JPEG] STEP6: header parsed, %" PRId32 "x%" PRId32 " (display: %dx%d)\r\n",
           header_info.width, header_info.height, width, height);

    /* 图片尺寸异常保护: 某些JPG(AI导出等)的header解析可能出错, 此时强制用显示尺寸 */
    if (header_info.width > (uint32_t)width * 2 || header_info.height > (uint32_t)height * 2) {
        printf("[JPEG] WARN: parsed size %" PRId32 "x%" PRId32 " looks wrong, force to %dx%d\r\n",
               header_info.width, header_info.height, width, height);
        header_info.width  = width;
        header_info.height = height;
    } else if (header_info.width > (uint32_t)width || header_info.height > (uint32_t)height) {
        printf("[JPEG] WARN: image %" PRId32 "x%" PRId32 " > display %dx%d!\r\n",
               header_info.width, header_info.height, width, height);
    }

    /* ESP-IDF JPEG decoder 要求宽16字节对齐 */
    int32_t aligned_w = ((header_info.width + 15) / 16) * 16;
    size_t rx_alloc_size = (size_t)aligned_w * (size_t)header_info.height * 2;
    
    printf("[JPEG] STEP6b: aligned_w=%" PRId32 ", need %zu bytes\r\n", aligned_w, rx_alloc_size);
    
    if (rx_alloc_size == 0 || (int32_t)rx_alloc_size < 0) {
        printf("[JPEG] FAIL-F: invalid alloc size!\r\n");
        f_close(fp); free(fp); free(tx_buf); tx_buf = NULL;
        xSemaphoreGive(s_jpeg_mutex);
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
        xSemaphoreGive(s_jpeg_mutex);
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

    free(rx_buf);  rx_buf = NULL;   /* ★ 释放后置NULL, 防止 double free */
    free(tx_buf);  tx_buf = NULL;

    xSemaphoreGive(s_jpeg_mutex);   /* ★ 解锁 */

    endTick = xTaskGetTickCount();
    diffTick = endTick - startTick;
    printf("[JPEG] DONE: elapsed %" PRIu32 " ms\r\n", diffTick * portTICK_PERIOD_MS);

    return 0;   /* 成功返回0, 耗时已在上方打印 */
}

/* ================================================================== */
/*            jpeg_decode_to_buffer — 仅解码不绘制                        */
/*   复用 jpeg_decode 的前半段逻辑(读文件+解码),                       */
/*   但跳过 LCD 绘制, 将 rx_buf 所有权转交给调用者                      */
/* ================================================================== */

int8_t jpeg_decode_to_buffer(const char *filename, int width, int height,
                             uint8_t **out_buf, size_t *out_size,
                             int32_t *out_w, int32_t *out_h)
{
    /* 懒初始化互斥锁(与 jpeg_decode 共用) */
    if (s_jpeg_mutex == NULL) {
        s_jpeg_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_jpeg_mutex, portMAX_DELAY);

    jpeg_decoder_handle_t jpgd_handle;
    FRESULT ret;

    printf("[JPEG-BUF] decode to buffer: [%s] %dx%d\r\n", filename, width, height);

    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&decode_eng_cfg, &jpgd_handle));

    FIL *fp = (FIL *)malloc(sizeof(FIL));
    if (fp == NULL) {
        printf("[JPEG-BUF] FAIL: malloc FIL\r\n");
        xSemaphoreGive(s_jpeg_mutex);
        return PIC_MEM_ERR;
    }

    ret = f_open(fp, (const TCHAR *)filename, FA_READ);
    if (ret != FR_OK) {
        printf("[JPEG-BUF] FAIL: f_open err=%d\r\n", ret);
        free(fp);
        xSemaphoreGive(s_jpeg_mutex);
        return PIC_FORMAT_ERR;
    }

    UINT bytes_read;
    FILINFO fno;
    ret = f_stat(filename, &fno);
    if (ret != FR_OK) {
        printf("[JPEG-BUF] FAIL: f_stat err=%d\r\n", ret);
        f_close(fp); free(fp);
        xSemaphoreGive(s_jpeg_mutex);
        return PIC_FORMAT_ERR;
    }
    size_t jpeg_size = fno.fsize;

    /* 分配JPEG输入缓冲区 */
    size_t tx_buffer_size = 0;
    uint8_t *tx_buf_local = (uint8_t *)jpeg_alloc_decoder_mem(jpeg_size, &tx_mem_cfg, &tx_buffer_size);
    if (tx_buf_local == NULL) {
        printf("[JPEG-BUF] FAIL: alloc tx (%zu bytes)\r\n", jpeg_size);
        f_close(fp); free(fp);
        xSemaphoreGive(s_jpeg_mutex);
        return PIC_MEM_ERR;
    }

    /* 读文件 */
    f_lseek(fp, SEEK_SET);
    ret = f_read(fp, tx_buf_local, jpeg_size, &bytes_read);
    if (jpeg_size != bytes_read) {
        printf("[JPEG-BUF] FAIL: f_read incomplete %u/%zu (SD card unstable?)\r\n",
               bytes_read, jpeg_size);
        f_close(fp); free(fp); free(tx_buf_local);
        xSemaphoreGive(s_jpeg_mutex);
        return PIC_FORMAT_ERR;  /* SD卡读取不完整 → 告诉调用者不要继续闪烁 */
    }

    /* 解析header */
    jpeg_decode_picture_info_t header_info;
    ESP_ERROR_CHECK(jpeg_decoder_get_info(tx_buf_local, jpeg_size, &header_info));

    /* 尺寸异常保护(同 jpeg_decode) */
    if (header_info.width > (uint32_t)width * 2 || header_info.height > (uint32_t)height * 2) {
        header_info.width  = width;
        header_info.height = height;
    } else if (header_info.width > (uint32_t)width || header_info.height > (uint32_t)height) {
        /* clamp到显示尺寸 */
        header_info.width  = width;
        header_info.height = height;
    }

    /* 分配RGB565输出缓冲区 */
    int32_t aligned_w = ((header_info.width + 15) / 16) * 16;
    size_t rx_alloc_size = (size_t)aligned_w * (size_t)header_info.height * 2;

    if (rx_alloc_size == 0 || (int32_t)rx_alloc_size < 0) {
        printf("[JPEG-BUF] FAIL: invalid alloc size!\r\n");
        f_close(fp); free(fp); free(tx_buf_local);
        xSemaphoreGive(s_jpeg_mutex);
        return PIC_MEM_ERR;
    }

    size_t rx_buffer_size = 0;
    uint8_t *rx_buf_local = (uint8_t *)jpeg_alloc_decoder_mem(rx_alloc_size, &rx_mem_cfg, &rx_buffer_size);
    if (rx_buf_local == NULL) {
        printf("[JPEG-BUF] FAIL: alloc rx (%zu bytes)\r\n", rx_alloc_size);
        f_close(fp); free(fp); free(tx_buf_local);
        xSemaphoreGive(s_jpeg_mutex);
        return PIC_MEM_ERR;
    }

    /* 解码 */
    static uint32_t out_size_tmp = 0;
    ESP_ERROR_CHECK(jpeg_decoder_process(jpgd_handle, &decode_cfg_rgb,
                                         tx_buf_local, tx_buffer_size,
                                         rx_buf_local, rx_buffer_size, &out_size_tmp));

    /* 清理临时资源(fclose/free tx_buf/释放引擎), 但保留 rx_buf 给调用者 */
    f_close(fp);
    free(fp);
    free(tx_buf_local);
    ESP_ERROR_CHECK(jpeg_del_decoder_engine(jpgd_handle));

    xSemaphoreGive(s_jpeg_mutex);

    /* 输出结果 */
    *out_buf  = rx_buf_local;
    *out_size = rx_buffer_size;
    *out_w    = header_info.width;
    *out_h    = header_info.height;

    printf("[JPEG-BUF] OK: %" PRIu32 "x%" PRIu32 ", buf=%zu bytes\r\n",
           header_info.width, header_info.height, rx_buffer_size);
    return 0;  /* 成功 */
}
