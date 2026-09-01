# STM32F103 OTA 升级项目

基于 **STM32F103C8**（64KB Flash / 20KB RAM）+ **W25Q64**（8MB SPI Flash）+ **ESP32**（联网下载固件）的完整 OTA 升级方案。

```
云端/HTTP 服务器
     │  HTTP GET 下载 .bin + version.txt
     ▼
  ESP32 (WiFi)
     │  USART2 分包发送 (256B/包)
     ▼
  STM32 APP ──写 W25Q64 下载区──校验 CRC32──备份旧固件──写 OTA 标志──软复位
     ▼
  Bootloader ──校验固件头──擦写 APP 区──校验──跳转新 APP (失败则回退)
```

---

## 1. 目录结构

| 路径 | 说明 |
|------|------|
| `OTA_bootloader/` | STM32 Bootloader 工程 (Keil)，引导 + 升级/回退 |
| `OTA_app/` | STM32 应用工程 (Keil)，业务 + 接收固件 |
| `ota_server.py` | PC 上位机（模拟 ESP32），直接连 STM32 的 USART2 测试 |
| `esp32_ota/` | （可选）ESP32 固件，实际工程在 PlatformIO 目录下 |

> ESP32 固件的实际 PlatformIO 工程位于 `c:/Users/zzz/Documents/PlatformIO/Projects/OTA/`，源码入口 `src/main.cpp`。

---

## 2. 硬件组成

| 模块 | 型号/说明 |
|------|-----------|
| 主控 | STM32F103C8（Cortex-M3，72MHz） |
| 外部 Flash | W25Q64（8MB，SPI） |
| 联网 | ESP32（WiFi，下载固件） |
| 下载 UART | STM32 USART2 ↔ ESP32 UART2 |
| 日志 UART | STM32 USART1 ↔ USB-TTL ↔ PC 串口助手 |

---

## 3. 内部 Flash 布局（64KB）

| 区域 | 地址范围 | 大小 | 说明 |
|------|----------|------|------|
| Bootloader | `0x08000000` ~ `0x080043FF` | 17KB | 引导 + 升级 |
| APP | `0x08004400` ~ `0x0800FBFF` | 约 46KB | 业务应用 |
| 参数区 | `0x0800FC00` ~ `0x0800FFFF` | 1KB | OTA 标志 / 固件信息 |

> 所有地址在 `OTA_app/User/ota_config.h` 中定义，两工程共用，可整体调整。

## 4. W25Q64 外部 Flash 布局（8MB）

| 区域 | 地址范围 | 大小 | 说明 |
|------|----------|------|------|
| 下载区 | `0x000000` ~ `0x07FFFF` | 512KB | 暂存新固件 |
| 备份区 | `0x080000` ~ `0x0FFFFF` | 512KB | 备份旧固件（回退用） |

---

## 5. 串口分配

| 串口 | 引脚 | 用途 | 方向 |
|------|------|------|------|
| USART1 | PA9 (TX) / PA10 (RX) | 日志打印 | 仅 TX，接 USB-TTL |
| USART2 | PA2 (TX) / PA3 (RX) | OTA 固件下载 | RX + TX，接 ESP32 |

> 日志和下载分离，互不干扰：OTA 下载占用 USART2 时，日志照常从 USART1 输出。

---

## 6. OTA 流程

```
云端/APP 触发 OTA
   → ESP32 连 WiFi，HTTP GET 下载固件 + version.txt
   → ESP32 分包(每包256B) 通过 USART2 发给 STM32
   → APP 逐包写入 W25Q64 下载区
   → 全部收完, APP 校验 CRC32
       校验失败 → 通知 ESP32 重传
       校验成功 → 备份当前 APP 到备份区
   → 写 OTA 标志到内部 Flash 参数区
   → 软复位进入 Bootloader
   → Bootloader 校验下载区 CRC + 校验固件头(SP/复位向量)
   → 擦除 APP 区 → 逐页写入 → CRC 校验
       校验失败 → 从备份区恢复旧固件
       校验成功 → 清除标志 → 跳转 APP 运行
```

---

## 7. UART 传输协议（USART2，115200 8N1）

### 帧格式（ESP32 → STM32）

| 字段 | 长度 | 说明 |
|------|------|------|
| 头 | 2 | `0xAA 0x55` |
| cmd | 1 | `0x01 START` / `0x02 DATA` / `0x03 END` / `0x04 ABORT` |
| seq | 2 | 大端；START=总包数，DATA=包序号 |
| len | 2 | 大端；payload 长度（≤256） |
| payload | len | 数据 |
| crc16 | 2 | 大端；对 cmd..payload 的 CRC16(Modbus) |

- **START** payload（12 字节）：`size(4) crc32(4) ver_major(2) ver_minor(2)`，均为大端
- **DATA** payload：固件分片（256 字节）
- **END** payload：空

### 应答格式（STM32 → ESP32）

`[0xAA][0x55][code][seq_H][seq_L]`

| code | 含义 |
|------|------|
| `0x06` | ACK（DATA 接收成功） |
| `0x15` | NAK（序号不符，需重传） |
| `0x02` | START 成功 |
| `0x03` | START 失败（尺寸非法） |
| `0x04` | 校验通过，即将软复位升级 |
| `0x05` | 校验失败，可重传 |

---

## 8. STM32 编译与烧写

### 8.1 链接地址（Keil `Options → Target → Read/Only Memory Areas`）

