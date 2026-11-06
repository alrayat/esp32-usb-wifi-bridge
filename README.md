# ESP32-S3 USB WiFi Bridge for Android

Give an Android phone internet over a USB cable, using an ESP32-S3 board as a
small "WiFi to USB network adapter". The phone sees a normal wired network
connection, and the ESP32 joins a WiFi network for it.

It also has a small web page, opened **from the phone itself over the USB
cable**, where you can scan for WiFi networks, pick one and type the password.
No PC and no second phone are needed to change the WiFi.

> **Status:** works on one phone (Samsung Galaxy M42 5G, SM-M426B, Android 13,
> One UI 5.1, kernel 4.19) with one ESP32-S3 board. Other phones are untested.
> See [Limitations](#limitations-and-honest-warnings).

## Why would anyone use this?

The original reason: a phone whose **WiFi chip and SIM/mobile data were broken**,
so it had no way online. USB was the only way in. Other reasons:

- Reuse a phone with dead WiFi hardware.
- Give a phone a stable wired-style connection from a WiFi network.
- Learn how USB network devices (CDC-NCM) work on an ESP32-S3.

## Who can use it / who should not

**Good fit**
- You are comfortable with a command line and flashing microcontrollers.
- You have an ESP32-S3 board with a **native USB (OTG) port**.
- You have an Android phone that you are **willing to root**
  (see warnings below) and a USB OTG adapter or cable.
- You want internet on a phone whose own WiFi/SIM is broken or unavailable.

**Not a good fit**
- You want a no-effort solution with no root and no flashing.
- You cannot or do not want to unlock your phone's bootloader and root it.
- You use a phone you rely on for banking, payments, work profiles or anything
  that must stay certified and unmodified.
- Your board is an ESP32 (original), ESP32-C3 or similar **without** a USB
  device peripheral. They cannot work. ESP32-S2 may work but is untested.
- You need a secure or high-speed link. See limitations.

## How it works

```
Router WiFi  <-->  ESP32-S3 (WiFi station)  <-->  USB (NCM)  <-->  Phone
```

1. The ESP32-S3 shows up on the phone as a USB network card (CDC-NCM).
2. It joins your WiFi and forwards network frames between WiFi and USB, using the
   same MAC address on both sides. The phone gets its address from your router.
3. Frames sent to `192.168.77.1` stay inside the ESP32. A tiny web server there
   serves the WiFi setup page.
4. On the phone, a root script renames the interface from `usb0` to `eth0`, so
   Android's Ethernet support uses it, and adds the address `192.168.77.2`
   plus a routing rule so the browser can reach the web page.

## Requirements

**Hardware**
- ESP32-S3 board with a native USB port (a board with two USB-C ports works best:
  one UART port for flashing, one USB/OTG port for the phone).
- USB OTG adapter or cable for the phone.

**Phone**
- Android phone with USB host (OTG) support.
- Kernel with the CDC-NCM network driver. Check with root (or adb):
  `zcat /proc/config.gz | grep -E "USBNET|CDC_NCM"` and look for `=y`.
- Unlocked bootloader and root (Magisk was used). **This is a big step, read the warnings.**

**PC**
- ESP-IDF v6.1 (tested). Newer or older versions may need small changes.

## Setup

### 1. Build and flash the firmware (PC)

Open the ESP-IDF terminal, then:

```
cd firmware
idf.py set-target esp32s3
idf.py menuconfig      # Example Configuration -> WiFi SSID / WiFi Password
idf.py -p COM4 build flash monitor
```

Use the ESP32's **UART/COM** port for flashing. Replace `COM4` with your port.
The WiFi name and password in menuconfig are only the default. Once you save a
network on the web page, the saved one is used.

Do **not** commit your own `sdkconfig`. It contains your WiFi password.

### 2. Install the phone script (rooted phone)

Copy `phone/usb-eth.sh` into Magisk's boot script folder and make it executable.
In a root shell (for example Termux, then `su`):

```
mkdir -p /data/adb/service.d
cp /sdcard/usb-eth.sh /data/adb/service.d/usb-eth.sh
chmod 755 /data/adb/service.d/usb-eth.sh
nohup sh /data/adb/service.d/usb-eth.sh > /dev/null 2>&1 &
```

It starts by itself after every reboot.

### 3. Connect

Plug the ESP32's **USB/OTG** port into the phone and wait about 15 seconds. The
phone should show an Ethernet connection and have internet.

## Using the WiFi setup page

1. On the phone, open `http://192.168.77.1` in a browser.
2. Tap **Scan networks**. Internet pauses for a few seconds while it scans.
3. Tap a network, type the password, tap **Save and connect**.
4. The ESP32 stores it in flash and reconnects. The status line at the top shows
   the connection.

If you save a wrong password, the page still opens (the USB link stays up), so
you can fix it.

## Troubleshooting

| Problem | What to check |
|---|---|
| No new network interface | `ls /sys/class/net` as root. Look for `usb0`/`eth0`. If nothing appears, the kernel may lack CDC-NCM or the OTG adapter or cable is the problem. |
| Interface exists but the Ethernet menu in Settings is greyed out | Normal on some Samsung builds. If the internet works, ignore it. |
| Web page does not open | Run `ip addr show eth0` (needs `192.168.77.2/24`) and `ip rule` (needs a line for `to 192.168.77.0/24 lookup 254`). |
| "!" icon, some apps work, DNS fails (`ping 8.8.8.8` works but `ping google.com` does not) | Your router's DNS is not working. Set the router's DNS to `8.8.8.8` / `1.1.1.1`, or use Private DNS on the phone. |
| Chip crashes with a panic in `esp_pbuf_free` | You are running an old version of this code. Use the current `main/tusb_ncm_main.c`. |

## Limitations and honest warnings

- **Rooting a phone can void the warranty, trips Knox on Samsung phones
  permanently, can break banking and payment apps, and can brick the phone if
  done wrong.** Unlocking the bootloader erases all data. You do this at your own risk.
- Tested on **one phone and one board** only.
- The setup page has **no password and no encryption**. Anyone connected to the
  USB link can use it. The WiFi password is stored in plain form in the ESP32's flash.
- Speed is limited (a few Mbit/s in my tests). It is meant for basic use.
- The phone-side rename and routing script needs root. Without root, some phones
  may still show the interface, but I have not tested that.
- The web page connection pauses while scanning.
- Only WPA/WPA2 personal and open networks are tested. Enterprise or captive
  portal networks are not supported.

## Project layout

```
firmware/        ESP-IDF project (based on the ESP-IDF tusb_ncm example)
  main/tusb_ncm_main.c
  main/CMakeLists.txt
phone/usb-eth.sh Root script for the phone (rename + web page routing)
README.md
LICENSE
```

## Credits

- Based on the ESP-IDF `tusb_ncm` example by Espressif (public domain / CC0).
- Built with ESP-IDF and TinyUSB.
- Developed with help from Claude (an AI assistant by Anthropic).

## License

MIT, see `LICENSE`. The ESP-IDF example this is based on is dedicated to the
public domain.

## Disclaimer

This software is provided as is, without any warranty. Flashing firmware and
rooting phones are risky. You are responsible for your own devices.
