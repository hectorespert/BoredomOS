#ifndef BOREDOMOS_SENSORS_H
#define BOREDOMOS_SENSORS_H

struct Sensors {
    float voltage;
};

extern Sensors sensors;

[[noreturn]] extern void TaskSensors(void *pvParameters);

#endif //BOREDOMOS_SENSORS_H