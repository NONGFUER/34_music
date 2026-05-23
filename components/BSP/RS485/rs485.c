
/**
 ****************************************************************************************************
 * @file        rs485.c
 * @author      sjwu
 * @version     V1.0
 * @date        2025-01-01
 * @brief       RS485驱动代码
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

#include "rs485.h"
#include <string.h>

/**
 * @brief       RS485模式控制.
 * @param       en:0,接收;1,发送
 * @retval      无
 */
void rs485_tx_set(uint8_t en)
{
    xl9555_pin_write(RS485_RE_IO, en);
}

/**
 * @brief       RS485初始化
 * @note        该函数主要是初始化串口
 * @param       baudrate:波特率,根据自己需要设置波特率值
 * @retval      ESP_OK:表示初始化成功
 */
esp_err_t rs485_init(uint32_t baudrate)
{
    uart_config_t rs485_config = {0};

    rs485_config.baud_rate  = baudrate;                 /* 设置波特率 */
    rs485_config.data_bits  = UART_DATA_8_BITS;         /* 数据位 */
    rs485_config.parity     = UART_PARITY_DISABLE;      /* 无奇偶校验位 */
    rs485_config.stop_bits  = UART_STOP_BITS_1;         /* 一位停止位 */
    rs485_config.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE; /* 无硬件流控 */
    rs485_config.source_clk = UART_SCLK_DEFAULT;        /* 选择时钟源 */
    ESP_ERROR_CHECK(uart_param_config(RS485_UART_PORT, &rs485_config));     /* RS485的UART配置 */

    /* 设置管脚 */  
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART_PORT, RS485_TX_GPIO_PIN, RS485_RX_GPIO_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* 安装串口驱动 */
    ESP_ERROR_CHECK(uart_driver_install(RS485_UART_PORT, RS485_REC_LEN, RS485_REC_LEN, 0, NULL, 0));  

    /* 设置超时时间 3.5T * 8 = 28 ticks, TOUT=3 -> ~24..33 ticks */
    ESP_ERROR_CHECK(uart_set_rx_timeout(RS485_UART_PORT, 3));

    rs485_tx_set(0);    /* 进入接收模式 */

    return ESP_OK;
}

/**
 * @brief       RS485发送len个字节
 * @param       buf:发送区首地址
 * @param       len:发送的字节数(为了和本代码的接收匹配,这里建议不要超过RS485_REC_LEN个字节)
 * @retval      ESP_OK:表示发送成功
 */
esp_err_t rs485_send_data(uint8_t *buf, uint8_t len)
{
    rs485_tx_set(1);    /* 进入发送模式 */

    uart_write_bytes(RS485_UART_PORT, buf, len); 
    ESP_ERROR_CHECK(uart_wait_tx_done(RS485_UART_PORT, 10));

    rs485_tx_set(0);    /* 退出发送模式 */

    return ESP_OK;
}

/**
 * @brief       RS485查询接收到的数据
 * @param       buf:接收缓冲区首地址
 * @param       len:接收到的数据长度
 *   @arg           0, 表示没有接收到任何数据; 其他, 表示接收到的数据长度
 * @retval      ESP_OK:表示接收成功
 */
esp_err_t rs485_receive_data(uint8_t *buf, uint8_t *len)
{
    *len = 0;

    ESP_ERROR_CHECK(uart_get_buffered_data_len(RS485_UART_PORT, (size_t*)len));
    
    if (*len > 0)
    {
        uart_read_bytes(RS485_UART_PORT, buf, *len, 100);
    }

    return ESP_OK;
}

/* ================================================================== */
/*                      HEX 单字节指令协议处理                          */
/* ================================================================== */

/**
 * @brief 解析HEX单字节指令
 * @note  每条指令 = 1个hex字节, 收到即匹配
 * @param buf  接收数据指针
 * @param len  数据长度 (期望为1)
 * @return >0: 命令ID(1~15), -1: 未识别
 */
int cmd_parse(const uint8_t *buf, uint8_t len)
{
    if (len < 1) return -1;

    switch (buf[0]) {
        case CMD_BOOT:           return  1;   /* 0xA1 开机 */
        case CMD_POUR_BOX1:      return  2;   /* 0xA3 倒一号菜 */
        case CMD_POUR_BOX2:      return  3;   /* 0xA4 倒二号菜 */
        case CMD_POUR_BOX3:      return  4;   /* 0xA5 倒三号菜 */
        case CMD_START_COOK:     return  5;   /* 0xA6 开始炒菜 */
        case CMD_FINISH:         return  6;   /* 0xA7 炒菜完成 */
        case CMD_ALARM_TEMP:     return  7;   /* 0xA8 温度异常 */
        case CMD_ALARM_FIRE:     return  8;   /* 0xA9 火警 */
        case CMD_C1:             return 16;   /* 0xC1 显示归位界面 */
        case CMD_VOLUME_MUTE:    return 10;   /* 0xB0 静音 */
        case CMD_VOLUME_LV2:     return 11;   /* 0xB1 音量2 */
        case CMD_VOLUME_LV3:     return 12;   /* 0xB2 音量3 */
        case CMD_VOLUME_LV4:     return 13;   /* 0xB3 音量4 */
        case CMD_VOLUME_LV5:     return 14;   /* 0xB4 音量5 */
        case CMD_VOLUME_MAX:     return 15;   /* 0xB5 最大音量 */
        default:                 return -1;
    }
}