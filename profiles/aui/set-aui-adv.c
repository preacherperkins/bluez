#include <error.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include "lib/uuid.h"

#include "ad_types.h"
#include "aui.h"
#include "hci_ext.h"

#define EIR_FLAGS                   0x01  /* flags */
#define EIR_UUID16_SOME             0x02  /* 16-bit UUID, more available */
#define EIR_UUID16_ALL              0x03  /* 16-bit UUID, all listed */
#define EIR_UUID32_SOME             0x04  /* 32-bit UUID, more available */
#define EIR_UUID32_ALL              0x05  /* 32-bit UUID, all listed */
#define EIR_UUID128_SOME            0x06  /* 128-bit UUID, more available */
#define EIR_UUID128_ALL             0x07  /* 128-bit UUID, all listed */
#define EIR_NAME_SHORT              0x08  /* shortened local name */
#define EIR_NAME_COMPLETE           0x09  /* complete local name */
#define EIR_TX_POWER                0x0A  /* transmit power level */
#define EIR_CLASS_OF_DEV            0x0D  /* Class of Device */
#define EIR_SSP_HASH                0x0E  /* SSP Hash */
#define EIR_SSP_RANDOMIZER          0x0F  /* SSP Randomizer */
#define EIR_DEVICE_ID               0x10  /* device ID */
#define EIR_GAP_APPEARANCE          0x19  /* GAP appearance */

/* Flags Descriptions */
#define EIR_LIM_DISC                0x01 /* LE Limited Discoverable Mode */
#define EIR_GEN_DISC                0x02 /* LE General Discoverable Mode */
#define EIR_BREDR_UNSUP             0x04 /* BR/EDR Not Supported */
#define EIR_CONTROLLER              0x08 /* Simultaneous LE and BR/EDR to Same
                                            Device Capable (Controller) */
#define EIR_SIM_HOST                0x10 /* Simultaneous LE and BR/EDR to Same
                                            Device Capable (Host) */

#define ANSI_RED		"\x1b[31m"
#define ANSI_GREEN		"\x1b[32m"
#define ANSI_BRIGHT		"\x1b[1m"
#define ANSI_DEFAULT		"\x1b[0m"

void adv_log (const char *col, const char *frm, ...);

#define COLOR_DBG 1
#ifdef COLOR_DBG
#undef DBG
#define DBG(args,...) do{adv_log(ANSI_GREEN ANSI_BRIGHT, args,##__VA_ARGS__);}while(0)
#endif /*COLOR_DBG*/

void adv_log (const char *col, const char *frm, ...)
{
        va_list	 args;
        char	str[64];

        if (!frm)
                return;

        va_start (args, frm);
        vsprintf(str, frm, args);
        va_end (args);

        printf("%s%s" ANSI_DEFAULT "\n", col, str);
}

static int set_powered_blocking(int dev_id)
{
        int ctl;

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
                close(ctl);
                return -1;
        }

        close(ctl);

        return 0;
}

static int set_advertise_params(int dev_id)
{
	int dd;
	bdaddr_t direct_address;

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

static int set_advertise_data(int dev_id)
{
	int dd;
	bt_uuid_t service_uuid;

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

static int set_scan_response_data(int dev_id, const char *complete_name)
{
	int dd;

	le_slave_connection_interval_range conn_range = {
		/* multiply by 1.25ms for wall clock time */
		.interval_min = 0x0050,
		.interval_max = 0x0320
	};

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		return -1;
	}

	return hci_le_set_scan_response_data(dd,
			EIR_NAME_COMPLETE,		(uint8_t *)complete_name,
			EIR_SLAVE_CONN_INT_RANGE,	&conn_range,
			EIR_INVALID, 1000);
}

static int set_advertise_enable(int dev_id, int enable)
{
	int dd;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		return -1;
	}

	return hci_le_set_advertise_enable(dd, enable, 1000);
}

int main(int argc, char *argv[])
{
        int dev_id = 0;
        char complete_name[16];

        if(getuid() != 0) {
                error(0, 0, "Must be run as root");
                goto error;
        }

        strcpy(complete_name, "Default Adv");

        if(argc > 3) {
                goto error;
        }

        if(argc == 2) {

                if(strncmp(argv[1], "hci", 3)!= 0) {
                        goto error;
                }

                dev_id = atoi(argv[1]+3);

                if(dev_id == 0 && strcmp(argv[1], "hci0") != 0) {
                        goto error;
                }
        }

        if(argc == 3) {

                if(strlen(argv[2]) < 16) {
                        strcpy(complete_name, argv[2]);
                }
                else {
                        goto error;
                }
        }

        DBG("Powering on hci%d", dev_id);
        if(set_powered_blocking(dev_id) < 0) {
                error(0, errno, "Could not power on device id %d", dev_id);
        }

        DBG("Setting advertising parameters for hci%d", dev_id);
        if(set_advertise_params(dev_id) < 0) {
                perror("Could not set advertising parameter data");
        }

        DBG("Setting advertising data for hci%d", dev_id);
        if(set_advertise_data(dev_id) < 0) {
                perror("Could not set advertising data");
        }

        DBG("Setting response data for hci%d", dev_id);
        if(set_scan_response_data(dev_id, complete_name) < 0) {
                perror("Could not set response data");
        }

        DBG("Enabling advertising for hci%d", dev_id);
        if(set_advertise_enable(dev_id, true) < 0) {
                perror("Could not enable advertising");
        }

        return 0;
error:
        error(1, EINVAL, "hciX <adv-name[16]>");
        return 1;
}
