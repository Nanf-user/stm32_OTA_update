# Bootloader 引导流程与代码详解

本文详细介绍本项目中 STM32F103 Bootloader 的引导过程、升级流程和关键代码实现。工程目录为 `OTA_bootloader/`，链接地址 `0x08000000`，占用 17KB（`0x4400`）。

---

## 1. Bootloader 的职责

Bootloader 是上电后**最先运行**的程序，固定烧在 `0x08000000`。它只做两件事：

1. **决定引导哪个程序**：有 OTA 升级任务时执行升级，否则直接跳转 APP。
2. **执行升级/回退**：从 W25Q64 读新固件 → 擦写内部 Flash 的 APP 区 → 校验 → 跳转；失败则从备份区恢复旧固件。

它本身**永远不接收 OTA 数据**（接收固件是 APP 的活），也**从不被擦除**（升级只擦 APP 区），所以是系统最后一道防线——只要 Bootloader 还在，就能救回 APP。

---

## 2. 内存布局

```
内部 Flash (64KB)
0x08000000 ┌──────────────┐
           │  Bootloader  │ 17KB   ← 本工程，引导 + 升级
0x08004400 ├──────────────┤
           │  APP         │ ~46KB  ← 业务应用，可被升级
0x0800FC00 ├──────────────┤
           │  参数区      │ 1KB    ← OTA 标志 / 固件信息
0x08010000 └──────────────┘

W25Q64 外部 Flash (8MB)
0x000000 ┌──────────────┐
         │  下载区      │ 512KB  ← 暂存新固件 (掉电不丢)
0x080000 ├──────────────┤
         │  备份区      │ 512KB  ← 备份旧固件 (回退用)
0x100000 └──────────────┘
```

相关地址都在 `User/ota_config.h` 中定义，Bootloader 与 APP 两工程共用同一份。

---

## 3. 上电启动序列（复位向量到 main）

Cortex-M3 上电复位后，硬件自动执行以下步骤：

```
1. 读取 0x08000000 处的值 → 装入 MSP (主堆栈指针)
2. 读取 0x08000004 处的值 → 跳转到 Reset_Handler
```

这两个值由启动文件 `Start/startup_stm32f10x_md.s` 的**中断向量表**提供：

```asm
__Vectors:
    DCD __initial_sp            ; 向量[0] = 初始堆栈指针
    DCD Reset_Handler           ; 向量[1] = 复位处理函数
    DCD NMI_Handler             ; 向量[2] ...
    ...                         ; 其余所有中断向量
```

接着：

```
Reset_Handler
    → SystemInit()     // 配置系统时钟 (HSE 8MHz → PLL 72MHz)、Flash 等待周期、VTOR
    → __main()         // C 运行时初始化：搬运 .data、清零 .bss
    → main()           // 我们的入口
```

**关键点**：Bootloader 运行在 `0x08000000`，向量表本来就在这，所以 `main()` 里**不需要**改 `SCB->VTOR`（而 APP 运行在 `0x08004400`，必须在 `main()` 首行做 `SCB->VTOR = OTA_APP_ADDR;` 重定位，这是两者的区别）。

---

## 4. main.c 逐步解释

```c
int main(void)
{
    LED_Init();        // 点亮状态灯 (PA8)
    Serial_Init();     // USART1 115200，日志输出
    W25Q64_Init();     // 初始化 SPI + 外部 Flash

    OtaLog_Str("[BL] Bootloader v1.0 started");   // 打印启动日志
    OtaLog_CRLF();

    /* 读取并打印 W25Q64 的 JEDEC ID，验证 SPI 通信正常 */
    uint8_t mid; uint16_t did;
    W25Q64_ReadID(&mid, &did);
    OtaLog_Str("[BL] W25Q64 ID: 0x");
    OtaLog_Hex8(mid); OtaLog_Hex16(did);
    OtaLog_CRLF();

    OtaBoot_Run();     // 核心流程，永不返回

    while (1) { }
}
```

逻辑很简单：**初始化外设 → 打印标识 → 进入 `OtaBoot_Run()`**。所有决策都在 `OtaBoot_Run()` 里。

---

## 5. 核心流程 OtaBoot_Run（ota_boot.c）

