#include <Arduino.h>
#include <SD.h>
#include <Arduino_FreeRTOS.h>
#include <Priority.h>
#include <Link.h>
#include <MAVLink.h>
#include <Data.h>
#include <Battery.h>
#include <SystemTime.h>

Battery battery = Battery();

SystemTime systemTime = SystemTime();

TaskHandle_t taskStatusHandler = NULL;

TaskHandle_t taskSerialWriteHandler = NULL;

TaskHandle_t taskSerialReadHandler = NULL;

TaskHandle_t taskHeartbeatHandler = NULL;

TaskHandle_t taskLoggerHandler = NULL;

TaskHandle_t taskSdWriteHandler = NULL;

TaskHandle_t taskMavlinkHandler = NULL;

QueueHandle_t serialReadQueue = NULL;

QueueHandle_t serialWriteQueue = NULL;

// Static storage for every task the scheduler will run. The word counts are the ones
// xTaskCreateStatic is given below, and together with the queue storage further down
// they are this firmware's RAM budget: the linker counts each array by name, so a
// build that does not fit fails here rather than on the board.
StackType_t serialReadStack[96];
StaticTask_t serialReadTcb;

StackType_t serialWriteStack[192];
StaticTask_t serialWriteTcb;

StackType_t heartbeatStack[128];
StaticTask_t heartbeatTcb;

StackType_t mavlinkStack[256];
StaticTask_t mavlinkTcb;

StackType_t loggerStack[96];
StaticTask_t loggerTcb;

StackType_t statusStack[128];
StaticTask_t statusTcb;

StackType_t sdWriteStack[256];
StaticTask_t sdWriteTcb;

// Queue structures and item storage. Each queue carries pointers, so the storage is
// depth x sizeof(pointer); what backs the items themselves is the FreeRTOS heap,
// sized in platformio.ini against the worst case computed in the change design.
StaticQueue_t sdWriteQueueBuffer;
uint8_t sdWriteQueueStorage[4 * sizeof(Data*)];

StaticQueue_t serialReadQueueBuffer;
uint8_t serialReadQueueStorage[8 * sizeof(mavlink_message_t*)];

StaticQueue_t serialWriteQueueBuffer;
uint8_t serialWriteQueueStorage[4 * sizeof(mavlink_message_t*)];

QueueHandle_t sdWriteQueue = NULL;

[[noreturn]] extern void TaskSerialWrite(void *pvParameters);

[[noreturn]] extern void TaskSerialRead(void *pvParameters);

[[noreturn]] extern void TaskHeartbeat(void *pvParameters);

[[noreturn]] extern void TaskLogger(void *pvParameters);

[[noreturn]] extern void TaskSdWrite(void *pvParameters);

[[noreturn]] extern void TaskSensors(void *pvParameters);

[[noreturn]] extern void TaskMavlinkBatteryStatus(void *pvParameters);

[[noreturn]] extern void TaskMavlink(void *pvParameters);

void setup()
{
  configASSERT(systemTime.begin());

  // The console (Serial) is already open: the core's main() calls
  // Serial.begin(115200) before setup(). Opening it again here would
  // initialise the same port twice whenever LINK_SERIAL is overridden onto it.
  LINK_SERIAL.begin(LINK_BAUD);

  configASSERT(SD.begin(9));

  sdWriteQueue = xQueueCreateStatic(4, sizeof(Data*), sdWriteQueueStorage, &sdWriteQueueBuffer);
  configASSERT(sdWriteQueue != NULL);

  serialReadQueue = xQueueCreateStatic(8, sizeof(mavlink_message_t*), serialReadQueueStorage, &serialReadQueueBuffer);
  configASSERT(serialReadQueue != NULL);

  serialWriteQueue = xQueueCreateStatic(4, sizeof(mavlink_message_t*), serialWriteQueueStorage, &serialWriteQueueBuffer);
  configASSERT(serialWriteQueue != NULL);

  // With static storage these cannot fail for want of memory, so a NULL handle means
  // an argument is wrong -- a programming error, and worth trapping at boot.
  taskSerialReadHandler = xTaskCreateStatic(TaskSerialRead, "SerialRead", 96, NULL, PRIORITY_HIGHEST, serialReadStack, &serialReadTcb);
  configASSERT(taskSerialReadHandler != NULL);

  taskSerialWriteHandler = xTaskCreateStatic(TaskSerialWrite, "SerialWrite", 192, NULL, PRIORITY_HIGH, serialWriteStack, &serialWriteTcb);
  configASSERT(taskSerialWriteHandler != NULL);

  taskHeartbeatHandler = xTaskCreateStatic(TaskHeartbeat, "Heartbeat", 128, NULL, PRIORITY_HIGH, heartbeatStack, &heartbeatTcb);
  configASSERT(taskHeartbeatHandler != NULL);

  taskMavlinkHandler = xTaskCreateStatic(TaskMavlink, "Mavlink", 256, NULL, PRIORITY_LOW, mavlinkStack, &mavlinkTcb);
  configASSERT(taskMavlinkHandler != NULL);

  taskLoggerHandler = xTaskCreateStatic(TaskLogger, "Logger", 96, NULL, PRIORITY_LOW, loggerStack, &loggerTcb);
  configASSERT(taskLoggerHandler != NULL);

  taskStatusHandler = xTaskCreateStatic(TaskMavlinkBatteryStatus, "MavlinkBatteryStatus", 128, NULL, PRIORITY_HIGH, statusStack, &statusTcb);
  configASSERT(taskStatusHandler != NULL);

  taskSdWriteHandler = xTaskCreateStatic(TaskSdWrite, "SdWrite", 256, NULL, PRIORITY_LOWEST, sdWriteStack, &sdWriteTcb);
  configASSERT(taskSdWriteHandler != NULL);

  vTaskStartScheduler();
}

void loop() {}
