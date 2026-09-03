/*
 * ESP32 OTA 发送端 (PlatformIO / Arduino 框架)
 *
 * 流程: 连 WiFi -> HTTP 下载固件 -> 分包(每包256B) -> UART2 发给 STM32
 *
 * 协议与 STM32 侧一致:
 *   帧: [0xAA][0x55][cmd][seq_H][seq_L][len_H][len_L][payload...][crc16_H][crc16_L]
 *   应答: [0xAA][0x55][code][seq_H][seq_L]
 *
 * 接线 (与 STM32 的 USART2 交叉):
 *   ESP32 GPIO17 (TX) -> STM32 PA3 (USART2 RX)
 *   ESP32 GPIO16 (RX) <- STM32 PA2 (USART2 TX)
 *   GND <-> GND
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <string.h>
#include <vector>

/* ==================== 用户配置 ==================== */
const char* WIFI_SSID     = "CMCC-2211";
const char* WIFI_PASS     = "blxy8888";
const char* FIRMWARE_URL  = "http://192.168.1.6:8080/Project.bin";
const char* VERSION_URL   = "http://192.168.1.6:8080/version.txt";  // 版本号来自服务器, 发布时在服务器改

/* OTA 串口引脚 (与 STM32 USART2 交叉相连) */
#define OTA_RXD 16            // ESP32 RX  <- STM32 PA2 (USART2 TX)
#define OTA_TXD 17            // ESP32 TX  -> STM32 PA3 (USART2 RX)

/* ==================== 协议常量 (与 STM32 一致, 勿改) ==================== */
#define PKT_SIZE      256
#define FRAME_HEAD0   0xAA
#define FRAME_HEAD1   0x55

#define CMD_START     0x01
#define CMD_DATA      0x02
#define CMD_END       0x03
#define CMD_ABORT     0x04

#define RESP_ACK          0x06
#define RESP_NAK          0x15
#define RESP_START_OK     0x02
#define RESP_START_ERR    0x03
#define RESP_VERIFY_OK    0x04
#define RESP_VERIFY_ERR   0x05

HardwareSerial OtaSerial(2);   // 硬件串口2, 专用于 OTA

/* ==================== CRC / 帧封装 ==================== */
uint16_t crc16_modbus(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

/* 标准 CRC32 (IEEE 802.3), 与 STM32 / python binascii.crc32 一致 */
uint32_t crc32_calc(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* 组装一帧, 结果写入 out, 返回实际长度 */
void build_frame(uint8_t cmd, uint16_t seq, const uint8_t* payload, uint16_t len,
                 uint8_t* out, size_t* out_len)
{
    uint8_t body[1 + 2 + 2 + PKT_SIZE];
    size_t bi = 0;

    body[bi++] = cmd;
    body[bi++] = (uint8_t)(seq >> 8);
    body[bi++] = (uint8_t)(seq & 0xFF);
    body[bi++] = (uint8_t)(len >> 8);
    body[bi++] = (uint8_t)(len & 0xFF);
    if (len > 0)
    {
        memcpy(&body[bi], payload, len);
        bi += len;
    }
    uint16_t crc = crc16_modbus(body, bi);

    size_t oi = 0;
    out[oi++] = FRAME_HEAD0;
    out[oi++] = FRAME_HEAD1;
    memcpy(&out[oi], body, bi);
    oi += bi;
    out[oi++] = (uint8_t)(crc >> 8);
    out[oi++] = (uint8_t)(crc & 0xFF);
    *out_len = oi;
}

/* 等待 STM32 应答, 跳过无关字节; 返回 true 并输出 code/seq */
bool read_response(uint8_t& code, uint16_t& seq, uint32_t timeout_ms)
{
    uint32_t start = millis();
    int prev = -1;

    while (millis() - start < timeout_ms)
    {
        if (!OtaSerial.available())
        {
            delay(1);
            continue;
        }
        int b = OtaSerial.read();
        if (prev == FRAME_HEAD0 && b == FRAME_HEAD1)
        {
            uint32_t t0 = millis();
            while (OtaSerial.available() < 3 && millis() - t0 < 100) delay(1);
            if (OtaSerial.available() >= 3)
            {
                code = (uint8_t)OtaSerial.read();
                seq  = (uint16_t)((OtaSerial.read() << 8) | OtaSerial.read());
                return true;
            }
            return false;
        }
        prev = b;
    }
    return false;
}

/* ==================== HTTP 下载 ==================== */
bool download_firmware(const char* url, std::vector<uint8_t>& fw)
{
    HTTPClient http;
    http.begin(url);
    http.setTimeout(20000);

    Serial.printf("[ESP32] 下载: %s\n", url);
    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        Serial.printf("[ESP32] HTTP 错误: %d\n", code);
        http.end();
        return false;
    }

    int len = http.getSize();
    if (len > 0) fw.reserve((size_t)len);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    while (stream->connected())
    {
        size_t avail = stream->available();
        if (avail == 0)
        {
            delay(1);
            if (!stream->connected()) break;
            continue;
        }
        int n = stream->read(buf, avail < sizeof(buf) ? avail : sizeof(buf));
        if (n <= 0) break;
        fw.insert(fw.end(), buf, buf + n);
    }
    http.end();

    Serial.printf("[ESP32] 下载完成: %d 字节\n", (int)fw.size());
    return !fw.empty();
}

/* 从服务器下载版本号 (version.txt 内容如 "2.1") */
bool download_version(uint16_t& major, uint16_t& minor)
{
    HTTPClient http;
    http.begin(VERSION_URL);
    http.setTimeout(5000);

    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        http.end();
        return false;
    }

    String ver = http.getString();
    http.end();
    ver.trim();

    int dot = ver.indexOf('.');
    if (dot < 0)
    {
        major = (uint16_t)ver.toInt();
        minor = 0;
    }
    else
    {
        major = (uint16_t)ver.substring(0, dot).toInt();
        minor = (uint16_t)ver.substring(dot + 1).toInt();
    }
    return true;
}

