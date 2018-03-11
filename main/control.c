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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "control.h"
#include "resources.h"
#include "avr_support.h"
#include "datastore/datastore.h"

#define POLL_PERIOD (1000)  // control loop period in milliseconds

#define TAG "control"

typedef struct
{
    const datastore_t * datastore;
} task_inputs_t;

static void control_cp_task(void * pvParameter)
{
    assert(pvParameter);
    ESP_LOGI(TAG, "Core ID %d", xPortGetCoreID());
    task_inputs_t * task_inputs = (task_inputs_t *)pvParameter;
    const datastore_t * datastore = task_inputs->datastore;

    TickType_t last_wake_time = xTaskGetTickCount();

    avr_pump_state_t state = AVR_PUMP_STATE_OFF;
    avr_support_set_cp_pump(AVR_PUMP_STATE_OFF);

    // wait 10 seconds for stable sensor readings
    vTaskDelay(10 * 1000 / portTICK_RATE_MS);

    while (1)
    {
        last_wake_time = xTaskGetTickCount();
        ESP_LOGD(TAG, "CP control loop: state %d", state);

        float t1 = 0.0f;
        float t2 = 0.0f;
        bool transition = false;

        // TODO: confirm pump state matches
        //datastore_get_uint32(datastore, RESOURCE_ID_PUMPS_CP_STATE, 0, &state);
        datastore_get_float(datastore, RESOURCE_ID_TEMP_VALUE, 0, &t1);
        datastore_get_float(datastore, RESOURCE_ID_TEMP_VALUE, 1, &t2);

        ESP_LOGD(TAG, "T1 %f, T2 %f", t1, t2);

        // transitions
        if (state == AVR_PUMP_STATE_OFF)
        {
            float delta = 0.0f;
            datastore_get_float(datastore, RESOURCE_ID_CONTROL_CP_ON_DELTA, 0, &delta);
//            ESP_LOGD(TAG, "delta on %f", delta);
            if (t1 - t2 >= delta)
            {
                state = AVR_PUMP_STATE_ON;
                transition = true;
            }
        }
        else
        {
            float delta = 0.0f;
            datastore_get_float(datastore, RESOURCE_ID_CONTROL_CP_OFF_DELTA, 0, &delta);
//            ESP_LOGD(TAG, "delta off %f", delta);
            if (t1 - t2 <= delta)
            {
                state = AVR_PUMP_STATE_OFF;
                transition = true;
            }
        }

        // output
        if (transition)
        {
            avr_support_set_cp_pump(state);
        }

        vTaskDelayUntil(&last_wake_time, POLL_PERIOD / portTICK_PERIOD_MS);
    }
}

static void control_pp_task(void * pvParameter)
{
    assert(pvParameter);
    ESP_LOGI(TAG, "Core ID %d", xPortGetCoreID());
    task_inputs_t * task_inputs = (task_inputs_t *)pvParameter;
    const datastore_t * datastore = task_inputs->datastore;

    TickType_t last_wake_time = xTaskGetTickCount();

    avr_support_set_pp_pump(AVR_PUMP_STATE_OFF);

    typedef enum
    {
        STATE_PP_OFF = 0,
        STATE_PP_ON,
        STATE_PP_PAUSE,
    } state_t;

    state_t state = STATE_PP_OFF;
    TickType_t cycle_start_time = 0;
    uint32_t n = 0;

    // wait 10 seconds for stable sensor readings
    vTaskDelay(10 * 1000 / portTICK_RATE_MS);

    while (1)
    {
        last_wake_time = xTaskGetTickCount();
        ESP_LOGD(TAG, "PP control loop: state %d, n %d", state, n);

        // TODO: daily time

        // TODO: handle tick count overflow

        // TODO: confirm pump state matches

        //ESP_LOGD(TAG, "cp_state %d, FR %f", cp_state, flow_rate);

        // outputs are on transitions

        switch(state)
        {
        case STATE_PP_OFF:
        {
            float flow_rate = 0.0f;
            datastore_get_float(datastore, RESOURCE_ID_FLOW_RATE, 0, &flow_rate);
            avr_pump_state_t cp_state = AVR_PUMP_STATE_OFF;
            datastore_get_uint32(datastore, RESOURCE_ID_PUMPS_CP_STATE, 0, &cp_state);
            float flow_threshold = 0.0f;
            datastore_get_float(datastore, RESOURCE_ID_CONTROL_FLOW_THRESHOLD, 0, &flow_threshold);

            ESP_LOGD(TAG, "flow rate %f, cp state %d, threshold %f", flow_rate, cp_state, flow_threshold);

            if ((cp_state == AVR_PUMP_STATE_ON) && (flow_rate <= flow_threshold))
            {
                state = STATE_PP_ON;
                avr_support_set_pp_pump(AVR_PUMP_STATE_ON);
                cycle_start_time = last_wake_time;
                datastore_get_uint32(datastore, RESOURCE_ID_CONTROL_PP_CYCLE_COUNT, 0, &n);
                --n;
            }
            break;
        }

        case STATE_PP_ON:
        {
            float duration = 0.0f;
            datastore_get_float(datastore, RESOURCE_ID_CONTROL_PP_CYCLE_ON_DURATION, 0, &duration);
            TickType_t cycle_end_time = cycle_start_time + (TickType_t)(duration * configTICK_RATE_HZ);

            if (last_wake_time >= cycle_end_time)
            {
                state = STATE_PP_PAUSE;
                cycle_start_time = last_wake_time;
                avr_support_set_pp_pump(AVR_PUMP_STATE_OFF);
            }
            break;
        }

        case STATE_PP_PAUSE:
        {
            float duration = 0.0f;
            datastore_get_float(datastore, RESOURCE_ID_CONTROL_PP_CYCLE_PAUSE_DURATION, 0, &duration);
            TickType_t cycle_end_time = cycle_start_time + (TickType_t)(duration * configTICK_RATE_HZ);

            if (n > 0)
            {
                if (last_wake_time >= cycle_end_time)
                {
                    state = STATE_PP_ON;
                    avr_support_set_pp_pump(AVR_PUMP_STATE_ON);
                    cycle_start_time = last_wake_time;
                    --n;
                }
            }
            else
            {
                if (last_wake_time >= cycle_end_time)
                {
                    state = STATE_PP_OFF;
                    avr_support_set_pp_pump(AVR_PUMP_STATE_OFF);
                }
            }
            break;
        }

        default:
            ESP_LOGE(TAG, "invalid case %d", state);
        }

        vTaskDelayUntil(&last_wake_time, POLL_PERIOD / portTICK_PERIOD_MS);
    }
}

void control_init(UBaseType_t priority, const datastore_t * datastore)
{
    ESP_LOGD(TAG, "%s", __FUNCTION__);

    // task will take ownership of this struct
    task_inputs_t * task_inputs = malloc(sizeof(*task_inputs));
    if (task_inputs)
    {
        memset(task_inputs, 0, sizeof(*task_inputs));
        task_inputs->datastore = datastore;
        xTaskCreate(&control_cp_task, "control_cp_task", 4096, task_inputs, priority, NULL);
        xTaskCreate(&control_pp_task, "control_pp_task", 4096, task_inputs, priority, NULL);
    }
}


