// Patterns in flash: a small log over the USRFLASH window (smcpad_flash_*, 7 x 4 KB sectors).
// A save appends a new record (header + sparse pattern, CRC-checked) and then marks the
// previous copy stale; at boot the newest valid copy of each slot wins, so a power cut never
// loses more than the save in progress. When no erased sector is left, the sector with the least live
// data is copied forward and erased.
#pragma once
#include "Model.h"

static const uint16_t REC_MAGIC = 0x5350;   // "PS"
enum { KIND_STALE = 0x00, KIND_PATTERN = 0x01, KIND_FREE = 0xFF };

struct RecordHeader {
  uint16_t magic;
  uint8_t kind;
  uint8_t slot;
  uint16_t length;            // payload bytes
  uint16_t crc;               // CRC16 of the payload
  uint32_t seq;
};

static const uint32_t TRACK_BYTES = 15;     // a packed Track
static const uint32_t SECTORS = (SMCPAD_FLASH_END - SMCPAD_FLASH_BEGIN) / SMCPAD_FLASH_SECTOR;
static const uint32_t MAX_PAYLOAD = SMCPAD_FLASH_SECTOR - sizeof(RecordHeader);

static uint32_t slotAddr[PATTERNS];         // 0: never saved
static uint32_t storeSeq;
static uint32_t writeAddr;                  // next free byte in the active sector
static uint8_t storeBuf[SMCPAD_FLASH_SECTOR];

static uint16_t crc16(const uint8_t *d, uint32_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t)*d++ << 8;
    for (int i = 0; i < 8; i++) crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

static uint32_t align4(uint32_t n) { return (n + 3) & ~3u; }
static uint32_t sectorOf(uint32_t addr) { return addr & ~(SMCPAD_FLASH_SECTOR - 1); }

// ---------------------------------------------------------------- pattern <-> bytes

static uint32_t patternPack(const Pattern &p, uint8_t *out) {
  uint32_t n = 0;
  out[n++] = p.length;
  out[n++] = p.swing;
  for (int t = 0; t < TRACKS; t++) {
    const Track &k = p.track[t];
    uint8_t v[] = {k.channel, k.note, k.velocity, k.length, k.speed, k.gate,
                   k.lockCC[0], k.lockCC[1], k.lockCC[2], k.lockCC[3],
                   k.lockBase[0], k.lockBase[1], k.lockBase[2], k.lockBase[3], (uint8_t)k.muted};
    memcpy(out + n, v, sizeof(v));
    n += sizeof(v);
  }
  uint32_t countAt = n;
  n += 2;
  uint16_t count = 0;
  for (int t = 0; t < TRACKS; t++) {
    for (int s = 0; s < MAX_STEPS; s++) {
      const Step &st = p.step[t][s];
      if (stepIsDefault(st)) continue;
      if (n + 12 > MAX_PAYLOAD) return 0;   // does not fit a record
      uint8_t mask = 0;
      for (int i = 0; i < LOCKS; i++) if (st.lock[i] != NO_LOCK) mask |= 1 << i;
      out[n++] = t;
      out[n++] = s;
      out[n++] = st.flags;
      out[n++] = st.velocity;
      out[n++] = (uint8_t)st.micro;
      out[n++] = st.cond;
      out[n++] = st.ratchet;
      out[n++] = mask;
      for (int i = 0; i < LOCKS; i++) if (mask & (1 << i)) out[n++] = st.lock[i];
      count++;
    }
  }
  out[countAt] = count;
  out[countAt + 1] = count >> 8;
  return n;
}

// every field read once into a local: Arduino's constrain() is a macro and evaluates its
// argument more than once (with in[n++] that read the wrong bytes: a track of length 0)
static uint8_t clampByte(uint8_t v, uint8_t lo, uint8_t hi) { return v < lo ? lo : v > hi ? hi : v; }

