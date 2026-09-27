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
String SdData::getLogFileName(int idx) {
    return String("data") + idx + ".BIN";
}

// Absent counts as not full: an absent slot is one the ring can open fresh. The
// comparison is write()'s own rotation test, so begin() and write() cannot disagree
// about which files are finished. The size comes from the directory entry.
bool SdData::slotIsFull(int idx) {
    String name = getLogFileName(idx);
    File f = SD.open(name.c_str(), FILE_READ);
    if (!f) {
        return false;
    }
    size_t size = f.size();
    f.close();
    return size >= _size;
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
// block is still ours instead of racing the eviction. The same holds for the
// slotIsFull() opens below, and none of them overlaps another.
//
// The index is checked against the card rather than trusted. A slot that is absent
// or not full is one to resume in; the one the index names wins when it qualifies,
// and otherwise the ring is walked on from it. Walking from the index rather than
// from 0 matters on the first lap, where the slots past the current one do not exist
// yet. When every slot is full there is nothing to resume, and this moves on exactly
// as a rotation does -- it never appends to a full file, because a full file past the
// index can be from the previous lap, and appending to it is what put two laps in
// one file.
void SdData::begin()
{
    if (_dataFile) {
        _dataFile.close();
    }

    int indexed = readLogIndex();
    int resume = -1;
    for (int step = 0; step < _files; ++step) {
        int slot = (indexed + step) % _files;
        if (!slotIsFull(slot)) {
            resume = slot;
            break;
        }
    }

    if (resume < 0) {
        _fileIdx = indexed;
        openNextSlot();
        return;
    }

    _fileIdx = resume;
    if (resume != indexed) {
        writeLogIndex();
    }
    String logFileName = getLogFileName(_fileIdx);
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
        openNextSlot();
    }
}

// Rotation, shared by write() and by a begin() that finds every slot full.
//
// close() syncs, so nothing accumulated is lost at a rotation boundary -- but the
// counter is reset explicitly rather than relying on that, because an interval that
// straddled two files would make the bound wrong for the new one.
//
// The next slot is deleted BEFORE the index moves to it, so the index never names a
// file from a previous lap. The other way round, a power cut between persisting and
// deleting left the index on the old lap's full file, and the next boot appended to
// it. Every cut now leaves either the index on the full file just closed -- which
// begin() walks on from -- or the index on a slot that is absent or freshly opened.
void SdData::openNextSlot()
{
    if (_dataFile) {
        _dataFile.close();
    }
    _sinceFlush = 0;

    _fileIdx = (_fileIdx + 1) % _files;
    String newLogFileName = getLogFileName(_fileIdx);
    if (SD.exists(newLogFileName.c_str())) {
        SD.remove(newLogFileName.c_str());
    }
    writeLogIndex();

    _dataFile = SD.open(newLogFileName.c_str(), FILE_WRITE);
    _sinceFlush = 0;
    notifyOpened();
}
