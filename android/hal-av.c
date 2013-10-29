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

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "hal-log.h"
#include "hal.h"
#include "hal-msg.h"
#include "hal-ipc.h"

static const btav_callbacks_t *cbs = NULL;

static bool interface_ready(void)
{
	return cbs != NULL;
}

static void handle_connection_state(void *buf)
{
	struct hal_ev_av_connection_state *ev = buf;

	if (cbs->connection_state_cb)
		cbs->connection_state_cb(ev->state,
						(bt_bdaddr_t *) (ev->bdaddr));
}

static void handle_audio_state(void *buf)
{
	struct hal_ev_av_audio_state *ev = buf;

	if (cbs->audio_state_cb)
		cbs->audio_state_cb(ev->state, (bt_bdaddr_t *)(ev->bdaddr));
}

/* will be called from notification thread context */
void bt_notify_av(uint16_t opcode, void *buf, uint16_t len)
{
	if (!interface_ready())
		return;

	switch (opcode) {
	case HAL_EV_AV_CONNECTION_STATE:
		handle_connection_state(buf);
		break;
	case HAL_EV_AV_AUDIO_STATE:
		handle_audio_state(buf);
		break;
	default:
		DBG("Unhandled callback opcode=0x%x", opcode);
		break;
	}
}

static bt_status_t av_connect(bt_bdaddr_t *bd_addr)
{
	struct hal_cmd_av_connect cmd;

	DBG("");

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	memcpy(cmd.bdaddr, bd_addr, sizeof(cmd.bdaddr));

	return hal_ipc_cmd(HAL_SERVICE_ID_A2DP, HAL_OP_AV_CONNECT,
					sizeof(cmd), &cmd, NULL, NULL, NULL);
}

static bt_status_t av_disconnect(bt_bdaddr_t *bd_addr)
{
	struct hal_cmd_av_disconnect cmd;

	DBG("");

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	memcpy(cmd.bdaddr, bd_addr, sizeof(cmd.bdaddr));

	return hal_ipc_cmd(HAL_SERVICE_ID_A2DP, HAL_OP_AV_DISCONNECT,
					sizeof(cmd), &cmd, NULL, NULL, NULL);
}

static bt_status_t av_init(btav_callbacks_t *callbacks)
{
	DBG("");

	cbs = callbacks;

	/* TODO: enable service */

	return BT_STATUS_SUCCESS;
}

static void av_cleanup()
{
	DBG("");

	if (!interface_ready())
		return;

	/* TODO: disable service */

	cbs = NULL;
}

static btav_interface_t iface = {
	.size = sizeof(iface),
	.init = av_init,
	.connect = av_connect,
	.disconnect = av_disconnect,
	.cleanup = av_cleanup
};

btav_interface_t *bt_get_av_interface()
{
	return &iface;
}
