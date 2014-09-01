/*
*  Author: Dave S. Desrochers <dave.desrochers@aether.com>
*
* Copyright (c) 2014 Morse Project. All rights reserved.
*
* @file Implements Aether User Interface (AUI) GATT server
*
*/

#ifndef __HCI_EXT_H
#define __HCI_EXT_H

#include <bluetooth/bluetooth.h>

struct generic_advertising_packet {
        uint8_t length;
        uint8_t data[31];
};

int hci_le_set_advertise_params(int dd, uint16_t min_interval, uint16_t max_interval,
                        uint8_t advtype, uint8_t own_bdaddr_type, uint8_t direct_bdaddr_type,
                        bdaddr_t direct_bdaddr, uint8_t chan_map, uint8_t filter, int to);

int hci_le_set_advertise_data(int dd, uint8_t ad_type1, ...);

int hci_le_set_scan_response_data(int dd, uint8_t ad_type1, ...);

#endif


