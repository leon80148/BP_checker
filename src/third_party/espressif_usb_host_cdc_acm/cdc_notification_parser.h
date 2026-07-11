/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usb/usb_types_cdc.h"

enum {
    CDC_ACM_NOTIFICATION_HEADER_SIZE = 8,
    CDC_ACM_NOTIFICATION_REQUEST_TYPE = 0xA1,
};

typedef struct {
    uint8_t notificationCode;
    uint16_t value;
    uint16_t interfaceIndex;
    uint16_t payloadLength;
    uint16_t serialState;
    const uint8_t *payload;
} CdcAcmNotificationView;

static inline uint16_t cdc_acm_read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static inline bool cdc_acm_notification_parse(
    const uint8_t *buffer, size_t actual_length,
    CdcAcmNotificationView *view)
{
    if (buffer == NULL || view == NULL ||
            actual_length < CDC_ACM_NOTIFICATION_HEADER_SIZE) {
        return false;
    }
    if (buffer[0] != CDC_ACM_NOTIFICATION_REQUEST_TYPE) return false;

    const uint16_t payload_length = cdc_acm_read_le16(&buffer[6]);
    if ((size_t)payload_length >
            actual_length - CDC_ACM_NOTIFICATION_HEADER_SIZE) {
        return false;
    }

    view->notificationCode = buffer[1];
    view->value = cdc_acm_read_le16(&buffer[2]);
    view->interfaceIndex = cdc_acm_read_le16(&buffer[4]);
    view->payloadLength = payload_length;
    view->payload = &buffer[CDC_ACM_NOTIFICATION_HEADER_SIZE];
    view->serialState = 0;

    if (view->notificationCode == USB_CDC_NOTIF_SERIAL_STATE) {
        if (payload_length != sizeof(uint16_t)) return false;
        view->serialState = cdc_acm_read_le16(view->payload);
    } else if (view->notificationCode == USB_CDC_NOTIF_NETWORK_CONNECTION &&
            payload_length != 0) {
        return false;
    }
    return true;
}
