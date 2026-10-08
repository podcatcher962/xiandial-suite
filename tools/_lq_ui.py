# -*- coding: utf-8 -*-
"""拾声/灵签 · 串口 UI 验证：在一次会话里「发若干命令 + 截当前屏」。

用法：
    python _lq_ui.py <输出.png> [等待秒] [命令1] [命令2] ...

例：
    python _lq_ui.py E:\\workbuddy\\_lq_art\\home.png 3
        → 只截【主菜单】（开机默认就在这）

    python _lq_ui.py E:\\workbuddy\\_lq_art\\lq.png 3 "app_open 0"
        → 先进灵签，再截封面

    python _lq_ui.py E:\\workbuddy\\_lq_art\\set.png 3 "app_open 0" "lq_demo set"
        → 进灵签 → 弹设置面板 → 截图（能看见新加的「主 页」按钮）

    python _lq_ui.py E:\\workbuddy\\_lq_art\\back.png 3 "app_open 0" "app_home"
        → 进灵签 → 回主菜单 → 截图（验证"退出"这条路）

    python _lq_ui.py E:\\workbuddy\\_lq_art\\speak.png 4 --log "app_open 1" "sleep:8" "ui_demo speak" "sleep:12"
        → 进天气 → 触发一次播报 → 截图 + 把设备日志打出来
          （--log 见下；播报这类"看截图看不出来"的改动必须靠日志）

★ shot:<文件名> 不是设备命令，是【本地动作】：在命令序列中间插一次截图。
   为什么需要（10-08 加）：原来一次会话只在最后截一张，于是
   「随时间变化的东西」根本没法验 —— 最典型的是播放页的 12 根频谱柱，
   它每 200 ms 刷一次，只截一帧看到的是"静止的一排"，分不清
   「没数据」和「恰好那一刻在低谷」。插几帧一对比就一眼分明。
   例：
     python _lq_ui.py a.png 3 "app_open 0" "st_play 0" "sleep:6" "radio view 1" \
                            "shot:b.png" "sleep:2" "shot:c.png" "sleep:2" "shot:d.png"
     → 播放页连拍 4 帧（a=列表 b/c/d=频谱的演化）

★ --log（10-08 加）：把等待期间收到的设备日志打到屏幕。
   不加这个开关时 drain() 是默默丢掉的，于是「拼了几段 / 多少字节 /
   缺哪个原子」这类信息全看不见 —— 而语音播报恰恰只有这些能证明它跑了。
   例：--log "st_ls"（顺带看看 _va_tmp0.mp3 有没有生成）

★ sleep:N 不是设备命令，是【本地等待】：进 App 后 HTTP 取数要 1~3 秒
   （冷启动更久），不等就只截到"载入中…"。

★ 为什么另写一个，而不直接用 _lq_shot.py：
    _lq_shot.py 只会发 lq_demo，而 10-08 起开机停在【主菜单】——
    灵签的屏还没建，lq_demo 会直接返回（"界面还没建立"）。
    要验证"主菜单 → 产品 → 回主菜单"，必须能在同一次串口会话里
    先发 app_open / app_home 再截图；而串口【不能并发打开】，
    所以只能合成一次会话做完。

★ 与 _lq_shot.py 共用同一套协议（设备侧 app_stlist.c 的 st_shot）：
    发 "lq_shot\\r\\n" ← "SHOT <w> <h> <nbytes> <crc32>\\r\\n" + 原始 RGB565 + "SHOT_END"
★ CRC 必须验：控制台日志与这帧二进制走同一条 USB-JTAG，
  中途插进一行日志就整体错位 —— 表现是颜色全乱，但字节数一个不差。
"""
import sys
import time
import zlib

import numpy as np
from PIL import Image
import serial

PORT = 'COM3'
BAUD = 115200

# ★ 10-08 加：--log 时把等待期间收到的【设备日志】打到屏幕。
#   为什么需要：有些改动【看截图看不出来】—— 最典型的是语音播报，
#   它不出声就是不出声，截图上一切正常。而"拼了几段、多少字节、
#   缺哪个原子"只有设备日志里有。原本 drain() 是默默丢掉的。
ECHO = False


