#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Serial.h>

extern QueueHandle_t serialWriteQueue;

[[noreturn]] void TaskHeartbeat(void *pvParameters)
{
    (void)pvParameters;

    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (!Serial) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    for (;;)
    {

        mavlink_message_t* heartbeatMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));

        mavlink_msg_heartbeat_pack(
            1, 
            MAV_COMP_ID_AUTOPILOT1, 
            heartbeatMsg, 
            MAV_TYPE_ROCKET, 
            MAV_AUTOPILOT_GENERIC, 
            MAV_MODE_FLAG_AUTO_ENABLED, 
            0, 
            MAV_STATE_ACTIVE
        );

        if (xQueueSend(serialWriteQueue, &heartbeatMsg, 0) != pdPASS)
        {
            vPortFree(heartbeatMsg);
        }

        vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
    }
}