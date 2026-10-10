#include "MeshOSFull.h"

bool MeshOSFull::blockChannel(uint8_t hash) {
  if (isChannelBlocked(hash)) return true;
  if (prefs.blocked_n >= MESHOS_MAX_BLOCKED) return false;
  prefs.blocked[prefs.blocked_n++] = hash;
  return true;
}

bool MeshOSFull::unblockChannel(uint8_t hash) {
  uint8_t n = prefs.blocked_n;
  for (uint8_t i = 0; i < n; i++) {
    if (prefs.blocked[i] == hash) {
      for (uint8_t j = i; j + 1 < n; j++) prefs.blocked[j] = prefs.blocked[j + 1];
      prefs.blocked_n = n - 1;
      return true;
    }
  }
  return false;
}

int MeshOSFull::matchEntry(const mesh::Packet* packet) const {
  if (prefs.entry_n == 0) return -1;
  uint8_t type = packet->getPayloadType();
  if ((type != PAYLOAD_TYPE_GRP_TXT && type != PAYLOAD_TYPE_GRP_DATA) || !packet->isRouteFlood()) return -1;
  uint8_t hops = packet->path_len & 63;
  uint8_t hash_size = (packet->path_len >> 6) + 1;
  if (hops == 0 || hash_size > MESHOS_MAX_ENTRY_LEN) return -1;
  for (uint8_t i = 0; i != MESHOS_MAX_ENTRIES && i < prefs.entry_n; i++) {
    uint8_t n = prefs.entries[i].len;
    if ((uint8_t)(n - 1) >= MESHOS_MAX_ENTRY_LEN) continue;
    if (n > hash_size) n = hash_size;
    if (memcmp(prefs.entries[i].id, packet->path, n) == 0) return i;
  }
  return -1;
}

bool MeshOSFull::allowPacketForward(const mesh::Packet* packet) {
  uint8_t type = packet->getPayloadType();
  if ((type == PAYLOAD_TYPE_GRP_TXT || type == PAYLOAD_TYPE_GRP_DATA) && packet->payload_len > 0
      && isChannelBlocked(packet->payload[0])) {
    return false;
  }
  int idx = matchEntry(packet);
  if (idx < 0) return MyMesh::allowPacketForward(packet);
  if (entry_hits[idx] != 0xFFFFFFFF) entry_hits[idx]++;
  return false;
}
