#ifndef BOREDOMOS_SERIAL_H
#define BOREDOMOS_SERIAL_H

[[noreturn]] extern void onSerialReceive();

[[noreturn]] extern void TaskSerialWrite(void *pvParameters);

[[noreturn]] extern void TaskSerialRead(void *pvParameters);

#endif //BOREDOMOS_SERIAL_H