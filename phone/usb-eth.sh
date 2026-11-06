#!/system/bin/sh
# Runs as root from Magisk's /data/adb/service.d/ at boot.
#
# 1. Renames the ESP32's USB network interface usb0 -> eth0, so Android's
#    Ethernet support uses it.
# 2. Gives the phone the address 192.168.77.2/24 on eth0 and a routing rule, so
#    apps (the browser) can reach the ESP32 setup page at http://192.168.77.1
#
# Table 254 is the "main" routing table. The name "main" is not accepted by
# the ip tool on Android, so the number is used.

# wait until Android has finished booting
while [ "$(getprop sys.boot_completed)" != "1" ]; do sleep 5; done

while true; do
  # the ESP32 appears as usb0 every time it is plugged in
  if [ -d /sys/class/net/usb0 ]; then
    ip link set usb0 down
    ip link set usb0 name eth0
    ip link set eth0 up
  fi

  if [ -d /sys/class/net/eth0 ]; then
    ip addr show eth0 | grep -q "192.168.77.2" || ip addr add 192.168.77.2/24 dev eth0
    ip rule | grep -q "192.168.77.0/24" || ip rule add to 192.168.77.0/24 lookup 254 pref 100
  fi

  sleep 2
done
