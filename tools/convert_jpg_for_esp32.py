"""
ESP32 兼容 JPG 转换工具
=======================
将普通JPG转换为 ESP32 硬件JPEG解码器可识别的格式:
  - YUV 4:2:2 或 4:4:4 采样 (非 4:2:0)
  - Baseline 模式 (非 Progressive/渐进式)

用法:
    python convert_jpg_for_esp32.py <输入文件> [输出文件]

依赖:
    pip install Pillow numpy
"""

import sys
import os
from PIL import Image


def convert_for_esp32(input_path, output_path=None):
    """转换为 ESP32 硬件解码器兼容的 JPG"""
    
    if not os.path.exists(input_path):
        print(f"[ERROR] 输入文件不存在: {input_path}")
        return False
    
    # 默认输出文件名
    if output_path is None:
        base, _ = os.path.splitext(input_path)
        output_path = f"{base}_esp32.jpg"
    
    print(f"[INFO] 读取: {input_path}")
    
    # 打开并转为RGB
    img = Image.open(input_path)
    print(f"[INFO] 原始尺寸: {img.size}, 模式: {img.mode}")
    
    # 转为RGB (去除可能的CMYK/RGBA等)
    if img.mode != 'RGB':
        img = img.convert('RGB')
        print(f"[INFO] 转换为 RGB 模式")
    
    # 保存为 Baseline JPG, 使用 4:2:2 子采样 (JPEG参数 '4:2:0'='4:2:2')
    # Pillow 的 subsampling 参数:
    #   0 = 4:4:4 (无子采样)
    #   1 = 4:2:2 (水平半采样) ← ESP32兼容!
    #   2 = 4:2:0 (水平和垂直半采样) ← ESP32不兼容!
    img.save(
        output_path,
        'JPEG',
        quality=85,
        subsampling=1,           # ★ 关键: 使用 4:2:2 而不是默认的 4:2:0
        optimize=False,          # 不优化 (避免生成渐进式标记)
        progressive=False,       # ★ 关键: 强制 Baseline 模式
    )
    
    out_size = os.path.getsize(output_path)
    print(f"[✓] 已输出: {output_path} ({out_size / 1024:.1f} KB)")
    print(f"[✓] 格式: Baseline JPEG, YUV 4:2:2 子采样")
    return True


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("=" * 50)
        print("  ESP32 兼容 JPG 转换工具")
        print("=" * 50)
        print()
        print(f"用法: python {sys.argv[0]} <输入JPG> [输出JPG]")
        print()
        print("示例:")
        print(f"  python {sys.argv[0]} photo.jpg")
        print(f"  python {sys.argv[0]} photo.jpg output_esp32.jpg")
        print()
        print("依赖安装:")
        print("  pip install Pillow")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None
    
    ok = convert_for_esp32(input_file, output_file)
    sys.exit(0 if ok else 1)
