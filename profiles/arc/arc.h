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
#define ARC_JID_UUID            "0677B8B1-D6DA-439E-BAB6-F22535991D05"


typedef enum {
	ARC_CHAR_FLAG_NONE	= 0,
	ARC_CHAR_FLAG_READABLE  = 1 << 0,
	ARC_CHAR_FLAG_WRITABLE  = 1 << 1,
	ARC_CHAR_FLAG_SERVER    = 1 << 2
} ARCCharFlags;

/**
 * ARC Characteristic
 *
 *
 *
 */
struct ARCChar {
	char		*name;
	GByteArray	*val, *val_scratch;
	guint16		 handle;
	guint16		 val_handle;
	ARCCharFlags     flags;
	char		*uuidstr;
	bt_uuid_t	 uuid;
	guint		 gatt_props;
	gboolean	 writing;

	/* we need to keep these for attrib... */
	char		 data[ATT_MAX_VALUE_LEN];
};
typedef struct ARCChar	 ARCChar;

/**
 * Set the value to some string
 *
 * @param achar
 * @param str a string, or NULL
 */
void arc_char_set_value_string (ARCChar *achar, const char *str);


/**
 * Get the ARChar's value as a string.
 *
 * @param achar
 *
 * @return the string (free with g_free)
 */
char* arc_char_get_value_string (ARCChar *achar);


/**
 * Clear the scratch-val, and copy the val's data into it
 *
 * @param achar
 * @param copy whether to copy val into scratch
 */
void arc_char_init_scratch (ARCChar *achar, gboolean copy);


/**
 * Create a new hashtable for UUID->ARCChars; free with
 * g_hash_table_unref
 *
 * @return  a new hash table
 */
GHashTable *arc_char_table_new (void);

/**
 * Add a characteristic to our table
 *
 * @param uuid
 * @param name
 *
 * @return the newly added ARCChar
 */
ARCChar *arc_char_table_add_char (GHashTable *chars, const char *uuid,
				  const char *name, ARCCharFlags flags);

/**
 * Get a characteristic from our table
 *
 * @param chars
 * @param uuid
 *
 * @return
 */
ARCChar* arc_char_table_find_by_uuid (GHashTable *chars,
				      const char *uuid);


/**
 * Get a characteristic based on an attribute
 *
 * @param table
 * @param attr
 *
 * @return
 */
ARCChar* arc_char_table_find_by_attr (GHashTable *table,
				      struct attribute* attr);


/**
 * Get a characteristic based on its name
 *
 * @param table
 * @param name
 *
 * @return
 */
ARCChar* arc_char_table_find_by_name (GHashTable *table,
				      const char *name);


/**
 * Clear all working data such as half-written chunked data
 *
 * @param table
 */
void arc_char_table_clear_working_data (GHashTable *table);

#define ARC_GATT_BLURB_PRE  0xfe
/**< prefix for an ARC blurb */

#define ARC_GATT_BLURB_POST 0xff
/**< suffix for an ARC blurb */


/* ids for the various handles */
typedef enum {
	ARC_EVENT_ID = 0,
	ARC_REQUEST_ID,
	ARC_RESULT_ID,
	ARC_TARGET_ID,
	ARC_DEVNAME_ID,
	ARC_JID_ID,

	ARC_ID_NUM
} ARCID;


int arc_probe_proxy (struct btd_service *service);
void arc_remove_proxy (struct btd_service *service);

int  arc_probe_server (struct btd_profile *profile, struct btd_adapter *adapter);
void arc_remove_server (struct btd_profile *profile, struct btd_adapter *adapter);


ARCID arc_prop_to_id (const char *prop) G_GNUC_CONST;
const char* arc_id_to_prop (ARCID id) G_GNUC_CONST;

void arc_dump_bytes (uint8_t *bytes, size_t len);

gboolean arc_enable_advertising (struct btd_adapter *adapter, uint8_t magic,
				 gboolean enable);

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



G_END_DECLS

#endif /* __ARC_H__ */
