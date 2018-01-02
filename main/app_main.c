/*
 * MIT License
 *
 * Copyright (c) 2017 David Antliff
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

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
//#include "nvs_flash.h"
#include "sdkconfig.h"

#include "led.h"
#include "mqtt.h"
#include "sensor_temp.h"
#include "sensor_flow.h"
#include "publish.h"
#include "wifi_support.h"
#include "avr_support.h"

#define GPIO_LED             (GPIO_NUM_2)
#define GPIO_ONE_WIRE        (CONFIG_ONE_WIRE_GPIO)

#define PUBLISH_QUEUE_DEPTH  (16)

#define TAG "poolmon"



//TODO: LED task, to blink LED when required
// - count number of connected devices
// - indicate when sampling
// - indicate MQTT connection state
// - indicate MQTT activity (publish, receive)



void app_main()
{
    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_LOGI(TAG, "[APP] Startup..");
    led_init(GPIO_LED);

    // Priority of queue consumer should be higher than producers
    UBaseType_t publish_priority = CONFIG_MQTT_PRIORITY;
    UBaseType_t sensor_priority = publish_priority - 1;
    UBaseType_t avr_priority = sensor_priority;

    QueueHandle_t publish_queue = publish_init(PUBLISH_QUEUE_DEPTH, publish_priority);

    // It works best to find all connected devices before starting WiFi, otherwise it can be unreliable.
    TempSensors * temp_sensors = sensor_temp_init(GPIO_ONE_WIRE, sensor_priority, publish_queue);
    //sensor_flow_init();
    avr_support_init(avr_priority);

    //    nvs_flash_init();
    wifi_support_init();

    // Run forever...
    while(1)
        ;

    sensor_temp_close(temp_sensors);
    //sensor_flow_close();
}