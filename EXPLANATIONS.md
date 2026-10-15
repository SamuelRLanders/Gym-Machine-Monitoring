# Explanations

Answers to technical questions that came up while building this project, kept here for reference.

---

## What is CMake, and what does `CMakeLists.txt` do?

**CMake is a build-system generator.** Compiling a C project means running the compiler on every source file with the right flags and include paths, then linking the results together. Writing those commands by hand (or in a Makefile) gets messy fast. With CMake you describe the project *at a high level* ("this is a project, these are its source files, it depends on these libraries"), and CMake generates the low-level build files for you (Ninja or Make files). A separate tool then runs those files to do the actual compiling.

**`CMakeLists.txt` is the file CMake reads.** Each directory that is part of the build can have one. It describes what's in that directory and how to build it.

ESP-IDF builds on top of CMake with its own conventions. In this project there are two kinds of `CMakeLists.txt`:

**`firmware/CMakeLists.txt`, the project file:**

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gym_machine_monitor)
```

- `cmake_minimum_required`: refuses to run on CMake versions that are too old.
- `include(.../project.cmake)`: pulls in ESP-IDF's build logic. This is what makes it an ESP-IDF project and not a plain CMake one. `IDF_PATH` is an environment variable set when you install or activate ESP-IDF.
- `project(...)`: names the project. The output binary will be `build/gym_machine_monitor.bin`.

**`firmware/main/CMakeLists.txt`, a component file:**

```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS ".")
```

ESP-IDF organizes code into **components**: self-contained modules, each with its own `CMakeLists.txt`. `main` is a special component that holds `app_main()`. `idf_component_register` tells the build:

- `SRCS`: which `.c` files to compile.
- `INCLUDE_DIRS`: which folders other components can `#include` headers from.
- (optionally) `REQUIRES` / `PRIV_REQUIRES`: which other components this one depends on, e.g. `esp_wifi`, `driver`.

When you add your own module under `firmware/components/` (say `components/ultrasonic/`), it gets its own `CMakeLists.txt` with an `idf_component_register(...)` call. ESP-IDF finds it automatically.

**You rarely run CMake directly.** `idf.py build` runs CMake to generate build files into `firmware/build/`, then runs Ninja to compile. That's why `build/` is in `.gitignore`: it is fully regenerated from the `CMakeLists.txt` files and your source.

---

## What does `sdkconfig.defaults` do?

ESP-IDF has **hundreds of configuration options**: flash size, CPU frequency, log level, Wi-Fi buffer sizes, FreeRTOS tick rate, partition table choice, and so on. They're browsed and edited with `idf.py menuconfig` (a text-based menu).

Your choices are saved in a file called **`sdkconfig`**. That file holds *every* option, often over 1,000 lines, mostly defaults. It's generated, it changes noisily between ESP-IDF versions, and it's hard to review in Git. So it's ignored.

**`sdkconfig.defaults` holds only the options you deliberately changed.** When `sdkconfig` doesn't exist (fresh clone, or after you delete it), ESP-IDF builds a new one by:

1. starting from ESP-IDF's built-in defaults,
2. applying every line in `sdkconfig.defaults` on top.

So it's a short, readable list of "this project needs these settings." That's what you commit. For example:

```
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
```

**The gotcha:** `sdkconfig.defaults` is only applied when `sdkconfig` is *created*. If `sdkconfig` already exists, editing the defaults file does nothing. After changing `sdkconfig.defaults`, delete `firmware/sdkconfig` (or run `idf.py fullclean`) and rebuild.

**The workflow:** experiment in `idf.py menuconfig`. When you find a setting you want to keep, add that line to `sdkconfig.defaults`. You can find the exact option name by searching `sdkconfig` or pressing `?` in menuconfig.

---

## Partitions: what do they have to do with this project?

You're right that a partition is just a division of memory. On the ESP32 it specifically means a division of the **external flash chip**, the non-volatile storage that holds your program and data (4 MB on the ESP-WROOM-32). The ESP32 has no hard drive or filesystem by default. Flash is just one big array of bytes, so something has to say "these bytes are the program, these bytes are saved settings, ...". That something is the **partition table**.

### How flash is laid out

```
0x0000   ┌──────────────────────────┐
         │ (reserved)               │
0x1000   ├──────────────────────────┤
         │ Bootloader               │  runs first on power-up
0x8000   ├──────────────────────────┤
         │ Partition table          │  the "map" of everything below
0x9000   ├──────────────────────────┤
         │ nvs        (24 KB)       │  key-value storage
0xF000   ├──────────────────────────┤
         │ phy_init   (4 KB)        │  radio calibration data
0x10000  ├──────────────────────────┤
         │ factory    (1 MB)        │  your compiled app
0x110000 ├──────────────────────────┤
         │ (unused, ~3 MB on a      │
         │  4 MB chip)              │
0x400000 └──────────────────────────┘
```

On power-up, the ROM runs the **bootloader**. The bootloader reads the **partition table** to find which partition holds the app, then loads and runs it.

### The default partition table

ESP-IDF's default is **"Single factory app, no OTA"**:

| Name       | Type | Purpose                                                                 | Size   |
|------------|------|-------------------------------------------------------------------------|--------|
| `nvs`      | data | Non-Volatile Storage: small key-value store (Wi-Fi credentials, settings, counters) that survives reboots | 24 KB  |
| `phy_init` | data | Calibration data for the Wi-Fi/Bluetooth radio                          | 4 KB   |
| `factory`  | app  | Your firmware                                                           | 1 MB   |

That's enough to get started. Your app can be up to 1 MB, and you can save small settings in NVS.

### Why you'd need a custom partition table

You write a `partitions.csv` and point menuconfig at it (`CONFIG_PARTITION_TABLE_CUSTOM=y`) when the default doesn't fit. Common reasons:

