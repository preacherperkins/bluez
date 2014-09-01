/*
 *  Author: Dave S. Desrochers <dave.desrochers@aether.com>
 *
 * Copyright (c) 2014 Morse Project. All rights reserved.
 *
 * @file Implements Aether User Interface (AUI) advertisement definition and delivery to the controller
 *
 */

#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "lib/bluetooth.h"
#include "lib/mgmt.h"
#include "src/adapter.h"
#include "lib/uuid.h"
#include "src/eir.h"

#include "ad_types.h"
#include "aui-advertise.h"
#include "hci_ext.h"
#include "aui.h"

int aui_set_powered_blocking(struct btd_adapter *adapter)
{
        int dev_id, ctl;

        dev_id = btd_adapter_get_index(adapter);
        if (dev_id == MGMT_INDEX_NONE) {
                return -1;
        }

        if ((ctl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI)) < 0) {
                perror("Can't open HCI socket.");
                return -1;
        }

        /* Start HCI device */
        if (ioctl(ctl, HCIDEVUP, dev_id) < 0) {
                if (errno == EALREADY) {
                        return -1;
                }
                fprintf(stderr, "Can't init device hci%d: %s (%d)\n",
                                                dev_id, strerror(errno), errno);
                return -1;
        }

        close(ctl);

        return 0;
}

int aui_set_advertise_params(struct btd_adapter *adapter)
{
	int dev_id, dd;
	bdaddr_t direct_address;

	dev_id = btd_adapter_get_index(adapter);
	if (dev_id == MGMT_INDEX_NONE) {
		return -1;
	}

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		return -1;
	}

	memset(&direct_address, 0, sizeof(direct_address));

	return hci_le_set_advertise_params(dd,
			htobs(0x0800),      /* advertising interval min  */
			htobs(0x0800),      /* advertising interval max  */
			0x00,               /* advertising type          */
			LE_PUBLIC_ADDRESS,  /* own address type          */
			LE_PUBLIC_ADDRESS,  /* direct address type       */
			direct_address,     /* direct address            */
			0x07,               /* advertising channel map   */
			0x00,               /* advertising filter policy */
			1000);
}

int aui_set_advertise_data(struct btd_adapter *adapter)
{
	int dev_id, dd;
	bt_uuid_t service_uuid;

	dev_id = btd_adapter_get_index(adapter);
	if (dev_id == MGMT_INDEX_NONE) {
		return -1;
	}

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		return -1;
	}

	bt_string_to_uuid(&service_uuid, AUI_SERVICE_UUID);

	return hci_le_set_advertise_data(dd,
			EIR_FLAGS,		EIR_GEN_DISC | EIR_CONTROLLER | EIR_SIM_HOST,
			EIR_UUID128_SOME,	&service_uuid,
			EIR_INVALID, 1000);
}

int aui_set_scan_response_data(struct btd_adapter *adapter, const char *complete_name)
{
	int dev_id, dd;

	le_slave_connection_interval_range conn_range = {
		/* multiply by 1.25ms for wall clock time */
		.interval_min = 0x0050,
		.interval_max = 0x0320
	};

	dev_id = btd_adapter_get_index(adapter);
	if (dev_id == MGMT_INDEX_NONE) {
		return -1;
	}

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		return -1;
	}

	return hci_le_set_scan_response_data(dd,
			EIR_NAME_COMPLETE,		(uint8_t *)complete_name,
			EIR_SLAVE_CONN_INT_RANGE,	&conn_range,
			EIR_INVALID, 1000);
}

int aui_set_advertise_enable(struct btd_adapter *adapter, int enable)
{
	int dev_id, dd;

	dev_id = btd_adapter_get_index(adapter);
	if (dev_id == MGMT_INDEX_NONE) {
		return -1;
	}

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		return -1;
	}

	return hci_le_set_advertise_enable(dd, enable, 1000);
}

