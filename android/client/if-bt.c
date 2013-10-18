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

#include "if-main.h"

const bt_interface_t *if_bluetooth;

static char *bdaddr2str(const bt_bdaddr_t *bd_addr)
{
	static char buf[18];

	return bt_bdaddr_t2str(bd_addr, buf);
}

static char *btuuid2str(const bt_uuid_t *uuid)
{
	static char buf[39];

	return bt_uuid_t2str(uuid, buf);
}

static bt_scan_mode_t str2btscanmode(const char *str)
{
	bt_scan_mode_t v = str2bt_scan_mode_t(str);

	if ((int) v != -1)
		return v;

	haltest_warn("WARN: %s cannot convert %s\n", __func__, str);
	return (bt_scan_mode_t) atoi(str);
}

static bt_ssp_variant_t str2btsspvariant(const char *str)
{
	bt_ssp_variant_t v = str2bt_ssp_variant_t(str);

	if ((int) v != -1)
		return v;

	haltest_warn("WARN: %s cannot convert %s\n", __func__, str);
	return (bt_ssp_variant_t) atoi(str);
}

static bt_property_type_t str2btpropertytype(const char *str)
{
	bt_property_type_t v = str2bt_property_type_t(str);

	if ((int) v != -1)
		return v;

	haltest_warn("WARN: %s cannot convert %s\n", __func__, str);
	return (bt_property_type_t) atoi(str);
}

static char *btproperty2str(bt_property_t property)
{
	static char buf[4096];
	char *p;

	p = buf + sprintf(buf, "type=%s len=%d val=",
			  bt_property_type_t2str(property.type), property.len);

	switch (property.type) {
	case BT_PROPERTY_BDNAME:
	case BT_PROPERTY_REMOTE_FRIENDLY_NAME:
		sprintf(p, "%*s", property.len,
			((bt_bdname_t *) property.val)->name);
		break;

	case BT_PROPERTY_BDADDR:
		sprintf(p, "%s", bdaddr2str((bt_bdaddr_t *) property.val));
		break;

	case BT_PROPERTY_CLASS_OF_DEVICE:
		sprintf(p, "%06x", *((int *) property.val));
		break;

	case BT_PROPERTY_TYPE_OF_DEVICE:
		sprintf(p, "%s", bt_device_type_t2str(
					*((bt_device_type_t *) property.val)));
		break;

	case BT_PROPERTY_REMOTE_RSSI:
		sprintf(p, "%d", *((char *) property.val));
		break;

	case BT_PROPERTY_ADAPTER_SCAN_MODE:
		sprintf(p, "%s",
			bt_scan_mode_t2str(*((bt_scan_mode_t *) property.val)));
		break;

	case BT_PROPERTY_ADAPTER_DISCOVERY_TIMEOUT:
		sprintf(p, "%d", *((int *) property.val));
		break;

	case BT_PROPERTY_ADAPTER_BONDED_DEVICES:
		{
			int count = property.len / sizeof(bt_bdaddr_t);
			char *ptr = property.val;

			strcat(p, "{");

			while (count--) {
				strcat(p, bdaddr2str((bt_bdaddr_t *) ptr));
				if (count)
					strcat(p, ", ");
				ptr += sizeof(bt_bdaddr_t);
			}

			strcat(p, "}");

		}
		break;

	case BT_PROPERTY_UUIDS:
		{
			int count = property.len / sizeof(bt_uuid_t);
			char *ptr = property.val;

			strcat(p, "{");

			while (count--) {
				strcat(p, btuuid2str((bt_uuid_t *) ptr));
				if (count)
					strcat(p, ", ");
				ptr += sizeof(bt_uuid_t);
			}

			strcat(p, "}");

		}
		break;

	case BT_PROPERTY_SERVICE_RECORD:
		{
			bt_service_record_t *rec = property.val;

			sprintf(p, "{%s, %d, %s}", btuuid2str(&rec->uuid),
				rec->channel, rec->name);
		}
		break;

	default:
		sprintf(p, "%p", property.val);
	}

	return buf;
}

static void dump_properties(int num_properties, bt_property_t *properties)
{
	int i;

	for (i = 0; i < num_properties; i++) {
		/*
		 * properities sometimes come unaligned hence memcp to
		 * aligned buffer
		 */
		bt_property_t prop;
		memcpy(&prop, properties + i, sizeof(prop));

		haltest_info("prop: %s\n", btproperty2str(prop));
	}
}

static void adapter_state_changed_cb(bt_state_t state)
{
	haltest_info("%s: state=%s\n", __func__, bt_state_t2str(state));
}

