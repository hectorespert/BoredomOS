#include <SdData.h>
#include <Arduino.h>

SdData::SdData()
{
    
}

int SdData::readLogIndex() {
    File idxFile = SD.open(LOG_INDEX_FILE, FILE_READ);
    if (!idxFile) return 0;
    int idx = 0;
    idxFile.readBytes((char*)&idx, sizeof(idx));
    idxFile.close();
    if (idx < 0 || idx >= LOG_FILE_COUNT) return 0;
    return idx;
}

String SdData::getLogFileName() {
    return String("data") + _fileIdx + ".mpk";
}

void SdData::begin()
{
    _fileIdx = readLogIndex();
    String logFileName = getLogFileName();
    if (!_dataFile) {
        _dataFile = SD.open(logFileName.c_str(), FILE_WRITE);
        if (!_dataFile) {
            return;
        }
    }
}

void SdData::writeLogIndex() {
    File idxFile = SD.open(LOG_INDEX_FILE, FILE_WRITE);
    if (!idxFile) {
        return;
    }

    idxFile.seek(0);
    idxFile.write((uint8_t*)&_fileIdx, sizeof(_fileIdx));
    idxFile.flush();
    idxFile.close();
}

void SdData::write(JsonDocument json)
{
    if (!_dataFile) {
        return;
    }

    serializeMsgPack(json, _dataFile);
    _dataFile.flush();
    size_t fileSize = _dataFile.size();

    if (fileSize >= LOG_FILE_SIZE_LIMIT) {
        _dataFile.close();
        _fileIdx = (_fileIdx + 1) % LOG_FILE_COUNT;
        writeLogIndex();
        String newLogFileName = getLogFileName();
        if (SD.exists(newLogFileName.c_str())) {
            SD.remove(newLogFileName.c_str());
        }

        _dataFile = SD.open(newLogFileName.c_str(), FILE_WRITE);
    }
}