#ifndef __PNG_H_
#define __PNG_H_

#include <stdint.h>
#include <stdbool.h>
#include "pngle.h"
#include "piclib.h"
#include "ff.h"
#include "esp_log.h"


/* 函数声明 */
void png_init(pngle_t *pngle, uint32_t w, uint32_t h);                                              /* PNG库初始化 */
void png_draw(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t rgba[4]);     /* PNG绘画 */
void png_finish(pngle_t *pngle);                                                                    /* PNG解码完成回调函数 */
TickType_t png_decode(const char *filename, int width, int height);                                 /* PNG图片解码 */

#endif
