#ifndef __OTA_CONFIG_H
#define __OTA_CONFIG_H

#include "stm32f10x.h"

/* ===================== 内部 Flash 布局 (STM32F103C8, 64KB) ===================== */
#define OTA_FLASH_BASE          0x08000000UL   /* Flash 基地址 */
#define OTA_BOOT_ADDR           0x08000000UL   /* Bootloader 起始地址 */
#define OTA_BOOT_SIZE           0x00004400UL   /* Bootloader 占用 17KB */
#define OTA_APP_ADDR            0x08004400UL   /* APP 起始地址 */
#define OTA_APP_MAX_SIZE        0x0000B800UL   /* APP 可用空间 (到参数页之前) */
#define OTA_PARAM_ADDR          0x0800FC00UL   /* 参数区: 最后一页 (1KB) */
#define OTA_PARAM_SIZE          0x00000400UL   /* 参数页大小 1KB */

/* ===================== W25Q64 外部 Flash 布局 (8MB) ===================== */
#define OTA_EXT_DL_ADDR         0x00000000UL   /* 下载区起始 */
#define OTA_EXT_DL_SIZE         0x00080000UL   /* 下载区 512KB */
#define OTA_EXT_BACKUP_ADDR     0x00080000UL   /* 备份区起始 */
#define OTA_EXT_BACKUP_SIZE     0x00080000UL   /* 备份区 512KB */

/* ===================== 参数区 magic / flag ===================== */
#define OTA_PARAM_MAGIC         0x4F544141UL   /* "OTAA" */
#define OTA_FLAG_NONE           0x00000000UL
#define OTA_FLAG_PENDING        0x55AA55AAUL

/* ===================== 版本号 (APP 每次发布需手动递增) ===================== */
#define OTA_APP_VERSION_MAJOR   2
#define OTA_APP_VERSION_MINOR   2

/* ===================== OTA 下载串口 (USART2, 与日志 USART1 分离) ===================== */
#define OTA_UART_BAUD           115200         /* OTA 下载波特率 (ESP32 <-> STM32) */

/* ===================== UART 传输协议 ===================== */
#define OTA_PKT_DATA_SIZE       256            /* 每包数据 256 字节 */

#define OTA_FRAME_HEAD0         0xAA
#define OTA_FRAME_HEAD1         0x55

/* 命令字 (上位机 -> STM32) */
#define OTA_CMD_START           0x01
#define OTA_CMD_DATA            0x02
#define OTA_CMD_END             0x03
#define OTA_CMD_ABORT           0x04

/* 应答字 (STM32 -> 上位机) */
#define OTA_ACK                 0x06
#define OTA_NAK                 0x15
#define OTA_RESP_START_OK       0x02
#define OTA_RESP_START_ERR      0x03
#define OTA_RESP_VERIFY_OK      0x04
#define OTA_RESP_VERIFY_ERR     0x05

/* ===================== 参数区结构体 ===================== */
typedef struct {
    uint32_t magic;          /* OTA_PARAM_MAGIC */
    uint32_t flag;           /* OTA_FLAG_PENDING / OTA_FLAG_NONE */
    uint32_t new_size;       /* 新固件大小 (字节) */
    uint32_t new_crc32;      /* 新固件 CRC32 */
    uint32_t new_version;    /* 高 16 位 = 主版本, 低 16 位 = 次版本 */
    uint32_t backup_size;    /* 备份固件大小 (0 = 无备份) */
    uint32_t old_version;    /* 被替换的旧版本号 */
} ota_param_t;

#endif
