#pragma once

#include <RadioLib.h>

// Continuous receive for SX126x-family chips. The radio keeps listening after RxDone and writes each
// packet straight after the last in its 256-byte buffer, so packets are read in place and in order.
// GetStats says how many finished. The start pointer moves at RxDone, but the length follows the newest
// LoRa header, so a length is only used when no newer header can have arrived; otherwise it is taken
// from where the next packet starts.
class SX126xRxRing {
  static const uint8_t MAX_UNREAD = 8;

  SX126x* _radio;
  uint8_t _next = 0;              // start of the oldest unread packet
  bool _lost = false;             // _next unknown: resync on the newest packet
  uint16_t _recvBase = 0, _crcBase = 0;
  uint8_t _n = 0;                 // finished, unread packets, oldest first
  bool _ok[MAX_UNREAD];
  uint32_t _at[MAX_UNREAD];       // no later than each one's RxDone (micros)
  bool _held = false;             // newest packet's length read too late to trust yet
  uint8_t _heldStart = 0, _heldLen = 0;
  bool _heldCRC = true;
  uint32_t _heldAt = 0;

  uint32_t symbolMicros() const { return ((uint32_t)1000 << _radio->spreadingFactor) / _radio->bandwidthKhz; }

  // a newer header needs at least the sync word and the header block after an RxDone
  uint32_t quietMicros() const { return symbolMicros() * 12; }

  // a whole 255-byte packet at CR 4/8 with low data rate optimisation
  uint32_t maxPayloadMicros() const {
    uint8_t sf = _radio->spreadingFactor;
    return symbolMicros() * (8 + ((8 * 255 + 64 - 4 * sf) / (4 * (sf - 2)) + 1) * 8);
  }

  // payload bytes the radio can have written since `from`: none before a header, CR 4/5 at most after,
  // plus a few carried in the header block
  uint32_t maxWritten(uint32_t from) const {
    uint32_t elapsed = micros() - from;
    if (elapsed > 10000000) return 256;
    uint32_t quiet = quietMicros();
    if (elapsed <= quiet) return 0;
    return (elapsed - quiet) * _radio->spreadingFactor / (10 * symbolMicros()) + 4;
  }

  bool readStats(uint16_t& recv, uint16_t& crc) {
    uint8_t buf[6];
    if (_radio->mod->SPIreadStream(RADIOLIB_SX126X_CMD_GET_STATS, buf, 6) != RADIOLIB_ERR_NONE) return false;
    recv = ((uint16_t)buf[0] << 8) | buf[1];
    crc = ((uint16_t)buf[2] << 8) | buf[3];
    return true;
  }

  void pop() {
    for (uint8_t i = 1; i < _n; i++) { _ok[i - 1] = _ok[i]; _at[i - 1] = _at[i]; }
    _n--;
  }

  void dropOlder() {
    while (_n > 1) { pop(); n_dropped++; }
  }

  int take(uint8_t* bytes, int sz, uint8_t start, uint8_t len, bool ok) {
    if (!ok || len == 0 || len > sz) { n_dropped++; return -1; }
    uint8_t first = (start + len > 256) ? 256 - start : len;
    if (_radio->readBuffer(bytes, first, start) != RADIOLIB_ERR_NONE) return -1;
    if (first < len && _radio->readBuffer(bytes + first, len - first, 0) != RADIOLIB_ERR_NONE) return -1;
    return len;
  }

  // `after`: most the radio can have written after the packet, now that it has been read
  int keep(int len, uint32_t after) {
    if (len > 0 && len + after > 256) { n_dropped++; return -1; }
    return len;
  }

