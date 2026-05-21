"""
ESP32 兼容 JPG 转换工具
=======================
将普通JPG转换为 ESP32 硬件JPEG解码器可识别的格式:
  - YUV 4:2:2 或 4:4:4 采样 (非 4:2:0)
  - Baseline 模式 (非 Progressive/渐进式)

用法:
    # 单个文件转换
    python convert_jpg_for_esp32.py <输入文件> [输出文件]

    # 批量转换整个文件夹（默认输出到 文件夹/esp32_output/ 子目录）
    python convert_jpg_for_esp32.py --dir <文件夹路径> [--out <输出文件夹>]

依赖:
    pip install Pillow numpy
"""

import sys
import os
from PIL import Image


def convert_one(input_path, output_path=None):
    """转换单个 JPG 文件"""
    
    if not os.path.exists(input_path):
        print(f"[ERROR] 输入文件不存在: {input_path}")
        return False
    
    if output_path is None:
        base, _ = os.path.splitext(input_path)
        output_path = f"{base}_esp32.jpg"
    
    print(f"[INFO] 读取: {input_path}")
    
    img = Image.open(input_path)
    print(f"[INFO] 原始尺寸: {img.size}, 模式: {img.mode}")
    
    if img.mode != 'RGB':
        img = img.convert('RGB')
        print(f"[INFO] 转换为 RGB 模式")
    
    img.save(
        output_path,
        'JPEG',
        quality=85,
        subsampling=1,
        optimize=False,
        progressive=False,
    )
    
    out_size = os.path.getsize(output_path)
    print(f"[OK] 已输出: {output_path} ({out_size / 1024:.1f} KB)")
    return True


def convert_dir(dir_path, out_dir=None):
    """批量转换文件夹内所有 JPG"""
    
    if not os.path.isdir(dir_path):
        print(f"[ERROR] 文件夹不存在: {dir_path}")
        return False
    
    if out_dir is None:
        out_dir = os.path.join(dir_path, "esp32_output")
    os.makedirs(out_dir, exist_ok=True)
    
    extensions = {'.jpg', '.jpeg', '.JPG', '.JPEG'}
    files = [f for f in os.listdir(dir_path) 
             if os.path.splitext(f)[1] in extensions]
    
    if not files:
        print(f"[INFO] 文件夹中没有找到 JPG 文件: {dir_path}")
        return True
    
    print("=" * 50)
    print(f"  批量转换: {dir_path}")
    print(f"  找到 {len(files)} 个 JPG 文件")
    print(f"  输出至:   {out_dir}")
    print("=" * 50)
    print()
    
    ok_count = 0
    fail_count = 0
    
    for fname in sorted(files):
        input_path = os.path.join(dir_path, fname)
        base, _ = os.path.splitext(fname)
        output_path = os.path.join(out_dir, f"{base}.jpg")
        
        print(f"\n--- [{ok_count + fail_count + 1}/{len(files)}] {fname} ---")
        if convert_one(input_path, output_path):
            ok_count += 1
        else:
            fail_count += 1
    
    print()
    print("=" * 50)
    print(f"  完成! 成功: {ok_count}, 失败: {fail_count}, 共: {len(files)}")
    print("=" * 50)
    return fail_count == 0


if __name__ == '__main__':
    args = sys.argv[1:]
    
    if not args:
        print("=" * 50)
        print("  ESP32 兼容 JPG 转换工具")
        print("=" * 50)
        print()
        print("用法:")
        print(f"  单文件: python {sys.argv[0]} <输入JPG> [输出JPG]")
        print(f"  批量:   python {sys.argv[0]} --dir <文件夹> [--out <输出文件夹>]")
        print()
        print("示例:")
        print(f"  python {sys.argv[0]} photo.jpg")
        print(f"  python {sys.argv[0]} photo.jpg output.jpg")
        print(f"  python {sys.argv[0]} --dir ./images")
        print(f"       -> 输出到 ./images/esp32_output/")
        print(f"  python {sys.argv[0]} --dir ./images --out ./converted")
        print()
        print("依赖安装:")
        print("  pip install Pillow")
        sys.exit(1)
    
    if args[0] == '--dir':
        if len(args) < 2:
            print("[ERROR] --dir 需要指定文件夹路径")
            sys.exit(1)
        dir_path = args[1]
        out_dir = None
        if '--out' in args:
            idx = args.index('--out')
            if idx + 1 >= len(args):
                print("[ERROR] --out 需要指定输出文件夹路径")
                sys.exit(1)
            out_dir = args[idx + 1]
        ok = convert_dir(dir_path, out_dir)
        sys.exit(0 if ok else 1)
    else:
        input_file = args[0]
        output_file = args[1] if len(args) > 2 else None
        ok = convert_one(input_file, output_file)
        sys.exit(0 if ok else 1)
