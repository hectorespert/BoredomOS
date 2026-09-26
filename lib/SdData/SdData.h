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
  // Four files of 1 MiB, which is a fixed 4 MiB of card. The size is derived from
  // how long a download may take rather than picked round: at the log's ~34 B/s a
  // megabyte covers ~8.6 h, so four of them hold ~34 h and rotate several times a
  // day, and one file comes down a 57 600 baud link in under four minutes through
  // either download path. design.md in the change that set these has the table.
  //
  // It was 1 GiB per file until then, at which filling one took about a year --
  // so rotation, index.bin and the resume-after-power-cycle path had never run on
  // a board doing its job, and the log needed 4 GiB of card to be safe from a
  // silent write failure.
  explicit SdData(int files = 4, size_t size = 1024UL * 1024UL);

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
  //
  // It also syncs on EVERY call, unlike write(). That asymmetry is deliberate: this
  // is the path the format preamble takes, and a preamble that does not reach the
  // card makes every record in the file undecodable rather than merely losing the
  // last few. It is worth a sync that an individual record is not.
  void writeRaw(const uint8_t *data, size_t length);

  // Appends bytes, then checks the size limit and rotates if it has been reached:
  // close, advance the index modulo the file count, persist the index, delete
  // whatever occupied the next slot, open it, and invoke onOpen. The record that
  // tripped the limit stays in the file it was written to.
  //
  // Unlike writeRaw(), this does NOT sync on every call. It accumulates and syncs
  // once per FLUSH_INTERVAL_BYTES, which is what bounds the cost: a per-record sync
  // evicts the single shared 512 B cache block that the record just went into, so
  // the next record has to read it back, and the directory entry is rewritten every
  // time. Batching lets the block fill and be written once.
  //
  // The consequence is the bound on what power loss costs, and it is stated in
  // openspec/specs/flight-log/spec.md rather than left as an implementation detail:
  // a cut loses at most the log written since the last sync.
  void write(const uint8_t *data, size_t length);

  // Which slot of the ring is being written, and how many slots there are. A reader
  // of the log (the download path in src/sdwrite.cpp) compares the first with the
  // slot it is serving after every write(): a write that rotates into that slot has
  // just deleted it.
  int currentFile() const { return _fileIdx; }
  int fileCount() const { return _files; }

private:
  // How much is appended through write() before it syncs. A BYTE count, so it is a
  // duration only at a given write rate -- at the log's ~34 B/s this is about two
  // minutes, and a change that adds records shortens it, which is the safe
  // direction. 4 KiB is where the returns stop: 16 KiB buys about 20 % fewer writes
  // for four times the loss, and 512 B gives up half the saving to gain seconds
  // nobody needs. See design.md's table.
  static const size_t FLUSH_INTERVAL_BYTES = 4096;

  // Appended through write() since the last sync. Reset by every sync, by an open
  // and by end(), so an interval never straddles two files.
  size_t _sinceFlush = 0;

  // Appends without syncing. Both public write paths go through this and differ
  // only in when they sync, which keeps "what gets written" in one place.
  void appendBytes(const uint8_t *data, size_t length);

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
