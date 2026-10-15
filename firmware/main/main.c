/*
 * Gym Machine Monitor
 *
 * Reads a motion sensor on GPIO 34 and reports whether the machine is in use.
 *
 * The sensor outputs about 0 V when idle and about 2.6 V when it detects
 * motion. We measure the actual voltage with the ADC (analog-to-digital
 * converter) and treat anything above a configurable threshold (default
 * 1.3 V) as motion.
 *
 * Time is split into back-to-back windows (default 5 s). During a window the
 * sensor is sampled often (default every 100 ms). When the window ends, the
 * ESP32 sends one HTTP POST to the server:
 *   - "occupied" if ANY sample in the window was above the threshold
 *   - "free"     otherwise
 *
 * Settings (Wi-Fi name/password, server URL, threshold, timings) live in
 * menuconfig under "Gym Machine Monitor". See main/Kconfig.projbuild.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_client.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

// The CONFIG_GYM_* values come from menuconfig (main/Kconfig.projbuild).
// ESP-IDF turns them into #defines in the generated header sdkconfig.h.
#define WIFI_SSID           CONFIG_GYM_WIFI_SSID
#define WIFI_PASSWORD       CONFIG_GYM_WIFI_PASSWORD
#define SERVER_URL          CONFIG_GYM_SERVER_URL
#define MACHINE_ID          CONFIG_GYM_MACHINE_ID
#define MOTION_THRESHOLD_MV CONFIG_GYM_MOTION_THRESHOLD_MV
#define REPORT_PERIOD_MS    CONFIG_GYM_REPORT_PERIOD_MS
#define SAMPLE_PERIOD_MS    CONFIG_GYM_SAMPLE_PERIOD_MS

// The sensor is wired to GPIO 34, which is channel 6 of ADC unit 1.
// It must be an ADC1 pin (GPIO 32-39): ADC2 is unavailable while Wi-Fi runs.
// An ADC pin is an analog-to-digital-converter pin.
#define SENSOR_ADC_UNIT    ADC_UNIT_1
#define SENSOR_ADC_CHANNEL ADC_CHANNEL_6

// Attenuation scales the input so higher voltages fit the ADC's range.
// 12 dB gives a usable range of roughly 0-3.1 V, enough for the 2.6 V signal.
#define SENSOR_ADC_ATTEN   ADC_ATTEN_DB_12

// Give up on an HTTP request after this long so a dead server can't stall us.
#define HTTP_TIMEOUT_MS    3000

static const char *TAG = "gym";

/* ------------------------------------------------------------------------ */
/* Wi-Fi                                                                    */
/* ------------------------------------------------------------------------ */

// An event group is a set of flag bits that tasks can set, clear and wait on.
// We use one bit to mean "connected and have an IP address".
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

/*
 * Called by the system event loop whenever something Wi-Fi related happens.
 * Wi-Fi in ESP-IDF is event-driven: we start the driver, and it reports back
 * through this function instead of us polling it.
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // The driver has started; now actually try to join the network.
        ESP_LOGI(TAG, "Connecting to Wi-Fi \"%s\"...", WIFI_SSID);
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Lost the connection (or never got it). Clear the flag so the main
        // loop knows we're offline, then keep retrying forever.
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
        esp_wifi_connect();

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // The router gave us an IP address. Only now can we talk to the server.
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected, IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/*
 * Initializes Wi-Fi in "station" mode (a client joining an existing network,
 * as opposed to "access point" mode where the ESP32 hosts its own network).
 * Blocks until we're connected and have an IP address.
 */
static void wifi_init(void)
{
    s_wifi_events = xEventGroupCreate();

    // Start the TCP/IP stack and the default event loop that delivers events
    // to wifi_event_handler.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Initialize the Wi-Fi driver with default settings (buffers, etc.).
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    // Ask to be told about every Wi-Fi event, plus the "got IP" event.
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    // Network name and password. strncpy because the config struct holds
    // fixed-size byte arrays, not pointers.
    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    // Starting the driver fires WIFI_EVENT_STA_START, and the handler above
    // then calls esp_wifi_connect().
    ESP_ERROR_CHECK(esp_wifi_start());

    // Sleep here until the handler sets WIFI_CONNECTED_BIT.
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                        pdFALSE,          // don't clear the bit on exit
                        pdTRUE,           // wait for all listed bits (just one)
                        portMAX_DELAY);   // wait forever
}

/* ------------------------------------------------------------------------ */
/* Motion sensor (ADC)                                                      */
/* ------------------------------------------------------------------------ */

static adc_oneshot_unit_handle_t s_adc;         // handle to ADC unit 1
static adc_cali_handle_t s_adc_cali = NULL;     // NULL if calibration unavailable

