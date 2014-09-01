/*
*  Author: Dave S. Desrochers <dave.desrochers@aether.com>
*
* Copyright (c) 2014 Morse Project. All rights reserved.
*
* @file Implements Aether User Interface (AUI) GATT server
*
*/

#ifndef __AD_TYPES_H
#define __AD_TYPES_H

/*
* All these and more are defined in Bluetooth v4.0 spec
* in Volume 3, Part C, Section 18 Appendix C.
*/

#define EIR_SLAVE_CONN_INT_RANGE 0x12
typedef struct {
        uint16_t	interval_min;
        uint16_t	interval_max;
} __attribute__ ((packed)) le_slave_connection_interval_range;

#define EIR_SERVICE_UUID16	0x14
#define EIR_SERVICE_UUID128	0x15

#define EIR_SERVICE_DATA	0x16

#define EIR_MANUFACTURER_DATA	0xFF

#define EIR_INVALID		0xEE

#endif
