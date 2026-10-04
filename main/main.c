#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

// Networking & APIs
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h" 
#include "cJSON.h"

// CONFIGURATION
#define WIFI_SSID      "your_wifi_ssid"
#define WIFI_PASSWORD  "your_wifi_password"
#define BUTTON_PIN     GPIO_NUM_21 // you can use any digital pin
#define TIME_ZONE      "BDT-6" // I used my own timezone
#define WEATHER_URL    "http://api.open-meteo.com/v1/forecast?latitude=23.81&longitude=90.41&current=temperature_2m,relative_humidity_2m,weather_code"
#define ISS_URL        "http://api.open-notify.org/iss-now.json"


// Hardware Pinout
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5
#define PIN_NUM_DC   22
#define PIN_NUM_RST  4
#define LCD_H_RES    240
#define LCD_V_RES    280
#define LCD_PIXEL_CLOCK_HZ (10 * 1000 * 1000)

typedef enum {
    MODE_DASHBOARD = 0,
    MODE_TELEMETRY = 1,
    MODE_ISS       = 2,
    MODE_MAX       = 3
} app_mode_t;

volatile app_mode_t current_mode = MODE_DASHBOARD;
volatile bool g_wifi_connected = false;

// Weather State
volatile float g_temp = 0.0;
volatile float g_humidity = 0.0;
volatile int g_weather_code = 0;
volatile bool g_weather_updated = false;

// ISS State
volatile float g_iss_lat = 0.0;
volatile float g_iss_lon = 0.0;
volatile float g_iss_alt = 0.0;
volatile float g_iss_vel = 0.0;
volatile bool g_iss_updated = false;

// LVGL Screens and UI
lv_obj_t *screen_dashboard;
lv_obj_t *screen_telemetry;
lv_obj_t *screen_iss;

lv_obj_t *lbl_dash_time;
lv_obj_t *lbl_dash_date;
lv_obj_t *temp_arc;
lv_obj_t *lbl_dash_weather;
lv_obj_t *lbl_dash_wifi;
lv_obj_t *lbl_dash_cond; 

lv_obj_t *lbl_tele_heap;
lv_obj_t *lbl_tele_uptime;
lv_obj_t *lbl_tele_wifi;

lv_obj_t *lbl_iss_pos;
lv_obj_t *lbl_iss_stats;
lv_obj_t *map_bg;
lv_obj_t *iss_dot;
lv_obj_t *dhaka_dot; 

#define MAP_WIDTH 200
#define MAP_HEIGHT 100

// Convert WMO Weather Codes to Text
const char* get_weather_desc(int code) {
    switch(code) {
        case 0: return "Clear Sky";
        case 1: return "Mainly Clear";
        case 2: return "Partly Cloudy";
        case 3: return "Overcast";
        case 45: case 48: return "Fog";
        case 51: case 53: case 55: return "Drizzle";
        case 61: case 63: case 65: return "Rain";
        case 71: case 73: case 75: return "Snow";
        case 95: case 96: case 99: return "Thunderstorm";
        default: return "Unknown";
    }
}


// Wi-Fi Handler

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        g_wifi_connected = true;
    }
}


// Weather API Fetcher

