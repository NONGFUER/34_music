/**
 ****************************************************************************************************
 * @file        bmp.c
 * @author      sjwu
 * @version     V1.0
 * @date        2025-01-01
 * @brief       图片编解码-bmp格式 代码
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

#include "bmp.h"


#define SHOW_TIME 1     /* 编解码速度显示功能 */

/**
 * @brief       BMP图片解码
 * @param       filename      : 包含路径的文件名(.bmp等)
 * @param       width, height : 区域大小
 * @retval      操作结果
 *   @arg       0   , 成功
 *   @arg       其他, 错误码
 */
uint8_t bmp_decode(const char *filename, int width, int height)
{
    FIL *f_bmp;
    uint8_t res = 1;
    UINT br = 0;

#if SHOW_TIME == 1                              
    TickType_t startTick, endTick, diffTick;
    startTick = xTaskGetTickCount();
#endif

    f_bmp = (FIL *)piclib_mem_malloc(sizeof(FIL));
    if (f_bmp == NULL)
    {
        ESP_LOGE(__FUNCTION__, "Failed to allocate memory for file handle");
        return PIC_MEM_ERR;
    }

    res = f_open(f_bmp, (const TCHAR *)filename, FA_READ);
    if (res != FR_OK)
    {
        ESP_LOGW(__FUNCTION__, "Failed to open file [%s]", filename);
        free(f_bmp);
        return res;
    }

    /* 读取BMP位图信息头 */
    BITMAPINFO *hbmp = (BITMAPINFO *)piclib_mem_malloc(sizeof(BITMAPINFO));
    if (hbmp == NULL)
    {
        ESP_LOGE(__FUNCTION__, "Failed to allocate memory for BMP file information");
        f_close(f_bmp);
        free(f_bmp);
        return PIC_MEM_ERR;
    }

    res = f_read(f_bmp, &hbmp->bmfHeader.bfType, 2, &br);   /* 读取标识"BM" */
    if (res != FR_OK || hbmp->bmfHeader.bfType != 0x4D42)
    {
        ESP_LOGW(__FUNCTION__, "File is not BMP");
        free(hbmp);
        f_close(f_bmp);
        free(f_bmp);
        return res;
    }

    res |= f_read(f_bmp, &hbmp->bmfHeader.bfSize,          4, &br);
    res |= f_read(f_bmp, &hbmp->bmfHeader.bfReserved1,     2, &br);
    res |= f_read(f_bmp, &hbmp->bmfHeader.bfReserved2,     2, &br);
    res |= f_read(f_bmp, &hbmp->bmfHeader.bfOffBits,       4, &br);
    res |= f_read(f_bmp, &hbmp->bmiHeader.biSize,          4, &br);
    res |= f_read(f_bmp, &hbmp->bmiHeader.biWidth,         4, &br);
    res |= f_read(f_bmp, &hbmp->bmiHeader.biHeight,        4, &br);
    res |= f_read(f_bmp, &hbmp->bmiHeader.biPlanes,        2, &br);
    res |= f_read(f_bmp, &hbmp->bmiHeader.biBitCount,      2, &br);
    res |= f_read(f_bmp, &hbmp->bmiHeader.biCompression,   4, &br);
    res |= f_read(f_bmp, &hbmp->bmiHeader.biSizeImage,     4, &br);
    res |= f_read(f_bmp, &hbmp->bmiHeader.biXPelsPerMeter, 4, &br);
    res |= f_read(f_bmp, &hbmp->bmiHeader.biYPelsPerMeter, 4, &br);
    res |= f_read(f_bmp, &hbmp->bmiHeader.biClrUsed,       4, &br);
    res |= f_read(f_bmp, &hbmp->bmiHeader.biClrImportant,  4, &br);
    if (res != FR_OK)
    {
        ESP_LOGE(__FUNCTION__, "Failed to read BMP file information");
        free(hbmp);
        f_close(f_bmp);
        free(f_bmp);
        return res;
    }

    /* 判断BMP图像深度 */
    if (hbmp->bmiHeader.biBitCount == 24 && hbmp->bmiHeader.biCompression == 0)
    {
        /* BMP行填充（如果需要）到4字节边界 */
        uint32_t rowSize = (hbmp->bmiHeader.biWidth * 3 + 3) & ~3;
        int w = hbmp->bmiHeader.biWidth;
        int h = hbmp->bmiHeader.biHeight;

        /* 为 RGB565 颜色数组分配内存 */
        uint16_t *colors = calloc(w * h, MALLOC_CAP_DMA);
        if (colors == NULL)
        {
            ESP_LOGE(__FUNCTION__, "Failed to allocate memory for colors");
            free(hbmp);
            f_close(f_bmp);
            free(f_bmp);
            return PIC_MEM_ERR;
        }

        /* 计算居中绘制的起始坐标 */
        int x_offset = (width - w) / 2;
        int y_offset = (height - h) / 2;
        /* 确保坐标合法性 */
        x_offset = x_offset < 0 ? 0 : x_offset;
        y_offset = y_offset < 0 ? 0 : y_offset;

        uint8_t *sdbuffer = (uint8_t *)calloc(rowSize, MALLOC_CAP_DMA);
        if (sdbuffer == NULL)
        {
            ESP_LOGE(__FUNCTION__, "Failed to allocate memory for sdbuffer");
            free(colors);
            free(hbmp);
            f_close(f_bmp);
            free(f_bmp);
            return PIC_MEM_ERR;
        }

        /* 读取像素数据: 只seek一次到数据起点, 之后顺序读取避免逐行lseek开销 */
        uint32_t data_pos = hbmp->bmfHeader.bfOffBits;
        f_lseek(f_bmp, data_pos);

        for (int row = 0; row < h; row++)
        {
            /* BMP是bottom-up存储, 写入colors时逆序 */
            int dst_row = h - 1 - row;

            f_read(f_bmp, sdbuffer, rowSize, &br);
            
            for (int col = 0; col < w; col++)
            {
                uint8_t b = sdbuffer[col * 3];
                uint8_t g = sdbuffer[col * 3 + 1];
                uint8_t r = sdbuffer[col * 3 + 2];
                colors[dst_row * w + col] = rgb565(r, g, b); /* RGB到RGB565转换 */
            }
        }

        pic_phy.multicolor(x_offset, y_offset, w + x_offset, h + y_offset, colors);
        free(colors);
        free(sdbuffer);
    }

    free(hbmp);
    f_close(f_bmp);

#if SHOW_TIME == 1   
    endTick = xTaskGetTickCount();
    diffTick = endTick - startTick;
    ESP_LOGI(__FUNCTION__, "elapsed time[ms]:%"PRIu32, diffTick * portTICK_PERIOD_MS);
#endif

    return 0;
}

