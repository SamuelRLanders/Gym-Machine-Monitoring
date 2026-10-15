# Gym-Machine-Monitoring
An embedded system that monitors gym machines and detects whether each one is available. This is an embedded systems project for CS48900, Fall 2026.

An ESP32 reads a motion sensor attached to a machine. Every few seconds it sends `"occupied"` or `"free"` over Wi-Fi to a server as an HTTP POST.

## Project structure

```
firmware/              ESP-IDF project (open this folder in VS Code for ESP work)
  main/                app entry point (main.c) and project settings (Kconfig.projbuild)
  components/          reusable modules (sensors, wifi, etc.)
  sdkconfig.defaults   committed config overrides
web/                   the webpage (index.html, style.css, app.js)
docs/                  wiring diagrams, notes, pinouts
EXPLANATIONS.md        background on the tools and concepts used
```

## What you need

**Hardware**
- An ESP32 development board with an **ESP-WROOM-32** module (4 MB flash)
- A USB cable that carries **data**. Many cables only charge, and the board won't show up with one of those.
- A motion sensor that outputs about 0 V when idle and about 2.6 V when it detects motion
- Jumper wires

**Software**
- [Visual Studio Code](https://code.visualstudio.com/)
- The **ESP-IDF** extension for VS Code, published by Espressif Systems
- ESP-IDF **v6.1**. The extension installs it for you (step 2 below).
- Git

**Network**
- A **2.4 GHz** Wi-Fi network. The ESP32 can't connect to 5 GHz networks.
- The network must use a plain password. Campus Wi-Fi with a username and password login (WPA2-Enterprise / eduroam) won't work. A phone hotspot works well. See [EXPLANATIONS.md](EXPLANATIONS.md) for why.

## Wiring

| Sensor pin | ESP32 pin |
|------------|-----------|
| Signal     | **GPIO 34** |
| GND        | **GND** (the ESP32 and sensor must share a ground) |
| VCC        | Whatever voltage your sensor needs (3V3 or 5V) |

- Use GPIO 34, or another pin in the GPIO 32–39 range. Pins on ADC2 stop working while Wi-Fi is on.
- **Never put more than 3.3 V on an ESP32 pin.** The sensor's 2.6 V signal is safe.
- If the signal pin floats when the sensor is idle, add a ~100 kΩ resistor from GPIO 34 to GND. "Floats" means the pin isn't driven to 0 V, so the reading drifts.

## Setup in VS Code

### 1. Install VS Code and the ESP-IDF extension

1. Install VS Code.
2. Open the **Extensions** view (`Ctrl+Shift+X`). Search for **ESP-IDF** and install the one published by **Espressif Systems**.

### 2. Install ESP-IDF (one time per computer)

The extension only provides the buttons and commands. It needs ESP-IDF itself, plus the compiler and build tools.

1. Press `Ctrl+Shift+P` to open the Command Palette. Run **ESP-IDF: Open ESP-IDF Installation Manager**.
2. Choose the easy (express) installation and version **v6.1**. Keep the default install location.
3. Wait for it to finish. It downloads 1–2 GB.
4. Run **ESP-IDF: Select Current ESP-IDF Version** from the Command Palette and choose v6.1.

### 3. Get the code

```
git clone https://github.com/SamuelRLanders/Gym-Machine-Monitoring.git
```

In VS Code, use **File → Open Folder** and open the **`firmware/`** folder, not the repository root. The extension only recognizes the project when `firmware/` is the open folder.

### 4. Connect the board

1. Plug the ESP32 into your computer with the USB cable.
2. Open **Device Manager** and expand **Ports (COM & LPT)**. Look for a new entry such as `Silicon Labs CP210x ... (COM3)`. Note the COM number.
3. If nothing appears, install the USB-to-serial driver for your board's chip. The chip is the small one next to the USB connector.
   - **CP2102 / CP210x:** Silicon Labs *CP210x USB to UART Bridge VCP* driver
   - **CH340 / CH9102:** WCH *CH340/CH343* driver

### 5. Point the extension at your board

Set these using the ESP-IDF items in the **status bar** at the bottom of VS Code. Hover over an item to see what it does. You can also find each one in the Command Palette by typing `ESP-IDF`.

| Status bar item | Set it to |
|-----------------|-----------|
| **Target** (chip icon) | `esp32`. If it asks about an OpenOCD board, pick the generic *ESP32 chip* option. |
| **Port** (plug icon)   | Your COM port from step 4 |
| **Flash method** (star icon) | `UART` |

Setting the target creates `firmware/sdkconfig`, the file that holds the project's settings.

### 6. Enter your Wi-Fi and server settings

Your own settings live in `firmware/sdkconfig`. Git ignores this file, so your Wi-Fi password is never committed.

1. Click the **gear icon** in the status bar (*SDK Configuration Editor*). You can also run **ESP-IDF: SDK Configuration Editor (menuconfig)** from the Command Palette.
2. Type `GYM` in the search box, or click **Gym Machine Monitor** in the left-hand list.
3. Fill in:

   | Setting | What to enter |
   |---------|---------------|
   | Wi-Fi network name (SSID) | Your 2.4 GHz network or hotspot name. The name is case-sensitive. |
   | Wi-Fi password | Its password. Leave it empty for an open network. |
   | Server URL | `http://<server computer's IP>:8000/status`. Find the IP with `ipconfig` on the server computer. `localhost` won't work. |
   | Machine ID | A name for this machine, e.g. `machine-1` |
   | Motion threshold / Report window / Sample interval | The defaults are fine to start with. |

4. Click **Save** at the top of the editor. If you skip this, your changes are lost.

> **Changing the `default` values in `main/Kconfig.projbuild` won't change your settings.** Those defaults are only used when `sdkconfig` is first created. After that, change settings in the SDK Configuration Editor. To reset everything to the defaults, delete `firmware/sdkconfig` and rebuild.

### 7. Build, flash, and monitor

Click **Build, Flash and Monitor** in the status bar (the flame icon). The first build takes several minutes. Later builds are much faster.

When the program is running, the monitor shows something like this:

```
I (...) gym: Gym Machine Monitor starting (machine "machine-1")
I (...) gym: Threshold 1300 mV, window 5000 ms, sampling every 100 ms
I (...) gym: Connecting to Wi-Fi "your-network"...
I (...) gym: Connected, IP address: 172.20.10.3
I (...) gym: Sent {"machine_id":"machine-1","status":"free","peak_mv":12} -> HTTP 200
```

Press `Ctrl+]` to close the monitor. The ESP32 keeps running after you close it.

To change settings later, repeat step 6, then step 7.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| It keeps trying to connect to the wrong network | Your settings weren't saved to `sdkconfig`. Repeat step 6 and make sure you click **Save**. |
| `Wi-Fi disconnected, retrying...` repeats forever | Check the SSID and password; both are case-sensitive. Make sure the network is 2.4 GHz. On an iPhone hotspot, turn on **Maximize Compatibility**. The program waits here until it connects. |
| `Send failed` or `Not connected to Wi-Fi, skipping send` | The server isn't reachable. Check that the server is running, that the URL uses the server computer's LAN IP, and that both devices are on the same network. Also make sure Windows Firewall allows port 8000. |
| The board doesn't show up as a COM port | Try a different (data) USB cable, or install the driver from step 4. |
| `Failed to connect to ESP32` / `Timed out waiting for packet header` | Hold the **BOOT** button on the board while flashing starts. Release it once you see `Writing at 0x...`. |
| `Could not open port COM3: Access is denied` | Another program has the port open, such as another monitor or the Arduino IDE. Close it. |
| ESP-IDF commands are "not found" | Run **Developer: Reload Window** from the Command Palette. Make sure you opened the `firmware/` folder. |
| `peak_mv` is always near 0, even with motion | Check the wiring. Make sure the sensor's GND is connected to the ESP32's GND. |

[EXPLANATIONS.md](EXPLANATIONS.md) covers the tools and design in more depth: CMake, sdkconfig, partitions, and how the firmware works.

## Building from a terminal (optional)

From an ESP-IDF terminal. To open one, run **ESP-IDF: Open ESP-IDF Terminal** from the Command Palette.

```
cd firmware
idf.py set-target esp32     # ESP-WROOM-32 module
idf.py menuconfig           # Gym Machine Monitor → enter settings, S to save, Q to quit
idf.py build flash monitor
```