static void adapter_properties_cb(bt_status_t status,
	int num_properties, bt_property_t *properties)
{
	haltest_info("%s: status=%s num_properties=%d\n",
	       __func__, bt_status_t2str(status), num_properties);

	dump_properties(num_properties, properties);
}

static void remote_device_properties_cb(bt_status_t status,
	bt_bdaddr_t *bd_addr, int num_properties, bt_property_t *properties)
{
	haltest_info("%s: status=%s bd_addr=%s num_properties=%d\n",
	       __func__, bt_status_t2str(status), bdaddr2str(bd_addr),
	       num_properties);

	dump_properties(num_properties, properties);
}

static void device_found_cb(int num_properties, bt_property_t *properties)
{
	haltest_info("%s: num_properties=%d\n", __func__, num_properties);

	dump_properties(num_properties, properties);
}

static void discovery_state_changed_cb(bt_discovery_state_t state)
{
	haltest_info("%s: state=%s\n", __func__,
		bt_discovery_state_t2str(state));
}

static void pin_request_cb(bt_bdaddr_t *remote_bd_addr, bt_bdname_t *bd_name,
	uint32_t cod)
{
	haltest_info("%s: remote_bd_addr=%s bd_name=%s cod=%06x\n", __func__,
			       bdaddr2str(remote_bd_addr), bd_name->name, cod);
}

static void ssp_request_cb(bt_bdaddr_t *remote_bd_addr, bt_bdname_t *bd_name,
				uint32_t cod, bt_ssp_variant_t pairing_variant,
				uint32_t pass_key)
{
	haltest_info("%s: remote_bd_addr=%s bd_name=%s cod=%06x pairing_variant=%s pass_key=%d\n",
		     __func__, bdaddr2str(remote_bd_addr), bd_name->name, cod,
		     bt_ssp_variant_t2str(pairing_variant), pass_key);
}

static void bond_state_changed_cb(bt_status_t status,
					bt_bdaddr_t *remote_bd_addr,
					bt_bond_state_t state)
{
	haltest_info("%s: status=%s remote_bd_addr=%s state=%s\n", __func__,
		       bt_status_t2str(status), bdaddr2str(remote_bd_addr),
		       bt_bond_state_t2str(state));
}

static void acl_state_changed_cb(bt_status_t status,
					bt_bdaddr_t *remote_bd_addr,
					bt_acl_state_t state)
{
	haltest_info("%s: status=%s remote_bd_addr=%s state=%s\n", __func__,
		       bt_status_t2str(status), bdaddr2str(remote_bd_addr),
		       bt_acl_state_t2str(state));
}

static void thread_evt_cb(bt_cb_thread_evt evt)
{
	haltest_info("%s: evt=%s\n", __func__, bt_cb_thread_evt2str(evt));
}

static void dut_mode_recv_cb(uint16_t opcode, uint8_t *buf, uint8_t len)
{
	haltest_info("%s\n", __func__);
}

static void le_test_mode_cb(bt_status_t status, uint16_t num_packets)
{
	haltest_info("%s %s %d\n", __func__, bt_state_t2str(status),
								num_packets);
}

static bt_callbacks_t bt_callbacks = {
	.size = sizeof(bt_callbacks),
	.adapter_state_changed_cb = adapter_state_changed_cb,
	.adapter_properties_cb = adapter_properties_cb,
	.remote_device_properties_cb = remote_device_properties_cb,
	.device_found_cb = device_found_cb,
	.discovery_state_changed_cb = discovery_state_changed_cb,
	.pin_request_cb = pin_request_cb,
	.ssp_request_cb = ssp_request_cb,
	.bond_state_changed_cb = bond_state_changed_cb,
	.acl_state_changed_cb = acl_state_changed_cb,
	.thread_evt_cb = thread_evt_cb,
	.dut_mode_recv_cb = dut_mode_recv_cb,
	.le_test_mode_cb = le_test_mode_cb
};

static void init_p(int argc, const char **argv)
{
	int err;
	const hw_module_t *module;
	hw_device_t *device;

	err = hw_get_module(BT_HARDWARE_MODULE_ID, &module);
	if (err) {
		haltest_error("he_get_module returned %d\n", err);
		return;
	}

	err = module->methods->open(module, BT_HARDWARE_MODULE_ID, &device);
	if (err) {
		haltest_error("module->methods->open returned %d\n", err);
		return;
	}

	if_bluetooth =
	    ((bluetooth_device_t *) device)->get_bluetooth_interface();
	if (!if_bluetooth) {
		haltest_error("get_bluetooth_interface returned NULL\n");
		return;
	}

	EXEC(if_bluetooth->init, &bt_callbacks);
}