- **Your app outgrows 1 MB.** Wi-Fi + Bluetooth + an HTTP server + TLS can get there. A custom table can give the app 2–3 MB.
- **Over-the-air (OTA) updates.** To update firmware over Wi-Fi without a USB cable, you need *two* app partitions (`ota_0`, `ota_1`) plus an `otadata` partition. The new firmware downloads into the inactive slot, and the device switches on reboot. If the update is bad, it can fall back. (ESP-IDF also has a built-in "Factory app, two OTA definitions" option for this.)
- **Storing files.** If the ESP32 itself serves the `web/` page (HTML/CSS/JS), you'd add a **SPIFFS, LittleFS, or FAT** partition to hold those files and flash them separately from the app.
- **Using the rest of the flash.** The default table leaves ~3 MB of a 4 MB chip unused.

**For this project:** the default is fine for now. The most likely trigger for a custom table is deciding to serve the webpage from the ESP32 itself (needs a filesystem partition) or wanting OTA updates.

---

## The ESP-WROOM-32 and the `esp32` target

The **ESP-WROOM-32** is a *module*: a small metal-shielded board that contains an **ESP32-D0WD** chip (dual-core Xtensa LX6, Wi-Fi + Bluetooth), a **4 MB flash** chip, a crystal, and a PCB antenna. Most "ESP32 DevKit" boards are this module on a breakout board with USB.

- **ESP-IDF target:** `esp32` (the original ESP32; not `esp32s3`, `esp32c3`, etc.), so `idf.py set-target esp32` is correct.
- **Flash size:** ESP-IDF assumes 2 MB unless told otherwise, so `sdkconfig.defaults` sets `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` to match the module.

---

## CMake vs. `idf.py`: who does what, and where `idf.py` lives

A common mix-up is to think CMake is a Python tool. It isn't. **CMake is a standalone program written in C++** (on this machine it's at `C:\Program Files\CMake\bin\cmake.exe`). **`idf.py` is the Python script.** It's ESP-IDF's front-end that drives everything else.

A full `idf.py build` runs through this chain:

```
idf.py build                 (Python: ESP-IDF's command-line front-end)
  └─> cmake                  (C++: reads CMakeLists.txt + sdkconfig, writes build/build.ninja)
        └─> ninja            (C++: runs the commands listed in build.ninja)
              └─> xtensa-esp32-elf-gcc   (the cross-compiler: turns .c into ESP32 machine code)
```

- **CMake** decides *what* commands are needed. It writes them to `build/build.ninja`. It doesn't compile anything itself.
- **Ninja** *runs* those commands, in parallel. It only reruns the ones whose inputs changed, so rebuilds are fast.
- **`xtensa-esp32-elf-gcc`** is a *cross-compiler*. It runs on your PC but produces code for the ESP32's Xtensa CPU, not for your PC's x86 CPU. That's why you can't use a normal `gcc` for this.
- **`idf.py`** wraps all of it so you don't have to remember the CMake/Ninja/esptool invocations.

So yes, you never write compile commands yourself. `idf.py build` produces everything, and the final firmware ends up at `firmware/build/gym_machine_monitor.bin`.

Other `idf.py` subcommands you'll use:

| Command                 | What it does                                                            |
|-------------------------|-------------------------------------------------------------------------|
| `idf.py set-target esp32` | Picks the chip. Wipes `build/` and regenerates `sdkconfig` from defaults |
| `idf.py menuconfig`     | Text menu for editing configuration options                             |
| `idf.py build`          | Configure (CMake) + compile (Ninja)                                     |
| `idf.py -p COM3 flash`  | Write the firmware to the board over USB (uses `esptool.py`)            |
| `idf.py -p COM3 monitor`| Show the board's serial output (your `ESP_LOGI` messages). Exit with `Ctrl+]` |
| `idf.py fullclean`      | Delete `build/` entirely                                                |

