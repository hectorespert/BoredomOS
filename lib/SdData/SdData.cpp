#include <SdData.h>
#include <Arduino.h>

#define LOG_INDEX_FILE "index.bin"

SdData::SdData(int files, size_t size): _files(files), _size(size)
{
    
}

int SdData::readLogIndex() {
    File idxFile = SD.open(LOG_INDEX_FILE, FILE_READ);
    if (!idxFile) return 0;
    int idx = 0;
    idxFile.readBytes((char*)&idx, sizeof(idx));
    idxFile.close();
    if (idx < 0 || idx >= _files) return 0;
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

void SdData::write(const JsonDocument& json)
{
    if (!_dataFile) {
        return;
    }

    serializeMsgPack(json, _dataFile);
    _dataFile.flush();
    size_t fileSize = _dataFile.size();

    if (fileSize >= _size ) {
        _dataFile.close();
        _fileIdx = (_fileIdx + 1) % _files;
        writeLogIndex();
        String newLogFileName = getLogFileName();
        if (SD.exists(newLogFileName.c_str())) {
            SD.remove(newLogFileName.c_str());
        }

        _dataFile = SD.open(newLogFileName.c_str(), FILE_WRITE);
    }
}