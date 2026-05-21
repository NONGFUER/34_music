#!/usr/bin/env python3
"""
炒菜机 HEX 单字节指令帧表生成器 V2
帧格式: [1字节hex指令], 无CRC, 无从机地址, 无帧尾
应答: "OK\r\n"
"""

CMD_TABLE = [
    # (序号, 功能,        hex值,  说明)
    (1,   "开机",         "A1",  "001.wav + 待机界面"),
    (2,   "保留",         "A2",  "(未使用)"),
    (3,   "倒一号菜",     "A3",  "003.wav"),
    (4,   "倒二号菜",     "A4",  "004.wav"),
    (5,   "倒三号菜",     "A5",  "005.wav"),
    (6,   "归位",         "A6",  "三号归位完成界面 + 006.wav"),
    (7,   "开始炒菜",     "A7",  "bg_reset + 007.wav"),
    (8,   "炒菜完成",     "A8",  "bg_done -> 3s后待机"),
    (9,   "温度异常(循环)","A9",  "循环播报009.wav"),
    (10,  "火警(循环)",   "AA",  "循环播报010.wav"),
]

VOL_TABLE = [
    # (序号, 功能,        hex值,  音量值)
    (11,  "静音",         "B0",   0),
    (12,  "音量1",        "B1",   7),
    (13,  "音量2",        "B2",   13),
    (14,  "音量3",        "B3",   19),
    (15,  "音量4",        "B4",   26),
    (16,  "最大(33)",     "B5",   33),
]

print("=" * 65)
print("  炒菜机 HEX 单字节指令协议帧表 V2")
print("  帧格式: [1字节hex]  |  应答: OK\\r\\n")
print("=" * 65)

print(f"\n--- A组: 触发命令 ---\n{'序号':<4} {'功能':<14} {'HEX':<6} 说明")
print("-" * 50)
for idx, name, cmd_hex, desc in CMD_TABLE:
    print(f"{idx:<4} {name:<14} 0x{cmd_hex:<4} {desc}")

print(f"\n--- B组: 音量控制 (6档) ---\n{'序号':<4} {'功能':<10} {'HEX':<6} {'音量值':<6}")
print("-" * 45)
for idx, name, cmd_hex, vol_val in VOL_TABLE:
    print(f"{idx:<4} {name:<10} 0x{cmd_hex:<4} {vol_val:<6}/33")

print("\n" + "=" * 65)
print("  归位(A6)统一使用三号归位图片(bg_pour3_done.jpg)")
print("=" * 65)

# ---- PLC发送用 ----
print("\n\n========== PLC发送用 ==========")
print("--- 触发命令 ---")
for idx, name, cmd_hex, desc in CMD_TABLE:
    if name != "保留":
        print(f"0x{cmd_hex}   # {name}: {desc}")
print("\n--- 音量控制 ---")
for idx, name, cmd_hex, vol_val in VOL_TABLE:
    print(f"0x{cmd_hex}   # {name}(音量={vol_val})")
print("=" * 35)
