# 烧录指引 · Flashing Guide

> 中文版固件仓库 → [README.md](README.md)

## 硬件要求

| 项 | 规格 |
|---|---|
| 主控 | **ESP32-S3** |
| 屏幕 | 3.5 英寸 IPS TFT，480×320，ST77922 |
| 触摸 | FT5x06 电容触摸（I2C） |
| 内存 | 8 MB PSRAM |
| Flash | 16 MB |
| 调试口 | 板载 USB（Serial-JTAG） |

**必须用 USB 数据线**（有些线只能充电）。找不到串口就换线试 —— 这是最常见的原因。

---

## ⚠️ 先看这一段，否则可能刷坏

ESP32-S3 的 flash 里要放**三段**东西，各有固定地址：

| 地址 | 内容 | 文件 |
|---|---|---|
| `0x0` | bootloader | `bootloader.bin` |
| `0x8000` | partition table | `partition-table.bin` |
| `0x10000` | application | `xiandial-radio.bin` |

**把 `xiandial-radio.bin` 单独刷到 `0x0` 是错的** —— 那是把应用程序写进引导程序区，板子会起不来（串口无日志、屏幕全黑）。

如果不想记地址，**用合并镜像 `XianDial-v1.51-merged.bin`**，它已经把三段拼好，烧录地址 `0x0`，一把刷完。

---

## 方式 A：网页烧录（不用装任何工具）

1. 打开 <https://espressif.github.io/esptool-js/>（Chrome / Edge）
2. 点 `Connect`，选你的串口
3. 文件选 **`XianDial-v1.51-merged.bin`**
4. 芯片型号 **ESP32-S3**，地址 **`0x0`**
5. `Start`，等它跑完

> 需要较新的 Chrome / Edge（要支持 Web Serial）。

---

## 方式 B：命令行

```bash
pip install esptool

# 找串口号
#   Windows : COM3
#   macOS   : /dev/cusbmodem*  或 /dev/tty.usbserial-*
#   Linux   : /dev/ttyACM0
esptool.py --list-ports
```

把下面的 `COM3` 换成你的串口号。

### B1 · 合并镜像（推荐）

```bash
# 换版本时建议先擦除（会清掉 WiFi 配置和收藏）
esptool.py --chip esp32s3 --port COM3 erase_flash

esptool.py --chip esp32s3 --port COM3 --baud 460800 \
    write_flash 0x0 XianDial-v1.51-merged.bin
```

### B2 · 三个分段文件

```bash
esptool.py --chip esp32s3 --port COM3 --baud 460800 write_flash \
    0x0     bootloader.bin \
    0x8000  partition-table.bin \
    0x10000 xiandial-radio.bin
```

### 看日志

```bash
esptool.py --chip esp32s3 --port COM3 monitor
```

正常启动会打印：版本号、台单数量、WiFi 状态。**「台单 0 条」是预期的** —— 这一版固件不内置任何电台地址，等你自己导入。

退出 monitor：`Ctrl+]`。

---

## 刷完第一次开机

1. 屏幕出现提示后，用手机连它广播的 WiFi 热点（名字形如 `XianDial-XXXX`）
2. 浏览器打开 `http://192.168.4.1`，填你家 WiFi 的名称和密码，提交
3. 机器重启，之后就正常用了

**没配网也能继续** —— 机器会停在「0 台」等你导入台源，不会白屏。

---

## 导入台源

**① 生成台单** —— 打开 `XianForge.html`（双击即用，不联网、不上传），
把你的 M3U / TXT 拖进去，导出 `stations.tsv`。

**② 放进卡里** —— 把 `stations.tsv` 复制到 TF 卡根目录。

**③ 插卡开机** —— 机器自动读取。

卡读不出、文件格式不对都不会白屏，只会停在「0 台」并提示导入。

### 没有读卡器怎么办

板载 Type-C 的 USB PHY 被 Serial-JTAG 占着，**板子不能当 USB 主机**（插 U 盘/读卡器无效），
所以在电脑上读不出这张卡。走串口推卡：

```bash
pip install pyserial

python tools/_sd_push.py stations.tsv     # 推台单进卡
python tools/_sd_cmd.py   st_reload       # 重读台单并重建界面，不重启
python tools/_sd_cmd.py   st_ls           # 列卡上文件
```

固件支持的串口命令：
`st_dump` `st_reload` `st_ls` `st_cat <file>` `st_rm <file>` `st_put <file>` `st_play <index>` `st_net <url>`

> `st_put` 是逐行下发（单行以 `.` 结束），适合小台单；大文件请用 `st_reload` 让机器直接读卡。

---

## 台单文件格式

路径固定为 **`/sdcard/stations.tsv`**（TF 卡根目录）：

```
台名<TAB>栏目<TAB>地区<TAB>URL
```

| 项 | 要求 |
|---|---|
| 编码 | UTF-8 **无 BOM** |
| 换行 | LF（**不要** CRLF） |
| 分隔符 | **TAB**（不是空格） |
| 行数上限 | 2000 |
| 空行 / `#` 开头 | 忽略，可当注释 |

> **BOM 和 CRLF 是最常见的坑**：带 BOM ⇒ 第一个台名多一个方块；CRLF ⇒ URL 尾部挂个 `\r`，
> 症状是「别的台都好，就这几台不行」，极难查。用 `XianForge.html` 导出就不会有这两个问题。

---

## 常见问题

| 现象 | 原因 |
|---|---|
| 串口列表里找不到板子 | 换一根**数据线**；或按住 BOOT 键再插线 |
| 烧录报 `Failed to connect` | 同上；或手动进下载模式：按住 `BOOT` → 点 `EN/RESET` → 松开 `BOOT` |
| 刷完屏幕全黑、串口无日志 | ★ 地址刷错了。用合并镜像，或检查 B2 的三个地址 |
| 一直重启 | 烧录不完整，重新 `erase_flash` 再刷 |
| 卡读不出 / 0 台 | 用了 CRLF 或带 BOM；或文件没放卡根目录；或超 2000 行 |
| 某些台播不了 | 源本身的问题（见 README「能播多少」），不是固件 bug |

---

## 从源码编译

需要 [ESP-IDF 6.0 以上](https://docs.espressif.io/projects/esp-idf/zh_CN/latest/esp32s3/get-started/index.html)（本工程编于 6.1）：

```bash
git clone <this repo> && cd xiandial-radio
idf.py set-target esp32s3
idf.py -p COM3 build flash monitor
```

改完源码想重新出 Release 镜像：

```bash
idf.py -B build_pub -D XS_FAV_PUBLISH=1 build
esptool --chip esp32s3 merge-bin --format raw -o XianDial-v1.51-merged.bin \
    --flash-mode dio --flash-size 16MB --flash-freq 80m \
    0x0 build_pub/bootloader/bootloader.bin \
    0x8000 build_pub/partition_table/partition-table.bin \
    0x10000 build_pub/xiandial-radio.bin
```

---

© 永远的兰兰 · Lanlan Eternal