/**
 * @brief       BMP编码函数
 *   @note      将图像源保存为16位格式的BMP文件 RGB565格式.
 *              保存为rgb555格式则需要颜色转换,耗时间比较久,所以保存为565是最快速的办法.
 *
 * @param       filename    : 包含存储路径的文件名(.bmp)
 * @param       image_addr  : 图像源
 * @param       width,height: 区域大小
 * @param       mode        : 保存模式
 *   @arg                     0, 仅仅创建新文件的方式编码;
 *   @arg                     1, 如果之前存在文件,则覆盖之前的文件.如果没有,则创建新的文件; 
 * @retval      操作结果
 *   @arg       0   , 成功
 *   @arg       其他, 错误码
 */
uint8_t bmp_encode(uint8_t *filename, uint16_t *image_addr, uint16_t width, uint16_t height, uint8_t mode)
{
#if SHOW_TIME == 1                              
    TickType_t startTick, endTick, diffTick;
    startTick = xTaskGetTickCount();
#endif

    FIL *f_bmp;
    uint32_t bw = 0;
    uint16_t bmpheadsize;       /* bmp头大小 */
    BITMAPINFO hbmp;            /* bmp头 */
    uint8_t res = 0;
    uint16_t *databuf;          /* 数据缓存区地址 */
    uint16_t pixcnt;            /* 像素计数器 */
    uint16_t bi4width;          /* 水平像素字节数 */
    uint16_t row_index = 0;

    uint16_t *img_addr = (uint16_t *)image_addr;

    if (width == 0 || height == 0) return PIC_WINDOW_ERR;   /* 区域错误 */
    
    /* 开辟至少bi4width大小的字节的内存区域 ,对240宽的屏,480个字节就够了.最大支持1024宽度的bmp编码 */
    databuf = (uint16_t *)piclib_mem_malloc(2160);

    if (databuf == NULL) return PIC_MEM_ERR;            /* 内存申请失败. */

    f_bmp = (FIL *)piclib_mem_malloc(sizeof(FIL));      /* 开辟FIL字节的内存区域 */
    if (f_bmp == NULL)                                  /* 内存申请失败 */
    {
        piclib_mem_free(databuf);
        return PIC_MEM_ERR;
    }

    bmpheadsize = sizeof(hbmp);                                 /* 得到bmp文件头的大小 */
    memset((uint8_t *)&hbmp, 0, sizeof(hbmp));                  /* 置零空申请到的内存 */

    hbmp.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);    /* 信息头大小 */
    hbmp.bmiHeader.biWidth       = width;                       /* bmp的宽度 */
    hbmp.bmiHeader.biHeight      = height;                      /* bmp的高度 */
    hbmp.bmiHeader.biPlanes      = 1;                           /* 恒为1 */
    hbmp.bmiHeader.biBitCount    = 16;                          /* bmp为16位色bmp */
    hbmp.bmiHeader.biCompression = BI_BITFIELDS;                /* 每个象素的比特由指定的掩码决定 */
    hbmp.bmiHeader.biSizeImage   = hbmp.bmiHeader.biHeight * hbmp.bmiHeader.biWidth * hbmp.bmiHeader.biBitCount / 8;  /* bmp数据区大小 */

    hbmp.bmfHeader.bfType    = ((uint16_t)'M' << 8) + 'B';      /* BM格式标志 */   
    hbmp.bmfHeader.bfSize    = bmpheadsize + hbmp.bmiHeader.biSizeImage; /* 整个bmp的大小 */
    hbmp.bmfHeader.bfOffBits = bmpheadsize;                     /* 到数据区的偏移 */

    hbmp.RGB_MASK[0] = 0X00F800;        /* 红色掩码 */
    hbmp.RGB_MASK[1] = 0X0007E0;        /* 绿色掩码 */
    hbmp.RGB_MASK[2] = 0X00001F;        /* 蓝色掩码 */

    if (mode == 1)
    {
        res = f_open(f_bmp, (const TCHAR *)filename, FA_READ | FA_WRITE);       /* 尝试打开之前的文件 */
    }
    
    if (mode == 0 || res == 0x04)
    {
        res = f_open(f_bmp, (const TCHAR *)filename, FA_WRITE | FA_CREATE_NEW); /* 模式0,或者尝试打开失败,则创建新文件 */
    }

    if ((hbmp.bmiHeader.biWidth * 2) % 4)       /* 水平像素(字节)不为4的倍数 */
    {
        bi4width = ((hbmp.bmiHeader.biWidth * 2) / 4 + 1) * 4;  /* 实际要写入的宽度像素,必须为4的倍数 */
    }
    else
    {
        bi4width = hbmp.bmiHeader.biWidth * 2;  /* 刚好为4的倍数 */
    }

    if (res == FR_OK)   /* 创建成功 */
    {
        res = f_write(f_bmp, (uint8_t *)&hbmp, bmpheadsize, (UINT *)&bw);       /* 写入BMP首部 */

        for (uint16_t ty = height - 1; hbmp.bmiHeader.biHeight; ty--)   /* 按一行一行操作 */
        {
            pixcnt = 0;

            for (uint16_t xpix_index = 0; pixcnt != (bi4width / 2);)
            {     
                if (pixcnt < hbmp.bmiHeader.biWidth)
                {
                    databuf[pixcnt] = img_addr[pixcnt + hbmp.bmiHeader.biWidth * row_index];   
                }
                else
                {
                    databuf[pixcnt] = 0Xffff;   /* 补充白色的像素 */
                }

                pixcnt++;
                xpix_index++;
            }
            hbmp.bmiHeader.biHeight--;
            row_index++;
            res = f_write(f_bmp, (uint8_t *)databuf, bi4width, (UINT *)&bw);    /* 写入一行数据 */
        }
        f_close(f_bmp);
    }


#if SHOW_TIME == 1   
    endTick = xTaskGetTickCount();
    diffTick = endTick - startTick;
    ESP_LOGI(__FUNCTION__, "elapsed time[ms]:%"PRIu32, diffTick * portTICK_PERIOD_MS);
#endif

    piclib_mem_free(databuf);
    piclib_mem_free(f_bmp);

    return res;
}