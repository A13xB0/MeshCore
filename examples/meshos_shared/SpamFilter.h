#pragma once

// ---------------------------------------------------------------------------
// MeshOS spam filter - shared by the full (ESP32) and lite (nRF52/RP2040)
// repeaters.
//
// Two lists decide which channel (group) packets are *not* repeated:
//
//   * blocked channels  - 1-byte channel hashes. Any group packet on one of
//                         these channels is dropped.
//   * blocked entries   - 1..3 byte repeater ID prefixes. A flooded group packet
//                         whose *first* hop (the repeater it entered the mesh
//                         through) matches one of these is dropped.
//
// Direct messages, acks, adverts etc. are never touched by the filter.
//
// The lists live in a FilterLists struct inside each repeater's prefs class, so
// they are saved with the rest of that repeater's settings. SpamFilter wraps a
// reference to them and adds the runtime counters (not persisted).
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <Mesh.h>

#define PUBLIC_CHANNEL_HASH    0x11  // channel hash of the default "Public" channel

#define FILTER_MAX_CHANNELS    16    // max blocked channel hashes
#define FILTER_MAX_ENTRIES     8     // max blocked entry repeaters
#define FILTER_MAX_ENTRY_LEN   3     // max bytes in an entry repeater ID prefix

// room for an entry ID as hex text, plus NUL and one spare
#define FILTER_ENTRY_HEX_LEN   (FILTER_MAX_ENTRY_LEN * 2 + 2)

struct FilterEntry {
  uint8_t len;                        // number of bytes of id[] in use (1..3)
  uint8_t id[FILTER_MAX_ENTRY_LEN];   // repeater ID prefix
};

// The persisted part of the filter. Each repeater's prefs class def()s these
// fields under the keys blocked_n / blocked / entry_n / entry.
struct FilterLists {
  uint8_t channel_count = 0;
  uint8_t channels[FILTER_MAX_CHANNELS] = {};
  uint8_t entry_count = 0;
  FilterEntry entries[FILTER_MAX_ENTRIES] = {};
};

// ---- small text helpers used by the filter commands ----

namespace meshos {

inline bool isGroupPayload(uint8_t type) {
  return type == PAYLOAD_TYPE_GRP_TXT || type == PAYLOAD_TYPE_GRP_DATA;
}

inline const char* skipSpaces(const char* s) {
  while (*s == ' ') s++;
  return s;
}

// If `command` starts with `prefix`, returns the text after it (leading spaces
// skipped); otherwise NULL. Lets command handlers avoid hand-counted lengths.
inline char* afterPrefix(char* command, const char* prefix) {
  size_t n = strlen(prefix);
  if (strncmp(command, prefix, n) != 0) return NULL;
  return (char*) skipSpaces(command + n);
}

inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Upper-case hex of `len` bytes, NUL-terminated. dest needs len*2 + 1 bytes.
inline void bytesToHex(const uint8_t* src, uint8_t len, char* dest) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  for (uint8_t i = 0; i < len; i++) {
    *dest++ = HEX_DIGITS[src[i] >> 4];
    *dest++ = HEX_DIGITS[src[i] & 0x0F];
  }
  *dest = 0;
}

inline const char* skipHexPrefix(const char* s) {
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return s + 2;
  return s;
}

// Hashtag channels derive their key from their name: secret = sha256(name)
// truncated to 16 bytes, and the channel hash is the first byte of
// sha256(secret). `name` includes the leading '#'.
inline uint8_t hashtagChannelHash(const char* name) {
  uint8_t secret[16];
  mesh::Utils::sha256(secret, sizeof(secret), (const uint8_t*) name, strlen(name));
  uint8_t hash;
  mesh::Utils::sha256(&hash, 1, secret, sizeof(secret));
  return hash;
}

