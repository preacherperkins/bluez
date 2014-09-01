/*
*  Author: Dave S. Desrochers <dave.desrochers@aether.com>
*
* Copyright (c) 2014 Morse Project. All rights reserved.
*
* @file Implements Aether User Interface (AUI) GATT server
*
*/

#ifndef __AUI_ADVERTISE_H
#define __AUI_ADVERTISE_H

int aui_set_powered_blocking(struct btd_adapter *adapter);

int aui_set_advertise_params(struct btd_adapter *adapter);

int aui_set_advertise_data(struct btd_adapter *adapter);

int aui_set_scan_response_data(struct btd_adapter *adapter, const char *complete_name);

int aui_set_advertise_enable(struct btd_adapter *adapter, int enable);

#endif