这是整个 Bootloader 的大脑，流程图如下：

```
读参数区 OtaFlash_ReadParam(&p)
        │
        ├─ magic 不对 或 flag != PENDING ──→ 无升级任务
        │                                        │
        │                                读 APP 向量表[0] (SP)
        │                                        │
        │                                 SP 合法? ──否──→ "No valid app!" 死循环
        │                                        │是
        │                                 "No OTA pending" → 跳转 APP
        │
        └─ 有升级任务 (PENDING)
                 │
                 ├─ 1. VerifyDownload()      下载区 CRC32 校验
                 │       失败 → 清标志 → 跳旧 APP
                 │
                 ├─ 2. VerifyFirmwareHeader()  固件头 (SP+复位向量) 校验
                 │       失败 → 清标志 → 跳旧 APP
                 │
                 ├─ 3. 擦除 APP 区 + 写入新固件
                 │
                 ├─ 4. VerifyAppRegion()      APP 区 CRC32 回读校验
                 │       成功 → 清标志 → 跳新 APP
                 │
                 └─ 失败 → 从备份区恢复旧固件 → 清标志 → 跳旧 APP
```

对应代码（精简）：

```c
void OtaBoot_Run(void)
{
    ota_param_t p;
    OtaFlash_ReadParam(&p);                        // 读参数区

    /* ---- 分支 A：无升级任务 ---- */
    if (p.magic != OTA_PARAM_MAGIC || p.flag != OTA_FLAG_PENDING)
    {
        uint32_t sp = *(volatile uint32_t *)OTA_APP_ADDR;   // 读 APP 的堆栈指针
        if (sp == 0xFFFFFFFFUL || sp < 0x20000000UL || sp > 0x20005000UL)
        {
            OtaLog_Str("[BL] No valid app!");
            while (1) { }                              // APP 无效，停在这里
        }
        OtaLog_Str("[BL] No OTA pending, boot normally.");
        OtaJumpToApp(OTA_APP_ADDR);                    // 跳转 APP
        return;
    }

    /* ---- 分支 B：有升级任务 ---- */
    // 1. 校验下载区 CRC32（先校验后擦，失败则 APP 仍完好）
    if (!VerifyDownload(p.new_size, p.new_crc32))
    {
        OtaFlash_ClearFlag();                          // 清标志
        OtaJumpToApp(OTA_APP_ADDR);                    // 仍启动旧 APP
        return;
    }

    // 2. 校验固件头，防止喂错文件（如把 .hex 当 .bin）
    if (!VerifyFirmwareHeader())
    {
        OtaFlash_ClearFlag();
        OtaJumpToApp(OTA_APP_ADDR);
        return;
    }

    // 3. 擦除 APP 区 + 写入新固件
    OtaFlash_EraseRange(OTA_APP_ADDR, p.new_size);
    WriteAppFromDownload(p.new_size);

    // 4. 回读校验 APP 区
    if (VerifyAppRegion(p.new_size, p.new_crc32))
    {
        OtaFlash_ClearFlag();
        OtaJumpToApp(OTA_APP_ADDR);                    // 升级成功，跳新 APP
        return;
    }

    // 5. 失败：从备份区恢复旧固件
    if (p.backup_size > 0)
    {
        OtaFlash_EraseRange(OTA_APP_ADDR, p.backup_size);
        RestoreAppFromBackup(p.backup_size);
    }
    OtaFlash_ClearFlag();
    OtaJumpToApp(OTA_APP_ADDR);                        // 回退后跳旧 APP
}
```

---

## 6. 子函数详解

### 6.1 VerifyDownload —— 校验下载区 CRC32

```c
static uint8_t VerifyDownload(uint32_t size, uint32_t expect)
{
    uint32_t crc = 0xFFFFFFFFUL, off, n;
    for (off = 0; off < size; off += OTA_PKT_DATA_SIZE)   // 256B 一块
    {
        n = (size - off) > OTA_PKT_DATA_SIZE ? OTA_PKT_DATA_SIZE : (size - off);
        W25Q64_ReadData(OTA_EXT_DL_ADDR + off, s_buf, n); // 从外部 Flash 读
        crc = CRC32_Update(crc, s_buf, n);                 // 增量算 CRC32
    }
    crc ^= 0xFFFFFFFFUL;
    return (crc == expect) ? 1 : 0;
}
```