static void cleanup_p(int argc, const char **argv)
{
	RETURN_IF_NULL(if_bluetooth);

	EXECV(if_bluetooth->cleanup);

	if_bluetooth = NULL;
}

static void enable_p(int argc, const char **argv)
{
	RETURN_IF_NULL(if_bluetooth);

	EXEC(if_bluetooth->enable);
}

static void disable_p(int argc, const char **argv)
{
	RETURN_IF_NULL(if_bluetooth);

	EXEC(if_bluetooth->disable);
}

static void get_adapter_properties_p(int argc, const char **argv)
{
	RETURN_IF_NULL(if_bluetooth);

	EXEC(if_bluetooth->get_adapter_properties);
}

static void get_adapter_property_p(int argc, const char **argv)
{
	int type = str2btpropertytype(argv[2]);

	RETURN_IF_NULL(if_bluetooth);

	EXEC(if_bluetooth->get_adapter_property, type);
}

static void set_adapter_property_p(int argc, const char **argv)
{
	bt_property_t property;
	bt_scan_mode_t mode;
	int timeout;

	RETURN_IF_NULL(if_bluetooth);

	property.type = str2btpropertytype(argv[2]);

	switch (property.type) {
	case BT_PROPERTY_BDNAME:
		property.len = strlen(argv[3]) + 1;
		property.val = (char *) argv[3];
		break;

	case BT_PROPERTY_ADAPTER_SCAN_MODE:
		mode = str2btscanmode(argv[3]);
		property.len = sizeof(bt_scan_mode_t);
		property.val = &mode;
		break;

	case BT_PROPERTY_ADAPTER_DISCOVERY_TIMEOUT:
		timeout = atoi(argv[3]);
		property.val = &timeout;
		property.len = sizeof(timeout);
		break;

	default:
		haltest_error("Invalid property %s\n", argv[3]);
		return;
	}

	EXEC(if_bluetooth->set_adapter_property, &property);
}

static void get_remote_device_properties_p(int argc, const char **argv)
{
	bt_bdaddr_t addr;

	RETURN_IF_NULL(if_bluetooth);

	str2bt_bdaddr_t(argv[2], &addr);

	EXEC(if_bluetooth->get_remote_device_properties, &addr);
}

static void get_remote_device_property_p(int argc, const char **argv)
{
	bt_property_type_t type;
	bt_bdaddr_t addr;

	RETURN_IF_NULL(if_bluetooth);

	str2bt_bdaddr_t(argv[2], &addr);
	type = str2btpropertytype(argv[3]);

	EXEC(if_bluetooth->get_remote_device_property, &addr, type);
}

static void set_remote_device_property_p(int argc, const char **argv)
{
	bt_property_t property;
	bt_bdaddr_t addr;

	RETURN_IF_NULL(if_bluetooth);

	str2bt_bdaddr_t(argv[2], &addr);
	property.type = str2btpropertytype(argv[3]);

	switch (property.type) {
	case BT_PROPERTY_REMOTE_FRIENDLY_NAME:
		property.len = strlen(argv[4]);
		property.val = (char *) argv[4];
		break;

	default:
		return;
	}

	EXEC(if_bluetooth->set_remote_device_property, &addr, &property);
}

static void get_remote_service_record_p(int argc, const char **argv)
{
	bt_bdaddr_t addr;
	bt_uuid_t uuid;

	RETURN_IF_NULL(if_bluetooth);

	str2bt_bdaddr_t(argv[2], &addr);
	str2bt_uuid_t(argv[3], &uuid);

	EXEC(if_bluetooth->get_remote_service_record, &addr, &uuid);
}

static void get_remote_services_p(int argc, const char **argv)
{
	bt_bdaddr_t addr;

	RETURN_IF_NULL(if_bluetooth);

	str2bt_bdaddr_t(argv[2], &addr);

	EXEC(if_bluetooth->get_remote_services, &addr);
}

static void start_discovery_p(int argc, const char **argv)
{
	RETURN_IF_NULL(if_bluetooth);

	EXEC(if_bluetooth->start_discovery);
}

static void cancel_discovery_p(int argc, const char **argv)
{
	RETURN_IF_NULL(if_bluetooth);

	EXEC(if_bluetooth->cancel_discovery);
}

static void create_bond_p(int argc, const char **argv)
{
	bt_bdaddr_t addr;

	RETURN_IF_NULL(if_bluetooth);

	str2bt_bdaddr_t(argv[2], &addr);

	EXEC(if_bluetooth->create_bond, &addr);
}

static void remove_bond_p(int argc, const char **argv)
{
	bt_bdaddr_t addr;

	RETURN_IF_NULL(if_bluetooth);

	str2bt_bdaddr_t(argv[2], &addr);

	EXEC(if_bluetooth->remove_bond, &addr);
}