/*
 * Set up the ADC to read the sensor pin, plus a calibration scheme that
 * converts raw readings (0-4095) into millivolts.
 */
static void sensor_init(void)
{
    // "Oneshot" mode: take a single reading each time we ask for one.
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = SENSOR_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = SENSOR_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,       // 12 bits -> raw values 0..4095
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, SENSOR_ADC_CHANNEL, &chan_cfg));

    // Calibration corrects for chip-to-chip differences in the ADC, using
    // values burned into the chip at the factory. The original ESP32 uses the
    // "line fitting" scheme. Older chips may lack the factory data, in which
    // case we fall back to a rough conversion in sensor_read_mv().
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = SENSOR_ADC_UNIT,
        .atten = SENSOR_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration enabled");
    } else {
        s_adc_cali = NULL;
        ESP_LOGW(TAG, "ADC calibration unavailable, voltages will be approximate");
    }
}

/*
 * Read the sensor once and return the voltage on the pin in millivolts.
 */
static int sensor_read_mv(void)
{
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc, SENSOR_ADC_CHANNEL, &raw));

    int mv = 0;
    if (s_adc_cali != NULL) {
        // Calibrated conversion: raw -> millivolts.
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_adc_cali, raw, &mv));
    } else {
        // Uncalibrated fallback: assume 4095 corresponds to ~3100 mV at 12 dB.
        mv = raw * 3100 / 4095;
    }
    return mv;
}

/* ------------------------------------------------------------------------ */
/* Sending to the server                                                    */
/* ------------------------------------------------------------------------ */

/*
 * POST one status message to the server as JSON, e.g.
 *   {"machine_id":"machine-1","status":"occupied","peak_mv":2580}
 * peak_mv is the highest voltage seen during the window, handy for checking
 * that the threshold is set sensibly.
 */
static void send_status(bool occupied, int peak_mv)
{
    // Skip the attempt if Wi-Fi is down. The event handler is already
    // reconnecting in the background.
    if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Not connected to Wi-Fi, skipping send");
        return;
    }

    // Build the JSON body. snprintf never writes past the buffer's end.
    char body[128];
    snprintf(body, sizeof(body),
             "{\"machine_id\":\"%s\",\"status\":\"%s\",\"peak_mv\":%d}",
             MACHINE_ID, occupied ? "occupied" : "free", peak_mv);

    esp_http_client_config_t http_cfg = {
        .url = SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to create HTTP client (check the server URL)");
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    // Perform the request. This blocks until the server answers or we time out.
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sent %s -> HTTP %d", body, esp_http_client_get_status_code(client));
    } else {
        ESP_LOGW(TAG, "Send failed (%s): %s", esp_err_to_name(err), body);
    }

    // Free the client's memory. Forgetting this would leak memory every
    // window until the ESP32 runs out and crashes.
    esp_http_client_cleanup(client);
}

/* ------------------------------------------------------------------------ */
/* Main program                                                             */
/* ------------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "Gym Machine Monitor starting (machine \"%s\")", MACHINE_ID);
    ESP_LOGI(TAG, "Threshold %d mV, window %d ms, sampling every %d ms",
             MOTION_THRESHOLD_MV, REPORT_PERIOD_MS, SAMPLE_PERIOD_MS);

    // NVS (non-volatile storage) must be initialized before Wi-Fi, which
    // stores calibration data there. If the NVS partition is full or was
    // written by a different ESP-IDF version, erase it and try again.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    sensor_init();
    wifi_init();      // blocks until connected

    // Use the FreeRTOS tick count as our clock so each window lasts exactly
    // REPORT_PERIOD_MS, even though sampling and sending take some time.
    TickType_t window_start = xTaskGetTickCount();

    while (1) {
        // --- Observe for one window --------------------------------------
        bool motion_seen = false;   // becomes true if any sample is "high"
        int peak_mv = 0;            // highest voltage seen this window

        while ((xTaskGetTickCount() - window_start) < pdMS_TO_TICKS(REPORT_PERIOD_MS)) {
            int mv = sensor_read_mv();

            if (mv > peak_mv) {
                peak_mv = mv;
            }
            if (mv > MOTION_THRESHOLD_MV) {
                motion_seen = true;
                // Keep sampling anyway so peak_mv stays accurate. Once
                // motion_seen is true it can't go back to false this window.
            }

            // Sleep between samples. This yields the CPU to Wi-Fi and other
            // tasks, and keeps the task watchdog happy.
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
        }

        // The next window starts now, so time spent sending below still
        // counts toward it and the windows stay back-to-back.
        // Note: the sensor isn't sampled while send_status() runs (normally
        // well under a second, at most HTTP_TIMEOUT_MS).
        window_start = xTaskGetTickCount();

        // --- Report ------------------------------------------------------
        send_status(motion_seen, peak_mv);
    }
}