逐块从 W25Q64 下载区读数据，增量计算 CRC32，与参数区里存的期望值对比。**这一步在任何擦除动作之前**，所以如果下载区数据坏了，APP 区还没被动过，直接启动旧 APP 即可。

### 6.2 VerifyFirmwareHeader —— 固件头校验（防喂错文件）

```c
static uint8_t VerifyFirmwareHeader(void)
{
    uint8_t hdr[8];
    uint32_t sp, pc;
    W25Q64_ReadData(OTA_EXT_DL_ADDR, hdr, 8);          // 读前 8 字节

    sp = hdr[0] | (hdr[1]<<8) | (hdr[2]<<16) | ((uint32_t)hdr[3]<<24);
    pc = hdr[4] | (hdr[5]<<8) | (hdr[6]<<16) | ((uint32_t)hdr[7]<<24);

    if (sp < 0x20000000UL || sp > 0x20005000UL) return 0;   // 堆栈指针必须指向 RAM
    if (pc < OTA_FLASH_BASE || pc > OTA_FLASH_BASE + 0x00010000UL) return 0; // 复位向量必须在 Flash 内
    return 1;
}
```

**为什么需要它**：CRC32 只能证明「数据和你发的一致」，不能证明「这是合法的 STM32 固件」。如果误把 `.hex`（文本格式，开头是 `:020`）或别的文件当固件喂进来，CRC 也能对上，但烧进去后跳转会直接 HardFault、导致变砖。

这个检查通过看固件头（向量表的前两项：堆栈指针 + 复位向量）是否落在合法区间，**在擦除 APP 之前**就拦截掉错误文件。

### 6.3 WriteAppFromDownload —— 从下载区写 APP 区

```c
static void WriteAppFromDownload(uint32_t size)
{
    uint32_t off, n;
    for (off = 0; off < size; off += OTA_PKT_DATA_SIZE)
    {
        n = (size - off) > OTA_PKT_DATA_SIZE ? OTA_PKT_DATA_SIZE : (size - off);
        W25Q64_ReadData(OTA_EXT_DL_ADDR + off, s_buf, n);       // 从 W25Q64 读
        OtaFlash_WriteRange(OTA_APP_ADDR + off, s_buf, n);      // 写内部 Flash
    }
}
```

每次 256 字节：先从外部 Flash 读到 RAM 缓冲区 `s_buf`，再写进内部 Flash。因为内部 Flash 写入时 CPU 会短暂停摆，256B 一块既能控制内存占用，也便于进度可控。

### 6.4 VerifyAppRegion —— 回读校验 APP 区

```c
static uint8_t VerifyAppRegion(uint32_t size, uint32_t expect)
{
    uint32_t crc = 0xFFFFFFFFUL, off, n;
    for (off = 0; off < size; off += OTA_PKT_DATA_SIZE)
    {
        n = ...;
        memcpy(s_buf, (const void *)(OTA_APP_ADDR + off), n);   // 从内部 Flash 读
        crc = CRC32_Update(crc, s_buf, n);
    }
    crc ^= 0xFFFFFFFFUL;
    return (crc == expect) ? 1 : 0;
}
```

把刚写进 APP 区的数据**重新读出来**算一遍 CRC，验证 Flash 写入的正确性（写入过程中可能因供电抖动等产生坏点）。

### 6.5 RestoreAppFromBackup —— 从备份区回退

```c
static void RestoreAppFromBackup(uint32_t size)
{
    uint32_t off, n;
    for (off = 0; off < size; off += OTA_PKT_DATA_SIZE)
    {
        n = ...;
        W25Q64_ReadData(OTA_EXT_BACKUP_ADDR + off, s_buf, n);
        OtaFlash_WriteRange(OTA_APP_ADDR + off, s_buf, n);
    }
}
```

与 `WriteAppFromDownload` 结构完全一样，只是数据源换成备份区。APP 在升级前会把旧固件备份到 W25Q64 备份区，这里把旧固件写回 APP 区。

---

## 7. 内部 Flash 操作（ota_flash.c）