// Parses a channel given as "#hashtag" or as a one-byte hex hash ("A5", "0xA5").
inline bool parseChannelHash(const char* arg, uint8_t& hash) {
  arg = skipSpaces(arg);
  if (*arg == '#') {
    hash = hashtagChannelHash(arg);
    return true;
  }
  arg = skipHexPrefix(arg);
  size_t len = strlen(arg);
  if (len < 1 || len > 2) return false;

  uint8_t value = 0;
  for (size_t i = 0; i < len; i++) {
    int nibble = hexNibble(arg[i]);
    if (nibble < 0) return false;
    value = (value << 4) | nibble;
  }
  hash = value;
  return true;
}

// Parses a repeater ID prefix in hex ("A0", "A07E", optional 0x, spaces ignored)
// of 1..FILTER_MAX_ENTRY_LEN whole bytes.
inline bool parseEntryId(const char* arg, uint8_t* id, uint8_t& len) {
  arg = skipHexPrefix(arg);
  memset(id, 0, FILTER_MAX_ENTRY_LEN);

  int nibbles = 0;
  for ( ; *arg; arg++) {
    if (*arg == ' ') continue;
    int nibble = hexNibble(*arg);
    if (nibble < 0 || nibbles >= FILTER_MAX_ENTRY_LEN * 2) return false;
    id[nibbles / 2] = (id[nibbles / 2] << 4) | nibble;
    nibbles++;
  }
  if (nibbles == 0 || nibbles % 2 != 0) return false;   // need whole bytes
  len = nibbles / 2;
  return true;
}

}  // namespace meshos

// ---------------------------------------------------------------------------

class SpamFilter {
  FilterLists& _lists;

public:
  // Runtime counters (reset on reboot / "clear chanstats").
  uint8_t  chan_heard[256] = {};    // group packets heard, by channel hash
  uint8_t  entry_heard[256] = {};   // flooded group packets heard, by first-hop hash
  uint32_t entry_hits[FILTER_MAX_ENTRIES] = {};  // packets dropped, per blocked entry
  uint32_t not_repeated = 0;        // packets dropped by the channel block list

  explicit SpamFilter(FilterLists& lists) : _lists(lists) { }

  const FilterLists& lists() const { return _lists; }
  uint8_t channelCount() const { return min(_lists.channel_count, (uint8_t) FILTER_MAX_CHANNELS); }
  uint8_t entryCount() const { return min(_lists.entry_count, (uint8_t) FILTER_MAX_ENTRIES); }

  bool isChannelBlocked(uint8_t hash) const {
    for (uint8_t i = 0; i < channelCount(); i++) {
      if (_lists.channels[i] == hash) return true;
    }
    return false;
  }

  int findEntry(const uint8_t* id, uint8_t len) const {
    for (uint8_t i = 0; i < entryCount(); i++) {
      const FilterEntry& e = _lists.entries[i];
      if (e.len == len && memcmp(e.id, id, len) == 0) return i;
    }
    return -1;
  }

  // Index of the blocked entry this packet came in through, or -1. Only applies
  // to flooded group packets; compares the entry's ID against the first path
  // hash (shortened to the packet's path hash size if needed).
  int matchEntry(const mesh::Packet* pkt) const {
    if (_lists.entry_count == 0) return -1;
    if (!meshos::isGroupPayload(pkt->getPayloadType()) || !pkt->isRouteFlood()) return -1;

    uint8_t hash_size = pkt->getPathHashSize();
    if (pkt->getPathHashCount() == 0 || hash_size > FILTER_MAX_ENTRY_LEN) return -1;

    for (uint8_t i = 0; i < entryCount(); i++) {
      const FilterEntry& e = _lists.entries[i];
      if (e.len < 1 || e.len > FILTER_MAX_ENTRY_LEN) continue;
      uint8_t compare_len = min(e.len, hash_size);
      if (memcmp(e.id, pkt->path, compare_len) == 0) return i;
    }
    return -1;
  }

  // Should this packet be dropped instead of repeated? Updates the drop counters.
  bool shouldDrop(const mesh::Packet* pkt) {
    if (meshos::isGroupPayload(pkt->getPayloadType()) && pkt->payload_len > 0
        && isChannelBlocked(pkt->payload[0])) {
      if (not_repeated != UINT32_MAX) not_repeated++;
      return true;
    }
    int entry = matchEntry(pkt);
    if (entry >= 0) {
      if (entry_hits[entry] != UINT32_MAX) entry_hits[entry]++;
      return true;
    }
    return false;
  }

