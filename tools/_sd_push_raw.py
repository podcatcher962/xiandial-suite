#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
★ 把【二进制文件】推进 SD 卡（10-07）—— 不拔卡、不走网络、不用读卡器。

用法：
    python _sd_push_raw.py lq001.mp3
    python _sd_push_raw.py lq001.mp3 --port COM5
    python _sd_push_raw.py --all E:\\workbuddy\\_lq_voice        # 整批推 100 个
    python _sd_push_raw.py --all <目录> --chunk 8192            # 调块大小

固件侧协议（main/app_stlist.c 的 st_putraw）：
    st_putraw <卡上文件名> <字节数> <CRC32>
    ← 设备回一行  STRAW_READY <名字> <字节数>     （看到它才开始灌）
    → 灌一块（默认 4 KB）
    ← 设备回一个字节 0x06                          （这一块已落盘）
    → 下一块 …… 直到灌完
    ← 设备回一行 "st_putraw: 完成 …" 或 "… 校验失败 …"

★★ 为什么不能用现成的 _sd_push.py：
   它是【按行】协议（每行 fputs + 自动补 LF）。mp3 里的 0x0A/0x0D
   会被当成行结束，数据必坏；包 base64 又要多传 33%（16 MB→21 MB）。

★★ 为什么一定要 ACK（第一条 mp3 就被它救了两次）：
   ① 没有 ACK，PC 一发到底，设备写卡那几毫秒里环（只有 256 B）就爆了
      —— 实测 158 KB 的文件少了 1024 字节，而【文件照样建出来了】。
   ② 固件侧还有一条更阴的：命令在读到第一个 \r 时派发，剩下的 \n
      会变成文件的第一个字节 —— 收到的字节数一个不差，只有 CRC 能发现。
   两条都是「大小/存在性检查会判它成功、只有内容校验能抓到」的坑。
   所以本脚本【只认 CRC】，并且失败会自动换更小的块重试。

★★ 为什么不用 WiFi：
   板子在 TP_Guest 访客网络，与有线内网互相不可达（实测：电脑 ping
   不到板子、板子 st_net 的 TCP 那步恒挂、ARP 表里是 Unreachable）。
   这条路是死的，别再试。
