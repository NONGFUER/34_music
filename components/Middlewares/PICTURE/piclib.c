#include "piclib.h"


_pic_info picinfo;      /* 图片信息 */
_pic_phy pic_phy;       /* 图片显示物理接口 */

/**
 * @brief       多点填充
 * @param       x, y          : 起始坐标
 * @param       width, height : 宽度和高度
 * @param       color         : 颜色数组
 * @retval      无
 */
static void piclib_multi_color(uint16_t x, uint16_t y, uint16_t ex, uint16_t ey, uint16_t *color)
{
    /* 边界裁剪: 确保结束坐标不超过LCD尺寸 */
    if (ex > lcddev.width)  ex = lcddev.width;
    if (ey > lcddev.height) ey = lcddev.height;
    if (x >= ex || y >= ey) return;  /* 非法坐标, 直接丢弃 */

    esp_lcd_panel_draw_bitmap(lcddev.lcd_panel_handle, x, y, ex, ey, color);
}

/**
 * @brief       画图初始化
 *   @note      在画图之前,必须先调用此函数, 指定相关函数
 * @param       无
 * @retval      无
 */
void piclib_init(void)
{
    pic_phy.draw_point = lcd_draw_point;            /* 画点函数实现,仅GIF需要 */
    pic_phy.fill = lcd_fill;                        /* 填充函数实现,仅GIF需要 */
    pic_phy.draw_hline = lcd_draw_hline;            /* 画线函数实现,仅GIF需要 */
    pic_phy.multicolor = piclib_multi_color;        /* 颜色填充函数实现,JPEG、BMP、PNG需要 */

    picinfo.lcdwidth = lcddev.width;                /* 得到LCD的宽度像素 */
    picinfo.lcdheight = lcddev.height;              /* 得到LCD的高度像素 */
}

/**
 * @brief       智能画图
 *   @note      图片仅在x,y和width, height限定的区域内显示.
 *
 * @param       filename      : 包含路径的文件名(.bmp/.jpg/.jpeg/.gif等)
 * @param       x, y          : 起始坐标
 * @param       width, height : 显示区域
 *   @note                      图片尺寸小于等于液晶分辨率,才支持快速解码
 * @retval      无
 */
uint8_t piclib_ai_load_picfile(char *filename, uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    uint8_t	res = 0;/* 返回值 */
    uint8_t temp;

    if ((x + width) > picinfo.lcdwidth) return PIC_WINDOW_ERR;      /* x坐标超范围了 */

    if ((y + height) > picinfo.lcdheight) return PIC_WINDOW_ERR;    /* y坐标超范围了 */

    /* 得到显示方框大小 */
    if (width == 0 || height == 0)return PIC_WINDOW_ERR;            /* 窗口设定错误 */

    /* 文件名传递 */
    temp = exfuns_file_type(filename);   /* 得到文件的类型 */
    ESP_LOGI("here","temp:%#x ", temp);
    switch (temp)
    {
        case T_BMP:
            ESP_LOGI("here","enter");
            res = bmp_decode(filename, width, height);           /* 解码BMP */
            break;

        case T_JPG:
        case T_JPEG:
            res = jpeg_decode(filename, width, height);          /* 解码JPG/JPEG */
            break;

        case T_GIF:
            res = gif_decode(filename, x, y, width, height);    /* 解码gif */
            break;

        case T_PNG:
            res = png_decode(filename, width, height);          /* 解码PNG */
            break;

        default:
            res = PIC_FORMAT_ERR;                               /* 非图片格式!!! */
            break;
    }

    return res;
}

/**
 * @brief       动态分配内存
 * @param       size: 要申请的内存大小(字节)
 * @retval      分配到的内存首地址
 */
void *piclib_mem_malloc(uint32_t size)
{
    return (void *)malloc(size);
}

/**
 * @brief       释放内存
 * @param       paddr: 内存首地址
 * @retval      无
 */
void piclib_mem_free(void *paddr)
{
    free(paddr);
}