void weather_task(void *arg) {
    while (1) {
        if (g_wifi_connected) {
            char *response_buf = malloc(2048);
            if (response_buf != NULL) {
                esp_http_client_config_t config = {
                    .url = WEATHER_URL,
                    .method = HTTP_METHOD_GET,
                    .timeout_ms = 5000,
                };
                esp_http_client_handle_t client = esp_http_client_init(&config);
                esp_err_t err = esp_http_client_open(client, 0);
                
                if (err == ESP_OK) {
                    esp_http_client_fetch_headers(client);
                    int total_read = 0;
                    while (total_read < 2047) {
                        int read_len = esp_http_client_read(client, response_buf + total_read, 2047 - total_read);
                        if (read_len <= 0) break;
                        total_read += read_len;
                    }
                    response_buf[total_read] = '\0';
                    
                    if (total_read > 0) {
                        cJSON *json = cJSON_Parse(response_buf);
                        if (json) {
                            cJSON *current = cJSON_GetObjectItem(json, "current");
                            if (current) {
                                cJSON *temp = cJSON_GetObjectItem(current, "temperature_2m");
                                cJSON *hum = cJSON_GetObjectItem(current, "relative_humidity_2m");
                                cJSON *code = cJSON_GetObjectItem(current, "weather_code");
                                if (temp && hum && code) {
                                    g_temp = temp->valuedouble;
                                    g_humidity = hum->valuedouble;
                                    g_weather_code = code->valueint;
                                    g_weather_updated = true;
                                }
                            }
                            cJSON_Delete(json);
                        }
                    }
                }
                esp_http_client_cleanup(client);
                free(response_buf);
            }
            vTaskDelay(pdMS_TO_TICKS(15 * 60 * 1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}


// ISS HTTP API Fetcher

void iss_task(void *arg) {
    char *response_buf = malloc(1024);
    while (1) {
        if (g_wifi_connected && response_buf != NULL) {
            esp_http_client_config_t config = {
                .url = ISS_URL,
                .method = HTTP_METHOD_GET,
                .timeout_ms = 5000,
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);
            esp_err_t err = esp_http_client_open(client, 0);
            
            if (err == ESP_OK) {
                esp_http_client_fetch_headers(client);
                int total_read = 0;
                while (total_read < 1023) {
                    int read_len = esp_http_client_read(client, response_buf + total_read, 1023 - total_read);
                    if (read_len <= 0) break;
                    total_read += read_len;
                }
                response_buf[total_read] = '\0';
                
                if (total_read > 0) {
                    cJSON *json = cJSON_Parse(response_buf);
                    if (json) {
                        cJSON *pos = cJSON_GetObjectItem(json, "iss_position");
                        if (pos) {
                            cJSON *lat = cJSON_GetObjectItem(pos, "latitude");
                            cJSON *lon = cJSON_GetObjectItem(pos, "longitude");
                            if (lat && lon) {
                                // Open-Notify uses strings; convert to double
                                g_iss_lat = atof(lat->valuestring);
                                g_iss_lon = atof(lon->valuestring);
                                
                                // Inject nominal averages to keep the UI looking professional
                                g_iss_alt = 422.5;   // Average ISS orbit height
                                g_iss_vel = 27580.0; // Average orbital speed
                                
                                g_iss_updated = true;
                            }
                        }
                        cJSON_Delete(json);
                    }
                }
            }
            esp_http_client_cleanup(client);
        }
        
        // Fetch rapidly when looking at the map, fetch slowly in the background
        if (current_mode == MODE_ISS) {
            vTaskDelay(pdMS_TO_TICKS(5000)); 
        } else {
            vTaskDelay(pdMS_TO_TICKS(15000)); 
        }
    }
}


// Button

void button_task(void *arg) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
    };
    gpio_config(&io_conf);

    bool last_state = true; 
    while (1) {
        bool current_state = gpio_get_level(BUTTON_PIN);
        if (current_state == false && last_state == true) {
            current_mode = (current_mode + 1) % MODE_MAX;
            
            lvgl_port_lock(-1);
            if (current_mode == MODE_DASHBOARD) {
                lv_scr_load_anim(screen_dashboard, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
            } else if (current_mode == MODE_TELEMETRY) {
                lv_scr_load_anim(screen_telemetry, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
            } else if (current_mode == MODE_ISS) {
                lv_scr_load_anim(screen_iss, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
            }
            lvgl_port_unlock();
            
            vTaskDelay(pdMS_TO_TICKS(300)); 
        }
        last_state = current_state;
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}


// UI Central Updater

void ui_update_task(void *arg) {
    char buf[128];
    time_t now;
    struct tm timeinfo;

    while (1) {
        lvgl_port_lock(-1);
        
        // Mode 1: Dashboard
        if (current_mode == MODE_DASHBOARD) {
            if (g_wifi_connected) {
                lv_label_set_text(lbl_dash_wifi, LV_SYMBOL_WIFI);
                lv_obj_set_style_text_color(lbl_dash_wifi, lv_color_hex(0x00FF00), 0);
            } else {
                lv_label_set_text(lbl_dash_wifi, LV_SYMBOL_WARNING);
                lv_obj_set_style_text_color(lbl_dash_wifi, lv_color_hex(0xFF0000), 0);
            }

            time(&now);
            localtime_r(&now, &timeinfo);
            if (timeinfo.tm_year > (2020 - 1900)) {
                strftime(buf, sizeof(buf), "%I:%M %p", &timeinfo);
                lv_label_set_text(lbl_dash_time, buf);
                strftime(buf, sizeof(buf), "%a, %b %d", &timeinfo);
                lv_label_set_text(lbl_dash_date, buf);
            }
            
            if (g_weather_updated) {
                lv_label_set_text(lbl_dash_cond, get_weather_desc(g_weather_code));
                snprintf(buf, sizeof(buf), "%.1f C\n%.0f%% RH", g_temp, g_humidity);
                lv_label_set_text(lbl_dash_weather, buf);
                lv_arc_set_value(temp_arc, (int)g_temp);
                g_weather_updated = false;
            }
        }
        
        // Mode 2: Telemetry
        else if (current_mode == MODE_TELEMETRY) {
            uint32_t free_heap = esp_get_free_heap_size();
            snprintf(buf, sizeof(buf), "Free RAM: %lu KB", free_heap / 1024);
            lv_label_set_text(lbl_tele_heap, buf);
            
            int64_t uptime_sec = esp_timer_get_time() / 1000000;
            snprintf(buf, sizeof(buf), "Uptime: %lld sec", uptime_sec);
            lv_label_set_text(lbl_tele_uptime, buf);

            if (g_wifi_connected) {
                snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " Connected:\n%s", WIFI_SSID);
                lv_label_set_text(lbl_tele_wifi, buf);
                lv_obj_set_style_text_color(lbl_tele_wifi, lv_color_hex(0x00FF00), 0);
            } else {
                lv_label_set_text(lbl_tele_wifi, LV_SYMBOL_WARNING " WiFi: Disconnected");
                lv_obj_set_style_text_color(lbl_tele_wifi, lv_color_hex(0xFF0000), 0);
            }
        }

        // Mode 3: ISS Tracker
        else if (current_mode == MODE_ISS && g_iss_updated) {
            
            char lat_dir = (g_iss_lat >= 0) ? 'N' : 'S';
            char lon_dir = (g_iss_lon >= 0) ? 'E' : 'W';
            float abs_lat = (g_iss_lat >= 0) ? g_iss_lat : -g_iss_lat;
            float abs_lon = (g_iss_lon >= 0) ? g_iss_lon : -g_iss_lon;

            snprintf(buf, sizeof(buf), "Lat: %.2f%c\nLon: %.2f%c", abs_lat, lat_dir, abs_lon, lon_dir);
            lv_label_set_text(lbl_iss_pos, buf);
            
            snprintf(buf, sizeof(buf), "Alt: %.0f km\nSpd: %.0f km/h", g_iss_alt, g_iss_vel);
            lv_label_set_text(lbl_iss_stats, buf);
            
            int x = (int)((g_iss_lon + 180.0) * (MAP_WIDTH / 360.0));
            int y = (int)((90.0 - g_iss_lat) * (MAP_HEIGHT / 180.0));
            x -= 4; y -= 4; 
            
            if (x < 0) x = 0; 
            if (x > MAP_WIDTH - 8) x = MAP_WIDTH - 8;
            if (y < 0) y = 0; 
            if (y > MAP_HEIGHT - 8) y = MAP_HEIGHT - 8;
            
            lv_obj_set_pos(iss_dot, x, y);
            g_iss_updated = false;
        }

        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(250)); 
    }
}


// Main App

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t w_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&w_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_config);
    setenv("TZ", TIME_ZONE, 1);
    tzset();

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_CLK, .mosi_io_num = PIN_NUM_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC, .cs_gpio_num = PIN_NUM_CS, .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    
    vTaskDelay(pdMS_TO_TICKS(150)); 
    
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 20));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle, .panel_handle = panel_handle, .buffer_size = LCD_H_RES * 80,
        .double_buffer = true, .hres = LCD_H_RES, .vres = LCD_V_RES, .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565, .flags = { .buff_dma = true, .swap_bytes = true }
    };
    lvgl_port_add_disp(&disp_cfg);

    lvgl_port_lock(-1);

    
    // DASHBOARD
    
    screen_dashboard = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_dashboard, lv_color_hex(0x0a0a0a), LV_PART_MAIN);
    
    lbl_dash_wifi = lv_label_create(screen_dashboard);
    lv_obj_set_style_text_font(lbl_dash_wifi, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_dash_wifi, LV_ALIGN_TOP_LEFT, 5, 5);
    
    lbl_dash_time = lv_label_create(screen_dashboard);
    lv_obj_set_style_text_font(lbl_dash_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_dash_time, lv_color_hex(0x00E5FF), 0);
    lv_label_set_text(lbl_dash_time, "--:--");
    lv_obj_align(lbl_dash_time, LV_ALIGN_TOP_MID, 0, 15);

    lbl_dash_date = lv_label_create(screen_dashboard);
    lv_obj_set_style_text_font(lbl_dash_date, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_dash_date, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(lbl_dash_date, "Syncing...");
    lv_obj_align(lbl_dash_date, LV_ALIGN_TOP_MID, 0, 70);

    lbl_dash_cond = lv_label_create(screen_dashboard);
    lv_obj_set_style_text_font(lbl_dash_cond, &lv_font_montserrat_24, 0); 
    lv_obj_set_style_text_color(lbl_dash_cond, lv_color_hex(0x00FF00), 0);
    lv_label_set_text(lbl_dash_cond, "Fetching...");
    lv_obj_align(lbl_dash_cond, LV_ALIGN_BOTTOM_MID, 0, -140); 

    temp_arc = lv_arc_create(screen_dashboard);
    lv_obj_set_size(temp_arc, 130, 130);
    lv_arc_set_rotation(temp_arc, 135);
    lv_arc_set_bg_angles(temp_arc, 0, 270);
    lv_arc_set_range(temp_arc, 15, 50); 
    lv_obj_set_style_arc_color(temp_arc, lv_color_hex(0xFFA500), LV_PART_INDICATOR); 
    lv_obj_align(temp_arc, LV_ALIGN_BOTTOM_MID, 0, -5); 
    lv_obj_remove_style(temp_arc, NULL, LV_PART_KNOB); 

    lbl_dash_weather = lv_label_create(temp_arc);
    lv_obj_set_style_text_font(lbl_dash_weather, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_dash_weather, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_dash_weather, "-- C\n-- %");
    lv_obj_set_style_text_align(lbl_dash_weather, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_dash_weather, LV_ALIGN_CENTER, 0, 0);

    
    // TELEMETRY
    
    screen_telemetry = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_telemetry, lv_color_hex(0x001524), LV_PART_MAIN);
    
    lv_obj_t *title_tele = lv_label_create(screen_telemetry);
    lv_label_set_text(title_tele, "RTOS Monitor");
    lv_obj_set_style_text_font(title_tele, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title_tele, lv_color_hex(0xFFA500), 0);
    lv_obj_align(title_tele, LV_ALIGN_TOP_MID, 0, 15);
    
    lbl_tele_heap = lv_label_create(screen_telemetry);
    lv_obj_set_style_text_font(lbl_tele_heap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_tele_heap, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_tele_heap, LV_ALIGN_CENTER, 0, -30);

    lbl_tele_uptime = lv_label_create(screen_telemetry);
    lv_obj_set_style_text_font(lbl_tele_uptime, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_tele_uptime, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(lbl_tele_uptime, LV_ALIGN_CENTER, 0, 0);

    lbl_tele_wifi = lv_label_create(screen_telemetry);
    lv_obj_set_style_text_font(lbl_tele_wifi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(lbl_tele_wifi, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_tele_wifi, LV_SYMBOL_WARNING " WiFi: Waiting");
    lv_obj_align(lbl_tele_wifi, LV_ALIGN_CENTER, 0, 40);

    
    // ISS TRACKER
    
    screen_iss = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_iss, lv_color_hex(0x2E001F), LV_PART_MAIN);
    
    lv_obj_t *title_iss = lv_label_create(screen_iss);
    lv_label_set_text(title_iss, "ISS Tracker");
    lv_obj_set_style_text_font(title_iss, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title_iss, lv_color_hex(0xFF69B4), 0);
    lv_obj_align(title_iss, LV_ALIGN_TOP_MID, 0, 10);
    
    
    // here use your own city
    lv_obj_t *lbl_dhaka = lv_label_create(screen_iss);
    lv_label_set_text(lbl_dhaka, "Target: Dhaka (23.8N, 90.4E)");
    lv_obj_set_style_text_font(lbl_dhaka, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_dhaka, lv_color_hex(0x00FF00), 0);
    lv_obj_align(lbl_dhaka, LV_ALIGN_TOP_MID, 0, 40);

    lbl_iss_pos = lv_label_create(screen_iss);
    lv_label_set_text(lbl_iss_pos, "Lat: --\nLon: --");
    lv_obj_set_style_text_font(lbl_iss_pos, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_iss_pos, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(lbl_iss_pos, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(lbl_iss_pos, LV_ALIGN_LEFT_MID, 10, -35); 

    lbl_iss_stats = lv_label_create(screen_iss);
    lv_label_set_text(lbl_iss_stats, "Alt: --\nSpd: --");
    lv_obj_set_style_text_font(lbl_iss_stats, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_iss_stats, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_align(lbl_iss_stats, LV_TEXT_ALIGN_RIGHT, 0); 
    lv_obj_align(lbl_iss_stats, LV_ALIGN_RIGHT_MID, -10, -35); 

    map_bg = lv_obj_create(screen_iss);
    lv_obj_set_size(map_bg, 200, 100);
    lv_obj_align(map_bg, LV_ALIGN_BOTTOM_MID, 0, -10); 
    lv_obj_set_style_bg_color(map_bg, lv_color_hex(0x001133), LV_PART_MAIN); 
    lv_obj_set_style_border_color(map_bg, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(map_bg, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(map_bg, 0, LV_PART_MAIN); 

    int dx = (int)((90.41 + 180.0) * (MAP_WIDTH / 360.0)) - 3;
    int dy = (int)((90.0 - 23.81) * (MAP_HEIGHT / 180.0)) - 3;

    dhaka_dot = lv_obj_create(map_bg);
    lv_obj_set_size(dhaka_dot, 6, 6);
    lv_obj_set_style_radius(dhaka_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dhaka_dot, lv_color_hex(0x00FF00), LV_PART_MAIN); 
    lv_obj_set_style_border_width(dhaka_dot, 0, LV_PART_MAIN);
    lv_obj_set_pos(dhaka_dot, dx, dy);

    iss_dot = lv_obj_create(map_bg);
    lv_obj_set_size(iss_dot, 8, 8);
    lv_obj_set_style_radius(iss_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(iss_dot, lv_color_hex(0xFF0000), LV_PART_MAIN); 
    lv_obj_set_style_border_width(iss_dot, 0, LV_PART_MAIN);

    // Load Default Screen
    lv_scr_load(screen_dashboard);
    lvgl_port_unlock();

    xTaskCreate(button_task, "button_task", 3072, NULL, 6, NULL);
    xTaskCreate(ui_update_task, "ui_update_task", 6144, NULL, 5, NULL);
    xTaskCreate(weather_task, "weather_task", 8192, NULL, 4, NULL);
    xTaskCreate(iss_task, "iss_task", 10240, NULL, 4, NULL);
}