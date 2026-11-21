/*
 * USB-Serial-JTAG transport for Grok Sphere (Thad + Claude Edition).
 * Runs alongside Wi-Fi WebSocket; USB takes priority whenever the Windows companion
 * is actively talking on the COM port.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*usb_json_cb_t)(const char *json, size_t len);
typedef void (*usb_pcm_cb_t)(const uint8_t *pcm, size_t len);
typedef void (*usb_conn_cb_t)(bool connected);

void usb_link_start(usb_json_cb_t on_json, usb_pcm_cb_t on_pcm, usb_conn_cb_t on_conn);
bool usb_link_connected(void);
bool usb_link_send_json(const char *json, size_t len);
bool usb_link_send_pcm(const uint8_t *pcm, size_t len);
