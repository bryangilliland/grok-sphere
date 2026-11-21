/*
 * Framed binary transport over USB-Serial-JTAG ("GST1" + CRC16-CCITT).
 * Pure C so it is unit-tested on a PC (host_test/).
 *
 * Wire format:
 *   [0..3]  Magic: 'G','S','T','1'
 *   [4]     Type:  1 = JSON text, 2 = 16 kHz mono s16le PCM audio
 *   [5]     Flags: 0
 *   [6..7]  Payload length (uint16 LE, 0..USB_FRAME_MAX_PAYLOAD)
 *   [8..]   Payload (len bytes)
 *   [+2]    CRC16-CCITT (init 0xFFFF, poly 0x1021) over bytes [4 .. 8+len-1], uint16 LE
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_FRAME_MAGIC_0      'G'
#define USB_FRAME_MAGIC_1      'S'
#define USB_FRAME_MAGIC_2      'T'
#define USB_FRAME_MAGIC_3      '1'
#define USB_FRAME_HDR_SIZE     8
#define USB_FRAME_CRC_SIZE     2
#define USB_FRAME_OVERHEAD     (USB_FRAME_HDR_SIZE + USB_FRAME_CRC_SIZE)
#define USB_FRAME_MAX_PAYLOAD  4096

typedef enum {
    USB_FRAME_JSON = 1,
    USB_FRAME_PCM  = 2,
} usb_frame_type_t;

typedef struct {
    uint8_t buf[USB_FRAME_HDR_SIZE + USB_FRAME_MAX_PAYLOAD + USB_FRAME_CRC_SIZE];
    size_t pos;
    uint16_t expected_len;
} usb_frame_rx_t;

uint16_t usb_frame_crc16(const uint8_t *data, size_t len);

/* Encodes a frame into out (must be >= payload_len + USB_FRAME_OVERHEAD). Returns total length or -1. */
int usb_frame_encode(uint8_t *out, size_t cap, uint8_t type, const void *payload, size_t payload_len);

void usb_frame_rx_init(usb_frame_rx_t *rx);

/* Feed incoming bytes; invokes cb whenever a valid CRC-checked frame completes. */
typedef void (*usb_frame_cb_t)(uint8_t type, const uint8_t *payload, uint16_t len, void *ctx);
void usb_frame_rx_feed(usb_frame_rx_t *rx, const uint8_t *data, size_t len, usb_frame_cb_t cb, void *ctx);
