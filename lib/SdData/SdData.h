#ifndef SD_DATA_H
#define SD_DATA_H

#include <SD.h>
#include <ArduinoJson.h>

#define LOG_FILE_COUNT 4
#define LOG_FILE_SIZE_LIMIT (1024UL * 1024UL * 1024UL) // 1GB
#define LOG_INDEX_FILE "data_index.bin"

class SdData
{
public:
  SdData();
  void begin();
  void write(JsonDocument json);
private:
  int _fileIdx = 0;
  File _dataFile;
  int readLogIndex();
  void writeLogIndex();
  String getLogFileName();
};

#endif