(`COM3` is an example. Check Device Manager → Ports for your board's port.)

### Where is `idf.py`?

It ships with ESP-IDF, at `<ESP-IDF install folder>/tools/idf.py`. It isn't on your `PATH` by default. ESP-IDF needs a specific Python virtual environment and toolchain paths set up first, and three things do that:

- **ESP-IDF installer on Windows:** it creates an **"ESP-IDF PowerShell"** / **"ESP-IDF CMD"** shortcut. Terminals opened from it have `idf.py` ready.
- **Manually in any terminal:** run the export script from the ESP-IDF folder, e.g. `C:\Espressif\frameworks\esp-idf-v5.x\export.ps1`. That sets `IDF_PATH` and `PATH` for that terminal session only.
- **The ESP-IDF VS Code extension:** its build/flash/monitor buttons and its "ESP-IDF Terminal" handle this for you.

If `idf.py` says "command not found," you're in a terminal where the export step hasn't run.

---

## What does `=y` mean in `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`?

ESP-IDF's configuration system is **Kconfig**, the same system the Linux kernel uses. Each option has a type, and the type decides how its value is written:

| Type     | Example in `sdkconfig`                         |
|----------|------------------------------------------------|
| bool     | `CONFIG_LOG_COLORS=y`, or `# CONFIG_LOG_COLORS is not set` for "no" |
| int/hex  | `CONFIG_FREERTOS_HZ=100`                       |
| string   | `CONFIG_ESPTOOLPY_FLASHSIZE="4MB"`             |

**`y` just means "yes / enabled"** for a bool option. "No" isn't written as `=n`. It's written as the comment line `# CONFIG_... is not set`.

Flash size is a **choice** (a group of bools where exactly one is `y`, like radio buttons): `..._1MB`, `..._2MB`, `..._4MB`, `..._8MB`, .... Setting `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` selects 4 MB, and the others automatically become "not set". It's the same as picking *Serial flasher config → Flash size → 4 MB* in `idf.py menuconfig`.

### When does it take effect?

1. **When `sdkconfig` is generated.** That happens on the first `idf.py build`, `set-target`, or `menuconfig` when no `sdkconfig` file exists yet. The line gets copied from `sdkconfig.defaults` into `sdkconfig` at that point. (This is why editing `sdkconfig.defaults` later does nothing until you delete `sdkconfig`.)
2. **At build time.** `sdkconfig` is turned into a C header, `build/config/sdkconfig.h`, containing `#define CONFIG_ESPTOOLPY_FLASHSIZE_4MB 1`. Any C code can `#include "sdkconfig.h"` and check `#ifdef CONFIG_...`. This is how all ESP-IDF options reach the code.
3. **At flash time.** `esptool.py` writes the flash size into the bootloader image's header. On boot, the chip reads that header to know how much flash it can address.

---

## Setting up VS Code to build and flash the ESP32

Written for ESP-IDF VS Code extension **v2.x**. Menu wording can shift slightly between versions.

### 1. Install ESP-IDF itself (one time)

The extension is only a front-end. It still needs the actual ESP-IDF framework, plus the toolchain (cross-compiler, CMake, Ninja, Python venv, esptool).

1. `Ctrl+Shift+P` → **ESP-IDF: Open ESP-IDF Installation Manager**. (Older 1.x versions called this *ESP-IDF: Configure ESP-IDF Extension*.)
2. Choose the **easy/express** installation, the **latest stable v5.x** release, and the default install location (`C:\Espressif`).
3. Wait. It downloads ~1–2 GB.
4. Run **ESP-IDF: Select Current ESP-IDF Version** and pick the version you just installed. After that, `IDF_PATH` and the tool paths are handled for you inside VS Code.

### 2. Open the right folder

**File → Open Folder → `firmware/`**, not the repo root. The extension treats the opened folder as the ESP-IDF project. It looks for the top-level `CMakeLists.txt` there, and it creates its `.vscode/` settings there.

### 3. Connect the board and find its COM port

- Use a **data** USB cable. Many cables are charge-only, and the board then never appears.
- The USB-to-serial chip next to the USB connector needs a Windows driver:
  - **CP2102 / CP210x** (most Espressif DevKitC boards): *Silicon Labs CP210x USB to UART Bridge VCP driver*
  - **CH340 / CH9102** (common on clone boards): *WCH CH340/CH343 driver*
- Check **Device Manager → Ports (COM & LPT)**. You should see something like `Silicon Labs CP210x ... (COM3)`. That's your port.

### 4. Tell the extension about this project

Use the ESP-IDF items on the **status bar** at the bottom of VS Code (or the matching `ESP-IDF:` commands in `Ctrl+Shift+P`):

| Setting        | Choose                                                                   |
|----------------|--------------------------------------------------------------------------|
| **Target**     | `esp32`. If asked for an OpenOCD board config, pick the generic *ESP32 chip* option. That only matters for JTAG debugging, which you aren't using |
| **Port**       | Your COM port from step 3                                                |
| **Flash method** | `UART`. This means "over the USB serial cable," which is what a DevKit uses |

Setting the target runs `idf.py set-target esp32` for you, which generates `sdkconfig` from `sdkconfig.defaults`.

### 5. Build, flash, monitor

The status bar has separate **Build**, **Flash**, and **Monitor** buttons, plus a combined **Build, Flash and Monitor** button (flame icon). Use the combined one. You should see:

```
I (xxx) main: Gym Machine Monitor starting
```

Exit the monitor with `Ctrl+]`.

### Common problems

| Symptom | Fix |
|---------|-----|
| `command 'espIdf.installManager' not found` (or any ESP-IDF command "not found") | The extension didn't finish loading. Run **Developer: Reload Window**, wait a few seconds, and try again. If it still fails, check **View → Output → ESP-IDF** for errors, or install the standalone ESP-IDF Installation Manager (see the [EIM docs](https://docs.espressif.com/projects/idf-im-ui/en/latest/)) and then run *ESP-IDF: Select Current ESP-IDF Version* |
| Board doesn't appear as a COM port | Charge-only cable, or missing CP210x/CH340 driver |
| `Failed to connect to ESP32: Wrong boot mode detected` / `Timed out waiting for packet header` | Hold the **BOOT** button while flashing starts (release once you see `Connecting...` progress or `Writing at 0x...`). Some boards' auto-reset circuit is unreliable |
| `Could not open port COM3: Access is denied` | Something else has the port open: another monitor, Arduino IDE, a serial terminal. Close it |
| Garbage characters in the monitor | Wrong baud rate. The extension's monitor uses 115200 by default, which is correct for ESP-IDF |

---

## What is the VS Code status bar, and where are the ESP-IDF buttons?

The **status bar** is the thin strip along the very bottom edge of the VS Code window. It's usually blue or purple, depending on your theme. VS Code and its extensions show small items there: on the left, the Git branch, error/warning counts; on the right, the line/column, file encoding, language mode.

```
┌───────────────────────────────────────────────────────────────┐
│  Explorer  │  main.c                                          │
│            │                                                  │
│            │   (editor)                                       │
│            │                                                  │
├────────────┴──────────────────────────────────────────────────┤
│ ⎇ main  ⊗0 ⚠0   [ESP-IDF items: version · port · target ·     │  ← status bar
│                   flash method · build · flash · monitor · 🔥] │
└───────────────────────────────────────────────────────────────┘
```

### The ESP-IDF items

Once the extension is active and you've opened an ESP-IDF project folder (`firmware/`), it adds clickable items to the status bar. Each one is an icon, sometimes with text beside it. **Hover over an item to see a tooltip** saying what it does. The ones you need:

| Item (hover text mentions…) | What clicking it does |
|-----------------------------|-----------------------|
| ESP-IDF version             | Pick which installed ESP-IDF version to use |
| Port (plug icon, shows e.g. `COM3`) | Choose the board's serial port |
| Target (chip icon, shows e.g. `esp32`) | Set the chip type (`idf.py set-target`) |
| Flash method (star icon, shows `UART`/`JTAG`) | Choose how to flash: pick `UART` |
| Build (wrench/cylinder icon) | `idf.py build` |
| Flash (lightning icon)      | `idf.py flash` |
| Monitor (screen icon)       | `idf.py monitor` |
| Build, Flash and Monitor (flame icon) | All three in a row |

### If you don't see the status bar or the ESP-IDF items

- **No status bar at all:** **View → Appearance → Status Bar** to turn it back on.
- **Status bar is there, but no ESP-IDF items:**
  - Make sure you opened `firmware/` (the folder with the top-level `CMakeLists.txt`), not the repo root.
  - The extension may still be loading, or it may not have an ESP-IDF install configured yet. Try **Developer: Reload Window**.
  - Right-click the status bar. It lists every item with a checkbox, so you can re-enable any that are hidden.
  - On a narrow window, VS Code drops some items. Widen the window.

### The same actions without the status bar

Every status bar button is also a command. Press `Ctrl+Shift+P` (the **Command Palette**) and type `ESP-IDF` to filter, then pick:

- **ESP-IDF: Set Espressif Device Target**
- **ESP-IDF: Select Port to Use (COM, tty, usbserial)**
- **ESP-IDF: Select Flash Method**
- **ESP-IDF: Build, Flash and Monitor**

Command wording can shift a bit between extension versions, but typing `ESP-IDF` plus a keyword (`target`, `port`, `flash`) will find them.

---

## Walking through `main/main.c` (the LED blink program)

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
```

**Includes** pull in declarations from other code so this file can use them:

- `stdio.h`: standard C input/output. Not strictly needed here, but harmless.
- `freertos/FreeRTOS.h` and `freertos/task.h`: **FreeRTOS** is the small operating system that ESP-IDF runs on. It lets the ESP32 run several *tasks* (like threads) at once and gives us `vTaskDelay`.
- `driver/gpio.h`: functions for controlling **GPIO pins** (General Purpose Input/Output), the physical pins on the board.
- `esp_log.h`: ESP-IDF's logging (`ESP_LOGI` etc.), which prints to the serial monitor.

```c
static const char *TAG = "main";
```

A label attached to every log message from this file. Output looks like `I (312) main: LED on`. With several files, the tag shows which one printed a line. `static` means the variable is private to this file.

```c
#define LED_GPIO GPIO_NUM_2
```

A **macro**: before compiling, the preprocessor replaces every `LED_GPIO` with `GPIO_NUM_2`. It gives the pin a meaningful name, and if the LED is on a different pin you change only this line. GPIO 2 is wired to the blue LED on most DevKit boards.

```c
void app_main(void)
{
```

**The entry point.** On a PC, C programs start in `main()`. In ESP-IDF, the system boots first (bootloader, FreeRTOS, CPU setup), then calls `app_main()` inside its own FreeRTOS task. `void` here means it takes no arguments and returns nothing.

```c
    ESP_LOGI(TAG, "Gym Machine Monitor starting");
```

Prints an **I**nfo-level log line. The others are `ESP_LOGE` (error), `ESP_LOGW` (warning), `ESP_LOGD` (debug), and `ESP_LOGV` (verbose). Which levels actually print is controlled by `CONFIG_LOG_DEFAULT_LEVEL_*` in `sdkconfig`.

```c
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
```

**Pin setup.** `gpio_reset_pin` puts the pin into a clean default state. (Some pins come out of boot assigned to other functions.) `gpio_set_direction(..., GPIO_MODE_OUTPUT)` makes it an **output**, so the ESP32 *drives* the pin high (3.3 V) or low (0 V), rather than reading it as an input. Later, an ultrasonic sensor's echo pin would be set up as an input instead.

```c
    int led_on = 0;
    while (1) {
        led_on = !led_on;
        gpio_set_level(LED_GPIO, led_on);
        ESP_LOGI(TAG, "LED %s", led_on ? "on" : "off");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
```

**The main loop:**

- `led_on` tracks the current state. `0` = off, `1` = on.
- `while (1)` loops forever. Embedded programs normally never exit. There's no OS shell to return to.
- `led_on = !led_on` flips it: `!` turns 0 into 1 and 1 into 0.
- `gpio_set_level(LED_GPIO, led_on)` drives the pin: 1 = 3.3 V (LED lights), 0 = 0 V (LED off).
- `ESP_LOGI(TAG, "LED %s", ...)` works like `printf`. `%s` is filled in with `"on"` or `"off"`. `cond ? a : b` is C's inline if/else: "if `led_on`, use `"on"`, otherwise `"off"`."
- `vTaskDelay(pdMS_TO_TICKS(500))` pauses this task for 500 ms. FreeRTOS measures time in **ticks** (by default 1 tick = 10 ms), so `pdMS_TO_TICKS` converts milliseconds to ticks.

**Why `vTaskDelay` and not a busy-wait loop?** `vTaskDelay` tells FreeRTOS "I have nothing to do for 500 ms." The scheduler runs other tasks in the meantime, such as Wi-Fi handling, or idles the CPU. A busy loop (`for (i = 0; i < 1000000; i++);`) would hog the CPU. It would also trigger the **task watchdog**, a safety timer that resets the chip if a task never yields.

---

## Why does the LED keep blinking after I stop the program in the terminal?

### 1. Why it keeps going

**The program isn't running on your PC. It's running on the ESP32.** Your terminal was never running it, so there was nothing to interrupt.

Two separate things happened:

- **Flashing:** `idf.py flash` copied the compiled program over USB into the ESP32's **flash memory**, then reset the chip. From then on the ESP32 runs the program on its own CPU. Your PC's part is over.
- **Monitoring:** `idf.py monitor` just opens the serial port and *displays* whatever the ESP32 prints (your `LED on` / `LED off` logs). When you press `Ctrl+]` (or `Ctrl+C`), you close the **viewer**. You don't stop the program, much like closing a security-camera feed doesn't stop the room it shows.

Because the program lives in flash, which is **non-volatile**, it also survives power loss. Unplug the board and plug it back in (even into a phone charger), and it starts blinking again.

### 2. How to stop it

| Want to… | Do this |
|----------|---------|
| **Stop it for a moment** | Unplug the USB cable. It restarts the blink program as soon as power returns |
| **Pause it while you hold a button** | Hold the **EN** (a.k.a. **RST**) button. The chip is held in reset. Release it and the program restarts from the beginning |
| **Replace it** (the normal way) | Flash a different program. Whatever you flash last is what runs. For example, change `main.c` to turn the LED off once and then just idle, then **Build, Flash and Monitor** |
| **Remove it entirely** | `idf.py -p COM3 erase-flash` (or **ESP-IDF: Erase Flash** in VS Code). This wipes all of flash, including the bootloader, so the chip has nothing to run. It sits in its built-in ROM, printing boot errors to serial, until you flash something again |

There's no "stop" button because on a microcontroller, "not running a program" isn't a normal state. Day to day, you "stop" a program by flashing the next version of it.

### 3. The mental model

**Think of the ESP32 as a separate, tiny computer whose only job is to run your one program, forever, from the moment it gets power.** Your PC is just the tool you use to install that program and to watch its output.

```
   Your PC                                 ESP32
┌──────────────────┐   USB cable    ┌──────────────────────────────┐
│ idf.py build     │                │ Flash (4 MB, survives power  │
│   compiles .bin  │                │   loss): bootloader +        │
│ idf.py flash ────┼── writes ─────>│   partition table + your app │
│                  │                │                              │
│ idf.py monitor <─┼── log text ────┤ CPU: runs your app, forever  │
│  (viewer only)   │                │                              │
└──────────────────┘                └──────────────────────────────┘
```

Some differences from a program on your laptop:

| On your PC | On the ESP32 |
|------------|--------------|
| You start a program, it runs, it exits | The program starts **every time the chip powers up or resets**, and is expected never to exit (hence `while (1)`) |
| Many programs installed; you choose which to run | **Exactly one** app. Flashing a new one replaces the old one |
| The terminal that launched a program owns it (`Ctrl+C` kills it) | The terminal is only a serial *window*. It doesn't own or control the program |
| Programs live on disk and are loaded into RAM to run | Same idea: the app lives in flash and the bootloader loads it. But this happens automatically on every boot, with no user or shell involved |

**What happens on every power-up or reset:**

1. The **ROM bootloader** runs. It's burned into the chip at the factory and can't be changed.
2. It loads the **second-stage bootloader** from flash (address `0x1000`).
3. That reads the **partition table** (see the Partitions section) and loads your app from the `factory` partition.
4. ESP-IDF sets up the CPU, memory, and **FreeRTOS**, then calls your `app_main()`.
5. Your code runs until the power goes off or the chip is reset.

**What resets the chip:** power-cycling, pressing **EN**, flashing (esptool resets it when done), a crash, the watchdog firing, or code calling `esp_restart()`. **Opening the monitor usually resets it too:** the USB-serial chip's control lines are wired to EN and BOOT, and the monitor toggles them on connect. That's why you see boot messages from the very start each time you open it.

**What if `app_main()` returns?** The chip doesn't halt or "exit." Only the `app_main` task ends. FreeRTOS keeps running any other tasks (Wi-Fi, timers, tasks you created), and otherwise just idles. Pins keep whatever level they were last set to. So an LED left on stays on.

---

## Design: reading the motion sensor and reporting over Wi-Fi

> **Note:** this was the first draft. The design was then changed to an "occupied/free per 5-second window" model, implemented in `main/main.c`. See *"The implemented design: occupied/free windows"* below. The hardware, pin, and network reasoning here still applies.

**Goal:** a motion sensor outputs **0.0 V** (no motion) or **2.6 V** (motion). Every 5 seconds the ESP32 reads it and sends the result over Wi-Fi to a server running on another computer.

### Decision 1: digital pin or analog (ADC) pin?

Because the sensor only has two states, it looks like a job for a plain digital input (`gpio_get_level`). The catch is the **voltage threshold**. The ESP32 runs at 3.3 V, and its datasheet only *guarantees* a digital input reads as HIGH above **0.75 × 3.3 V ≈ 2.48 V**. 2.6 V clears that by only ~0.1 V. Long wires, noise, or a slightly weak sensor output could make it read LOW sometimes.

**Using the ADC (analog-to-digital converter) is more robust.** It measures the actual voltage, as a number from 0 to 4095. We then pick our own threshold halfway between the two states (~1.3 V). That gives ~1.3 V of margin on both sides instead of ~0.1 V.

### Decision 2: which pin? (It does matter.)

The ESP32 has two ADCs, and **ADC2 can't be used while Wi-Fi is on**, because the Wi-Fi driver uses it. So the sensor must go on an **ADC1** pin: **GPIO 32–39**.

**GPIO 34** is a good choice (ADC1 channel 6). GPIO 34–39 are input-only, which is fine here. They also have no internal pull-up or pull-down resistors. So if the sensor's output *floats* (disconnected) rather than being driven to 0 V when idle, add an external ~100 kΩ resistor from the pin to GND.

Wiring checklist:

- Sensor signal → GPIO 34
- Sensor GND → ESP32 GND. A **common ground** is required, or the voltage reading is meaningless.
- 2.6 V is below 3.3 V, so it's safe for the pin. **Never put more than 3.3 V on an ESP32 pin.**

### Decision 3: how to send the message

| Option | Pros | Cons |
|--------|------|------|
| **HTTP POST with JSON** | Easy server side (Python Flask/FastAPI, Node Express); easy to test with a browser/`curl`; human-readable | A bit more overhead per message (irrelevant at one message per 5 s) |
| Raw UDP/TCP socket | Minimal overhead | You design your own message format; UDP messages can be silently lost |
| MQTT | Standard for IoT; good with many devices | Needs a broker (e.g. Mosquitto) as an extra piece |

**HTTP POST is the recommended default.** Each message looks like:

```
POST http://192.168.1.50:8000/reading
Content-Type: application/json

{"machine_id": "treadmill-1", "motion": true, "raw": 3210}
```

### Network gotchas

- The ESP32 only does **2.4 GHz** Wi-Fi, not 5 GHz.
- It can't easily join **WPA2-Enterprise** networks (username + password logins, like most university Wi-Fi/eduroam). For development, use a home router or a **phone hotspot**, with the server laptop on the same network.
- The server must listen on `0.0.0.0` (not `127.0.0.1`/`localhost`), or other devices can't reach it. Windows Firewall may also need to allow the port.
- Use the server computer's **LAN IP** (from `ipconfig`) in the URL, and keep in mind it can change. The ESP32 is a separate device, so `localhost` would mean the ESP32 itself.

### Structure of `main.c`

```
app_main()
 ├─ nvs_flash_init()      Wi-Fi driver stores calibration data in NVS, so this is required
 ├─ wifi_init()           connect to the access point, block until we have an IP address
 ├─ sensor_init()         configure ADC1 channel 6 (GPIO 34)
 └─ loop forever:
      ├─ raw = read ADC
      ├─ motion = raw > THRESHOLD
      ├─ send_reading(motion, raw)    HTTP POST to the server
      └─ vTaskDelay(5 s)
```

### Sketch of the code

This is an outline to show the shape, with error handling trimmed for readability (ESP-IDF v5.x APIs).

```c
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_adc/adc_oneshot.h"

#define WIFI_SSID        "your-network"       // better: set via menuconfig, not hardcoded
#define WIFI_PASS        "your-password"
#define SERVER_URL       "http://192.168.1.50:8000/reading"
#define MACHINE_ID       "treadmill-1"

#define SENSOR_CHANNEL   ADC_CHANNEL_6        // GPIO 34 on ADC1
#define MOTION_THRESHOLD 1600                 // raw ADC value ≈ 1.3 V
#define SAMPLE_PERIOD_MS 5000

static const char *TAG = "main";
static EventGroupHandle_t wifi_events;
#define WIFI_CONNECTED_BIT BIT0
static adc_oneshot_unit_handle_t adc;
```

**Wi-Fi.** Wi-Fi in ESP-IDF is **event-driven**. You start the driver, and it later *calls you back* when things happen: started, connected, got an IP, disconnected. The handler reacts to those events. An **event group** (a set of flag bits) lets `app_main` wait until the "got IP" flag is set.

```c
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();                   // keep retrying if the network drops
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_events = xEventGroupCreate();
    esp_netif_init();                         // TCP/IP stack
    esp_event_loop_create_default();          // system event loop
    esp_netif_create_default_wifi_sta();      // "station" = client that joins a network

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wc = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();                         // triggers WIFI_EVENT_STA_START → connect

    // Block until the handler reports we have an IP address
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Wi-Fi connected");
}
```

**Sensor.** The "oneshot" ADC driver takes a single reading on request. `ADC_ATTEN_DB_12` sets the input range to roughly 0–3.1 V, which covers our 2.6 V. (Older ESP-IDF versions call it `ADC_ATTEN_DB_11`.)

```c
static void sensor_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&unit_cfg, &adc);

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,     // 12-bit → 0..4095
    };
    adc_oneshot_config_channel(adc, SENSOR_CHANNEL, &chan_cfg);
}

