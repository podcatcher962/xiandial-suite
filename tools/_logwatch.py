# -*- coding: utf-8 -*-
"""纯日志监视：只读串口、只落盘，【不发任何命令】。

用法：
    python _logwatch.py <秒数> <输出.log> [COM口]

为什么单独写一个（而不是用 _lq_ui.py）：
    _lq_ui.py 会在末尾发 lq_shot 截图，而 shot() 里有一次
    reset_input_buffer() —— 那会丢掉这期间攒下的设备日志。
    验证「手机配网」这类【别人操作、我只旁观】的流程时，
    我需要一条"绝不碰设备、只记录"的通道。

★ dtr/rts 必须在 open() 之前置 False：ESP32-S3 的 USB-Serial-JTAG
  用 DTR/RTS 组合做复位 —— 否则一打开串口就把板子重启，
  正在观察的那次操作（热点、引导层）当场消失。
"""
import sys
import time

import serial

secs = float(sys.argv[1])
out_path = sys.argv[2]
port = sys.argv[3] if len(sys.argv) > 3 else 'COM3'

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.3
ser.dtr = False
ser.rts = False
ser.open()

print('已打开 %s，只读记录 %.0f 秒 -> %s' % (port, secs, out_path), flush=True)

f = open(out_path, 'w', encoding='utf-8')
t0 = time.time()
n = 0
while time.time() - t0 < secs:
    d = ser.read(4096)
    if d:
        f.write(d.decode('utf-8', 'replace'))
        f.flush()
        n += len(d)
ser.close()
f.close()
print('结束：共收到 %d 字节' % n, flush=True)
