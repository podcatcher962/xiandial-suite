#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
★ 把文件推进 SD 卡（10-06）—— 不拔卡、不用读卡器。

背景 / 为什么需要这个脚本
------------------------
板载 TF 走 SDIO，Windows 看不到这张卡；板载 Type-C 的 PHY 被 Serial-JTAG
占着，板子也不能当 USB 主机给电脑用。⇒ 改卡上文件（stations.tsv /
wifi.txt）以前只能拔卡插读卡器，改一次拔一次。

固件 10-06 加了 st_put 写通道后，这一步在电脑上就能做完：
    python _sd_push.py stations.tsv
    python _sd_push.py wifi.txt
脚本自己开串口、发命令、收回读校验的日志，不用手敲。

协议（固件侧 app_stlist.c 的 st_serial_task）
-------------------------------------------
    st_put <文件名>      进入写入模式
    <一行内容>          原样写入（固件自动补 LF）
    .                   单独一行，结束；固件改名 .tmp→正式名并自动 st_cat 回读

★ 为什么用「一行一条」而不是发裸字节：串口是字符设备，裸字节要自己处理
   转义/半包/粘包，出了问题肉眼看不出来。慢一点但每次都能看见。
   13139 字节的台单 = 144 行，115200 波特下约十几秒。

★ 为什么不校验内容、只信「写成功」：固件那边会 rename + st_cat 回读，
   真正的判据是回读出来的行数与字节数。脚本只做二次核对。
