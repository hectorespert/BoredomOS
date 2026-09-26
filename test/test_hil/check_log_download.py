"""The flight log, listed and downloaded over the MAVLink log protocol.

Covers the flight-log spec requirements "The log can be listed from the ground", "A
listed log can be downloaded, including the one being written", "Downloading costs
neither the log nor the link's cadence", "The ground is told when there is nothing to
download" and "No command from the ground erases the log"
(openspec/changes/download-the-flight-log/specs/flight-log/spec.md).

What these cases cannot see: that the bytes downloaded are the bytes on the card, and
that no log record is lost while a download runs. Both need the card read by some other
means -- pulled, or downloaded again afterwards -- and are tasks 4.2 and 4.3 of the change
rather than cases here. The no-card answer is checked in the reduced configuration, which
takes the same path, and self-skips otherwise.

Listens fresh, never through link.sample()'s cached window.
"""

import os
import tempfile
import time

from hil import NoLinkError

CHUNK = 90


def _mav():
    from pymavlink import mavutil

    return mavutil.mavlink


def _drain(link):
    while link.mav.recv_match(blocking=False) is not None:
        pass


def _collect(link, seconds, types=None, stop=None):
    out = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        msg = link.mav.recv_match(type=types, blocking=True, timeout=max(0.05, deadline - time.time()))
        if msg is None or msg.get_type() == "BAD_DATA":
            continue
        out.append(msg)
        if stop and stop(msg):
            break
    return out


def _list(link):
    _drain(link)
    link.mav.mav.log_request_list_send(1, _mav().MAV_COMP_ID_AUTOPILOT1, 0, 0xFFFF)
    entries = _collect(link, 4.0, types="LOG_ENTRY")
    return {e.id: e for e in entries if e.num_logs > 0}, entries


def _require_logs(link):
    by_id, raw = _list(link)
    if raw and not by_id:
        raise NoLinkError("the board reports no logs (no card, or the reduced configuration)")
    assert by_id, "no LOG_ENTRY in answer to LOG_REQUEST_LIST"
    return by_id


def _request(link, log_id, ofs, count):
    link.mav.mav.log_request_data_send(1, _mav().MAV_COMP_ID_AUTOPILOT1, log_id, ofs, count)


def _end(link):
    link.mav.mav.log_request_end_send(1, _mav().MAV_COMP_ID_AUTOPILOT1)


def _download(link, log_id, size, timeout=300.0):
    """The whole log, MAVProxy's way: everything from 0, until a chunk shorter than 90."""
    _drain(link)
    _request(link, log_id, 0, 0xFFFFFFFF)
    chunks = {}
    others = []
    last = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        msg = link.mav.recv_match(blocking=True, timeout=2.0)
        if msg is None:
            break
        if msg.get_type() != "LOG_DATA":
            others.append((time.time(), msg.get_type()))
            continue
        chunks[msg.ofs] = bytes(msg.data[:msg.count])
        last = msg
        if msg.count < CHUNK:
            break
    _end(link)
    return chunks, last, others


# --- listing -------------------------------------------------------------------------


def test_logs_are_listed_consistently(link):
    by_id = _require_logs(link)
    counts = {(e.num_logs, e.last_log_num) for e in by_id.values()}
    assert len(counts) == 1, f"entries disagree on num_logs/last_log_num: {counts}"
    num_logs, last_log_num = counts.pop()
    assert num_logs == len(by_id), f"num_logs {num_logs}, but {len(by_id)} entries arrived"
    assert last_log_num == max(by_id), f"last_log_num {last_log_num}, highest id {max(by_id)}"
    assert all(1 <= i <= 4 for i in by_id), f"ids outside the ring's 1..4: {sorted(by_id)}"
    newest = by_id[last_log_num]
    assert newest.size > 0, "the log being written reports size 0"


def test_ids_do_not_move_between_listings(link):
    first = _require_logs(link)
    time.sleep(3)
    second = _require_logs(link)
    for log_id in set(first) & set(second):
        assert first[log_id].time_utc == second[log_id].time_utc, (
            f"id {log_id} changed time_utc between listings: a different file under the same id"
        )
        assert second[log_id].size >= first[log_id].size, f"id {log_id} shrank between listings"


# --- downloading ---------------------------------------------------------------------


def test_a_range_is_downloaded_in_chunks_of_90(link):
    by_id = _require_logs(link)
    log_id = max(by_id)
    if by_id[log_id].size < 2700:
        raise NoLinkError("the newest log is too small for a 900-byte range at offset 1800")

    _drain(link)
    _request(link, log_id, 1800, 900)
    data = [m for m in _collect(link, 4.0, types="LOG_DATA") if m.id == log_id]
    assert [m.ofs for m in data] == list(range(1800, 2700, CHUNK)), (
        f"offsets {[m.ofs for m in data]}, expected 1800..2610 in steps of 90"
    )
    assert all(m.count == CHUNK for m in data), "a chunk of the range was short"


