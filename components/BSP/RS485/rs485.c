
/**
 ****************************************************************************************************
 * @file        rs485.c
 * @author      sjwu
 * @version     V1.0
 * @date        2025-01-01
 * @brief       RS485驱动代码
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
/*                      MODBUS RTU 协议处理                            */
/* ================================================================== */

/**
 * @brief MODBUS CRC16 计算
 * @note  多项式 0xA001, 初始值 0xFFFF, LSB first
 */
uint16_t modbus_crc16(const uint8_t *buf, uint8_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint8_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}

/**
 * @brief MODBUS RTU 请求帧解析 + 响应帧构建
 * @note  仅处理功能码 0x06 (写单个寄存器)
 *        V2: 地址即语义模式
 *        - 炒菜机命令区 0x0001~0x000A: 数据值无限制, 写入即触发
 *          (其中 0x0006=归位, 数据区值区分box_id: 1/2/3)
 *        - 音量控制区 0x0100: 数据 0000~0021
 */
int modbus_parse_frame(const uint8_t *rx_buf, uint8_t rx_len,
                       uint8_t *tx_buf, uint8_t *tx_len,
                       uint16_t *reg_addr, uint16_t *reg_data)
{
    *tx_len = 0;

    /* ---- 1. 帧长检查: MODBUS 写单寄存器最小帧=8字节 ---- */
    if (rx_len < 8) {
        return -1;  /* 帧不完整, 不回应 */
    }

    /* ---- 2. 从机地址检查 ---- */
    if (rx_buf[0] != MODBUS_SLAVE_ADDR) {
        return -1;  /* 不是发给我们的, 不回应 */
    }

    /* ---- 3. CRC16 校验 ---- */
    uint16_t recv_crc = rx_buf[6] | ((uint16_t)rx_buf[7] << 8);
    uint16_t calc_crc = modbus_crc16(rx_buf, 6);

    if (recv_crc != calc_crc) {
        return -1;  /* CRC错误, 不回应 */
    }

    /* ---- 4. 功能码检查 ---- */
    uint8_t func = rx_buf[1];
    if (func != MODBUS_FUNC_WRITE_REG) {
        /* 异常响应: 非法功能码 */
        tx_buf[0] = MODBUS_SLAVE_ADDR;
        tx_buf[1] = func | 0x80;
        tx_buf[2] = MODBUS_EX_ILLEGAL_FUNC;
        uint16_t crc = modbus_crc16(tx_buf, 3);
        tx_buf[3] = crc & 0xFF;
        tx_buf[4] = (crc >> 8) & 0xFF;
        *tx_len = 5;
        return 1;
    }

    /* ---- 5. 提取寄存器地址和数据 ---- */
    *reg_addr = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
    *reg_data = ((uint16_t)rx_buf[4] << 8) | rx_buf[5];

    /* ---- 6. 验证寄存器地址和数据 ---- */
    if (*reg_addr >= REG_COOK_FIRST && *reg_addr <= REG_COOK_LAST) {
        /* 炒菜机命令区: 数据 0x0000=停止播报, 非0=触发命令(具体值由各命令解释) */
    } else if (*reg_addr == MODBUS_REG_VOLUME) {
        /* 音量控制区: 数据范围 0x0000 ~ 0x0021 (0~33) */
        if (*reg_data > 0x0021) {
            goto exception_illegal_data;
        }
    } else {
        /* 非法地址 */
        goto exception_illegal_addr;
    }

    /* ---- 7. 有效命令 → 回显响应 ---- */
    memcpy(tx_buf, rx_buf, 6);
    uint16_t resp_crc = modbus_crc16(tx_buf, 6);
    tx_buf[6] = resp_crc & 0xFF;
    tx_buf[7] = (resp_crc >> 8) & 0xFF;
    *tx_len = 8;
    return 0;

exception_illegal_addr:
    tx_buf[0] = MODBUS_SLAVE_ADDR;
    tx_buf[1] = MODBUS_FUNC_WRITE_REG | 0x80;
    tx_buf[2] = MODBUS_EX_ILLEGAL_ADDR;
    goto build_exception_crc;

exception_illegal_data:
    tx_buf[0] = MODBUS_SLAVE_ADDR;
    tx_buf[1] = MODBUS_FUNC_WRITE_REG | 0x80;
    tx_buf[2] = MODBUS_EX_ILLEGAL_DATA;

build_exception_crc:
    {
        uint16_t crc = modbus_crc16(tx_buf, 3);
        tx_buf[3] = crc & 0xFF;
        tx_buf[4] = (crc >> 8) & 0xFF;
        *tx_len = 5;
    }
    return 1;
}