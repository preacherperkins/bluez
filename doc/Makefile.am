XML_SOURCES = \
	adapter-api.xml \
	agent-api.xml \
	device-api.xml \
	gatt-api.xml \
	media-api.xml \
	network-api.xml \
	profile-api.xml \
	arc-api.xml

org.bluez.xml: $(XML_SOURCES) head.xml foot.xml
	cat head.xml $(XML_SOURCES) foot.xml > $@

DBUS_IFACES     = org.bluez.xml
dbus_ifacedir   = $(datadir)/dbus-1/interfaces
dbus_iface_DATA = $(DBUS_IFACES)

EXTRA_DIST=                                     \
        $(DBUS_IFACES)