- **Bootloader**：`IROM1` 起始 `0x08000000` 大小 `0x4400`
- **APP**：`IROM1` 起始 `0x08004400` 大小 `0xB800`

### 8.2 编译

1. 打开 `OTA_bootloader/Project.uvprojx` → Rebuild → 得到 `Objects/Project.hex`
2. 打开 `OTA_app/Project.uvprojx` → Rebuild → 得到 `Objects/Project.hex`

### 8.3 生成 bin（ESP32 下载用）

Keil `Options → User → After Build/Rebuild` 添加命令（**注意不要带反引号**）：

```
fromelf --bin -o "$L@L.bin" "#L"
```

编译后得到 `Objects/Project.bin`（裸 bin，喂给 ESP32/HTTP 服务器）。

### 8.4 烧写（ST-LINK Utility）

1. 烧 Bootloader：`File → Open File` 选 bootloader 的 hex → `Target → Program & Verify`
2. 烧 APP：`File → Open File` 选 app 的 hex → `Target → Program & Verify`

> hex 自带地址，直接烧即可；先 boot 后 app。

---

## 9. ESP32 固件（PlatformIO / Arduino）

工程：`c:/Users/zzz/Documents/PlatformIO/Projects/OTA/`（board = esp32dev，framework = arduino）。

### 9.1 配置（`src/main.cpp` 顶部）

```cpp
const char* WIFI_SSID     = "你的WiFi";
const char* WIFI_PASS     = "你的密码";
const char* FIRMWARE_URL  = "http://<电脑IP>:8080/Project.bin";
const char* VERSION_URL   = "http://<电脑IP>:8080/version.txt";
```

### 9.2 版本号管理（不用改 ESP32 代码）

版本号写在服务器的 `version.txt` 里，ESP32 每次上电下载它。**发新固件时只需更新服务器上的两个文件**：

1. `Project.bin`（新固件）
2. `version.txt`（内容如 `2.3`，格式 `主版本.次版本`）

ESP32 固件永远不用重烧。

---

## 10. 固件服务器（HTTP）

在 bin 所在目录起服务器（**端口 8000 常被 C-Lodop 占用，用 8080**）：

```powershell
cd g:\stm32_project\OTA_103\OTA_app\Objects
python -m http.server 8080
```

浏览器验证：`http://<电脑IP>:8080/` 应列出文件。

> 电脑 IP 用 `ipconfig` 查（WLAN 的 IPv4 地址），ESP32 与电脑须在同一局域网。

---

## 11. 接线

### 11.1 STM32 ↔ ESP32（OTA 下载）

```
ESP32 GPIO17 (TX)  ──►  STM32 PA3  (USART2 RX)
ESP32 GPIO16 (RX)  ◄──  STM32 PA2  (USART2 TX)
GND                 ────  GND
```

### 11.2 STM32 ↔ USB-TTL（日志）

```
STM32 PA9  (USART1 TX)  ──►  USB-TTL RX
STM32 GND               ────  USB-TTL GND
```

---

## 12. 版本号对应关系

| 东西 | 管哪里 | 什么时候改 |
|------|--------|-----------|
| 当前 APP 版本 | STM32 `ota_config.h` 的 `OTA_APP_VERSION_*` | 每次编译新 APP 时 |
| 新固件版本 | 服务器 `version.txt` | 每次发新固件时 |
| ESP32 代码 | —— | 永远不用改 |

> **注意**：每次发新固件，STM32 的 `ota_config.h` 和服务器 `version.txt` 要**同步改成同一个版本号**，否则 Bootloader 打印的版本和 APP 启动打印的版本会不一致。

---

## 13. 恢复（变砖时）

若 OTA 后停在 `[BL] No valid app!`，说明 APP 区损坏，需 ST-LINK 物理重烧：

1. Keil 重建 `OTA_bootloader` → ST-LINK 烧到 `0x08000000`
2. Keil 重建 `OTA_app` → ST-LINK 烧到 `0x08004400`

> Bootloader 已在擦写 APP 前校验固件头（SP + 复位向量），喂错文件时会打印 `Invalid firmware header, abort.` 并直接启动旧 APP，不再变砖。

---

## 14. 常见问题

| 现象 | 排查 |
|------|------|
| 烧完 `[BL] No valid app!` | APP 没烧对（地址/文件），重新 ST-LINK 烧 app hex |
| ESP32 `START 失败`，STM32 无 `OTA START` | USART2 RX 线（ESP32 TX→PA3）不通 |
| ESP32 `START 失败`，STM32 有 `OTA START` | USART2 TX 线（PA2→ESP32 RX）不通 |
| 浏览器打不开服务器 | IP 写错 / 端口被占(8000→C-Lodop) / 防火墙 |
| 喂了 `.hex` 当固件 | bin 头应是 `40 0a 00 20`（SP 指向 0x2000xxxx），hex 是 `3a 30 32` |
| Bootloader 超 17KB 溢出 | Keil 优化等级改 `-O2/-O3`，或增大 `OTA_BOOT_SIZE` 并同步改 `OTA_APP_ADDR` |

---

## 15. 测试工具（PC 模拟 ESP32）

没接 ESP32 时，可用 `ota_server.py` 直接在 PC 上模拟发送（把 USB-TTL 接到 STM32 的 USART2）：

```powershell
pip install pyserial
python ota_server.py COM8 OTA_app/Objects/Project.bin 115200
```

> 注意：这里接的是 **USART2**（PA2/PA3），不是 USART1。日志仍从 USART1 的另一块 USB-TTL 看。