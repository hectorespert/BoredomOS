#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Serial.h>

extern QueueHandle_t serialWriteQueue;

[[noreturn]] void TaskHeartbeat(void *pvParameters)
{
    (void)pvParameters;

    mavlink_message_t heartbeatMsg;

    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (!Serial) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    for (;;)
    {

        mavlink_msg_heartbeat_pack(
            1, 
            MAV_COMP_ID_AUTOPILOT1, 
            &heartbeatMsg, 
            MAV_TYPE_ROCKET, 
            MAV_AUTOPILOT_GENERIC, 
            MAV_MODE_FLAG_AUTO_ENABLED, 
            0, 
            MAV_STATE_ACTIVE
        );

        Serial.println("Sending heartbeat...");

        if (xQueueSend(serialWriteQueue, &heartbeatMsg, 0) != pdPASS)
        {
            Serial.println("Error: Cola de escritura llena, HEARTBEAT descartado");
        }

        vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
    }
}