  // Count a received packet in the heard-by-channel / heard-by-entry stats.
  void noteHeard(const mesh::Packet* pkt) {
    if (!meshos::isGroupPayload(pkt->getPayloadType()) || pkt->payload_len == 0) return;
    countHeard(chan_heard, pkt->payload[0]);
    if (pkt->isRouteFlood() && pkt->getPathHashCount() > 0) {
      countHeard(entry_heard, pkt->path[0]);
    }
  }

  void clearStats() {
    memset(chan_heard, 0, sizeof(chan_heard));
    memset(entry_heard, 0, sizeof(entry_heard));
    memset(entry_hits, 0, sizeof(entry_hits));
    not_repeated = 0;
  }

  // ---- list edits. Each returns true on success; the caller saves the prefs. ----

  // false only if the list is full (already blocked counts as success)
  bool blockChannel(uint8_t hash) {
    if (isChannelBlocked(hash)) return true;
    if (_lists.channel_count >= FILTER_MAX_CHANNELS) return false;
    _lists.channels[_lists.channel_count++] = hash;
    return true;
  }

  // false if the channel was not blocked
  bool unblockChannel(uint8_t hash) {
    uint8_t n = channelCount();
    for (uint8_t i = 0; i < n; i++) {
      if (_lists.channels[i] != hash) continue;
      memmove(&_lists.channels[i], &_lists.channels[i + 1], n - i - 1);
      _lists.channel_count = n - 1;
      return true;
    }
    return false;
  }

  // false if the list is full or len is out of range (already blocked counts as success)
  bool addEntry(const uint8_t* id, uint8_t len) {
    if (len < 1 || len > FILTER_MAX_ENTRY_LEN) return false;
    if (findEntry(id, len) >= 0) return true;
    if (_lists.entry_count >= FILTER_MAX_ENTRIES) return false;

    FilterEntry& e = _lists.entries[_lists.entry_count];
    memset(&e, 0, sizeof(e));
    e.len = len;
    memcpy(e.id, id, len);
    entry_hits[_lists.entry_count] = 0;
    _lists.entry_count++;
    return true;
  }

  // false if the entry was not blocked
  bool removeEntry(const uint8_t* id, uint8_t len) {
    int index = findEntry(id, len);
    if (index < 0) return false;

    uint8_t n = entryCount();
    for (uint8_t i = index; i + 1 < n; i++) {
      _lists.entries[i] = _lists.entries[i + 1];
      entry_hits[i] = entry_hits[i + 1];
    }
    _lists.entry_count = n - 1;
    memset(&_lists.entries[_lists.entry_count], 0, sizeof(FilterEntry));
    entry_hits[_lists.entry_count] = 0;
    return true;
  }

  // Clamp counts and drop malformed entries after loading from flash.
  void sanitise() {
    if (_lists.channel_count > FILTER_MAX_CHANNELS) _lists.channel_count = FILTER_MAX_CHANNELS;

    uint8_t kept = 0;
    for (uint8_t i = 0; i < entryCount(); i++) {
      uint8_t len = _lists.entries[i].len;
      if (len < 1 || len > FILTER_MAX_ENTRY_LEN) continue;
      _lists.entries[kept++] = _lists.entries[i];
    }
    for (uint8_t i = kept; i < FILTER_MAX_ENTRIES; i++) {
      memset(&_lists.entries[i], 0, sizeof(FilterEntry));
    }
    _lists.entry_count = kept;
  }

private:
  // 8-bit counters: when one would overflow, halve them all so the relative
  // ranking is kept.
  static void countHeard(uint8_t counts[256], uint8_t hash) {
    if (counts[hash] == 0xFF) {
      for (int i = 0; i < 256; i++) counts[i] >>= 1;
    }
    counts[hash]++;
  }
};
