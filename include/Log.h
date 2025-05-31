#ifndef BOREDOMOS_LOG_H
#define BOREDOMOS_LOG_H
#include <System.h>

typedef struct 
{
    String timestamp;
    uint32_t unixtime;
    uint32_t uptime;
    System system;
} Log;


#endif //BOREDOMOS_LOG_H