// what the engine divides by or indexes with, kept valid whatever the flash held
static void patternSanitize(Pattern &p) {
  p.length = clampByte(p.length / 16 * 16, 16, MAX_STEPS);
  p.swing = clampByte(p.swing, 0, 4);
  for (int t = 0; t < TRACKS; t++) {
    Track &k = p.track[t];
    k.channel &= 15;
    k.note &= 127;
    k.length = clampByte(k.length, 1, MAX_STEPS);
    k.speed = clampByte(k.speed, SPEED_HALF, SPEED_DOUBLE);
    k.gate = clampByte(k.gate, 5, 100);
    for (int s = 0; s < MAX_STEPS; s++) {
      Step &st = p.step[t][s];
      st.ratchet = clampByte(st.ratchet, 1, 4);
      st.velocity = clampByte(st.velocity, 1, 127);
      if (st.micro < -11 || st.micro > 11) st.micro = 0;
      if (st.cond > COND_LAST) st.cond = COND_ALWAYS;
    }
  }
}

static bool patternUnpack(Pattern &p, const uint8_t *in, uint32_t len) {
  uint32_t n = 0;
  if (len < 2 + TRACKS * TRACK_BYTES + 2) return false;
  patternClear(p);
  p.length = in[n++];
  p.swing = in[n++];
  for (int t = 0; t < TRACKS; t++) {
    Track &k = p.track[t];
    k.channel = in[n++] & 15;
    k.note = in[n++] & 127;
    k.velocity = in[n++];
    k.length = in[n++];
    k.speed = in[n++];
    k.gate = in[n++];
    for (int i = 0; i < LOCKS; i++) k.lockCC[i] = in[n++];
    for (int i = 0; i < LOCKS; i++) k.lockBase[i] = in[n++] & 127;
    k.muted = in[n++];
  }
  uint16_t count = in[n] | in[n + 1] << 8;
  n += 2;
  while (count-- && n + 8 <= len) {
    int t = in[n++] & 15, s = in[n++] & 63;
    Step &st = p.step[t][s];
    st.flags = in[n++];
    st.velocity = in[n++];
    st.micro = (int8_t)in[n++];
    st.cond = in[n++];
    st.ratchet = in[n++];
    uint8_t mask = in[n++];
    for (int i = 0; i < LOCKS; i++) st.lock[i] = mask & (1 << i) && n < len ? in[n++] : NO_LOCK;
  }
  patternSanitize(p);
  return true;
}

// ---------------------------------------------------------------- the log

static bool readHeader(uint32_t addr, RecordHeader &h) {
  return smcpad_flash_read(addr, &h, sizeof(h)) == sizeof(h);
}

static bool recordValid(uint32_t addr, RecordHeader &h, bool checkCrc) {
  if (!readHeader(addr, h) || h.magic != REC_MAGIC || h.kind != KIND_PATTERN || h.slot >= PATTERNS ||
      h.length > MAX_PAYLOAD || addr + sizeof(h) + h.length > sectorOf(addr) + SMCPAD_FLASH_SECTOR)
    return false;
  if (!checkCrc) return true;
  smcpad_flash_read(addr + sizeof(h), storeBuf, h.length);
  return crc16(storeBuf, h.length) == h.crc;
}

// walks the records of a sector; returns the first free address in it (or its end)
template <typename F> static uint32_t sectorWalk(uint32_t sector, F f) {
  uint32_t a = sector, end = sector + SMCPAD_FLASH_SECTOR;
  RecordHeader h;
  while (a + sizeof(h) <= end && readHeader(a, h) && h.magic == REC_MAGIC) {
    if (h.length > MAX_PAYLOAD) break;
    f(a, h);
    a += align4(sizeof(h) + h.length);
  }
  return a;
}

static bool sectorErased(uint32_t sector) {
  RecordHeader h;
  readHeader(sector, h);
  return h.magic == 0xFFFF;
}

static uint32_t liveBytes(uint32_t sector) {
  uint32_t live = 0;
  sectorWalk(sector, [&](uint32_t a, const RecordHeader &h) {
    if (h.kind == KIND_PATTERN && slotAddr[h.slot] == a) live += align4(sizeof(h) + h.length);
  });
  return live;
}

