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
    _sinceFlush = 0;
    if (!_dataFile) {
        return;
    }
    notifyOpened();
}

void SdData::end()
{
    if (_dataFile) {
        _dataFile.close();
        _sinceFlush = 0;
    }
}

// Opened WITHOUT O_APPEND, which FILE_WRITE includes: with it, the library moves the
// offset to the end before every write, the seek(0) below is lost, and each index is
// appended. readLogIndex() reads the first four bytes, so every restart after a second
// rotation reopened the first full file, rotated on its first write, and deleted the file
// the previous boot had been logging into. No O_TRUNC either: truncating frees the cluster
// and the write reallocates it, and a power cut between the two would leave an empty index
// and restart the ring at file 0. Bytes a card may still carry past offset 4 from the old
// behaviour are never read.
void SdData::writeLogIndex() {
    File idxFile = SD.open(LOG_INDEX_FILE, O_WRITE | O_CREAT);
    if (!idxFile) {
        return;
    }

    idxFile.seek(0);
    idxFile.write(reinterpret_cast<uint8_t*>(&_fileIdx), sizeof(_fileIdx));
    idxFile.flush();
    idxFile.close();
}

void SdData::appendBytes(const uint8_t *data, size_t length)
{
    if (!_dataFile || data == nullptr || length == 0) {
        return;
    }

    _dataFile.write(data, length);
}

// Syncs every call. See the header for why this path is not batched: it carries the
// format preamble, and a preamble that does not reach the card costs the whole file
// rather than its tail.
void SdData::writeRaw(const uint8_t *data, size_t length)
{
    appendBytes(data, length);

    if (_dataFile) {
        _dataFile.flush();
        _sinceFlush = 0;
    }
}

void SdData::write(const uint8_t *data, size_t length)
{
    // The argument guard is repeated here even though appendBytes() has it, and the
    // duplication is the point: appendBytes() returning early is silent, while the
    // accounting below would still advance _sinceFlush by bytes that were never
    // written -- moving the sync point, and with it the bound this class promises on
    // what power loss costs. write() used to be null-safe by delegating to writeRaw();
    // splitting the append out took that away, and this puts it back.
    if (!_dataFile || data == nullptr || length == 0) {
        return;
    }

    appendBytes(data, length);

    // Accumulate rather than sync. SdFile::write() has already updated the in-memory
    // fileSize_, so size() below is correct whether or not this synced -- rotation
    // still triggers on the right byte.
    _sinceFlush += length;
    if (_sinceFlush >= FLUSH_INTERVAL_BYTES) {
        _dataFile.flush();
        _sinceFlush = 0;
    }

    size_t fileSize = _dataFile.size();

    if (fileSize >= _size ) {
        // close() syncs, so nothing accumulated is lost at a rotation boundary --
        // but the counter is reset explicitly rather than relying on that, because
        // an interval that straddled two files would make the bound wrong for the
        // new one.
        _dataFile.close();
        _sinceFlush = 0;

        _fileIdx = (_fileIdx + 1) % _files;
        writeLogIndex();
        String newLogFileName = getLogFileName();
        if (SD.exists(newLogFileName.c_str())) {
            SD.remove(newLogFileName.c_str());
        }

        _dataFile = SD.open(newLogFileName.c_str(), FILE_WRITE);
        _sinceFlush = 0;
        notifyOpened();
    }
}
