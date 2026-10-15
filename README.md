# Gym-Machine-Monitoring
An embedded system to monitor machines at the gym to detect whether or not they are available--embedded systems project for CS48900 Fall 2026.

## Project structure

```
firmware/              ESP-IDF project (open this folder in VS Code for ESP work)
  main/                app entry point (main.c)
  components/          reusable modules (sensors, wifi, etc.)
  sdkconfig.defaults   committed config overrides
web/                   the webpage (index.html, style.css, app.js)
docs/                  wiring diagrams, notes, pinouts
EXPLANATIONS.md        background on the tools and concepts used
```

## Building the firmware

```
cd firmware
idf.py set-target esp32     # ESP-WROOM-32 module
idf.py build flash monitor
```
