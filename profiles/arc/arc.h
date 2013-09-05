/*-*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
**
**  Author: Dirk-Jan C. Binnema <dirk@morseproject.com>
**
** Copyright (c) 2013 Morse Project. All rights reserved.
**
** @file
*/

#ifndef __ARC_H__
#define __ARC_H__

#include <glib.h>

G_BEGIN_DECLS

/* DBus */
#define ARC_OBJECT_PATH	        "/org/bluez"
#define ARC_SERVER_IFACE	"org.bluez.ARCServer1"
#define ARC_PROXY_IFACE	        "org.bluez.ARCProxy1"

/* the GATT service */
#define ARC_SERVICE_UUID        "939DCB26-B6CB-4519-B6CA-A0D617C403BB"

/* characteristics */
#define ARC_REQUEST_UUID	"8D4DD795-D603-4D0A-93F7-02DE511F4B70"
#define ARC_EVENT_UUID		"1BA9AF1F-686E-4E1B-90A7-6945584BECA0"
#define ARC_RESULT_UUID		"B7F2D698-B677-4B93-8D9B-83E3B6ED9AE0"
#define ARC_TARGET_UUID		"F6FECADF-4148-46F8-B63A-47427634A5D5"
#define ARC_DEVNAME_UUID        "6C39EC45-C012-47B5-ADC2-B98A91EA0494"


#define ARC_PROP_RESULT  "Result"
#define ARC_PROP_TARGET  "Target"
#define ARC_PROP_EVENT   "Event"
#define ARC_PROP_REQUEST "Request"



/* ids for the various handles */
typedef enum {
	ARC_EVENT_ID = 0,
	ARC_REQUEST_ID,
	ARC_RESULT_ID,
	ARC_TARGET_ID,
	ARC_DEVNAME_ID,

	ARC_ID_NUM
} ARCID;


#define ANSI_RED		"\x1b[31m"
#define ANSI_GREEN		"\x1b[32m"
#define ANSI_BRIGHT		"\x1b[1m"
#define ANSI_DEFAULT		"\x1b[0m"

/* special colorful log */
void arc_log (const char *col, const char *frm, ...);

#define COLOR_DBG 1
#ifdef COLOR_DBG
#undef DBG
#define DBG(args,...) do{arc_log(ANSI_GREEN ANSI_BRIGHT, args,##__VA_ARGS__);}while(0)
#define error(args,...) do{arc_log(ANSI_RED ANSI_BRIGHT, args,##__VA_ARGS__);}while(0)
#endif /*COLOR_DBG*/


int arc_probe_proxy (struct btd_service *service);
void arc_remove_proxy (struct btd_service *service);

int  arc_probe_server (struct btd_profile *profile, struct btd_adapter *adapter);
void arc_remove_server (struct btd_profile *profile, struct btd_adapter *adapter);


ARCID arc_prop_to_id (const char *prop) G_GNUC_CONST;
const char* arc_id_to_prop (ARCID id) G_GNUC_CONST;

G_END_DECLS

#endif /* __ARC_H__ */