  int readHeld(uint8_t* bytes, int sz) {
    if (micros() - _heldAt < maxPayloadMicros()) return 0;
    // nothing finished since; if no header has started either, the held length was this packet's
    uint32_t irq = _radio->getIrqFlags();
    if (irq & (RADIOLIB_SX126X_IRQ_HEADER_VALID | RADIOLIB_SX126X_IRQ_RX_DONE)) return 0;
    uint8_t start = 0;
    uint8_t len = _radio->getPacketLength(true, &start);
    if (start != _heldStart || len != _heldLen) return 0;
    _held = false;
    bool ok = _ok[0];
    pop();
    _next = start + len;
    uint32_t checked = micros() - quietMicros();
    int n = take(bytes, sz, start, len, ok && _heldCRC);
    return keep(n, maxWritten(checked));
  }

public:
  uint32_t n_recovered = 0, n_dropped = 0;

  SX126xRxRing(SX126x& radio) : _radio(&radio) { }

  // after startReceive(), which puts the rx base address back to 0
  void reset() {
    n_dropped += _n;
    _next = 0;
    _lost = false;
    _n = 0;
    _held = false;
    readStats(_recvBase, _crcBase);
  }

  bool hasUnread() const { return _n > 0; }

  // worth calling read() without an interrupt
  bool due() const { return _n > 0 && (!_held || micros() - _heldAt >= maxPayloadMicros()); }

  // `irq`: an RxDone interrupt at or after `irqAt` (micros). > 0 packet length, 0 nothing, < 0 dropped
  int read(uint8_t* bytes, int sz, bool irq, uint32_t irqAt) {
    if (irq) {
      uint32_t flags = _radio->getIrqFlags();
      _radio->clearIrqStatus(flags);
      uint16_t recv, crc;
      if (readStats(recv, crc)) {
        // a packet with a bad CRC may or may not be in the received count: counting
        // it twice only drops packets, missing it misreads them
        uint16_t bad = crc - _crcBase, done = (uint16_t)(recv - _recvBase) + bad;
        _recvBase = recv;
        _crcBase = crc;
        if (done > 0 && bad == 0 && (flags & RADIOLIB_SX126X_IRQ_CRC_ERR)) bad = 1;
        for (uint16_t i = 0; i < done; i++) {
          if (_n == MAX_UNREAD) { pop(); n_dropped++; _lost = true; }
          _ok[_n] = bad == 0;   // a CRC error among several can't be placed, so drop them all
          _at[_n] = irqAt;
          _n++;
        }
        if (done > 0) _held = false;
      }
    }
    if (_n == 0) return 0;
    if (_held) return readHeld(bytes, sz);

    uint8_t start = 0;
    uint8_t len = _radio->getPacketLength(true, &start);
    bool hasCRC = true;
    _radio->getLoRaRxHeaderInfo(NULL, &hasCRC);   // header says no CRC: payload never checked
    uint32_t newest = _at[_n - 1];
    bool trusted = micros() - newest < quietMicros();
    if (_lost) {
      _next = start;
      _lost = false;
      dropOlder();
    }

    uint8_t gap = start - _next;
    if (gap != 0) {
      if (_n > 2) {
        // several packets before the newest, and only their total length is known
        dropOlder();
        _next = start;
      } else {
        // the oldest ends where the newest starts
        uint8_t from = _next;
        bool ok = _ok[0];
        uint32_t at = _at[0];
        pop();
        _next = start;
        n_recovered++;
        int n = take(bytes, sz, from, gap, ok);
        // the newest packet and whatever followed it, or everything since this one finished
        uint32_t after = maxWritten(at);
        if (_n > 0 && trusted && len + maxWritten(newest) < after) after = len + maxWritten(newest);
        return keep(n, after);
      }
    }
    if (_n > 1) dropOlder();   // exactly 256 bytes of them: overwritten

    if (!trusted) {
      _held = true;
      _heldStart = start;
      _heldLen = len;
      _heldCRC = hasCRC;
      _heldAt = micros();
      return 0;
    }
    bool ok = _ok[0];
    pop();
    _next = start + len;
    int n = take(bytes, sz, start, len, ok && hasCRC);
    return keep(n, maxWritten(newest));
  }
};
