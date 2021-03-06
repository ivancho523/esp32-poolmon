/*
 * MIT License
 *
 * Copyright (c) 2018 David Antliff
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "display.h"
#include "constants.h"
#include "resources.h"
#include "utils.h"
#include "i2c_master.h"
#include "smbus.h"
#include "i2c-lcd1602.h"
#include "avr_support.h"
#include "sensor_temp.h"
#include "wifi_support.h"
#include "mqtt.h"
#include "button.h"
#include "rotary_encoder.h"
#include "datastore/datastore.h"
#include "led.h"
#include "control.h"
#include "sdkconfig.h"

#define TAG "display"

#define SMBUS_TIMEOUT      1000   // milliseconds
#define TICKS_PER_UPDATE   (500 / portTICK_RATE_MS)
#define MEASUREMENT_EXPIRY (15 * 1000000)  // microseconds after which a measurement is not displayed (except temp)

#define LCD_NUM_ROWS               4
#define LCD_NUM_COLUMNS            40
#define LCD_NUM_VISIBLE_COLUMNS    20

#define DISPLAY_WIDTH      LCD_NUM_VISIBLE_COLUMNS
#define ROW_STRING_WIDTH   (DISPLAY_WIDTH + 1)    // room for null terminator

#ifndef BUILD_TIMESTAMP
#  warning "Please ensure BUILD_TIMESTAMP is defined"
#  define BUILD_TIMESTAMP "undefined"
#endif

#define INITIAL_PAGE DISPLAY_PAGE_MAIN

typedef struct
{
    char * row[LCD_NUM_ROWS];
} page_buffer_t;

typedef void (*page_handler_t)(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);

typedef struct
{
    display_page_id_t id;
    page_handler_t handler;
    void * state;
} page_spec_t;

static TaskHandle_t _task_handle = NULL;

// page handlers are responsible for displaying their content
static void _handle_page_blank(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_main(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_sensors_temp(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_sensors_temp2(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_sensors_light(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_sensors_flow(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_power(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_switches(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_pump_status(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_cp_control(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_pp_control(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_alarm(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_wifi_status(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_mqtt_status(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_resource_status(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);
static void _handle_page_avr_status(page_buffer_t * page_buffer, void * state, const datastore_t * datastore);

static bool main_activity = false;
static bool blink_arrow = false;

static const page_spec_t page_specs[] = {
    // ID                       handler                        state
    { DISPLAY_PAGE_BLANK,               _handle_page_blank,            NULL },
    { DISPLAY_PAGE_MAIN,                _handle_page_main,             &main_activity },
    { DISPLAY_PAGE_SENSORS_TEMP,        _handle_page_sensors_temp,     &blink_arrow },
    { DISPLAY_PAGE_SENSORS_TEMP_2,      _handle_page_sensors_temp2,    &blink_arrow },
    { DISPLAY_PAGE_SENSORS_LIGHT,       _handle_page_sensors_light,    NULL },
    { DISPLAY_PAGE_SENSORS_FLOW,        _handle_page_sensors_flow,     NULL },
    { DISPLAY_PAGE_POWER,               _handle_page_power,            NULL },
    { DISPLAY_PAGE_SWITCHES,            _handle_page_switches,         NULL },
    { DISPLAY_PAGE_PUMP_STATUS,         _handle_page_pump_status,      NULL },
    { DISPLAY_PAGE_CP_CONTROL,          _handle_page_cp_control,       NULL },
    { DISPLAY_PAGE_PP_CONTROL,          _handle_page_pp_control,       NULL },
    { DISPLAY_PAGE_ALARM,               _handle_page_alarm,            NULL },
    { DISPLAY_PAGE_WIFI_STATUS,         _handle_page_wifi_status,      NULL },
    { DISPLAY_PAGE_MQTT_STATUS,         _handle_page_mqtt_status,      NULL },
    { DISPLAY_PAGE_RESOURCE_STATUS,     _handle_page_resource_status,  NULL },
    { DISPLAY_PAGE_AVR_STATUS,          _handle_page_avr_status,       NULL },
};

// page transition table
typedef struct
{
    display_page_id_t current;
    display_page_id_t on_counter_clockwise;  // next page on turn counter-clockwise
    display_page_id_t on_clockwise;          // next page on turn clockwise
    display_page_id_t on_short;              // new page on single short press
    display_page_id_t on_long;               // new page on single long press
} transition_t;

static const transition_t transitions[] = {
    // ID                       counter-clockwise      clockwise               short                  long
    { DISPLAY_PAGE_BLANK,               DISPLAY_PAGE_MAIN,             DISPLAY_PAGE_MAIN,              DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_MAIN,                DISPLAY_PAGE_AVR_STATUS,       DISPLAY_PAGE_SENSORS_TEMP,      DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_SENSORS_TEMP,        DISPLAY_PAGE_MAIN,             DISPLAY_PAGE_SENSORS_LIGHT,     DISPLAY_PAGE_SENSORS_TEMP_2,   DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_SENSORS_TEMP_2,      DISPLAY_PAGE_MAIN,             DISPLAY_PAGE_SENSORS_LIGHT,     DISPLAY_PAGE_SENSORS_TEMP,     DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_SENSORS_LIGHT,       DISPLAY_PAGE_SENSORS_TEMP,     DISPLAY_PAGE_SENSORS_FLOW,      DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_SENSORS_FLOW,        DISPLAY_PAGE_SENSORS_LIGHT,    DISPLAY_PAGE_POWER,             DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_POWER,               DISPLAY_PAGE_SENSORS_FLOW,     DISPLAY_PAGE_SWITCHES,          DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_SWITCHES,            DISPLAY_PAGE_SENSORS_FLOW,     DISPLAY_PAGE_PUMP_STATUS,       DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_PUMP_STATUS,         DISPLAY_PAGE_SWITCHES,         DISPLAY_PAGE_CP_CONTROL,        DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_CP_CONTROL,          DISPLAY_PAGE_PUMP_STATUS,      DISPLAY_PAGE_PP_CONTROL,        DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_PP_CONTROL,          DISPLAY_PAGE_CP_CONTROL,       DISPLAY_PAGE_ALARM,             DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_ALARM,               DISPLAY_PAGE_PP_CONTROL,       DISPLAY_PAGE_WIFI_STATUS,       DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_WIFI_STATUS,         DISPLAY_PAGE_ALARM,            DISPLAY_PAGE_MQTT_STATUS,       DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_MQTT_STATUS,         DISPLAY_PAGE_WIFI_STATUS,      DISPLAY_PAGE_RESOURCE_STATUS,   DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_RESOURCE_STATUS,     DISPLAY_PAGE_MQTT_STATUS,      DISPLAY_PAGE_AVR_STATUS,        DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
    { DISPLAY_PAGE_AVR_STATUS,          DISPLAY_PAGE_RESOURCE_STATUS,  DISPLAY_PAGE_MAIN,              DISPLAY_PAGE_IGNORE,           DISPLAY_PAGE_IGNORE },
};

static const char * BLANK_LINE = "                    ";

typedef struct
{
    i2c_master_info_t * i2c_master_info;
    const datastore_t * datastore;
    QueueHandle_t input_queue;
} task_inputs_t;

#define DOT "\xa5"    // 0b10100101

static const uint8_t degrees_C[8]  = { 0x10, 0x06, 0x09, 0x08, 0x08, 0x09, 0x06, 0x00 };
#define DEGREES_C "\x8"

static const uint8_t arrow_down[8]  = { 0b00000,
                                        0b11111,
                                        0b11111,
                                        0b01110,
                                        0b01110,
                                        0b00100,
                                        0b00100,
                                        0b00000 };
#define ARROW_DOWN "\x9"

static const uint8_t arrow_up[8]  = { 0b00000,
                                      0b00100,
                                      0b00100,
                                      0b01110,
                                      0b01110,
                                      0b11111,
                                      0b11111,
                                      0b00000 };
#define ARROW_UP "\xa"

static const uint8_t delta[8]  = { 0b00000,
                                   0b00001,
                                   0b00011,
                                   0b00101,
                                   0b01001,
                                   0b10001,
                                   0b11111,
                                   0b00000 };
#define DELTA "\xb"

static esp_err_t _display_reset(const i2c_lcd1602_info_t * lcd_info)
{
    ESP_LOGI(TAG, "display reset");
    esp_err_t err = i2c_lcd1602_reset(lcd_info);
    // Define custom characters
    if (err == ESP_OK)
    {
        err = i2c_lcd1602_define_char(lcd_info, I2C_LCD1602_INDEX_CUSTOM_0, degrees_C);
        I2C_LCD1602_ERROR_CHECK(err);
    }
    if (err == ESP_OK)
    {
        err = i2c_lcd1602_define_char(lcd_info, I2C_LCD1602_INDEX_CUSTOM_1, arrow_down);
        I2C_LCD1602_ERROR_CHECK(err);
    }
    if (err == ESP_OK)
    {
        err = i2c_lcd1602_define_char(lcd_info, I2C_LCD1602_INDEX_CUSTOM_2, arrow_up);
        I2C_LCD1602_ERROR_CHECK(err);
    }
    if (err == ESP_OK)
    {
        err = i2c_lcd1602_define_char(lcd_info, I2C_LCD1602_INDEX_CUSTOM_3, delta);
        I2C_LCD1602_ERROR_CHECK(err);
    }
    return err;
}

// display wrappers to reset and reinitialise display on any I2C error
static esp_err_t _clear(const i2c_lcd1602_info_t * lcd_info)
{
    esp_err_t err = ESP_FAIL;
    int count = 0;
    while (count < 10 && (err = i2c_lcd1602_clear(lcd_info)) != ESP_OK)
    {
        ++count;
        vTaskDelay(10 / portTICK_RATE_MS);
        _display_reset(lcd_info);
        ESP_LOGW(TAG, "retry _clear %d", count);
    }
    return err;
}

static esp_err_t _move_cursor(const i2c_lcd1602_info_t * lcd_info, uint8_t col, uint8_t row)
{
    esp_err_t err = ESP_FAIL;
    int count = 0;
    while (count < 10 && (err = i2c_lcd1602_move_cursor(lcd_info, col, row)) != ESP_OK)
    {
        ++count;
        vTaskDelay(10 / portTICK_RATE_MS);
        _display_reset(lcd_info);
        ESP_LOGW(TAG, "retry _move_cursor %d", count);
    }
    return err;
}

static esp_err_t _write_string(const i2c_lcd1602_info_t * lcd_info, const char * string)
{
    esp_err_t err = ESP_FAIL;
    int count = 0;
    while (count < 10 && (err = i2c_lcd1602_write_string(lcd_info, string)) != ESP_OK)
    {
        ++count;
        vTaskDelay(10 / portTICK_RATE_MS);
        _display_reset(lcd_info);
        ESP_LOGW(TAG, "retry _write_string %d", count);
    }
    return err;
}


static void _handle_page_blank(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    for (int i = 0; i < LCD_NUM_ROWS; ++i)
    {
        page_buffer->row[i][0] = '\0';
    }
}

static void _split_time(uint32_t time, uint32_t * days, uint32_t * hours, uint32_t * minutes, uint32_t * seconds)
{
    *days = time / 60 / 60 / 24;
    *hours = time / 60 / 60 % 24;
    *minutes = time / 60 % 60;
    *seconds = time % 60;
}

static void _render_uptime(char * buffer, size_t size, uint32_t uptime)
{
    uint32_t days, hours, minutes, seconds;
    _split_time(uptime, &days, &hours, &minutes, &seconds);
    snprintf(buffer, size, "Up %4dd %02d:%02d:%02d", days, hours, minutes, seconds);
}

static void _handle_page_main(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    bool * activity = (bool *)state;

    char version[SYSTEM_LEN_VERSION] = "";
    char build_date_time[SYSTEM_LEN_BUILD_DATE_TIME] = "";
    char git_commit[SYSTEM_LEN_BUILD_GIT_COMMIT] = "";
    datastore_get_string(datastore, RESOURCE_ID_SYSTEM_VERSION, 0, version, sizeof(version));
    datastore_get_string(datastore, RESOURCE_ID_SYSTEM_BUILD_DATE_TIME, 0, build_date_time, sizeof(build_date_time));
    datastore_get_string(datastore, RESOURCE_ID_SYSTEM_BUILD_GIT_COMMIT, 0, git_commit, sizeof(git_commit));
//
    snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "PoolMon v%-6s", version);
    snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "%s", build_date_time);
    snprintf(page_buffer->row[2], ROW_STRING_WIDTH, "%s", git_commit);

    uint32_t uptime = seconds_since_boot(); // in seconds
    _render_uptime(page_buffer->row[3], ROW_STRING_WIDTH, uptime);

    if (*activity)
    {
        strncat(page_buffer->row[3], "  "DOT, ROW_STRING_WIDTH);
    }

    led_flash(50, 0, 1);
    *activity = !(*activity);
}

static void _get_temp_sensor(const datastore_t * store, datastore_instance_id_t instance, float * value, char * label, size_t label_size, datastore_age_t * age, const datastore_t * datastore)
{
    assert(value);
    assert(label);
    assert(age);
    *value = 0.0f;
    label[0] = '\0';
    *age = DATASTORE_INVALID_AGE;
    datastore_get_float(datastore, RESOURCE_ID_TEMP_VALUE, instance, value);
    datastore_get_string(datastore, RESOURCE_ID_TEMP_LABEL, instance, label, label_size);
    datastore_get_age(datastore, RESOURCE_ID_TEMP_VALUE, instance, age);
}

static void _render_temp_line(char * line, unsigned int len, datastore_instance_id_t instance, const datastore_t * datastore)
{
    float value = 0.0f;
    char label[SENSOR_TEMP_LEN_LABEL] = "";
    datastore_age_t age = DATASTORE_INVALID_AGE;

    _get_temp_sensor(datastore, instance, &value, label, sizeof(label), &age, datastore);

    if (age < sensor_temp_expiry(datastore))
    {
        snprintf(line, ROW_STRING_WIDTH, "T%d %-10s %4.1f"DEGREES_C, instance + 1, label, value);
    }
    else
    {
        snprintf(line, ROW_STRING_WIDTH, "T%d %-10s --.-  "DEGREES_C, instance + 1, label);
    }
}

static void _handle_page_sensors_temp(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    _render_temp_line(page_buffer->row[0], ROW_STRING_WIDTH, 0, datastore);
    _render_temp_line(page_buffer->row[1], ROW_STRING_WIDTH, 1, datastore);
    _render_temp_line(page_buffer->row[2], ROW_STRING_WIDTH, 2, datastore);
    _render_temp_line(page_buffer->row[3], ROW_STRING_WIDTH, 3, datastore);

    bool * blink_arrow = (bool *)state;
    if (*blink_arrow)
    {
        page_buffer->row[3][LCD_NUM_VISIBLE_COLUMNS - 1] = I2C_LCD1602_CHARACTER_CUSTOM_1;
    }
    *blink_arrow = !*blink_arrow;
}

static void _handle_page_sensors_temp2(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    _render_temp_line(page_buffer->row[0], ROW_STRING_WIDTH, 1, datastore);
    _render_temp_line(page_buffer->row[1], ROW_STRING_WIDTH, 2, datastore);
    _render_temp_line(page_buffer->row[2], ROW_STRING_WIDTH, 3, datastore);
    _render_temp_line(page_buffer->row[3], ROW_STRING_WIDTH, 4, datastore);

    bool * blink_arrow = (bool *)state;
    if (*blink_arrow)
    {
        page_buffer->row[0][LCD_NUM_VISIBLE_COLUMNS - 1] = I2C_LCD1602_CHARACTER_CUSTOM_2;
    }
    *blink_arrow = !*blink_arrow;
}

static void _handle_page_sensors_light(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    bool detected = false;
    uint32_t full = 0, visible = 0, infrared = 0, illuminance = 0;
    datastore_get_bool(datastore, RESOURCE_ID_LIGHT_DETECTED, 0, &detected);

    if (detected)
    {
        datastore_age_t age = 0;
        datastore_get_age(datastore, RESOURCE_ID_LIGHT_FULL, 0, &age);

        if (age < MEASUREMENT_EXPIRY)
        {
            datastore_get_uint32(datastore, RESOURCE_ID_LIGHT_FULL, 0, &full);
            datastore_get_uint32(datastore, RESOURCE_ID_LIGHT_VISIBLE, 0, &visible);
            datastore_get_uint32(datastore, RESOURCE_ID_LIGHT_INFRARED, 0, &infrared);
            datastore_get_uint32(datastore, RESOURCE_ID_LIGHT_ILLUMINANCE, 0, &illuminance);

            snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "Light Full     %5d", full);
            snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "      Lux      %5d", illuminance);
            snprintf(page_buffer->row[2], ROW_STRING_WIDTH, "      Infrared %5d", infrared);
            snprintf(page_buffer->row[3], ROW_STRING_WIDTH, "      Visible  %5d", visible);
        }
        else
        {
            snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "Light Full     -----");
            snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "      Lux      -----");
            snprintf(page_buffer->row[2], ROW_STRING_WIDTH, "      Infrared -----");
            snprintf(page_buffer->row[3], ROW_STRING_WIDTH, "      Visible  -----");
        }
    }
    else
    {
        // sensor not detected at boot
        snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "Light Full     ????");
        snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "      Lux      ????");
        snprintf(page_buffer->row[2], ROW_STRING_WIDTH, "      Infrared ????");
        snprintf(page_buffer->row[3], ROW_STRING_WIDTH, "      Visible  ????");
    }
}

static void _handle_page_sensors_flow(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    float frequency = 0.0f, rate = 0.0f;
    datastore_get_float(datastore, RESOURCE_ID_FLOW_FREQUENCY, 0, &frequency);
    datastore_get_float(datastore, RESOURCE_ID_FLOW_RATE, 0, &rate);

    datastore_age_t age = DATASTORE_INVALID_AGE;
    datastore_get_age(datastore, RESOURCE_ID_FLOW_FREQUENCY, 0, &age);

    if (age < MEASUREMENT_EXPIRY)
    {
        snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "Flow Rate  %5.1f LPM", rate);
        snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "           %5.1f Hz", frequency);
    }
    else
    {
        // measurement timed out
        snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "Flow Rate  ---.- LPM");
        snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "           ---.- Hz");
    }
}

static void _handle_page_power(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "Power Calculation");

    float delta = 0;
    datastore_get_float(datastore, RESOURCE_ID_POWER_TEMP_DELTA, 0, &delta);

    datastore_age_t age = DATASTORE_INVALID_AGE;
    datastore_get_age(datastore, RESOURCE_ID_POWER_TEMP_DELTA, 0, &age);
    if (age < MEASUREMENT_EXPIRY)
    {
        snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "Temp Delta %5.1f "DEGREES_C, delta);
    }
    else
    {
        // measurement timed out
        snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "Temp Delta ---.- "DEGREES_C);
    }

    float rate = 0;
    datastore_get_float(datastore, RESOURCE_ID_FLOW_RATE, 0, &rate);

    age = DATASTORE_INVALID_AGE;
    datastore_get_age(datastore, RESOURCE_ID_FLOW_FREQUENCY, 0, &age);
    if (age < MEASUREMENT_EXPIRY)
    {
        snprintf(page_buffer->row[2], ROW_STRING_WIDTH, "Flow Rate  %5.1f LPM", rate);
    }
    else
    {
        // measurement timed out
        snprintf(page_buffer->row[2], ROW_STRING_WIDTH, "Flow Rate  ---.- LPM");
    }

    float power = 0.0f;
    age = DATASTORE_INVALID_AGE;
    datastore_get_age(datastore, RESOURCE_ID_POWER_VALUE, 0, &age);
    if (age < MEASUREMENT_EXPIRY)
    {
        datastore_get_float(datastore, RESOURCE_ID_POWER_VALUE, 0, &power);
        snprintf(page_buffer->row[3], ROW_STRING_WIDTH, "Power    %7.1f W", power);
    }
    else
    {
        snprintf(page_buffer->row[3], ROW_STRING_WIDTH, "Power       --.- W");
    }
}

static void _build_switch_mode_line(char * buf, uint8_t len, const char * label, datastore_resource_id_t value_id, datastore_resource_id_t count_id, const datastore_t * datastore)
{
    avr_switch_mode_t value = 0;
    uint32_t count = 0;

    datastore_get_uint32(datastore, value_id, 0, &value);
    datastore_get_uint32(datastore, count_id, 0, &count);

    snprintf(buf, len, "%s Swi Mode %4s %3d", label, value == AVR_SWITCH_MODE_AUTO ? "AUTO" : "MAN", count);
}

static void _build_switch_man_line(char * buf, uint8_t len, datastore_resource_id_t value_id, datastore_resource_id_t count_id, const datastore_t * datastore)
{
    avr_switch_manual_t value = 0;
    uint32_t count = 0;

    datastore_get_uint32(datastore, value_id, 0, &value);
    datastore_get_uint32(datastore, count_id, 0, &count);

    snprintf(buf, len, "       Man  %4s %3d", value == AVR_SWITCH_MANUAL_ON ? "ON" : "OFF", count);
}

static void _handle_page_switches(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    _build_switch_mode_line(page_buffer->row[0], ROW_STRING_WIDTH, "CP", RESOURCE_ID_SWITCHES_CP_MODE_VALUE, RESOURCE_ID_AVR_COUNT_CP_MODE, datastore);
    _build_switch_man_line(page_buffer->row[1], ROW_STRING_WIDTH, RESOURCE_ID_SWITCHES_CP_MAN_VALUE, RESOURCE_ID_AVR_COUNT_CP_MAN, datastore);
    _build_switch_mode_line(page_buffer->row[2], ROW_STRING_WIDTH, "PP", RESOURCE_ID_SWITCHES_PP_MODE_VALUE, RESOURCE_ID_AVR_COUNT_PP_MODE, datastore);
    _build_switch_man_line(page_buffer->row[3], ROW_STRING_WIDTH, RESOURCE_ID_SWITCHES_PP_MAN_VALUE, RESOURCE_ID_AVR_COUNT_PP_MAN, datastore);
 }

static void _build_pump_state_line(char * buf, uint8_t len, const char * label, datastore_instance_id_t state_id, const datastore_t * datastore)
{
    avr_pump_state_t state = AVR_PUMP_STATE_OFF;
    datastore_get_uint32(datastore, state_id, 0, &state);
    snprintf(buf, len, "%s Status        %3s", label, state == AVR_PUMP_STATE_OFF ? "OFF" : "ON");
}

static void _build_pump_stats_line(char * buf, uint8_t len, datastore_instance_id_t count_id, const datastore_t * datastore)
{
    uint32_t count = 0;
    datastore_get_uint32(datastore, count_id, 0, &count);
    datastore_age_t age = 0;
    datastore_get_age(datastore, count_id, 0, &age);
    uint32_t days, hours, minutes, seconds;
    if (age == DATASTORE_INVALID_AGE)
    {
        age = microseconds_since_boot();
    }
    _split_time(age / 1000000, &days, &hours, &minutes, &seconds);
    snprintf(buf, len, "# %3d %4dd %02d:%02d:%02d", count, days, hours, minutes, seconds);
}

static void _handle_page_pump_status(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    _build_pump_state_line(page_buffer->row[0], ROW_STRING_WIDTH, "CP", RESOURCE_ID_PUMPS_CP_STATE, datastore);
    _build_pump_stats_line(page_buffer->row[1], ROW_STRING_WIDTH, RESOURCE_ID_AVR_COUNT_CP, datastore);
    _build_pump_state_line(page_buffer->row[2], ROW_STRING_WIDTH, "PP", RESOURCE_ID_PUMPS_PP_STATE, datastore);
    _build_pump_stats_line(page_buffer->row[3], ROW_STRING_WIDTH, RESOURCE_ID_AVR_COUNT_PP, datastore);
}

static void _handle_page_cp_control(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    control_cp_state_t cp_state = CONTROL_CP_STATE_OFF;
    datastore_get_uint32(datastore, RESOURCE_ID_CONTROL_STATE_CP, 0, &cp_state);
    snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "CP Control       %3s", cp_state == CONTROL_CP_STATE_ON ? "ON" : "OFF");

    float temp_low = 0.0f, temp_high = 0.0f;
    datastore_get_float(datastore, RESOURCE_ID_TEMP_VALUE, CONTROL_CP_SENSOR_LOW_INSTANCE, &temp_low);
    datastore_get_float(datastore, RESOURCE_ID_TEMP_VALUE, CONTROL_CP_SENSOR_HIGH_INSTANCE, &temp_high);
    snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "Lo  %2.1f"DEGREES_C "  Hi  %2.1f"DEGREES_C , temp_low, temp_high);

    float delta_on = 0.0f, delta_off = 0.0f;
    datastore_get_float(datastore, RESOURCE_ID_CONTROL_CP_ON_DELTA, 0, &delta_on);
    datastore_get_float(datastore, RESOURCE_ID_CONTROL_CP_OFF_DELTA, 0, &delta_off);
    snprintf(page_buffer->row[2], ROW_STRING_WIDTH, DELTA"on %4.1f"DEGREES_C " "DELTA"off %4.1f"DEGREES_C , delta_on, delta_off);

    float diff = temp_high - temp_low;
    float active_delta = cp_state == CONTROL_CP_STATE_OFF ? delta_on : delta_off;
    float margin = active_delta - diff;
    snprintf(page_buffer->row[3], ROW_STRING_WIDTH, DELTA"T  %4.1f"DEGREES_C " "DELTA"Th  %4.1f"DEGREES_C , diff, -margin);
}

// TODO: refactor into a Time module
static void _get_local_time(time_t * now_time, struct tm * timeinfo)
{
    assert(now_time != NULL);
    assert(timeinfo != NULL);
    time(now_time);
    localtime_r(now_time, timeinfo);
}

static void _handle_page_pp_control(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    control_pp_state_t pp_state = CONTROL_PP_STATE_OFF;
    datastore_get_uint32(datastore, RESOURCE_ID_CONTROL_STATE_PP, 0, &pp_state);
    const char * state_desc = NULL;
    switch (pp_state)
    {
        case CONTROL_PP_STATE_ON: state_desc = "ON"; break;
        case CONTROL_PP_STATE_OFF: state_desc = "OFF"; break;
        case CONTROL_PP_STATE_PAUSE: state_desc = "PAUSE"; break;
        case CONTROL_PP_STATE_EMERGENCY: state_desc = "EMERGENCY"; break;
        default: state_desc = "ERROR";
    }
    snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "PP Control %9s", state_desc);

    control_cp_state_t cp_state = CONTROL_CP_STATE_OFF;
    datastore_get_uint32(datastore, RESOURCE_ID_CONTROL_STATE_CP, 0, &cp_state);
    if (cp_state == CONTROL_CP_STATE_ON)
    {
        datastore_age_t cp_state_age = DATASTORE_INVALID_AGE;
        datastore_get_age(datastore, RESOURCE_ID_CONTROL_STATE_CP, 0, &cp_state_age);
        uint32_t days, hours, minutes, seconds;
        _split_time(cp_state_age / 1000000, &days, &hours, &minutes, &seconds);
        snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "CP ON     %4d:%02d:%02d", days * 24 + hours, minutes, seconds);
    }
    else
    {
        snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "CP OFF");
    }

    float flow = 0.0f, flow_threshold = 0.0f;
    datastore_get_float(datastore, RESOURCE_ID_FLOW_RATE, 0, &flow);
    datastore_get_float(datastore, RESOURCE_ID_CONTROL_FLOW_THRESHOLD, 0, &flow_threshold);
    if (cp_state == CONTROL_CP_STATE_ON)
    {
        snprintf(page_buffer->row[2], ROW_STRING_WIDTH, "Flow %4.1f   Min %4.1f", flow, flow_threshold);
    }
    else
    {
        // don't care about flow if CP isn't running
        snprintf(page_buffer->row[2], ROW_STRING_WIDTH, "Flow ----   Min %4.1f", flow_threshold);
    }

    // borrowed this code from control.c but modified to show remaining seconds (not just minutes)
    bool system_time_set = false;
    datastore_get_bool(datastore, RESOURCE_ID_SYSTEM_TIME_SET, 0, &system_time_set);
    if (system_time_set)
    {
        bool daily_enable = false;
        int32_t daily_hour = -1;
        int32_t daily_minute = -1;
        datastore_get_bool(datastore, RESOURCE_ID_CONTROL_PP_DAILY_ENABLE, 0, &daily_enable);
        datastore_get_int32(datastore, RESOURCE_ID_CONTROL_PP_DAILY_HOUR, 0, &daily_hour);
        datastore_get_int32(datastore, RESOURCE_ID_CONTROL_PP_DAILY_MINUTE, 0, &daily_minute);

        if (daily_enable && daily_hour >= 0 && daily_minute >= 0)
        {
            time_t now;
            struct tm timeinfo;
            _get_local_time(&now, &timeinfo);

            int now_seconds = timeinfo.tm_hour * 60 * 60 + timeinfo.tm_min * 60 + timeinfo.tm_sec;
            int set_seconds = daily_hour * 60 * 60 + daily_minute * 60;
            int rem_seconds = now_seconds - 1 < set_seconds ? set_seconds - now_seconds : 24 * 60 * 60 - now_seconds + set_seconds;
            int hours_remaining = rem_seconds / 60 / 60;  // floor
            int minutes_remaining = (rem_seconds - (hours_remaining * 60 * 60)) / 60;
            int seconds_remaining = rem_seconds - (hours_remaining * 60 * 60) - (minutes_remaining * 60);
            snprintf(page_buffer->row[3], ROW_STRING_WIDTH, "%02d:%02d:00  T-%02d:%02d:%02d",
                     daily_hour, daily_minute, hours_remaining, minutes_remaining, seconds_remaining);
        }
        else
        {
            snprintf(page_buffer->row[3], ROW_STRING_WIDTH, "Daily disabled");
        }
    }
    else
    {
        snprintf(page_buffer->row[3], ROW_STRING_WIDTH, "Waiting for time");
    }
}

static void _handle_page_alarm(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "ALARM");
}

static void _handle_page_wifi_status(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    wifi_status_t wifi_status = 0;
    datastore_get_uint32(datastore, RESOURCE_ID_WIFI_STATUS, 0, &wifi_status);

    // connection state
    switch (wifi_status)
    {
        case WIFI_STATUS_DISCONNECTED:
            snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "WiFi disconnected");
            break;
        case WIFI_STATUS_CONNECTED:
            snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "WiFi connecting");
            break;
        case WIFI_STATUS_GOT_ADDRESS:
        {
            uint32_t connection_count = 0;
            datastore_get_uint32(datastore, RESOURCE_ID_WIFI_CONNECTION_COUNT, 0, &connection_count);
            snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "WiFi connected %d", connection_count);
            break;
        }
        default:
            ESP_LOGE(TAG, "unhandled wifi status %d", wifi_status);
            break;
    }

    char ssid[WIFI_LEN_SSID] = "";
    int8_t rssi = 0;

    datastore_get_string(datastore, RESOURCE_ID_WIFI_SSID, 0, ssid, sizeof(ssid));
    datastore_get_int8(datastore, RESOURCE_ID_WIFI_RSSI, 0, &rssi);

    // truncate ssid at 7 characters
    ssid[8] = '\0';
    snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "%-16s %3d", ssid, rssi);

    switch (wifi_status)
    {
        case WIFI_STATUS_DISCONNECTED:
            snprintf(page_buffer->row[2], ROW_STRING_WIDTH, BLANK_LINE);
            break;
        case WIFI_STATUS_CONNECTED:
            snprintf(page_buffer->row[2], ROW_STRING_WIDTH, "Waiting for IP");
            break;
        case WIFI_STATUS_GOT_ADDRESS:
        {
            uint32_t ip_address = 0;
            datastore_get_uint32(datastore, RESOURCE_ID_WIFI_ADDRESS, 0, &ip_address);
            snprintf(page_buffer->row[2], ROW_STRING_WIDTH, "%d.%d.%d.%d",
                     (ip_address & 0xff),
                     (ip_address & 0xff00) >> 8,
                     (ip_address & 0xff0000) >> 16,
                     (ip_address & 0xff000000) >> 24);
            break;
        }
        default:
            ESP_LOGE(TAG, "unhandled wifi status %d", wifi_status);
            break;
    }

    // calculate time since last connection
    uint32_t timestamp = 0;
    datastore_get_uint32(datastore, RESOURCE_ID_WIFI_TIMESTAMP, 0, &timestamp);
    uint32_t connected_time = seconds_since_boot() - timestamp;
    _render_uptime(page_buffer->row[3], ROW_STRING_WIDTH, connected_time);
}

static void _handle_page_mqtt_status(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    mqtt_status_t mqtt_status = 0;
    datastore_get_uint32(datastore, RESOURCE_ID_MQTT_STATUS, 0, &mqtt_status);

    // connection state
    switch (mqtt_status)
    {
        case MQTT_STATUS_DISCONNECTED:
            snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "MQTT disconnected");
            break;
        case MQTT_STATUS_CONNECTING:
            snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "MQTT connecting");
            break;
        case MQTT_STATUS_CONNECTED:
        {
            uint32_t connection_count = 0;
            datastore_get_uint32(datastore, RESOURCE_ID_MQTT_CONNECTION_COUNT, 0, &connection_count);
            snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "MQTT connected %d", connection_count);
            break;
        }
        default:
            ESP_LOGE(TAG, "unhandled mqtt status %d", mqtt_status);
            break;
    }

    // broker address
    char broker_address[MQTT_LEN_BROKER_ADDRESS] = "";
    uint32_t broker_port = 0;
    datastore_get_string(datastore, RESOURCE_ID_MQTT_BROKER_ADDRESS, 0, broker_address, sizeof(broker_address));
    datastore_get_uint32(datastore, RESOURCE_ID_MQTT_BROKER_PORT, 0, &broker_port);

    char port[6] = "";
    snprintf(port, 6, "%d", broker_port);
    int port_len = strlen(port);
    int addr_len = DISPLAY_WIDTH - port_len - 1;  // space for the colon
    snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "%.*s:%-*d", addr_len, broker_address, port_len, broker_port);

    // counters
    uint32_t count_rx = 0, count_tx = 0;
    datastore_get_uint32(datastore, RESOURCE_ID_MQTT_MESSAGE_RX_COUNT, 0, &count_rx);
    datastore_get_uint32(datastore, RESOURCE_ID_MQTT_MESSAGE_TX_COUNT, 0, &count_tx);
    snprintf(page_buffer->row[2], ROW_STRING_WIDTH, "RX %d  TX %d", count_rx, count_tx);

    // calculate time since last connection
    uint32_t timestamp = 0;
    datastore_get_uint32(datastore, RESOURCE_ID_MQTT_TIMESTAMP, 0, &timestamp);
    uint32_t connected_time = seconds_since_boot() - timestamp;
    _render_uptime(page_buffer->row[3], ROW_STRING_WIDTH, connected_time);
}

static void _handle_page_resource_status(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "MEM Free  %8d B", esp_get_free_heap_size());
    snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "IRAM Free %8d B", heap_caps_get_free_size(MALLOC_CAP_32BIT));
    snprintf(page_buffer->row[2], ROW_STRING_WIDTH, "Datastore %8d B", datastore_get_ram_usage(datastore));
    snprintf(page_buffer->row[3], ROW_STRING_WIDTH, "Tasks %14d", uxTaskGetNumberOfTasks());
}

static void _handle_page_avr_status(page_buffer_t * page_buffer, void * state, const datastore_t * datastore)
{
    uint8_t version = 0;
    uint32_t count_reset = 0;
    datastore_age_t age_us = 0;
    datastore_get_uint8(datastore, RESOURCE_ID_AVR_VERSION, 0, &version);
    datastore_get_uint32(datastore, RESOURCE_ID_AVR_COUNT_RESET, 0, &count_reset);
    datastore_get_age(datastore, RESOURCE_ID_AVR_COUNT_RESET, 0, &age_us);

    // if the AVR was detected, then it will have been reset at least once
    if (count_reset > 0)
    {
        snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "AVR Version %d", version);
        snprintf(page_buffer->row[1], ROW_STRING_WIDTH, "Reset Count %d", count_reset);
        snprintf(page_buffer->row[2], ROW_STRING_WIDTH, BLANK_LINE);
        _render_uptime(page_buffer->row[3], ROW_STRING_WIDTH, age_us / 1000000);
    }
    else
    {
        snprintf(page_buffer->row[0], ROW_STRING_WIDTH, "AVR not detected");
    }
}

static void dispatch_to_handler(page_buffer_t * buffer, display_page_id_t current_page, const datastore_t * datastore)
{
    assert(sizeof(page_specs) / sizeof(page_specs[0]) == DISPLAY_PAGE_LAST);

    if (current_page >= 0 && current_page < DISPLAY_PAGE_LAST)
    {
        if (page_specs[current_page].id == current_page)
        {
            if (page_specs[current_page].handler)
            {
                page_specs[current_page].handler(buffer, page_specs[current_page].state, datastore);
            }
            else
            {
                ESP_LOGE(TAG, "page %d has no handler", current_page);
                current_page = DISPLAY_PAGE_BLANK;
            }
        }
        else
        {
            ESP_LOGE(TAG, "page spec mismatch at position %d", current_page);
            current_page = DISPLAY_PAGE_BLANK;
        }
    }
    else
    {
        ESP_LOGE(TAG, "current page %d out of range", current_page);
        current_page = DISPLAY_PAGE_BLANK;
    }
}

static display_page_id_t _handle_transition(int input, display_page_id_t current_page)
{
    display_page_id_t new_page = DISPLAY_PAGE_BLANK;
    if (current_page >= 0 && current_page < DISPLAY_PAGE_LAST)
    {
        switch (input)
        {
            case ROTARY_ENCODER_EVENT_CLOCKWISE:
                new_page = transitions[current_page].on_clockwise;
                break;
            case ROTARY_ENCODER_EVENT_COUNTER_CLOCKWISE:
                new_page = transitions[current_page].on_counter_clockwise;
                break;
            case BUTTON_EVENT_SHORT:
                new_page = transitions[current_page].on_short;
                break;
            case BUTTON_EVENT_LONG:
                new_page = transitions[current_page].on_long;
                break;
            default:
                ESP_LOGE(TAG, "invalid input %d", input);
                new_page = current_page;
                break;
        }
    }
    return new_page;
}

static void dump_datastore_task(void * pvParameter)
{
    assert(pvParameter);
    const datastore_t * datastore = (datastore_t *)pvParameter;
    datastore_dump(datastore);
    vTaskDelete(NULL);
}

static void _dump_datastore(const datastore_t * datastore)
{
    xTaskCreate(&dump_datastore_task, "dump_datastore_task", 4096, (void *)datastore, tskIDLE_PRIORITY, NULL);
}

static void _extend_page_buffer_rows(page_buffer_t * buffer)
{
    for (int i = 0; i < LCD_NUM_ROWS; ++i)
    {
        // pad to visible columns
        for (int j = strlen(buffer->row[i]); j < LCD_NUM_VISIBLE_COLUMNS; ++j)
        {
            buffer->row[i][j] = ' ';
        }
        buffer->row[i][LCD_NUM_VISIBLE_COLUMNS] = '\0';
    }
}

static void _render_page_buffer(i2c_master_info_t * i2c_master_info, i2c_lcd1602_info_t * lcd_info, page_buffer_t * buffer)
{
    assert(i2c_master_info);
    assert(lcd_info);
    assert(buffer);
    i2c_master_lock(i2c_master_info, portMAX_DELAY);
    for (int i = 0; i < LCD_NUM_ROWS; ++i)
    {
        I2C_LCD1602_ERROR_CHECK(_move_cursor(lcd_info, 0, i));
        I2C_LCD1602_ERROR_CHECK(_write_string(lcd_info, buffer->row[i]));
    }
    i2c_master_unlock(i2c_master_info);
}

static void display_task(void * pvParameter)
{
    assert(pvParameter);
    ESP_LOGI(TAG, "Core ID %d", xPortGetCoreID());

    task_inputs_t * task_inputs = (task_inputs_t *)pvParameter;
    i2c_master_info_t * i2c_master_info = task_inputs->i2c_master_info;
    i2c_port_t i2c_port = i2c_master_info->port;
    const datastore_t * datastore = task_inputs->datastore;
    QueueHandle_t input_queue = task_inputs->input_queue;

    // before accessing I2C, use a lock to gain exclusive use of the bus
    i2c_master_lock(i2c_master_info, portMAX_DELAY);

    // Set up the SMBus
    smbus_info_t * smbus_info = smbus_malloc();
    ESP_ERROR_CHECK(smbus_init(smbus_info, i2c_port, CONFIG_LCD1602_I2C_ADDRESS));
    ESP_ERROR_CHECK(smbus_set_timeout(smbus_info, SMBUS_TIMEOUT / portTICK_RATE_MS));

    // Set up the LCD1602 device with backlight on
    i2c_lcd1602_info_t * lcd_info = i2c_lcd1602_malloc();
    ESP_ERROR_CHECK(i2c_lcd1602_init(lcd_info, smbus_info, true,
            LCD_NUM_ROWS, LCD_NUM_COLUMNS, LCD_NUM_VISIBLE_COLUMNS));
    ESP_ERROR_CHECK(_display_reset(lcd_info));

    // Move to home position
    I2C_LCD1602_ERROR_CHECK(_move_cursor(lcd_info, 0, 0));
    i2c_lcd1602_write_char(lcd_info, 'B');

    i2c_master_unlock(i2c_master_info);

    display_page_id_t current_page = INITIAL_PAGE;

    page_buffer_t buffer;
    for (int i = 0; i < LCD_NUM_ROWS; ++i)
    {
        buffer.row[i] = (char *)malloc(LCD_NUM_COLUMNS + 1);
    }

    // backlight age
    uint32_t backlight_timestamp = seconds_since_boot();

    // update pages once per second
    while (1)
    {
        ESP_LOGD(TAG, "display loop");

        for (int i = 0; i < LCD_NUM_ROWS; ++i)
        {
            buffer.row[i][0] = '\0';
        }

        dispatch_to_handler(&buffer, current_page, datastore);
        _extend_page_buffer_rows(&buffer);
        _render_page_buffer(i2c_master_info, lcd_info, &buffer);

        button_event_t input = 0;
        BaseType_t rc = xQueueReceive(input_queue, &input, TICKS_PER_UPDATE);
        if (rc == pdTRUE)
        {
            ESP_LOGI(TAG, "from queue: %d", input);

            // turn on backlight
            i2c_lcd1602_set_backlight(lcd_info, true);
            backlight_timestamp = seconds_since_boot();

            display_page_id_t new_page = _handle_transition(input, current_page);
            if (new_page != current_page && new_page >= 0 && new_page < DISPLAY_PAGE_LAST)
            {
                ESP_LOGI(TAG, "change to page %d", new_page);
                current_page = new_page;
                datastore_set_int32(datastore, RESOURCE_ID_DISPLAY_PAGE, 0, current_page);

                // reset the display when going through the Main page
                if (current_page == DISPLAY_PAGE_MAIN)
                {
                    // reset display when changing page
                    _display_reset(lcd_info);
                    I2C_LCD1602_ERROR_CHECK(_clear(lcd_info));
                }
            }

            // special case - short button press on Main page will dump datastore to console
            if (current_page == DISPLAY_PAGE_MAIN && input == BUTTON_EVENT_SHORT)
            {
                _dump_datastore(datastore);
            }
        }

        // TODO: reset display every 5 seconds as a precaution

        // backlight timeout
        uint32_t backlight_timeout = 0;
        datastore_get_uint32(datastore, RESOURCE_ID_DISPLAY_BACKLIGHT_TIMEOUT, 0, &backlight_timeout);
        if (backlight_timeout > 0 && ((seconds_since_boot() - backlight_timestamp) > backlight_timeout))
        {
            i2c_lcd1602_set_backlight(lcd_info, false);
        }
    }

    for (int i = 0; i < LCD_NUM_ROWS; ++i) {
        free(buffer.row[i]);
    }
    free(task_inputs);
    _task_handle = NULL;
    vTaskDelete(NULL);
}

void display_init(i2c_master_info_t * i2c_master_info, UBaseType_t priority, const datastore_t * datastore)
{
    ESP_LOGD(TAG, "%s", __FUNCTION__);

    static bool init = false;
    if (!init)
    {
        QueueHandle_t input_queue = xQueueCreate(10, sizeof(button_event_t));

        // task will take ownership of this struct
        task_inputs_t * task_inputs = malloc(sizeof(*task_inputs));
        if (task_inputs)
        {
            memset(task_inputs, 0, sizeof(*task_inputs));
            task_inputs->i2c_master_info = i2c_master_info;
            task_inputs->datastore = datastore;
            task_inputs->input_queue = input_queue;
            xTaskCreate(&display_task, "display_task", 4096, task_inputs, priority, &_task_handle);
        }

        button_init(priority, input_queue, CONFIG_DISPLAY_BUTTON_GPIO);
        rotary_encoder_init(priority, input_queue, CONFIG_DISPLAY_ROTARY_ENCODER_A_GPIO, CONFIG_DISPLAY_ROTARY_ENCODER_B_GPIO);
        init = true;
    }
    else
    {
        ESP_LOGE(TAG, "display already initialised");
    }
}

void display_delete(void)
{
    rotary_encoder_delete();
    button_delete();
    if (_task_handle)
        vTaskDelete(_task_handle);
}

bool display_is_currently(const datastore_t * datastore, display_page_id_t page)
{
    bool displaying = false;
    if (datastore)
    {
        display_page_id_t current_page = DISPLAY_PAGE_IGNORE;
        if (datastore_get_int32(datastore, RESOURCE_ID_DISPLAY_PAGE, 0, &current_page) == DATASTORE_STATUS_OK)
        {
            displaying = page == current_page;
        }
    }
    return displaying;
}