"""
import os
import sys
import time

try:
    import serial          # pyserial
except ImportError:
    print("缺 pyserial。装一下：pip install pyserial")
    sys.exit(1)

BAUD = 115200
PORT = "COM3"
TIMEOUT = 0.4

# ★★ 每行之间的间隔（秒）。这个数字是【必须】的，不是保险起见。
#
#   踩过的坑：10-06 第一次推送时 13139 字节 0.0 秒全灌进去，结果卡上
#   只剩一个 0 字节的 stations.tsv.tmp，结束符「.」也没收到 —— 看起来
#   像固件的写通道坏了。真实原因：USB 串口接收缓冲只有 256 字节
#   （控制台装驱动时用的 USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT），
#   固件是「每次读 1 字节」的循环，电脑发太快就把缓冲区冲爆、后面的
#   字节全丢。⇒ 「推完了」不等于「机器收到了」。
#
#   判据：改成每行等 2.5 秒做诊断，两行一字不差 —— 通道本身没问题。
#   8ms 已经很宽松（固件读一行只要几十微秒），144 行约 1.2 秒。
LINE_DELAY = 0.008


def drain(ser, seconds=0.6):
    """把串口里积着的日志读出来打屏（固件日志很吵，只取尾部关心的行）。"""
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        try:
            chunk = ser.read(4096)
        except Exception:
            break
        if not chunk:
            time.sleep(0.02)
            continue
        buf += chunk
    return buf.decode("utf-8", "replace")


def find_marker(ser, marker, wait=25.0):
    """轮询直到日志里出现 marker，返回 True/False + 累积日志。"""
    end = time.time() + wait
    acc = ""
    while time.time() < end:
        acc += drain(ser, 0.4)
        if marker in acc:
            return True, acc
    return False, acc


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not args:
        print(__doc__)
        print("用法：python _sd_push.py <本地文件> [串口号]")
        print("例：  python _sd_push.py stations.tsv")
        print("      python _sd_push.py wifi.txt")
        sys.exit(1)

    src = args[0]
    port = args[1] if len(args) > 1 else PORT
    # --as <卡上文件名>：本地文件名叫什么不重要，卡上必须是契约规定的名字。
    #   ★ 踩过：本地 _test_stations.tsv 直接推上去，卡里就多了个
    #   _test_stations.tsv，而固件只读 /sdcard/stations.tsv ⇒ 推成功了
    #   但机器根本不读，日志还照样回落内置 1254 台，极易误判成「写通道没用」。
    name = None
    if "--as" in sys.argv:
        i = sys.argv.index("--as")
        if i + 1 < len(sys.argv):
            name = sys.argv[i + 1]
    if name is None:
        name = "stations.tsv" if src.lower().endswith(".tsv") else os.path.basename(src)

    if not os.path.isfile(src):
        print("本地文件不存在：%s" % src)
        sys.exit(1)

    raw = open(src, "rb").read()
    # ★ 契约要求 UTF-8 无 BOM、LF。台单带 BOM 会让第一个台名多一个方块
    #   （收藏按名匹配永远存不上），CRLF 会把 URL 尾部挂个 \r，
    #   症状是「别的台都好，就这几台不行」，极难查。
    if raw[:3] == b"\xef\xbb\xbf":
        raw = raw[3:]
        print("★ 已剥掉 UTF-8 BOM")
    raw = raw.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    text = raw.decode("utf-8")
    lines = [l for l in text.split("\n")]
    if lines and lines[-1] == "":
        lines.pop()                      # 结尾空段不算一行

    print("本地文件：%s" % src)
    print("  %d 字节 / %d 行" % (len(raw), len(lines)))
    print("  卡上文件名：%s %s" % (name, "（契约规定名）" if name == "stations.tsv" else ""))
    if name != "stations.tsv" and not name.lower().endswith(".txt"):
        print("  ⚠ 卡上叫 %s，固件只读 stations.tsv —— 这文件机器不会用" % name)

    try:
        ser = serial.Serial(port, BAUD, timeout=TIMEOUT)
    except Exception as e:
        print("打不开串口 %s：%s" % (port, e))
        # ⚠ 原来这里让用户去跑 _ports.py —— 那个脚本【不在发布包里】，
        #   是条死指引（我自己也被它坑过一次：找遍 tools/ 也没有）。
        #   ⇒ 换成一步就能跑的命令，pyserial 自带的 list_ports 就够。
        print("（板子插好了吗？端口对吗？下面这条命令会列出本机所有串口：）")
        print("    python -c \"import serial.tools.list_ports as p;"
              "[print(x.device, x.description) for x in p.comports()]\"")
        print("  查到口之后，用第二个参数指定：python _sd_push.py stations.tsv COM5")
        sys.exit(1)

    try:
        print("\n[1/4] 握手，等固件起来……")
        ser.reset_input_buffer()
        # 敲个空命令把日志刷出来，顺便确认通道活着
        ser.write(b"\r\n")
        time.sleep(0.5)
        drain(ser, 2.0)

        print("[2/4] st_ls —— 看卡上现在有什么")
        ser.write(b"st_ls\r\n")
        ok, out = find_marker(ser, "项", wait=10)
        for l in out.splitlines():
            if "st_ls" in l or "B " in l:
                print("   ", l.split("] ", 1)[-1] if "] " in l else l)
        if not ok:
            print("    ⚠ 没等到 st_ls 的回显 —— 固件可能还没到这一步，或串口不对")

        print("[3/4] st_put %s —— 推 %d 行" % (name, len(lines)))
        # 先清掉上一轮失败留下的 .tmp（写通道是 .tmp + 改名收尾，
        # 中途断了就会留下一个残缺的 .tmp 躺在卡上）
        ser.write(("st_rm %s.tmp\r\n" % name).encode())
        ser.flush()
        time.sleep(0.6)
        drain(ser, 1.0)

        ser.write(("st_put %s\r\n" % name).encode())
        ser.flush()
        ok, out = find_marker(ser, "st_put: 开始写", wait=10)
        if not ok:
            print("    ✗ 没进写入模式，先别发内容。日志尾巴：")
            print("    " + out[-500:].replace("\n", "\n    "))
            sys.exit(1)

        # 一行一条，带进度。
        # ★ 必须给固件留出读取时间：RX 缓冲只有 256 字节，
        #   一次性灌 13 KB 会把缓冲冲爆、数据全丢（10-06 踩过）。
        t0 = time.time()
        for i, l in enumerate(lines):
            ser.write(l.encode("utf-8") + b"\r\n")
            ser.flush()
            if LINE_DELAY:
                time.sleep(LINE_DELAY)
            if (i + 1) % 25 == 0 or i + 1 == len(lines):
                pct = 100.0 * (i + 1) / max(1, len(lines))
                sys.stdout.write("\r      %d/%d 行（%.0f%%）" % (i + 1, len(lines), pct))
                sys.stdout.flush()
        print("\n      发完，用时 %.1f 秒" % (time.time() - t0))

        print("[4/4] 结束符 . —— 让固件改名并回读校验")
        ser.write(b".\r\n")
        ok, out = find_marker(ser, "st_cat:", wait=25)
        print()
        for l in out.splitlines():
            if "st_put" in l or "st_cat" in l or "XSTLIST" in l:
                print("   ", l.split("] ", 1)[-1] if "] " in l else l)

        if not ok:
            print("\n⚠ 没等到回读校验的日志。板子可能被拔了或复位了。")
            sys.exit(1)
        if "改名失败" in out:
            print("\n✗ 改名失败，卡上可能没这个文件")
            sys.exit(1)

        # 二次核对：固件报的行数应等于本地行数
        want = len(lines)
        got = None
        for l in out.splitlines():
            if "st_cat:" in l and "行" in l:
                for tok in l.split():
                    if tok.isdigit():
                        got = int(tok)
                        break
        print("\n=== 结果 ===")
        if got is None:
            print("未取到行数，请人工看上面的回读内容")
        elif got == want:
            print("★ 行数一致：卡上 %d 行 = 本地 %d 行" % (got, want))
        else:
            print("✗ 行数不一致：卡上 %d 行，本地 %d 行 —— 别当成成功" % (got, want))
            sys.exit(1)

        # 写的是台单就顺手重载一次，把「机器真的用上了这张表」也一并验掉。
        # ★ 不做这一步就只验了「文件在卡上」，没验「机器读它」。
        #   v1.45 那次就是卡上没文件、日志回落内置，台单功能等于没测过。
        if name == "stations.tsv":
            print("\n[5/5] st_reload —— 让机器重读并重建界面")
            ser.write(b"st_reload\r\n")
            ok2, out2 = find_marker(ser, "重读完成", wait=25)
            for l in out2.splitlines():
                if any(k in l for k in ("XSTLIST", "XSUI", "重读完成", "收到")):
                    print("   ", l.split("] ", 1)[-1] if "] " in l else l)
            if ok2 and "来源=TSV" in out2:
                print("   ★ 来源=TSV —— 机器已经在用卡上这张台单了")
            elif ok2:
                print("   ⚠ 重读了，但来源不是 TSV —— 看上面几行，机器还在用内置台单")
            else:
                print("   ⚠ 没等到重读日志")

        print("\n接着可以：")
        if name != "stations.tsv":
            print("  改完台单记得 st_reload；想看台单 st_dump")
        print("  想确认卡上文件 st_ls；想核对内容 st_cat stations.tsv")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
