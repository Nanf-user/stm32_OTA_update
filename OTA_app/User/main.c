#include "stm32f10x.h"
#include "generic.h"
#include "Serial.h"
#include "LED.h"
#include "W25Q64.h"
#include "ota_config.h"
#include "ota_uart.h"
#include "ota_app.h"
#include "ota_log.h"

int main(void)
{
	/* 关键: APP 运行在 0x08004400, 必须在开中断前重定位向量表 */
	SCB->VTOR = OTA_APP_ADDR;

//	delay_init();
	LED_Init();
	Serial_Init();
	OtaUart_Init();
	W25Q64_Init();
	OtaApp_Init();

	OtaLog_Str("[APP] Application v");
	OtaLog_Num(OTA_APP_VERSION_MAJOR);
	OtaLog_Str(".");
	OtaLog_Num(OTA_APP_VERSION_MINOR);
	OtaLog_Str(" started");
	OtaLog_CRLF();

	{
		uint8_t mid;
		uint16_t did;
		W25Q64_ReadID(&mid, &did);
		OtaLog_Str("[APP] W25Q64 ID: 0x");
		OtaLog_Hex8(mid);
		OtaLog_Hex16(did);
		OtaLog_CRLF();
	}

	while (1)
	{
		OtaApp_Process();   /* 处理 OTA 数据帧 */

		/* ---- 用户应用代码 ---- */
		static uint8_t led_on = 0;
		if (led_on)
		{
			LED_1_Off();
			led_on = 0;
		}
		else
		{
			LED_1_On();
			led_on = 1;
		}
		Delay_ms(2000);
	}
}
