/*
*  Author: Dave S. Desrochers <dave.desrochers@aether.com>
*
* Copyright (c) 2014 Morse Project. All rights reserved.
*
* @file Implements Aether User Interface (AUI) advertisement definition and delivery to the controller
*
*/

#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>
#include <stdbool.h>
#include <errno.h>

#include "lib/bluetooth/sdp.h"
#include "lib/uuid.h"
#include "src/eir.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"

#include "ad_types.h"
#include "hci_ext.h"

struct ad_types {
	bt_uuid_t *uuid;
	uint8_t *data;
	uint8_t size;
	le_slave_connection_interval_range *range;
};

static int parse_ad_types(struct generic_advertising_packet *pkt, uint8_t pkt_size,
		uint8_t ad_type1, va_list *args)
{
	uint8_t ad_type = ad_type1;
	uint8_t index = 0;
	struct ad_types info;

	memset(pkt, 0, pkt_size);

	while (ad_type != EIR_INVALID) {

		if(index > 31) {
			errno = EINVAL;
			return -1;
		}

		switch (ad_type) {
		case EIR_NAME_COMPLETE:
			info.data = va_arg(*args, uint8_t *);
			info.size = strlen((char*)info.data);

			pkt->data[index++] = info.size + 1;
			pkt->data[index++] = EIR_NAME_COMPLETE;
			memcpy(&pkt->data[index], info.data, info.size);
			index += info.size;
			break;
		case EIR_TX_POWER:
			info.size = 1;

			pkt->data[index++] = info.size + 1;
			pkt->data[index++] = EIR_TX_POWER;
			pkt->data[index++] = (uint8_t)va_arg(*args, int);
			break;
		case EIR_SLAVE_CONN_INT_RANGE:
			info.range = va_arg(*args, le_slave_connection_interval_range *);
			info.size  = sizeof(le_slave_connection_interval_range);

			pkt->data[index++] = info.size + 1;
			pkt->data[index++] = EIR_SLAVE_CONN_INT_RANGE;
			memcpy(&pkt->data[index], info.range, info.size);
                        index += info.size;
			break;
		case EIR_FLAGS:
			info.size = 1;

			pkt->data[index++] = info.size + 1;
			pkt->data[index++] = EIR_FLAGS;
			pkt->data[index++] = (uint8_t)va_arg(*args, int);
			break;
		case EIR_UUID128_SOME:
			info.uuid = va_arg(*args, bt_uuid_t *);
			info.size = BT_UUID128 / 8;

			pkt->data[index++] = info.size + 1;
			pkt->data[index++] = EIR_UUID128_SOME;
			memcpy(&(pkt->data[index]), &info.uuid->value, info.size);
                        index += info.size;
			break;
		default:
			errno = EINVAL;
			return -1;
		}

		ad_type = (uint8_t)va_arg(*args, int);
	}

	pkt->length = index;

	return 0;
}

int hci_le_set_advertise_params(int dd, uint16_t min_interval, uint16_t max_interval,
		uint8_t advtype, uint8_t own_bdaddr_type, uint8_t direct_bdaddr_type,
		bdaddr_t direct_bdaddr, uint8_t chan_map, uint8_t filter, int to)
{
	struct hci_request rq;
	le_set_advertising_parameters_cp adv_params_cp;
	uint8_t  status;

	memset(&adv_params_cp, 0, sizeof(adv_params_cp));
	adv_params_cp.min_interval	 = min_interval;
	adv_params_cp.max_interval	 = max_interval;
	adv_params_cp.advtype		 = advtype;
	adv_params_cp.own_bdaddr_type	 = own_bdaddr_type;
	adv_params_cp.direct_bdaddr_type = direct_bdaddr_type;
	adv_params_cp.direct_bdaddr	 = direct_bdaddr;
	adv_params_cp.chan_map		 = chan_map;
	adv_params_cp.filter		 = filter;

	memset(&rq, 0, sizeof(rq));
	rq.ogf	  = OGF_LE_CTL;
	rq.ocf	  = OCF_LE_SET_ADVERTISING_PARAMETERS;
	rq.cparam = &adv_params_cp;
	rq.clen	  = LE_SET_ADVERTISING_PARAMETERS_CP_SIZE;
	rq.rparam = &status;
	rq.rlen	  = 1;

	if (hci_send_req(dd, &rq, to) < 0)
		return -1;

	if (status) {
		errno = EIO;
		return -1;
	}

	return 0;
}

int hci_le_set_advertise_data(int dd, uint8_t ad_type1, ...)
{
	struct hci_request rq;
	le_set_advertising_data_cp adv_data_cp;
	uint8_t to;
	uint8_t status;
	va_list args;

	va_start(args, ad_type1);
	if(parse_ad_types((struct generic_advertising_packet *)&adv_data_cp,
				sizeof(le_set_advertising_data_cp),ad_type1, &args) < 0) {
		va_end(args);
		return -1;
	}
	to = (uint8_t)va_arg(args, int);
	va_end(args);

	memset(&rq, 0, sizeof(rq));
	rq.ogf	  = OGF_LE_CTL;
	rq.ocf	  = OCF_LE_SET_ADVERTISING_DATA;
	rq.cparam = &adv_data_cp;
	rq.clen	  = LE_SET_ADVERTISING_DATA_CP_SIZE;
	rq.rparam = &status;
	rq.rlen	  = 1;

	if (hci_send_req(dd, &rq, to) < 0)
		return -1;

	if (status) {
		errno = EIO;
		return -1;
	}

	return 0;
}

int hci_le_set_scan_response_data(int dd, uint8_t ad_type1, ...)
{
	struct hci_request rq;
	le_set_scan_response_data_cp adv_scan_rsp_cp;
	uint8_t to;
	uint8_t status;
	va_list args;

	va_start(args, ad_type1);
	if(parse_ad_types((struct generic_advertising_packet *)&adv_scan_rsp_cp,
				sizeof(le_set_scan_response_data_cp), ad_type1, &args) < 0) {
		va_end(args);
		return -1;
	}
	to = (uint8_t)va_arg(args, int);
	va_end(args);

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_LE_CTL;
	rq.ocf    = OCF_LE_SET_SCAN_RESPONSE_DATA;
	rq.cparam = &adv_scan_rsp_cp;
	rq.clen   = LE_SET_SCAN_RESPONSE_DATA_CP_SIZE;
	rq.rparam = &status;
	rq.rlen   = 1;

	if (hci_send_req(dd, &rq, to) < 0)
		return -1;

	if (status) {
		errno = EIO;
		return -1;
	}

	return 0;

}
