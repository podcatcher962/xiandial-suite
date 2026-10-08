# -*- coding: utf-8 -*-
"""灵签固件 · 屏幕截图（走串口的 lq_shot 命令）。

用法：
    python _lq_shot.py [COM口] [输出.png] [启动等待秒] [页面] [签号]

为什么需要它：
    改 UI 最怕的是「我看不到」。没这个工具时，每一轮都得烧固件、
    等人看一眼、再听描述 —— 一轮十几分钟，而且描述经过一次转述。
    有了它，改完自己先看，把"明显不对"的那几轮挡在自己这边。

协议（设备侧 app_stlist.c 的 st_shot）：
    发  "lq_shot\r\n"
    ←   "SHOT <w> <h> <nbytes> <crc32>\r\n"
    ←   nbytes 个原始字节（LVGL 行主序 RGB565，小端）
    ←   "\r\nSHOT_END\r\n"
★ CRC 必须验：控制台日志与这帧二进制走同一个 USB-JTAG，
  中途插进一行日志就会整体错位 —— 表现是颜色全乱，但字节数一个不差。
"""
import sys
import time
import zlib

import numpy as np
from PIL import Image
import serial


def drain(ser, sec):
    t0 = time.time()
    while time.time() - t0 < sec:
        ser.read(16384)
        time.sleep(0.05)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else 'COM3'
    # ★ 默认输出到【当前目录】，不要写死开发机上的绝对路径 ——
    #   文件名是发布物的一部分，本机路径属于发布禁区（发布前扫描会拦下）。
    out = sys.argv[2] if len(sys.argv) > 2 else 'shot.png'
    after = float(sys.argv[3]) if len(sys.argv) > 3 else 16.0
    page = sys.argv[4] if len(sys.argv) > 4 else None
    pick = sys.argv[5] if len(sys.argv) > 5 else None

    ser = serial.Serial(port, 115200, timeout=0.3)
    print('已打开 %s —— 等板子起来（%.0f 秒）…' % (port, after))
    drain(ser, after)
    # ★★ 必须【彻底静默】再发命令：控制台日志与二进制帧走同一条 USB-JTAG，
    #   中间插进任何一行日志都会把帧流整体错位（字节数照样"看起来"对）。
    #   这里读空 2 秒确认安静；日志止不住就退避重试。
    for attempt in range(6):
        drain(ser, 2.0)
        pending = ser.in_waiting
        print('  静默确认 %d/6：残留 %d 字节' % (attempt + 1, pending))
        if pending == 0:
            break
        time.sleep(1.0)
    ser.reset_input_buffer()

    if page is not None:
        cmd = 'lq_demo %s' % page
        if pick is not None:
            cmd += ' %s' % pick
        print('先跳页：%s' % cmd)
        ser.write((cmd + '\r\n').encode())
        drain(ser, 1.5)

    print('发 lq_shot …')
    ser.write(b'lq_shot\r\n')

    buf = bytearray()
    hdr = None
    payload = b''
    t0 = time.time()
    while time.time() - t0 < 20:
        d = ser.read(8192)
        if d:
            buf += d
        i = buf.find(b'SHOT ')
        if i >= 0:
            j = buf.find(b'\r\n', i)
            if j > i:
                hdr = buf[i:j].decode('ascii', 'replace')
                payload = bytes(buf[j + 2:])
                break
        if len(buf) > 200000:
            del buf[:-200000]
    if not hdr:
        print('❌ 没等到 SHOT 头。最后收到的内容：')
        print(bytes(buf[-400:]))
        ser.close()
        return 2

    p = hdr.split()
    w, h, nb = int(p[1]), int(p[2]), int(p[3])
    want = int(p[4], 16)
    print('头：%s' % hdr)

    data = bytearray(payload)
    t0 = time.time()
    last = 0
    while len(data) < nb and time.time() - t0 < 40:
        d = ser.read(65536)
        if d:
            data += d
            if len(data) - last >= 32768:
                last = len(data)
                print('    …%d/%d 字节（%.0f%%）'
                      % (len(data), nb, 100.0 * len(data) / nb), flush=True)
    if len(data) < nb:
        print('❌ 只收到 %d/%d 字节' % (len(data), nb))
        ser.close()
        return 3
    data = bytes(data[:nb])

    got = zlib.crc32(data) & 0xFFFFFFFF
    print('CRC %08X / %08X  %s' % (got, want, '✅ 一致' if got == want else '❌ 不一致（混进日志了）'))

    a = np.frombuffer(data, dtype='<u2').reshape(h, w)
    r = (((a >> 11) & 0x1F).astype(np.uint16) * 255 // 31)
    g = (((a >> 5) & 0x3F).astype(np.uint16) * 255 // 63)
    b = ((a & 0x1F).astype(np.uint16) * 255 // 31)
    img = np.dstack([r, g, b]).astype(np.uint8)
    Image.fromarray(img).save(out)
    print('已保存', out, img.shape)
    ser.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
