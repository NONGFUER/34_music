#!/usr/bin/env python3
"""
Modbus RTU 帧生成器 - 炒菜机 V2 协议 (完整版)
从机地址: 0x11, 功能码: 0x06, CRC16 (多项式 0xA001)
"""

def modbus_crc16(buf: bytes) -> int:
    """MODBUS CRC16 计算 (多项式 0xA001, 初始值 0xFFFF)"""
    crc = 0xFFFF
    for byte in buf:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def make_frame(reg_addr: int, reg_data: int) -> str:
    """构建完整的 MODBUS RTU 写单寄存器帧, 返回十六进制字符串(空格分隔)"""
    slave_addr = 0x11
    func_code = 0x06
    frame = bytes([slave_addr, func_code,
                   (reg_addr >> 8) & 0xFF, reg_addr & 0xFF,
                   (reg_data >> 8) & 0xFF, reg_data & 0xFF])
    crc = modbus_crc16(frame)
    full_frame = frame + bytes([crc & 0xFF, (crc >> 8) & 0xFF])
    return " ".join(f"{b:02X}" for b in full_frame)


# ============================================================
#                    炒菜机完整帧表
# ============================================================

CMD_TRIGGER = [
    # (名称,           寄存器, 数据,   说明)
    ("开机",           0x0001, 0x0001, "001.wav + 待机界面"),
    ("倒一号菜",       0x0003, 0x0001, "003.wav"),
    ("倒二号菜",       0x0004, 0x0001, "004.wav"),
    ("倒三号菜",       0x0005, 0x0001, "005.wav"),
    ("一号菜盒归位",   0x0006, 0x0001, "box1归位完成界面(数据=1)"),
    ("二号菜盒归位",   0x0006, 0x0002, "box2归位完成界面(数据=2)"),
    ("三号菜盒归位",   0x0006, 0x0003, "box3归位完成界面(数据=3)"),
    ("开始炒菜",       0x0007, 0x0001, "bg_reset + 007.wav"),
    ("炒菜完成",       0x0008, 0x0001, "bg_done → 3s后待机"),
    ("温度异常(循环)", 0x0009, 0x0001, "循环播报009.wav"),
    ("火警(循环)",     0x000A, 0x0001, "循环播报010.wav"),
]

# 停止播报帧 (数据区=0x0000, 任意命令地址均有效)
STOP_ADDR = [0x0001, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000A]

VOLUME_TABLE = [
    ("音量=0(静音)",   0x0100, 0x0000),
    ("音量=10",        0x0100, 0x000A),
    ("音量=21(最大)",  0x0100, 0x0015),
]


print("=" * 85)
print("  炒菜机 MODBUS RTU V2 完整帧表")
print("  从机地址: 0x11 | 功能码: 0x06 | CRC16: 多项式0xA001 初始值0xFFFF")
print("=" * 85)

# ---- 触发命令 ----
print(f"\n{'序号':<4} {'功能':<18} {'寄存器':<10} {'数据':<8} {'完整帧(Hex)':<46} 说明")
print("-" * 110)
idx = 1
for name, addr, data, desc in CMD_TRIGGER:
    f = make_frame(addr, data)
    print(f"{idx:<4} {name:<18} 0x{addr:04X}      0x{data:04X}   {f:<44} {desc}")
    idx += 1

# ---- 停止播报 ----
print(f"\n{'─'*40} 停止播报 (数据区=0x0000) {'─'*35}")
for addr in STOP_ADDR:
    name = f"停止@0x{addr:04X}"
    f = make_frame(addr, 0x0000)
    print(f"{idx:<4} {name:<18} 0x{addr:04X}      0x0000   {f:<44} 停语音+停报警+界面不变")
    idx += 1

# ---- 音量控制 ----
print(f"\n{'─'*40} 音量控制 (0x0100) {'─'*38}")
for name, addr, data in VOLUME_TABLE:
    f = make_frame(addr, data)
    print(f"{idx:<4} {name:<18} 0x{addr:04X}      0x{data:04X}   {f:<44}")
    idx += 1

print("\n" + "=" * 85)
print("  协议说明:")
print("  ├─ 数据区 0x0000 = 停止播报(任意命令地址均可)")
print("  ├─ 数据区 非0   = 触发对应地址功能")
print("  ├─ REG_BOX_RETURN(0x0006): 数据区 1/2/3 区分归位的菜盒")
print("  ├─ 温度异常/火警: 激活后自动循环播报, 需发0x0000停止")
print("  └─ 炒菜完成: 显示done界面3秒后自动跳回待机")
print("=" * 85)

# ---- PLC复制用纯文本版 ----
print("\n\n========== PLC直接复制版 (纯Hex, 每行一帧) ==========")
for name, addr, data, desc in CMD_TRIGGER:
    print(f"# {name}: {desc}")
    print(make_frame(addr, data))
print("# --- 停止播报 (任选其一即可) ---")
print(make_frame(STOP_ADDR[0], 0x0000))  # 用第一个地址示例
print("# --- 音量 ---")
for name, addr, data in VOLUME_TABLE[:2]:
    print(f"# {name}")
    print(make_frame(addr, data))
print("=" * 60)
