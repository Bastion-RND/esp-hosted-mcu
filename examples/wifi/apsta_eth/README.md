# Wi-Fi AP + STA + remote W5500

This example exposes three independent network interfaces on an ESP32-P4 MCU
host while the radios and Ethernet controller are attached to an ESP32-C6
coprocessor:

| Host interface | Hardware on C6 | MAC address | IPv4 configuration |
| --- | --- | --- | --- |
| Wi-Fi STA | ESP32-C6 Wi-Fi | STA MAC | DHCP client |
| Wi-Fi AP | ESP32-C6 Wi-Fi | AP MAC | Static SoftAP address and DHCP server |
| `ETH_W5500` | W5500 over SPI2 | C6 factory-derived Ethernet MAC | DHCP client |

The C6 forwards raw Ethernet frames over the existing ESP-Hosted SDIO link.
The W5500 has no IP stack: LwIP, DHCP, sockets, and routing remain on the MCU
host. Link state and the Ethernet MAC are sent to the host in a small control
frame. The host creates a normal `esp_netif` for `ETH_W5500`, so applications
can use the standard ESP-IDF network APIs.

The Wi-Fi STA remains the default route and the SoftAP NAPT uplink, matching the
original `wifi/apsta` example. Ethernet receives its own address and is directly
usable by binding sockets to that interface or changing the default route in
the application.

## Wiring

Wire host and coprocessor as described for the ESP-Hosted four-bit SDIO
transport. Connect W5500 only to the ESP32-C6:

| W5500 signal | ESP32-C6 setting |
| --- | --- |
| MOSI | `SPI2 MOSI GPIO` |
| MISO | `SPI2 MISO GPIO` |
| SCLK | `SPI2 SCLK GPIO` |
| SCS/CS | `W5500 chip-select GPIO` |
| INT | `W5500 interrupt GPIO`, or `-1` for polling |
| RST | `W5500 reset GPIO`, or `-1` when not wired |
| VCC/GND | 3.3 V and common ground |

Choose pins that do not overlap the C6 SDIO pins or board flash pins. There are
no universal W5500 pin defaults, so the four SPI pins must be set before the CP
firmware can run.

## Build the ESP32-C6 firmware

Use ESP-IDF 5.5 or newer. From `examples/wifi/apsta_eth/cp`:

```text
idf.py set-target esp32c6
idf.py menuconfig
```

Set all required pins under `W5500 on ESP32-C6 coprocessor`. The default SPI
clock is 10 MHz. The example uses the SDIO streaming mode because the current
1536-byte ESP-Hosted transport slot is large enough for one standard Ethernet
frame and its transport header.

```text
idf.py build
idf.py -p <C6_PORT> flash monitor
```

## Build the ESP32-P4 host firmware

Configure the Wi-Fi credentials, SoftAP settings, and SDIO pins from
`examples/wifi/apsta_eth/mcu_host`:

```text
idf.py set-target esp32p4
idf.py menuconfig
idf.py build
idf.py -p <P4_PORT> flash monitor
```

When the Ethernet cable is connected, the host log reports the W5500 MAC, link
state, and DHCP address. `esp_netif_get_handle_from_ifkey("ETH_W5500")` returns
the Ethernet interface to application code.

## Frame size limitation

The bridge accepts untagged Ethernet frames up to 1514 bytes without FCS (1500
byte MTU). Together with the 20-byte ESP-Hosted V2 header this occupies 1534 of
the 1536 bytes available in one SDIO transport slot. VLAN frames with a full
1500-byte payload are therefore not supported by this version of the bridge.