static int sensor_read_raw(void)
{
    int raw = 0;
    adc_oneshot_read(adc, SENSOR_CHANNEL, &raw);
    return raw;
}
```

**Sending.** Build a small JSON string with `snprintf` and POST it.

```c
static void send_reading(bool motion, int raw)
{
    char body[96];
    snprintf(body, sizeof(body),
             "{\"machine_id\":\"%s\",\"motion\":%s,\"raw\":%d}",
             MACHINE_ID, motion ? "true" : "false", raw);

    esp_http_client_config_t cfg = {
        .url = SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);   // blocks until done or timeout
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sent %s -> HTTP %d", body,
                 esp_http_client_get_status_code(client));
    } else {
        ESP_LOGW(TAG, "Send failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}
```

**Putting it together.**

```c
void app_main(void)
{
    // NVS init, with the standard "erase and retry if the partition is full/outdated"
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    wifi_init();
    sensor_init();

    while (1) {
        int raw = sensor_read_raw();
        bool motion = raw > MOTION_THRESHOLD;
        send_reading(motion, raw);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}
```

### Things to decide or watch out for

- **Wi-Fi password in source code:** don't commit it. The cleanest ESP-IDF approach is a `main/Kconfig.projbuild` file. It adds "SSID" and "Password" fields to `idf.py menuconfig`, and the values live in the git-ignored `sdkconfig`.
- **Sampling every 5 s can miss short motion.** If the sensor's output goes high for only 1 s between samples, you never see it. Many PIR-style sensors hold the output high for several seconds after motion, which helps. Alternatives are to sample faster and report "any motion in the last 5 s", or to use a GPIO interrupt.
- **Send every time vs. only on change:** sending every 5 s is simple, and it doubles as a "still alive" heartbeat for the server. If traffic or power matters later, send only when the state changes, plus a periodic heartbeat.
- **Choosing the threshold:** log the `raw` values with the sensor idle and triggered, then pick a number in between. The ESP32's ADC isn't perfectly accurate, but that doesn't matter for a yes/no threshold.
- **File organization:** once this works, it's natural to move the Wi-Fi code into `wifi.c`/`wifi.h` and the sensor code into `sensor.c`/`sensor.h`, keeping `main.c` short. Each new `.c` file must be added to `SRCS` in `main/CMakeLists.txt`.

---

## Why is campus Wi-Fi a problem? Can't we just enter our password?

**Short answer:** it's *possible*, but harder than a home network, and it may still not work for reasons unrelated to the password.

### Two kinds of Wi-Fi login

| | **WPA2-Personal (PSK)** | **WPA2/WPA3-Enterprise (802.1X)** |
|---|---|---|
| Where | Home routers, phone hotspots | Universities (eduroam, campus networks), companies |
| You enter | One shared network password | *Your own* username + password |
| How it works | Everyone uses the same key | The access point forwards your login to a central authentication server (RADIUS), which checks it against your university account |

The basic ESP-IDF Wi-Fi setup (`ssid` + `password` in `wifi_config_t`, what `main.c` uses) only handles **Personal**. Enterprise needs extra code with ESP-IDF's EAP client API (`esp_eap_client_*`). The device has to:

- pick the right EAP method (commonly **PEAP + MSCHAPv2**),
- send an "identity", plus your username and password,
- ideally verify the authentication server's certificate, which means embedding the university's CA certificate in the firmware. Skipping that check works but is insecure: a fake access point could capture your password.

ESP-IDF has an example of exactly this (`examples/wifi/wifi_enterprise`), so it's doable.

### Problems a password doesn't fix

- **Your personal credentials live on the device.** Anyone holding the board could read them off the flash chip. And your university account password is probably also your email/grades/etc. password.
- **Client isolation.** Many campus networks block devices on the Wi-Fi from talking *directly to each other*. The ESP32 could join fine and still be unable to reach your laptop's server. This is the most common surprise.
- **Device registration / captive portals.** Some networks require you to register a device's MAC address, or to log in through a web page. The ESP32 can't click through a web page.
- **Password changes** break the device until you reflash it.

### What to do

- **For development:** a phone hotspot or home router (WPA2-Personal, 2.4 GHz), with the server laptop on the same network. That's simple and reliable.
- **For a campus deployment:** ask campus IT whether there's an **IoT / device network**. Many universities have one exactly for this. It's usually WPA2-Personal with MAC registration. Also ask whether devices on it can reach each other, or reach a server you host. Alternatively, host the server somewhere public (a cloud VM), so the ESP32 only needs outgoing internet access, which client isolation doesn't block.

---

## The implemented design: occupied/free windows

This is what `main/main.c` now does.

### Behavior

Time is divided into back-to-back **windows** (5 s by default). During each window the ESP32 reads the sensor's **voltage in millivolts** every 100 ms. When the window ends, it sends one message:

- `"occupied"` if **any** reading in that window was above the threshold (default 1300 mV),
- `"free"` otherwise.

```
time ─────────────────────────────────────────────────────────────►
     │◄──── window 1 (5 s) ────►│◄──── window 2 (5 s) ────►│
       . . . . . . . . . . . . .   . . . . ▲ . . . . . . . .      ← samples every 100 ms
                                           │ one sample > 1300 mV
                                ▼                           ▼
                           POST "free"               POST "occupied"
```

Message format:

```json
{"machine_id":"machine-1","status":"occupied","peak_mv":2580}
```

`peak_mv` is the highest voltage seen in the window. It's useful for checking that the threshold is sensible: ~0 when idle, ~2600 when triggered.

### Why sample every 100 ms rather than once per window?

The goal is "was there *any* motion in the last 5 s?". A single reading at the end of each window would miss motion that started and stopped in between. Sampling 50 times per window catches anything lasting longer than ~100 ms. The FreeRTOS delay between samples costs almost nothing, because the CPU is free for Wi-Fi in the meantime.

### Reading real voltages: ADC calibration

The ADC returns a raw number from 0 to 4095. To compare against a threshold *in volts*, `main.c` converts raw values to millivolts using ESP-IDF's **calibration** driver (`adc_cali_*`). Every ESP32's ADC is slightly different, so Espressif measures each chip at the factory and burns correction values into it. The **line fitting** scheme uses those values to convert raw → mV accurately (typically within a few percent). If a chip lacks the factory data, the code falls back to a rough `raw × 3100 / 4095` conversion and logs a warning.

### Settings live in menuconfig

`main/Kconfig.projbuild` adds a **"Gym Machine Monitor"** menu to `idf.py menuconfig` (or the extension's **SDK Configuration Editor**, the gear icon in the status bar):

| Setting | Default | Meaning |
|---|---|---|
| Wi-Fi network name (SSID) | `my-network` | 2.4 GHz network to join |
| Wi-Fi password | (empty) | Its password |
| Server URL | `http://192.168.1.50:8000/status` | Where to POST |
| Machine ID | `machine-1` | Identifies this device to the server |
| Motion threshold (mV) | `1300` | Above this = motion |
| Report window (ms) | `5000` | Window length |
| Sample interval (ms) | `100` | Time between readings |

These are saved in `sdkconfig`, which is git-ignored, so **your Wi-Fi password never gets committed**. In code they appear as `CONFIG_GYM_...` macros (see the `=y` section above for how Kconfig values reach C code). If you want different defaults for everyone on the team (not secrets), you can put them in `sdkconfig.defaults`, e.g. `CONFIG_GYM_MOTION_THRESHOLD_MV=1300`.

### Known limitation

While the HTTP request is being sent (usually a few tens of milliseconds, up to the 3 s timeout if the server is unreachable), the sensor isn't being read. That time still counts toward the next window. If this ever matters, the fix is to move sampling into its own FreeRTOS task, so sending and sampling run independently.

---

## Where to find the documentation to write this code yourself

Nobody writes ESP-IDF code from memory. Experienced embedded developers do almost exactly this: find an official example that does roughly what they want, copy it, then look up each function in the API reference while trimming it down. `main.c` was built the same way. Reading code fluently but being unable to write it from a blank file is normal at this stage. That skill comes from knowing *where to look*, not from memorizing.

### 1. The official examples (the most useful source)

They're already on your computer, inside the ESP-IDF install. Each one is a complete project you can build and flash as-is.

| Part of `main.c` | Example folder (under `C:\esp\v6.1\esp-idf\examples\`) |
|---|---|
| Overall structure, `app_main`, `vTaskDelay`, logging | `get-started\blink`, `get-started\hello_world` |
| Connecting to Wi-Fi (event group, `wifi_event_handler`, `esp_wifi_*`) | `wifi\getting_started\station` |
| Reading the sensor voltage (`adc_oneshot_*`, `adc_cali_*`) | `peripherals\adc\oneshot_read` |
| Sending the HTTP POST (`esp_http_client_*`) | `protocols\esp_http_client` |
| `menuconfig` settings (`Kconfig.projbuild`) | The `main\Kconfig.projbuild` file in almost any example, e.g. `wifi\getting_started\station` |

Compare `wifi_init_sta()` in our `main.c` with `station\main\station_example_main.c` and you'll see they're nearly identical. That's where it came from.

### 2. The ESP-IDF Programming Guide (the reference)

https://docs.espressif.com/projects/esp-idf/en/stable/esp32/

Use the version dropdown at the top left to choose **v6.1**, since APIs change between versions. Two sections matter most:

- **API Guides**: explains *concepts*, e.g. "Wi-Fi Driver" (the connection steps and events), "Event Handling", "Partition Tables".
- **API Reference**: one page per component, each with an overview in plain English and then every function, struct, and enum with its parameters. The pages you need for this project:
  - Peripherals API → **Analog to Digital Converter (ADC) Oneshot Mode Driver** and **ADC Calibration Driver**
  - Networking APIs → **Wi-Fi**, **ESP-NETIF**
  - Application Protocols → **ESP HTTP Client**
  - Storage API → **Non-Volatile Storage Library** (that's `nvs_flash_init`)
  - System API → **FreeRTOS**, **Event Loop Library**, **Logging library**, **Error Codes and Helper Functions** (`ESP_ERROR_CHECK`)

The Search box on that site works well. Paste in a function name like `esp_http_client_perform` to jump to its documentation.

### 3. The header files (the ground truth)

Every `#include` in `main.c` is a real file under `C:\esp\v6.1\esp-idf\components\`. For example, `esp_http_client.h` is in `components\esp_http_client\include\`. The comment above each function declaration is the same text the website shows. In VS Code, **Ctrl+click** a function name (or right-click → Go to Definition) to jump straight there. This only works after the ESP-IDF extension has configured the project (a build creates the info it needs).

### 4. The ESP32 datasheet and Technical Reference Manual (for hardware questions)

https://www.espressif.com/en/support/documents/technical-documents

Use these for questions like "which pins are input-only?" or "which ADC channel is GPIO 34?". The datasheet shows the pin list. The ADC page in the Programming Guide also has a GPIO ↔ channel table, which is easier to read.

### 5. C itself

If something is a C question rather than an ESP-IDF question (pointers, `struct`s, `static`, `snprintf`, `#define`), use https://en.cppreference.com/w/c. For learning rather than looking things up, *C Programming: A Modern Approach* (K. N. King) or the free *Beej's Guide to C Programming* are good.

### A practice routine

To get from "can read it" to "can write it", rebuild the project in pieces, one example at a time:

1. Build and flash `hello_world` unchanged. Then change what it prints.
2. Copy `peripherals\adc\oneshot_read` into a scratch project, cut it down until it only reads GPIO 34 and logs millivolts. Use the ADC docs page to understand each call you keep.
3. Separately, get `wifi\getting_started\station` to connect to your network.
4. Add the `esp_http_client` code to the Wi-Fi project and POST a fixed string.
5. Combine them. At that point you've written your own version of `main.c`, and you can diff it against ours.

At each step, for every function you keep, look it up and be able to say what each argument is for. That habit matters more than anything else on this list.

---

## Why didn't changing `default` in `Kconfig.projbuild` change the Wi-Fi network?

`Kconfig.projbuild` and `sdkconfig` do different jobs:

- **`Kconfig.projbuild` defines the options**: their names, types, help text, and a *default* value.
- **`sdkconfig` stores the values actually used.** The build reads values from here, not from Kconfig.

A Kconfig `default` is only used when `sdkconfig` **has no value yet** for that option. The first time you built, `sdkconfig` didn't have `CONFIG_GYM_WIFI_SSID`, so it copied the default at that time (`"my-network"`) into `sdkconfig`. From then on, `sdkconfig` had a value, so ESP-IDF kept it. Changing the default later to `"iPhone"` did nothing. (This is the same gotcha as `sdkconfig.defaults` above.)

You can check what the firmware will really use with:

```
grep GYM_ firmware/sdkconfig
```

**How to change a value** (pick one):

1. **menuconfig** (recommended): `idf.py menuconfig` → *Gym Machine Monitor*, or the ESP-IDF extension's SDK Configuration Editor. This writes to `sdkconfig`.
2. **Edit `sdkconfig` directly**: change the `CONFIG_GYM_WIFI_SSID="..."` line. Fine for simple string values.
3. **Reset to the defaults**: delete `firmware/sdkconfig` and rebuild. Every option goes back to its Kconfig default and `sdkconfig.defaults`. Any other menuconfig changes you made are lost.

Then rebuild and flash.

**Don't put the real password in `Kconfig.projbuild`.** That file is committed to Git. `sdkconfig` is git-ignored, so set secrets through menuconfig and keep placeholder defaults like `"my-network"` / `""` in Kconfig.

### Step by step: changing a project setting (Wi-Fi, server URL, etc.)

**Option A: VS Code SDK Configuration Editor (easiest)**

1. Open the `firmware/` folder in VS Code.
2. Open the editor: click the **gear icon** in the ESP-IDF status bar items (tooltip: *SDK Configuration Editor*), or press `Ctrl+Shift+P` and run **ESP-IDF: SDK Configuration Editor (menuconfig)**.
3. A settings page opens. In the left-hand list, click **Gym Machine Monitor**, or type `GYM` in the search box at the top.
4. Edit the fields, e.g. *Wi-Fi network name (SSID)* and *Wi-Fi password*.
5. Click **Save** at the top of the page. This writes to `firmware/sdkconfig`. If you close the page without saving, nothing changes.
6. Click **Build, Flash and Monitor** (flame icon).

**Option B: `menuconfig` in a terminal**

1. Open an ESP-IDF terminal: `Ctrl+Shift+P` → **ESP-IDF: Open ESP-IDF Terminal**. A plain terminal won't work because `idf.py` won't be on the PATH there.
2. Make sure you're in `firmware/` and run `idf.py menuconfig`.
3. A text menu opens. Use the arrow keys to go to **Gym Machine Monitor** and press `Enter`.
4. Highlight an option such as *Wi-Fi network name (SSID)* and press `Enter`. Delete the old text, type the new value, and press `Enter`.
5. Press `S` (save) and then `Enter` to confirm the filename `sdkconfig`. Then press `Q` (quit).
6. Run `idf.py build flash monitor`, or use the flame button.

**Option C: edit the file**

Open `firmware/sdkconfig` in VS Code, press `Ctrl+F`, search for `GYM_`, change the value inside the quotes, save, and rebuild. Only do this for simple values like strings and numbers. Some options depend on others, and menuconfig handles those dependencies for you.

**Check it worked:** the serial monitor log shows which SSID it's connecting to. You can also run `grep GYM_ sdkconfig` before you flash.

**Things that do *not* change the value once `sdkconfig` exists:** editing `default` in `Kconfig.projbuild`, and editing `sdkconfig.defaults`. Those only matter when `sdkconfig` is created from scratch.
