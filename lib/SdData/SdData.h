#ifndef SD_DATA_H
#define SD_DATA_H

#include <SD.h>
#include <ArduinoJson.h>

class SdData
{
public:
  SdData(int files = 4, size_t size = 1024UL * 1024UL * 1024UL);
  void begin();
  void write(const JsonDocument& json);
private:
  int _fileIdx = 0;
  int _files;
  size_t _size;
  File _dataFile;
  int readLogIndex();
  void writeLogIndex();
  String getLogFileName();
};

#endif