def drain(ser, sec):
    t0 = time.time()
    while time.time() - t0 < sec:
        d = ser.read(16384)
        if ECHO and d:
            sys.stdout.write(d.decode('utf-8', 'replace'))
            sys.stdout.flush()
        time.sleep(0.05)


def send_cmd(ser, cmd, settle=1.2):
    print('发命令：%s' % cmd, flush=True)
    ser.write((cmd + '\r\n').encode('utf-8'))
    drain(ser, settle)


def shot(ser, out):
    ser.reset_input_buffer()
    print('发 lq_shot …', flush=True)
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
        return False

    p = hdr.split()
    w, h, nb = int(p[1]), int(p[2]), int(p[3])
    want = int(p[4], 16)
    print('头：%s' % hdr)

    data = bytearray(payload)
    t0 = time.time()
    while len(data) < nb and time.time() - t0 < 40:
        d = ser.read(65536)
        if d:
            data += d
    if len(data) < nb:
        print('❌ 只收到 %d/%d 字节' % (len(data), nb))
        return False
    data = bytes(data[:nb])

    got = zlib.crc32(data) & 0xFFFFFFFF
    ok = (got == want)
    print('CRC %08X / %08X  %s' % (got, want, '✅ 一致' if ok else '❌ 不一致（混进日志了）'))

    a = np.frombuffer(data, dtype='<u2').reshape(h, w)
    r = (((a >> 11) & 0x1F).astype(np.uint16) * 255 // 31)
    g = (((a >> 5) & 0x3F).astype(np.uint16) * 255 // 63)
    b = ((a & 0x1F).astype(np.uint16) * 255 // 31)
    img = np.dstack([r, g, b]).astype(np.uint8)
    Image.fromarray(img).save(out)
    print('已保存 %s  %s' % (out, img.shape), flush=True)
    return ok


def main():
    global ECHO
    argv = [a for a in sys.argv[1:] if a != '--log']
    ECHO = ('--log' in sys.argv)

    if len(argv) < 1:
        print(__doc__)
        return 1
    out = argv[0]
    wait = float(argv[1]) if len(argv) > 1 else 3.0
    cmds = argv[2:]

    # ★★ 必须先设 dtr/rts 再 open：ESP32-S3 的 USB-Serial-JTAG 认 DTR/RTS
    #   组合做复位，pyserial 默认电平会把板子重启 —— 那样"发完命令还没截"
    #   就已经被复位掉了，表现是"截图总是封面页 / 命令像没生效"。
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 0.3
    ser.dtr = False
    ser.rts = False
    ser.open()

    print('已打开 %s —— 静默 %.1f 秒…' % (PORT, wait), flush=True)
    drain(ser, wait)
    for attempt in range(6):
        drain(ser, 1.5)
        pending = ser.in_waiting
        print('  静默确认 %d/6：残留 %d 字节' % (attempt + 1, pending), flush=True)
        if pending == 0:
            break
        time.sleep(0.8)
    ser.reset_input_buffer()

    for c in cmds:
        # ★ "shot:<文件>" 不是设备命令，是【本地动作】：就地截一张。
        #   让"随时间变化"的东西（频谱柱、载入动画）能在一次会话里连拍。
        if c.startswith('shot:'):
            shot(ser, c.split(':', 1)[1])
            continue
        # ★ "sleep:N" 不是设备命令，是【本地等待】。
        #   为什么需要：send_cmd 之后只 drain 1.2 秒就截图，而天气/股票
        #   两个 App 进屏后要等 HTTP 取数回来（1~3 秒，冷启动更久），
        #   1.2 秒截到的是"载入中…"。用 sleep:N 把等待补足。
        #   截图前 shot() 会 reset_input_buffer，所以等待期间刷出来的
        #   日志不会污染这一帧的二进制（这点和 _lq_shot.py 同源）。
        if c.startswith('sleep:'):
            sec = float(c.split(':', 1)[1])
            print('等待 %.1f 秒（等 App 把网络数据取回来）…' % sec, flush=True)
            drain(ser, sec)
            continue
        send_cmd(ser, c)

    ok = shot(ser, out)
    ser.close()
    return 0 if ok else 2


if __name__ == '__main__':
    sys.exit(main())