static void storeBegin() {
  uint32_t best[PATTERNS] = {0}, newest = 0, activeSector = 0;
  memset(slotAddr, 0, sizeof(slotAddr));
  storeSeq = 0;
  for (uint32_t i = 0; i < SECTORS; i++) {
    uint32_t sector = SMCPAD_FLASH_BEGIN + i * SMCPAD_FLASH_SECTOR;
    RecordHeader h;
    readHeader(sector, h);
    if (h.magic != REC_MAGIC && h.magic != 0xFFFF) {   // not ours (the stock presets): format
      smcpad_flash_erase(sector);
      continue;
    }
    sectorWalk(sector, [&](uint32_t a, const RecordHeader &rh) {
      RecordHeader v;
      if (recordValid(a, v, true) && v.seq >= best[v.slot]) {
        best[v.slot] = v.seq;
        slotAddr[v.slot] = a;
      }
      if (rh.seq > newest && rh.seq != 0xFFFFFFFF) {
        newest = rh.seq;
        activeSector = sector;
      }
    });
  }
  storeSeq = newest;
  writeAddr = activeSector ? sectorWalk(activeSector, [](uint32_t, const RecordHeader &) {}) : 0;
}

static uint32_t erasedSector(uint32_t except) {
  for (uint32_t i = 0; i < SECTORS; i++) {
    uint32_t sector = SMCPAD_FLASH_BEGIN + i * SMCPAD_FLASH_SECTOR;
    if (sector != except && sectorErased(sector)) return sector;
  }
  return 0;
}

static bool appendRecord(uint8_t slot, const uint8_t *payload, uint32_t len);

// frees a sector: the one with the least live data moves its live records forward
static bool collectGarbage() {
  uint32_t victim = 0, least = ~0u;
  for (uint32_t i = 0; i < SECTORS; i++) {
    uint32_t sector = SMCPAD_FLASH_BEGIN + i * SMCPAD_FLASH_SECTOR;
    if (sector == sectorOf(writeAddr) || sectorErased(sector)) continue;
    uint32_t live = liveBytes(sector);
    if (live < least) { least = live; victim = sector; }
  }
  if (!victim) return false;
  for (int slot = 0; slot < PATTERNS; slot++) {
    if (slotAddr[slot] && sectorOf(slotAddr[slot]) == victim) {
      RecordHeader h;
      if (!recordValid(slotAddr[slot], h, true)) continue;
      static uint8_t copy[SMCPAD_FLASH_SECTOR];
      memcpy(copy, storeBuf, h.length);
      if (!appendRecord(slot, copy, h.length)) return false;
    }
  }
  return smcpad_flash_erase(victim) == 0;
}

static bool appendRecord(uint8_t slot, const uint8_t *payload, uint32_t len) {
  uint32_t need = align4(sizeof(RecordHeader) + len);
  if (len > MAX_PAYLOAD) return false;
  if (!writeAddr || writeAddr + need > sectorOf(writeAddr) + SMCPAD_FLASH_SECTOR) {
    uint32_t next = erasedSector(writeAddr ? sectorOf(writeAddr) : 0);
    if (!next) return false;
    writeAddr = next;
    if (!erasedSector(next) && !collectGarbage()) return false;   // keep one sector spare
  }
  uint32_t old = slotAddr[slot];
  RecordHeader h{REC_MAGIC, KIND_PATTERN, slot, (uint16_t)len, crc16(payload, len), ++storeSeq};
  uint32_t at = writeAddr;
  // header first: a write cut short leaves a record whose CRC fails, skipped (by its length)
  // at boot, rather than dirty bytes where the next record would be written
  if (smcpad_flash_write(at, &h, sizeof(h)) < 0) return false;
  if (smcpad_flash_write(at + sizeof(h), payload, len) < 0) return false;
  writeAddr = at + need;
  slotAddr[slot] = at;
  if (old) {
    uint8_t stale = KIND_STALE;
    smcpad_flash_write(old + offsetof(RecordHeader, kind), &stale, 1);
  }
  return true;
}

static bool storeSave(uint8_t slot, const Pattern &p) {
  static uint8_t packed[SMCPAD_FLASH_SECTOR];
  uint32_t len = patternPack(p, packed);
  return len && appendRecord(slot, packed, len);
}

static bool storeHas(uint8_t slot) { return slotAddr[slot] != 0; }

static bool storeLoad(uint8_t slot, Pattern &p) {
  RecordHeader h;
  if (!slotAddr[slot] || !recordValid(slotAddr[slot], h, true)) {
    patternClear(p);
    return false;
  }
  return patternUnpack(p, storeBuf, h.length);
}
