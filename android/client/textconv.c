/*
 * Copyright (C) 2013 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <string.h>
#include <stdio.h>
#include <hardware/bluetooth.h>

#include "textconv.h"

/*
 * Following are maps of defines found in bluetooth header files to strings
 *
 * Those mappings are used to accurately use defines as input parameters in
 * command line as well as for printing of statuses
 */

INTMAP(bt_status_t, -1, "(unknown)")
	DELEMENT(BT_STATUS_SUCCESS),
	DELEMENT(BT_STATUS_FAIL),
	DELEMENT(BT_STATUS_NOT_READY),
	DELEMENT(BT_STATUS_NOMEM),
	DELEMENT(BT_STATUS_BUSY),
	DELEMENT(BT_STATUS_DONE),
	DELEMENT(BT_STATUS_UNSUPPORTED),
	DELEMENT(BT_STATUS_PARM_INVALID),
	DELEMENT(BT_STATUS_UNHANDLED),
	DELEMENT(BT_STATUS_AUTH_FAILURE),
	DELEMENT(BT_STATUS_RMT_DEV_DOWN),
ENDMAP

INTMAP(bt_state_t, -1, "(unknown)")
	DELEMENT(BT_STATE_OFF),
	DELEMENT(BT_STATE_ON),
ENDMAP

INTMAP(bt_device_type_t, -1, "(unknown)")
	DELEMENT(BT_DEVICE_DEVTYPE_BREDR),
	DELEMENT(BT_DEVICE_DEVTYPE_BLE),
	DELEMENT(BT_DEVICE_DEVTYPE_DUAL),
ENDMAP

INTMAP(bt_scan_mode_t, -1, "(unknown)")
	DELEMENT(BT_SCAN_MODE_NONE),
	DELEMENT(BT_SCAN_MODE_CONNECTABLE),
	DELEMENT(BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE),
ENDMAP

INTMAP(bt_discovery_state_t, -1, "(unknown)")
	DELEMENT(BT_DISCOVERY_STOPPED),
	DELEMENT(BT_DISCOVERY_STARTED),
ENDMAP

INTMAP(bt_acl_state_t, -1, "(unknown)")
	DELEMENT(BT_ACL_STATE_CONNECTED),
	DELEMENT(BT_ACL_STATE_DISCONNECTED),
ENDMAP

INTMAP(bt_bond_state_t, -1, "(unknown)")
	DELEMENT(BT_BOND_STATE_NONE),
	DELEMENT(BT_BOND_STATE_BONDING),
	DELEMENT(BT_BOND_STATE_BONDED),
ENDMAP

INTMAP(bt_ssp_variant_t, -1, "(unknown)")
	DELEMENT(BT_SSP_VARIANT_PASSKEY_CONFIRMATION),
	DELEMENT(BT_SSP_VARIANT_PASSKEY_ENTRY),
	DELEMENT(BT_SSP_VARIANT_CONSENT),
	DELEMENT(BT_SSP_VARIANT_PASSKEY_NOTIFICATION),
ENDMAP

INTMAP(bt_property_type_t, -1, "(unknown)")
	DELEMENT(BT_PROPERTY_BDNAME),
	DELEMENT(BT_PROPERTY_BDADDR),
	DELEMENT(BT_PROPERTY_UUIDS),
	DELEMENT(BT_PROPERTY_CLASS_OF_DEVICE),
	DELEMENT(BT_PROPERTY_TYPE_OF_DEVICE),
	DELEMENT(BT_PROPERTY_SERVICE_RECORD),
	DELEMENT(BT_PROPERTY_ADAPTER_SCAN_MODE),
	DELEMENT(BT_PROPERTY_ADAPTER_BONDED_DEVICES),
	DELEMENT(BT_PROPERTY_ADAPTER_DISCOVERY_TIMEOUT),
	DELEMENT(BT_PROPERTY_REMOTE_FRIENDLY_NAME),
	DELEMENT(BT_PROPERTY_REMOTE_RSSI),
#if PLATFORM_SDK_VERSION > 17
	DELEMENT(BT_PROPERTY_REMOTE_VERSION_INFO),
#endif
	DELEMENT(BT_PROPERTY_REMOTE_DEVICE_TIMESTAMP),
ENDMAP

INTMAP(bt_cb_thread_evt, -1, "(unknown)")
	DELEMENT(ASSOCIATE_JVM),
	DELEMENT(DISASSOCIATE_JVM),
ENDMAP

/* Find first index of given value in table m */
int int2str_findint(int v, const struct int2str m[])
{
	int i;

	for (i = 0; m[i].str; ++i) {
		if (m[i].val == v)
			return i;
	}
	return -1;
}

/* Find first index of given string in table m */
int int2str_findstr(const char *str, const struct int2str m[])
{
	int i;

	for (i = 0; m[i].str; ++i) {
		if (strcmp(m[i].str, str) == 0)
			return i;
	}
	return -1;
}

/*
 * convert bd_addr to string
 * buf must be at least 18 char long
 *
 * returns buf
 */
char *bt_bdaddr_t2str(const bt_bdaddr_t *bd_addr, char *buf)
{
	const uint8_t *p = bd_addr->address;

	snprintf(buf, MAX_ADDR_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x",
					p[0], p[1], p[2], p[3], p[4], p[5]);

	return buf;
}

/* converts string to bt_bdaddr_t */
void str2bt_bdaddr_t(const char *str, bt_bdaddr_t *bd_addr)
{
	uint8_t *p = bd_addr->address;

	sscanf(str, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
				&p[0], &p[1], &p[2], &p[3], &p[4], &p[5]);
}

static const char BT_BASE_UUID[] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
	0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb
};

/*
 * converts uuid to string
 * buf should be at least 39 bytes
 *
 * returns string representation of uuid
 */
char *bt_uuid_t2str(const bt_uuid_t *uuid, char *buf)
{
	int shift = 0;
	int i;
	int is_bt;

	is_bt = !memcmp(&uuid->uu[4], &BT_BASE_UUID[4], sizeof(bt_uuid_t) - 4);

	for (i = 0; i < (int) sizeof(bt_uuid_t); i++) {
		if (i == 4 && is_bt)
			break;

		if (i == 4 || i == 6 || i == 8 || i == 10) {
			buf[i * 2 + shift] = '-';
			shift++;
		}
		sprintf(buf + i * 2 + shift, "%02x", uuid->uu[i]);
	}

	return buf;
}

/* converts string to uuid */
void str2bt_uuid_t(const char *str, bt_uuid_t *uuid)
{
	int i = 0;

	memcpy(uuid, BT_BASE_UUID, sizeof(bt_uuid_t));

	while (*str && i < (int) sizeof(bt_uuid_t)) {
		while (*str == '-')
			str++;

		if (sscanf(str, "%02hhx", &uuid->uu[i]) != 1)
			break;

		i++;
		str += 2;
	}
}

const char *enum_defines(void *v, int i)
{
	const struct int2str *m = v;

	return m[i].str != NULL ? m[i].str : NULL;
}

const char *enum_strings(void *v, int i)
{
	const char **m = v;

	return m[i] != NULL ? m[i] : NULL;
}

const char *enum_one_string(void *v, int i)
{
	const char *m = v;

	return (i == 0) && (m[0] != 0) ? m : NULL;
}
