The AUI Profile
======================

## Introduction

This is not really a profile in the strict sense. It is a plugin that defines
the simple Aether User Interface (AUI) GATT service.  It also defines and
controls the AUI advertisement packets.  This requires interfacing with the HCI
device directly and so also extends the `lib/hci_lib.h` interface to include
writing advertisement parameters, advertisement data, and scan response data.

## The AUI GATT Service

This is best described through a [JSON](http://json.org) representation.

```javascript
{
  AUI_RCV_UUID : {
    properties : WRITE,
    value : <cpu_mcu_message.h mcu -> cpu cmds>
  },
  AUI_SEND_UUID : {
    properties : READ | NOTIFY,
    value : <cpu_mcu_message.h cpu -> mcu cmds>,
    descriptors : CCCD_NOTIFICATION_EN
  }
  AUI_DEVID_UUID : {
    properties : READ,
    value : <DEV1234>
  }
}
```

Here are the defines for the AUI UUIDs.

```c
#define AUI_SERVICE_UUID  "cf0244d6-5081-4e0a-8236-b486a3985162"
#define AUI_RCV_UUID      "409497e8-c42d-4870-aa4f-fe4e5b516410"
#define AUI_SEND_UUID     "9e847894-d33c-4271-a3a5-bf7849fc0e03"
#define AUI_DEVID_UUID    "a8bb3a1f-0afa-463a-83ca-a10054087787"
```

When a client writes to the AUI_RCV_UUID characteristic, the AUI service emits a
DBUS signal

The only way for the server to send information back to the client is via the
GATT character value notification in the `AUI_SEND_UUD` characteristic. The client
must enable this asynchronous messaging via the `CCCD_NOTIFICATION_EN` descriptor.
This notification is triggered by a DBUS client.

## DBus-interfaces

Besides the normal bluez API, there exists a property called "RemoteCmd" in the
adapter object path (usually `/org/bluez/hci0`). This property is part of a new
interface called

_org.bluez.AuiManager1_

RemoteCmd has a type "y" (unsigned 8-bit integer).  When a client updates the
AUI_RCV_UUID value, bluez emits a PropertyChanged signal with the new value.

This value should match the following enumeration:

```c
enum {
	NOP                = 255,
	VOL_UP             = 10,
	VOL_DOWN           = 11,
	NEXT_TRACK         = 12,
	PREV_TRACK         = 13,
	NEXT_SET           = 14,
	PREV_SET           = 15,
	PLAY_PAUSE_TOGGLE  = 16
};
```

And here's a sample output when using `dbus-monitor --system`:

```
signal sender=:1.195 -> dest=(null destination) serial=11 path=/org/bluez/hci0; interface=org.freedesktop.DBus.Properties; member=PropertiesChanged
   string "org.bluez.AuiManager1"
   array [
      dict entry(
         string "RemoteCmd"
         variant             byte 17
      )
   ]
   array [
   ]
```

## HCI Interface Extension

There are a few commands that were added to support setting the advertisement
parameters properly.  They are shown below


```c
int hci_le_set_advertise_params(int dd, uint16_t min_interval, uint16_t max_interval,
                        uint8_t advtype, uint8_t own_bdaddr_type, uint8_t direct_bdaddr_type,
                        bdaddr_t direct_bdaddr, uint8_t chan_map, uint8_t filter, int to);

int hci_le_set_advertise_data(int dd, uint8_t ad_type1, ...);

int hci_le_set_scan_response_data(int dd, uint8_t ad_type1, ...);
```

Because the advertising data and scan response data is variable, we need to use
va_args.  The AD data type has a key/value format, so these functions expect this
in the va_args section.  In fact, these two functions follow the same approach used
by `gatt_service_add()`

## Advertising

### Packet

Here's the output of `hcidump -X` when scanning with `hcitool lescan`

```
> HCI Event: LE Meta Event (0x3e) plen 68
    LE Advertising Report
      ADV_IND - Connectable undirected advertising (0)
      bdaddr 00:1A:7D:DA:71:13 (Public)
      Flags: 0x1a
      Unknown type 0x06 with 16 bytes data
      RSSI: -50
      SCAN_RSP - Scan Response (4)
      bdaddr 00:1A:7D:DA:71:13 (Public)
      Complete local name: 'Aether UI Profile'
      Unknown type 0x12 with 4 bytes data
      RSSI: -50
```

hcidump does not properly decode the "More 128-bit UUIDs Available" service
type (0x6).  The "Unknown type" actually represents the AUI_SERVICE_UUID.

Similarly, 0x12 is actually the "Slave Connection Interval Range". The minimum
range is set to 0x50, maximum is set to 0x320

### Control

The advertisement is done only when there are no BLE connections.



## Building and running

   To run Bluez with AUI-support, first get the version that supports it:
```
 $ git clone -b aether-bluez-aui git@github.com:MorseProject/bluez
```

   This version is based on Bluez-master, currently set to v5.21.

   Then, you can build it with:

```
 $ autoreconf -i; ./configure --enable-experimental --enable-maintainer-mode --enable-debug --enable-tools --enable-monitor --enable-client --disable-systemd; make clean; make
```
__Note --disable-systemd is require if you're running ubuntu. If running on Fedora or Cone, then this should not be used__

   The build-process should give you appropriate warnings when you miss some
   dependencies.

   After this has been completed succesfully, you can try to start it. Before
   doing so, it's important to shutdown any running bluetooth-daemons:

```
  $ sudo systemctl stop bluetooth.service
```

   (or the equivalent for /SysV-init/, /upstart/, etc.)

   Then, start it with:

```
 $ sudo ./bluetoothd -nEd -p aui
```

   This starts `bluetoothd` with _just_ the AUI-profile

## Issues

* Current implementation attempts to disable advertising in the callback when
the adapter is removed. This will always fail since bluez triggers this callback
_after_ it disables the adapter

* If there are multiple connections, advertisement will get enabled after only
  one client disconnects