STM32 内部 Flash 操作必须通过标准外设库 `stm32f10x_flash.c` 完成，规则是：**写之前先擦除，擦除按页（1KB），写入按半字（16bit）**。

### 7.1 参数区读写

```c
/* 读参数区：直接内存映射，把 Flash 当普通内存读 */
void OtaFlash_ReadParam(ota_param_t *p)
{
    volatile uint32_t *src = (volatile uint32_t *)OTA_PARAM_ADDR;
    uint32_t *dst = (uint32_t *)p;
    uint32_t i, n = sizeof(ota_param_t) / 4;
    for (i = 0; i < n; i++) dst[i] = src[i];
}

/* 写参数区：解锁 → 擦页 → 半字编程 → 上锁 */
void OtaFlash_WriteParam(ota_param_t *p)
{
    uint16_t *hw = (uint16_t *)p;
    uint32_t i, n = sizeof(ota_param_t) / 2;

    FLASH_Unlock();
    FLASH_ErasePage(OTA_PARAM_ADDR);                  // 擦 1KB 页
    for (i = 0; i < n; i++)
        FLASH_ProgramHalfWord(OTA_PARAM_ADDR + i*2, hw[i]);  // 逐个半字写
    FLASH_Lock();
}
```

参数区结构（`ota_config.h` 中的 `ota_param_t`）：

```c
typedef struct {
    uint32_t magic;          // 魔数 "OTAA"，判断参数区是否初始化过
    uint32_t flag;           // OTA_FLAG_PENDING / OTA_FLAG_NONE
    uint32_t new_size;       // 新固件大小
    uint32_t new_crc32;      // 新固件 CRC32
    uint32_t new_version;    // 新固件版本 (高16=主, 低16=次)
    uint32_t backup_size;    // 备份固件大小 (0=无备份)
    uint32_t old_version;    // 被替换的旧版本
} ota_param_t;
```

### 7.2 擦除一个范围

```c
void OtaFlash_EraseRange(uint32_t addr, uint32_t size)
{
    uint32_t start = addr & ~0x3FFUL;                 // 向下对齐到 1KB
    uint32_t end   = (addr + size + 0x3FFUL) & ~0x3FFUL; // 向上对齐到 1KB
    uint32_t a;
    FLASH_Unlock();
    for (a = start; a < end; a += 0x400)
        FLASH_ErasePage(a);                            // 逐页擦除
    FLASH_Lock();
}
```

因为 Flash 只能按 1KB 页擦除，所以把 `[addr, addr+size)` 覆盖到的所有页都擦掉（首尾可能多擦一点，多擦的部分变成 `0xFF`，无害）。

### 7.3 按半字写入

```c
void OtaFlash_WriteRange(uint32_t addr, const uint8_t *data, uint32_t size)
{
    uint32_t i; uint16_t hw;
    FLASH_Unlock();
    for (i = 0; i + 1 < size; i += 2)
    {
        hw = (uint16_t)(data[i] | ((uint16_t)data[i+1] << 8));  // 小端拼半字
        FLASH_ProgramHalfWord(addr + i, hw);
    }
    if (size & 1)                                        // 奇数长度，末尾补 0xFF
    {
        hw = (uint16_t)(data[size-1] | 0xFF00);
        FLASH_ProgramHalfWord(addr + size - 1, hw);
    }
    FLASH_Lock();
}
```

STM32F1 内部 Flash 的编程单位是 **16 位半字**。固件是字节流，所以每两个字节拼成一个半字写入；如果固件大小是奇数，最后一个半字高位补 `0xFF`。

---

## 8. 跳转 APP 的技术细节（OtaJumpToApp）

这是 Bootloader 最关键也最容易出错的一步。

```c
void OtaJumpToApp(uint32_t addr)
{
    uint32_t stack = *(volatile uint32_t *)addr;         // 读 APP 向量[0] = SP
    uint32_t entry = *(volatile uint32_t *)(addr + 4);   // 读 APP 向量[1] = Reset_Handler
    void (*jump)(void) = (void (*)(void))entry;

    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);  // 等串口发完

    SysTick->CTRL = 0; SysTick->LOAD = 0; SysTick->VAL = 0;        // 复位 SysTick

    SCB->VTOR = addr;                                   // 重定位向量表到 APP
    __set_MSP(stack);                                   // 设置主堆栈指针

    jump();                                             // 跳转到 APP 的 Reset_Handler
    while (1) { }
}
```