static void cancel_bond_p(int argc, const char **argv)
{
	bt_bdaddr_t addr;

	RETURN_IF_NULL(if_bluetooth);

	str2bt_bdaddr_t(argv[2], &addr);

	EXEC(if_bluetooth->cancel_bond, &addr);
}

static void pin_reply_p(int argc, const char **argv)
{
	bt_bdaddr_t addr;
	bt_pin_code_t pin;
	int pin_len = 0;
	int accept;

	RETURN_IF_NULL(if_bluetooth);

	if (argc < 3) {
		haltest_error("No address specified\n");
		return;
	}
	str2bt_bdaddr_t(argv[2], &addr);

	if (argc >= 4) {
		accept = 1;
		pin_len = strlen(argv[3]);
		memcpy(pin.pin, argv[3], pin_len);
	}

	EXEC(if_bluetooth->pin_reply, &addr, accept, pin_len, &pin);
}

static void ssp_reply_p(int argc, const char **argv)
{
	bt_bdaddr_t addr;
	bt_ssp_variant_t var;
	int accept;
	int passkey;

	RETURN_IF_NULL(if_bluetooth);

	if (argc < 3) {
		haltest_error("No address specified\n");
		return;
	}
	str2bt_bdaddr_t(argv[2], &addr);
	if (argc < 4) {
		haltest_error("No ssp variant specified\n");
		return;
	}
	var = str2btsspvariant(argv[3]);
	if (argc < 5) {
		haltest_error("No accept value specified\n");
		return;
	}
	accept = atoi(argv[4]);
	passkey = 0;

	if (accept && var == BT_SSP_VARIANT_PASSKEY_ENTRY && argc >= 5)
		passkey = atoi(argv[4]);

	EXEC(if_bluetooth->ssp_reply, &addr, var, accept, passkey);
}

static void get_profile_interface_p(int argc, const char **argv)
{
	const char *id = argv[2];
	const void **pif = NULL;
	const void *dummy = NULL;

	RETURN_IF_NULL(if_bluetooth);

	if (strcmp(BT_PROFILE_HANDSFREE_ID, id) == 0)
		pif = &dummy; /* TODO: change when if_hf is there */
	else if (strcmp(BT_PROFILE_ADVANCED_AUDIO_ID, id) == 0)
		pif = &dummy; /* TODO: change when if_av is there */
	else if (strcmp(BT_PROFILE_HEALTH_ID, id) == 0)
		pif = &dummy; /* TODO: change when if_hl is there */
	else if (strcmp(BT_PROFILE_SOCKETS_ID, id) == 0)
		pif = &dummy; /* TODO: change when if_sock is there */
	else if (strcmp(BT_PROFILE_HIDHOST_ID, id) == 0)
		pif = &dummy; /* TODO: change when if_hh is there */
	else if (strcmp(BT_PROFILE_PAN_ID, id) == 0)
		pif = &dummy; /* TODO: change when if_pan is there */
	else if (strcmp(BT_PROFILE_AV_RC_ID, id) == 0)
		pif = &dummy; /* TODO: change when if_rc is there */
	else
		haltest_error("%s is not correct for get_profile_interface\n",
		     id);

	if (pif != NULL) {
		*pif = if_bluetooth->get_profile_interface(id);
		haltest_info("get_profile_interface(%s) : %p\n", id, *pif);
	}
}

static void dut_mode_configure_p(int argc, const char **argv)
{
	uint8_t mode;

	RETURN_IF_NULL(if_bluetooth);

	mode = strtol(argv[2], NULL, 0);

	EXEC(if_bluetooth->dut_mode_configure, mode);
}

static struct method methods[] = {
	STD_METHOD(init),
	STD_METHOD(cleanup),
	STD_METHOD(enable),
	STD_METHOD(disable),
	STD_METHOD(get_adapter_properties),
	STD_METHOD(get_adapter_property),
	STD_METHOD(set_adapter_property),
	STD_METHOD(get_remote_device_properties),
	STD_METHOD(get_remote_device_property),
	STD_METHOD(set_remote_device_property),
	STD_METHOD(get_remote_service_record),
	STD_METHOD(get_remote_services),
	STD_METHOD(start_discovery),
	STD_METHOD(cancel_discovery),
	STD_METHOD(create_bond),
	STD_METHOD(remove_bond),
	STD_METHOD(cancel_bond),
	STD_METHOD(pin_reply),
	STD_METHOD(ssp_reply),
	STD_METHOD(get_profile_interface),
	STD_METHOD(dut_mode_configure),
	END_METHOD
};

const struct interface bluetooth_if = {
	.name = "adapter",
	.methods = methods
};
