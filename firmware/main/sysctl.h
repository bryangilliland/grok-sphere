#pragma once
/* Restart into the ESP32-S3 ROM's USB download ("flash") mode. The USB-Serial-JTAG port then
 * shows up on the computer and esptool / the web flasher can write firmware. Unplug to leave. */
void sys_enter_flash_mode(void);
