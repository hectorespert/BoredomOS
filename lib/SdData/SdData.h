#ifndef SD_DATA_H
#define SD_DATA_H

#include <SD.h>

class SdData;

// Invoked after this object opens a log file -- at begin() and again on every
// rotation -- so that whoever owns the log's FORMAT can write its preamble while
// this class stays ignorant of it. It must write through writeRaw() only: see the
// re-entrancy note on write() below.
typedef void (*SdDataOnOpen)(SdData &);

// A fixed-footprint ring of log files. Format-agnostic on purpose: it moves bytes
// and owns the ring, the index and rotation, and nothing more. src/sdwrite.cpp owns
// what those bytes MEAN, because it owns the card.
class SdData
{
public:
  // explicit: both arguments default, so without it this doubles as an implicit
  // conversion from int. cppcheck only started reporting that when ArduinoJson.h
  // left this header -- it had been giving up on the translation unit before, so
  // this library was never actually analysed.
  explicit SdData(int files = 4, size_t size = 1024UL * 1024UL * 1024UL);

  // Opens the file the persisted index names, closing whatever this object was
  // already holding. Opening always opens: calling this twice reopens, and the
  // second call lands on the file the index names rather than keeping the first.
  // It used to open only when nothing was held, which made a second call a
  // silent no-op that kept a file the index had moved away from -- and the object
  // cannot detect that for itself, because getLogFileName() derives the name from
  // _fileIdx, which readLogIndex() has just overwritten. Closing first is what
  // removes the question.
  void begin();

  // Closes the open file, and nothing else: the index is not reset, the callback
  // is not cleared and index.bin is not touched. begin() is the way back.
  //
  // The firmware never calls this -- TaskSdWrite calls begin() once and then
  // writes until power goes. It exists for a caller that needs the card to be
  // modifiable underneath it without this object holding a handle into a file
  // that is about to stop existing, which is exactly what the Unity suite's
  // tearDown() does before it deletes the log files.
  void end();

  // Registers the callback. Set it BEFORE begin() to catch the first file's
  // open: every begin() opens, so a callback registered afterwards catches
  // subsequent opens -- the next rotation, or the next begin() -- and never the
  // one that has already happened. Passing nullptr disables it.
  void setOnOpen(SdDataOnOpen callback);

  // Appends bytes and does NOT check the size limit, so it can never rotate. This
  // is what the onOpen callback writes through, and it is the whole reason the
  // callback cannot re-enter the rotation that invoked it.
  void writeRaw(const uint8_t *data, size_t length);

  // Appends bytes, then checks the size limit and rotates if it has been reached:
  // close, advance the index modulo the file count, persist the index, delete
  // whatever occupied the next slot, open it, and invoke onOpen. The record that
  // tripped the limit stays in the file it was written to.
  void write(const uint8_t *data, size_t length);

private:
  int _fileIdx = 0;
  int _files;
  size_t _size;
  File _dataFile;
  SdDataOnOpen _onOpen = nullptr;
  int readLogIndex();
  void writeLogIndex();
  String getLogFileName();
  void notifyOpened();
};

#endif
