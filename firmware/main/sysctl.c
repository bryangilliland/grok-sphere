#include "sysctl.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "ui.h"

/* USB-Serial-JTAG must never be disabled and GPIO19/20 never repurposed (see board.h). */
#if !CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
#error "USB-Serial-JTAG must stay enabled: it is the only way to reflash a sealed sphere"
#endif

void sys_enter_flash_mode(void)
{
    ESP_LOGW("sysctl", "restarting into ROM download mode");
    ui_show_notice("USB flash mode", "Plug into a computer and\nrun the flasher.\n\nUnplug to go back to normal.");
    vTaskDelay(pdMS_TO_TICKS(800));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}