def test_the_log_being_written_downloads_whole_and_parses(link):
    by_id = _require_logs(link)
    log_id = max(by_id)
    size = by_id[log_id].size

    chunks, last, _ = _download(link, log_id, size)
    assert last is not None, "no LOG_DATA arrived"
    assert last.count < CHUNK, "the download did not end with a short chunk"
    offsets = sorted(chunks)
    assert offsets == list(range(0, offsets[-1] + 1, CHUNK)), "the download has gaps"
    blob = b"".join(chunks[o] for o in offsets)
    assert len(blob) >= size, f"{len(blob)} bytes downloaded, the listing said {size}"

    from pymavlink import DFReader

    fd, path = tempfile.mkstemp(suffix=".bin")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(blob)
        reader = DFReader.DFReader_binary(path)
        types = set()
        while True:
            msg = reader.recv_msg()
            if msg is None:
                break
            types.add(msg.get_type())
    finally:
        os.remove(path)
    assert {"FMT", "TIME", "SYS"} <= types, f"the downloaded log does not parse as expected: {types}"


def test_an_id_that_does_not_exist(link):
    _drain(link)
    _request(link, 99, 0, 0xFFFFFFFF)
    data = [m for m in _collect(link, 3.0, types="LOG_DATA") if m.id == 99]
    assert len(data) == 1 and data[0].count == 0, (
        f"expected one LOG_DATA with count 0 for id 99, got {[(m.ofs, m.count) for m in data]}"
    )


def test_log_request_end_stops_the_download(link):
    by_id = _require_logs(link)
    log_id = max(by_id)
    if by_id[log_id].size < 20 * CHUNK:
        raise NoLinkError("the newest log is too small to interrupt")

    _drain(link)
    _request(link, log_id, 0, 0xFFFFFFFF)
    _collect(link, 5.0, types="LOG_DATA", stop=lambda m: m.ofs >= 5 * CHUNK)
    _end(link)
    _collect(link, 0.5, types="LOG_DATA")  # whatever was already queued
    late = [m for m in _collect(link, 2.0, types="LOG_DATA") if m.id == log_id]
    assert not late, f"{len(late)} LOG_DATA still arrived 0.5-2.5 s after LOG_REQUEST_END"


def test_telemetry_keeps_its_cadence_during_a_download(link):
    by_id = _require_logs(link)
    log_id = max(by_id)
    started = time.time()
    _, _, others = _download(link, log_id, by_id[log_id].size)
    elapsed = time.time() - started
    if elapsed < 4.0:
        raise NoLinkError(f"the download took {elapsed:.1f} s, too short to measure a cadence")
    for name in ("HEARTBEAT", "SYSTEM_TIME", "SYS_STATUS"):
        n = sum(1 for _, t in others if t == name)
        assert n >= int(elapsed) - 1, f"{n} {name} during a {elapsed:.0f} s download, expected ~1 Hz"


# --- erasing -------------------------------------------------------------------------


def test_log_erase_changes_nothing_and_sends_nothing(link):
    before = _require_logs(link)
    _drain(link)
    link.mav.mav.log_erase_send(1, _mav().MAV_COMP_ID_AUTOPILOT1)
    answer = [m for m in _collect(link, 2.0)
              if m.get_type() in ("STATUSTEXT", "COMMAND_ACK", "LOG_ENTRY", "LOG_DATA")]
    assert not answer, f"LOG_ERASE drew {[m.get_type() for m in answer]}"
    after = _require_logs(link)
    assert set(after) == set(before), f"ids before {sorted(before)}, after {sorted(after)}"
    for log_id in before:
        assert after[log_id].size >= before[log_id].size, f"id {log_id} shrank after LOG_ERASE"


# --- nothing to download (manual) ----------------------------------------------------


def test_the_reduced_configuration_reports_no_logs(link):
    """The same no-TaskSdWrite path a missing card takes. Put the board in the reduced
    configuration by hand (three boots that fail the stability window) to run it."""
    mav = _mav()
    hb = link.mav.recv_match(type="HEARTBEAT", blocking=True, timeout=15.0)
    if hb is None or hb.system_status != mav.MAV_STATE_CRITICAL:
        raise NoLinkError("board is not in the reduced configuration")
    _, raw = _list(link)
    assert raw and raw[0].num_logs == 0, "expected a LOG_ENTRY with num_logs 0"
    _drain(link)
    _request(link, 1, 0, 0xFFFFFFFF)
    data = _collect(link, 3.0, types="LOG_DATA")
    assert len(data) == 1 and data[0].count == 0, "expected one LOG_DATA with count 0"