逐步解释每一步为什么必须做：

| 步骤 | 为什么 |
|------|--------|
| 读 `stack`、`entry` | APP 的向量表前两项是它的初始 SP 和复位函数，跳转前必须先拿到 |
| 等 `USART_FLAG_TC` | 最后一个日志字节可能还在串口移位寄存器里，直接跳转会因 APP 的 `SystemInit()` 重配时钟而把它打成乱码 |
| 复位 SysTick | Bootloader 用的延时依赖 SysTick，带病跳转会污染 APP |
| `SCB->VTOR = addr` | 让中断向量表指向 APP（虽然 APP 的 `SystemInit()` 又会把它重置回 `0x08000000`，但 APP 的 `main()` 首行会再次设成 `OTA_APP_ADDR`） |
| `__set_MSP(stack)` | 把主堆栈指针切到 APP 的堆栈 |
| `jump()` | 跳转到 APP 的 `Reset_Handler`，它会重新跑 `SystemInit → __main → main` |

**为什么不清全局中断**：APP 的启动代码会重新初始化 NVIC，跳转前清不清都行；反而清掉后 APP 若没重新使能会出问题，所以这里保持默认（不清）。

---

## 9. 掉电与容错设计

Bootloader 的核心容错思想是：**「待升级」标志只有升级完全成功后才清除，而且升级是幂等的，可以反复重跑**。

| 断电时机 | 标志状态 | 下次上电 |
|----------|----------|----------|
| 下载固件中（APP 侧） | 未写 | 旧 APP 正常运行，重新下载 |
| 备份中（APP 侧） | 未写 | 旧 APP 正常运行 |
| 已写标志、未擦 APP | PENDING | Bootloader 继续升级 |
| 擦写 APP 中途 | PENDING | Bootloader 从头重跑（幂等） |
| 升级完成、已清标志 | 已清 | 新 APP 正常启动 |

关键保证：

1. **新固件源头在 W25Q64 下载区**，掉电不丢，Bootloader 重跑时数据还在。
2. **先校验后擦除**：下载区 CRC 和固件头都通过后，才擦 APP 区，失败时 APP 完好。
3. **标志最后清**：`OtaFlash_ClearFlag()` 只在「升级成功」或「回退完成」后调用，中途掉电标志仍是 PENDING，下次自动重跑。
4. **Bootloader 自身不被擦**：升级只擦 `0x08004400` 之后的 APP 区，Bootloader 永远在，所以永远有救。

---

## 10. 关键注意事项

1. **Bootloader 体积限制 17KB**：若编译溢出，在 Keil `Options → C/C++ → Optimization` 调成 `-O2/-O3`，或增大 `OTA_BOOT_SIZE` 并同步把 `OTA_APP_ADDR` 往后移。

2. **跳转失败的典型现象**：串口出现 `jump to new APP` 后紧接着又打印一次 `[BL] Bootloader started`，说明 APP 的 SP/复位向量非法导致 HardFault 复位。这通常是喂错了固件文件（`.hex` 当 `.bin`），`VerifyFirmwareHeader` 就是为此加的。

3. **`No valid app!` 的含义**：无升级任务时，Bootloader 读 APP 区首地址的堆栈指针，若不在 RAM 区间内则报错并停住。出现这个说明 APP 没烧或已损坏，需 ST-LINK 重烧。

4. **APP 侧必须做 `SCB->VTOR = OTA_APP_ADDR`**：Bootloader 跳转过去后，APP 的 `SystemInit()` 会把向量表重置回 `0x08000000`，所以 APP 的 `main()` 第一行必须重定位，否则 APP 的中断（含 USART2 接收）全部指向 Bootloader 的向量表。

5. **参数区的魔数 `magic`**：用来区分「参数区是有效数据」还是「从未初始化/被擦除的全 0xFF」。读参数区后先看 `magic == OTA_PARAM_MAGIC`，避免把空白 Flash 当成有效标志。
