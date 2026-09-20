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
    idxFile.readBytes(reinterpret_cast<char*>(&idx), sizeof(idx));
    idxFile.close();
    if (idx < 0 || idx >= _files) return 0;
    return idx;
}

// data<i>.BIN, not .mpk: .BIN is what the ground-side log tools filter on, and the
// contents are DataFlash records rather than MessagePack.
String SdData::getLogFileName() {
    return String("data") + _fileIdx + ".BIN";
}

void SdData::setOnOpen(SdDataOnOpen callback)
{
    _onOpen = callback;
}

// Called only where a file has just been opened successfully. The callback writes
// the format preamble through writeRaw(), which does not check the size limit, so
// this cannot start another rotation.
void SdData::notifyOpened()
{
    if (_onOpen != nullptr && _dataFile) {
        _onOpen(*this);
    }
}

// Unconditional: see the header for why the "open only if nothing is held" guard
// this used to carry could not be repaired by inverting it.
//
// The close comes BEFORE readLogIndex() rather than after. SdVolume's cache is a
// single 512 B block shared by every file, and readLogIndex() opens index.bin,
// which evicts it. Closing first means this file's own sync() happens while the
// block is still ours instead of racing the eviction.
void SdData::begin()
{
    if (_dataFile) {
        _dataFile.close();
    }

    _fileIdx = readLogIndex();
    String logFileName = getLogFileName();
    _dataFile = SD.open(logFileName.c_str(), FILE_WRITE);
    if (!_dataFile) {
        return;
    }
    notifyOpened();
}

void SdData::end()
{
    if (_dataFile) {
        _dataFile.close();
    }
}

void SdData::writeLogIndex() {
    File idxFile = SD.open(LOG_INDEX_FILE, FILE_WRITE);
    if (!idxFile) {
        return;
    }

    idxFile.seek(0);
    idxFile.write(reinterpret_cast<uint8_t*>(&_fileIdx), sizeof(_fileIdx));
    idxFile.flush();
    idxFile.close();
}

void SdData::writeRaw(const uint8_t *data, size_t length)
{
    if (!_dataFile || data == nullptr || length == 0) {
        return;
    }

    _dataFile.write(data, length);
    _dataFile.flush();
}

void SdData::write(const uint8_t *data, size_t length)
{
    if (!_dataFile) {
        return;
    }

    writeRaw(data, length);

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
        notifyOpened();
    }
}