"""
import os
import sys
import time
import zlib

try:
    import serial
except ImportError:
    print("缺 pyserial。请用装有 pyserial 的 python 跑本脚本，例如：")
    print("  python -m pip install pyserial   # 然后重跑")
    print("若使用 ESP-IDF，也可直接用它的 python 环境（路径见 idf.py 所在环境）。")
    sys.exit(1)

DEFAULT_PORT = "COM3"
BOOT_WAIT = 7.0        # 打开串口会让板子复位，等它起来
READY_WAIT = 20.0
ACK_WAIT = 8.0         # 单块落盘最长等多久（正常 5~26ms，给 8 秒已极宽松）
DONE_WAIT = 60.0


def wait_ack(ser):
    """等设备回一个 ACK(0x06)。返回 (ok, 收集到的文本)。"""
    log = ""
    deadline = time.time() + ACK_WAIT
    while time.time() < deadline:
        try:
            b = ser.read(1)
        except Exception:
            return False, log
        if not b:
            continue
        if b == b"\x06":
            return True, log
        log += b.decode("utf-8", "replace")
        if "st_putraw: " in log:          # 设备已经宣判了，别再等
            return False, log
    return False, log


def drain_until(ser, marker, timeout):
    end = time.time() + timeout
    acc = ""
    while time.time() < end:
        try:
            chunk = ser.read(4096)
        except Exception:
            break
        if not chunk:
            time.sleep(0.05)
            continue
        acc += chunk.decode("utf-8", "replace")
        if marker in acc:
            return True, acc
    return False, acc


def push_one(ser, src, name, chunk):
    """推一个文件。返回 (成功?, 吞吐 KB/s, 失败说明)"""
    data = open(src, "rb").read()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    # ★ 只发 '\n'，【不要】"\r\n"：固件在第一个 \r 就派发命令，
    #   剩下的 \n 会变成文件的第一个字节（大小还对得上，只有 CRC 能发现）。
    cmd = "st_putraw %s %d %08x\n" % (name, len(data), crc)

    ser.reset_input_buffer()
    ser.write(cmd.encode())
    ser.flush()
    ok, out = drain_until(ser, "STRAW_READY", READY_WAIT)
    if not ok:
        return False, 0.0, "没等到 STRAW_READY（固件没这条命令？板子没起来？）\n" + out[-300:]

    t0 = time.time()
    sent = 0
    mv = memoryview(data)
    while sent < len(data):
        n = min(chunk, len(data) - sent)
        ser.write(mv[sent:sent + n])
        ser.flush()
        sent += n
        ok, log = wait_ack(ser)
        if not ok:
            return False, 0.0, "第 %d 字节处等 ACK 失败：\n%s" % (sent, log[-300:])
    dt = time.time() - t0

    ok, out = drain_until(ser, "st_putraw: ", DONE_WAIT)
    kbps = (len(data) / 1024.0) / max(dt, 1e-6)
    if not ok:
        return False, kbps, "没等到结果行\n" + out[-300:]
    if "完成" in out and "校验失败" not in out:
        return True, kbps, ""
    lines = [l for l in out.splitlines() if "st_putraw" in l]
    return False, kbps, "\n".join(lines[-3:])


def main():
    argv = sys.argv[1:]
    port = DEFAULT_PORT
    chunk = 4096
    name = None
    all_dir = None
    srcs = []

    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--port":
            port = argv[i + 1]; i += 2
        elif a == "--chunk":
            chunk = int(argv[i + 1]); i += 2
        elif a == "--as":
            name = argv[i + 1]; i += 2
        elif a == "--all":
            all_dir = argv[i + 1]; i += 2
        else:
            srcs.append(a); i += 1

    if all_dir:
        files = sorted(f for f in os.listdir(all_dir)
                       if f.lower().endswith((".mp3", ".m4a", ".wav", ".bin")))
        srcs = [os.path.join(all_dir, f) for f in files]
        print("批量模式：%s 下 %d 个文件" % (all_dir, len(srcs)))

    if not srcs:
        print(__doc__)
        return 2

    ser = serial.Serial(port, 115200, timeout=0.02)
    try:
        ser.setDTR(False)
        ser.setRTS(False)
    except Exception:
        pass
    try:
        ser.reset_input_buffer()
    except Exception:
        pass
    print("---- 打开 %s 会让板子复位，等 %.1f 秒启动 ----" % (port, BOOT_WAIT))
    time.sleep(BOOT_WAIT)
    try:
        ser.reset_input_buffer()
    except Exception:
        pass

    print("---- 卡上现有（st_ls）----")
    ser.write(b"st_ls\n")
    ok, out = drain_until(ser, "st_ls: 共", 8)
    for line in out.splitlines():
        if "st_ls" in line or ".mp3" in line or "DIR" in line:
            print("   ", line.split("] ", 1)[-1] if "] " in line else line)

    # 失败就换更小的块重试 —— 块越小，设备一次要吞的字节越少，
    # 越不容易在写卡的间隙里被填满 RX 环。
    ladder = [chunk] if "--chunk" in sys.argv else [chunk, 2048, 512]
    n_ok = 0
    t_all = time.time()
    for k, src in enumerate(srcs):
        nm = name if (name and len(srcs) == 1) else os.path.basename(src)
        sz = os.path.getsize(src)
        done = False
        for attempt, ck in enumerate(ladder):
            sys.stdout.write("\r[%d/%d] %-12s %6.1f KB  %d 字节/块 …"
                             % (k + 1, len(srcs), nm, sz / 1024.0, ck))
            sys.stdout.flush()
            good, kbps, why = push_one(ser, src, nm, ck)
            if good:
                print("\r[%d/%d] %-12s %6.1f KB  ✓ %6.1f KB/s (%d 字节/块)"
                      % (k + 1, len(srcs), nm, sz / 1024.0, kbps, ck))
                n_ok += 1
                done = True
                break
            # ★ 每档失败都当场打出来。第一版只在"全档失败"时才打，
            #   结果三档挨个失败、屏幕上只有进度在爬，看不到任何原因
            #   —— 把「等 20 秒超时」误当成「传得慢」，白等三分钟。
            print("\r[%d/%d] %-12s %6.1f KB  ✗ %d 字节/块：%s"
                  % (k + 1, len(srcs), nm, sz / 1024.0, ck,
                     why.splitlines()[0] if why else "?"))
            if why:
                print("      " + why.replace("\n", "\n      "))
        if not done:
            print("\r[%d/%d] %-12s  ✗ 全部档位失败" % (k + 1, len(srcs), nm))
            print("    " + why.replace("\n", "\n    "))
            break
    ser.close()

    print("\n=== 结果 ===  成功 %d/%d，用时 %.1f 秒（%.1f KB/s 平均）"
          % (n_ok, len(srcs), time.time() - t_all,
             sum(os.path.getsize(s) for s in srcs[:n_ok]) / 1024.0 / max(time.time() - t_all, 1e-6)))
    if n_ok == len(srcs):
        print("★ 全部 CRC 校验通过 —— 卡上的字节与本地逐位一致")
    return 0 if n_ok == len(srcs) else 1


if __name__ == "__main__":
    sys.exit(main())