/* ==================== OTA 发送 ==================== */
bool ota_update(const uint8_t* fw, size_t size, uint32_t crc32, uint16_t ver_major, uint16_t ver_minor)
{
    size_t total = (size + PKT_SIZE - 1) / PKT_SIZE;
    uint8_t frame[2 + 1 + 2 + 2 + PKT_SIZE + 2];
    size_t flen;
    uint8_t code;
    uint16_t seq;

    /* ---- START ---- */
    uint8_t payload[12];
    payload[0] = (uint8_t)(size >> 24);
    payload[1] = (uint8_t)(size >> 16);
    payload[2] = (uint8_t)(size >> 8);
    payload[3] = (uint8_t)(size);
    payload[4] = (uint8_t)(crc32 >> 24);
    payload[5] = (uint8_t)(crc32 >> 16);
    payload[6] = (uint8_t)(crc32 >> 8);
    payload[7] = (uint8_t)(crc32);
    payload[8]  = (uint8_t)(ver_major >> 8);
    payload[9]  = (uint8_t)(ver_major);
    payload[10] = (uint8_t)(ver_minor >> 8);
    payload[11] = (uint8_t)(ver_minor);

    build_frame(CMD_START, (uint16_t)total, payload, 12, frame, &flen);
    OtaSerial.write(frame, flen);
    if (!read_response(code, seq, 5000) || code != RESP_START_OK)
    {
        Serial.println("[ESP32] START 失败");
        return false;
    }
    Serial.println("[ESP32] START ok");

    /* ---- DATA ---- */
    for (size_t s = 0; s < total; s++)
    {
        size_t off = s * PKT_SIZE;
        size_t n = (size - off) > PKT_SIZE ? PKT_SIZE : (size - off);
        build_frame(CMD_DATA, (uint16_t)s, fw + off, (uint16_t)n, frame, &flen);

        bool acked = false;
        for (int retry = 0; retry < 3; retry++)
        {
            OtaSerial.write(frame, flen);
            if (read_response(code, seq, 2000) && code == RESP_ACK && seq == s)
            {
                acked = true;
                break;
            }
        }
        if (!acked)
        {
            Serial.printf("[ESP32] DATA seq=%d 失败\n", (int)s);
            return false;
        }
        if (s % 20 == 0 || s == total - 1)
            Serial.printf("[ESP32] 进度 %d/%d\n", (int)(s + 1), (int)total);
    }

    /* ---- END ---- */
    build_frame(CMD_END, 0, NULL, 0, frame, &flen);
    OtaSerial.write(frame, flen);
    if (!read_response(code, seq, 20000))
    {
        Serial.println("[ESP32] END 无应答");
        return false;
    }
    if (code == RESP_VERIFY_OK)
    {
        Serial.println("[ESP32] VERIFY_OK, STM32 将软复位升级");
        return true;
    }
    Serial.println("[ESP32] VERIFY_ERR (CRC 校验失败)");
    return false;
}

/* ==================== 主流程 ==================== */
void setup()
{
    Serial.begin(115200);                                   // UART0 调试输出
    OtaSerial.begin(115200, SERIAL_8N1, OTA_RXD, OTA_TXD);  // UART2 -> STM32

    Serial.println("\n[ESP32] OTA sender start");

    /* WiFi 连接 */
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[ESP32] 连 WiFi");
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - t > 15000)
        {
            Serial.println("\n[ESP32] WiFi 连接超时, 重启");
            ESP.restart();
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.printf("[ESP32] WiFi OK, IP = %s\n", WiFi.localIP().toString().c_str());

    /* 下载版本号 (来自服务器 version.txt, 不用改 ESP32 代码) */
    uint16_t ver_major = 0, ver_minor = 0;
    if (!download_version(ver_major, ver_minor))
    {
        Serial.println("[ESP32] 版本号下载失败, 用 0.0");
    }
    else
    {
        Serial.printf("[ESP32] 版本号: %d.%d\n", ver_major, ver_minor);
    }

    /* 下载固件 */
    std::vector<uint8_t> fw;
    if (!download_firmware(FIRMWARE_URL, fw))
    {
        Serial.println("[ESP32] 固件下载失败");
        return;
    }

    /* 发送 */
    uint32_t crc = crc32_calc(fw.data(), fw.size());
    Serial.printf("[ESP32] size=%d crc=0x%08X\n", (int)fw.size(), crc);
    if (ota_update(fw.data(), fw.size(), crc, ver_major, ver_minor))
        Serial.println("[ESP32] OTA success");
    else
        Serial.println("[ESP32] OTA failed");
}

void loop()
{
    /* 这里可以放触发逻辑: 按键 / MQTT / HTTP 轮询, 触发时再调用 ota_update() */
    delay(1000);
}