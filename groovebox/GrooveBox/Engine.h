// The sequencer: a 96 PPQN clock (internal, or locked to an incoming 24 PPQN MIDI clock), the
// steps of every track scheduled one step ahead into an event queue (micro-timing, swing,
// ratchets, conditions, p-locks), pattern switching at the end of the pattern, chains.
#pragma once
#include "Model.h"
#include "Midi.h"
#include "Store.h"

static const uint32_t PPQN = 96;
static const uint32_t TICKS_PER_STEP = 24;          // a 1/16 at normal speed

static Pattern pattern;                             // playing (and edited)
static Pattern queued;                              // loaded, waiting for the end of pattern
static int8_t currentSlot = 0, queuedSlot = -1;
static int8_t chain[PATTERNS], chainLength, chainPos;
static bool patternDirty;

static bool playing;
static uint32_t tick;                               // since start
static uint32_t patternStartTick;
static uint32_t nextTickUs;
static uint16_t bpm = 120;
static bool fillHeld;

// external clock
static bool syncExternal;                           // follow 0xF8 instead of the own tempo
static uint32_t extClocks, extLastUs, extIntervalUs = 20833;
static bool extAlive() { return syncExternal && micros() - extLastUs < 500000; }

// ---------------------------------------------------------------- the event queue

enum { EV_FREE = 0, EV_NOTE_ON, EV_NOTE_OFF, EV_CC };
struct Event { uint32_t at; uint8_t type, status, d1, d2, track; };
static void onTrackHit(int t);                      // the UI: flash the pad
static const int QUEUE = 96;
static Event events[QUEUE];

static void schedule(uint32_t at, uint8_t type, uint8_t status, uint8_t d1, uint8_t d2,
                     uint8_t track = 0xFF) {
  for (int i = 0; i < QUEUE; i++) {
    if (events[i].type == EV_FREE) {
      events[i] = Event{at, type, status, d1, d2, track};
      return;
    }
  }
  if (type == EV_NOTE_OFF) midiSend(status, d1, d2);   // full: never leave a note hanging
}

static void runEvents() {
  // note offs first, so a ratchet's next hit is not cut by its own previous note off
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < QUEUE; i++) {
      Event &e = events[i];
      if (e.type == EV_FREE || (int32_t)(tick - e.at) < 0) continue;
      if ((pass == 0) != (e.type == EV_NOTE_OFF)) continue;
      midiSend(e.status, e.d1, e.d2);
      if (e.type == EV_NOTE_ON && e.track < TRACKS) onTrackHit(e.track);
      e.type = EV_FREE;
    }
  }
}

static void flushEvents() {                         // stop: offs go out now, the rest is dropped
  for (int i = 0; i < QUEUE; i++) {
    if (events[i].type == EV_NOTE_OFF || events[i].type == EV_CC) midiSend(events[i].status, events[i].d1, events[i].d2);
    events[i].type = EV_FREE;
  }
}

// ---------------------------------------------------------------- steps

static uint32_t trackTicksPerStep(const Track &k) {
  return k.speed == SPEED_HALF ? 48 : k.speed == SPEED_DOUBLE ? 12 : 24;
}

static bool conditionPasses(uint8_t cond, uint32_t loop) {
  if (cond == COND_ALWAYS) return true;
  if (cond <= COND_PROB_MAX) return (uint32_t)random(100) < cond;
  switch (cond) {
    case COND_1_2: return loop % 2 == 0;
    case COND_2_2: return loop % 2 == 1;
    case COND_1_3: return loop % 3 == 0;
    case COND_2_3: return loop % 3 == 1;
    case COND_3_3: return loop % 3 == 2;
    case COND_1_4: return loop % 4 == 0;
    case COND_2_4: return loop % 4 == 1;
    case COND_3_4: return loop % 4 == 2;
    case COND_4_4: return loop % 4 == 3;
    case COND_FILL: return fillHeld;
    case COND_NOT_FILL: return !fillHeld;
  }
  return true;
}

static int8_t skipStep[TRACKS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static uint8_t lastPlayedStep[TRACKS];              // for the playhead

// the step that starts at stepCount (counted from the pattern start), scheduled from `start`
static void scheduleStep(int t, uint32_t stepCount, uint32_t start) {
  Track &k = pattern.track[t];
  uint32_t tps = trackTicksPerStep(k);
  int index = stepCount % k.length;
  Step &s = pattern.step[t][index];
  if (skipStep[t] == index) {                       // just recorded live: already heard
    skipStep[t] = -1;
    return;
  }
  if (!(s.flags & STEP_ON) || k.muted) return;
  if (!conditionPasses(s.cond, stepCount / k.length)) return;
  int32_t at = (int32_t)start + s.micro * (int32_t)tps / 24;
  if (stepCount & 1) at += tps * pattern.swing * 8 / 100;
  if (at < (int32_t)tick) at = tick;
  uint8_t ch = k.channel;
  uint32_t hitTicks = max(tps / s.ratchet, 1u);
  uint32_t gate = max(hitTicks * min(k.gate, (uint8_t)100) / 100, 1u);
  bool locked = false;
  for (int i = 0; i < LOCKS; i++) {
    if (s.lock[i] != NO_LOCK) {
      schedule(at, EV_CC, 0xB0 | ch, k.lockCC[i], s.lock[i]);
      schedule(at + tps - 1, EV_CC, 0xB0 | ch, k.lockCC[i], k.lockBase[i]);
      locked = true;
    }
  }
  (void)locked;
  for (uint32_t r = 0; r < s.ratchet; r++) {
    uint32_t hit = at + r * hitTicks;
    schedule(hit, EV_NOTE_ON, 0x90 | ch, k.note, s.velocity, t);
    schedule(hit + gate, EV_NOTE_OFF, 0x80 | ch, k.note, 0);
  }
}

// ---------------------------------------------------------------- patterns

static void patternSave() {
  if (patternDirty && storeSave(currentSlot, pattern)) patternDirty = false;
}

static void queuePattern(int slot) {
  if (slot == currentSlot && queuedSlot < 0) return;
  storeLoad(slot, queued);
  queuedSlot = slot;
  if (!playing) {                                   // stopped: switch now
    patternSave();
    pattern = queued;
    currentSlot = slot;
    queuedSlot = -1;
  }
}

static void setChain(const int8_t *slots, int n) {
  memcpy(chain, slots, n);
  chainLength = n;
  chainPos = 0;
  if (n) queuePattern(chain[0]);
}

// a pattern switch: what the old pattern scheduled ahead does not play
static void dropFutureEvents() {
  for (int i = 0; i < QUEUE; i++) {
    if (events[i].type != EV_NOTE_OFF && (int32_t)(events[i].at - tick) >= 0) events[i].type = EV_FREE;
  }
}

// at the end of the pattern: take the queued one (or the next of the chain); true if switched
static bool patternBoundary() {
  if (queuedSlot < 0 && chainLength > 1) {
    chainPos = (chainPos + 1) % chainLength;
    if (chain[chainPos] != currentSlot) {
      storeLoad(chain[chainPos], queued);
      queuedSlot = chain[chainPos];
    }
  }
  if (queuedSlot >= 0) {
    patternSave();                                  // a few ms of flash: at the bar line
    pattern = queued;
    currentSlot = queuedSlot;
    queuedSlot = -1;
    patternStartTick = tick;
    dropFutureEvents();
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- the clock

static void noteRepeatClock();                     // the UI's note repeat, 24 PPQN

static void onTick() {
  uint32_t rel = tick - patternStartTick;
  if (rel && rel % (TICKS_PER_STEP * pattern.length) == 0 && patternBoundary()) rel = 0;
  if (tick % 4 == 0) {
    if (!extAlive()) midiSend(0xF8);                // clock out, 24 PPQN
    noteRepeatClock();
  }
  for (int t = 0; t < TRACKS; t++) {
    uint32_t tps = trackTicksPerStep(pattern.track[t]);
    if (rel % tps) continue;
    uint32_t stepCount = rel / tps;
    lastPlayedStep[t] = stepCount % pattern.track[t].length;
    // one step ahead (micro-timing can be early); at the pattern start, step 0 now too
    if (rel == 0) scheduleStep(t, 0, tick);
    scheduleStep(t, stepCount + 1, tick + tps);
  }
  runEvents();
  tick++;
}

static void transportStart() {
  if (playing) return;
  playing = true;
  tick = patternStartTick = 0;
  nextTickUs = micros();
  if (!extAlive()) midiSend(0xFA);
}

static void transportStop() {
  if (!playing) return;
  playing = false;
  flushEvents();
  if (!extAlive()) midiSend(0xFC);
  for (int t = 0; t < TRACKS; t++) skipStep[t] = -1;
  patternSave();
}

static uint32_t tickIntervalUs() {
  return extAlive() ? extIntervalUs / 4 : 625000UL / bpm;
}

static void engineRun() {
  uint32_t now = micros();
  if (!playing) {
    // master clock keeps running while stopped, so a synced DAW knows the tempo
    static uint32_t nextClockUs;
    if (!extAlive() && (int32_t)(now - nextClockUs) >= 0) {
      midiSend(0xF8);
      nextClockUs = now + 4 * (625000UL / bpm);
    }
    return;
  }
  if (extAlive()) {
    // interpolate between incoming clocks, never more than 3 ticks past the last one
    uint32_t limit = extClocks * 4 + 3;
    while ((int32_t)(now - nextTickUs) >= 0 && tick < limit) {
      onTick();
      nextTickUs += extIntervalUs / 4;
    }
    return;
  }
  if ((int32_t)(now - nextTickUs) > 200000) nextTickUs = now;    // after a long stall
  while ((int32_t)(now - nextTickUs) >= 0) {
    onTick();
    nextTickUs += tickIntervalUs();
  }
}

// incoming realtime messages (from midiIn)
static void engineClockIn() {
  if (!syncExternal) return;
  uint32_t now = micros(), dt = now - extLastUs;
  if (dt < 200000) extIntervalUs = (extIntervalUs * 3 + dt) / 4;
  extLastUs = now;
  if (!playing) return;
  extClocks++;
  while (tick < extClocks * 4) onTick();            // catch up to the clock
  nextTickUs = now + extIntervalUs / 4;
}

static void engineStartIn(bool fromZero) {
  if (!syncExternal) return;
  if (fromZero) {
    flushEvents();
    tick = patternStartTick = 0;
  }
  extClocks = tick / 4;                             // continue: carry on from where we are
  playing = true;
  nextTickUs = micros();
}

static void engineStopIn() {
  if (!syncExternal) return;
  transportStop();
}

// ---------------------------------------------------------------- live recording

// a pad played while recording: the nearest step of its track, micro-timing kept
static void recordHit(int t, uint8_t velocity) {
  Track &k = pattern.track[t];
  uint32_t tps = trackTicksPerStep(k);
  uint32_t rel = tick - patternStartTick;
  uint32_t stepCount = rel / tps, frac = rel % tps;
  int32_t micro = frac * 24 / tps;
  if (frac > tps / 2) {
    stepCount++;
    micro -= 24;
    skipStep[t] = stepCount % k.length;             // the sequencer would play it again
  }
  Step &s = pattern.step[t][stepCount % k.length];
  s.flags |= STEP_ON;
  s.velocity = velocity;
  s.micro = constrain(micro, -11, 11);
  patternDirty = true;
}

// the step a track is on now (playhead), or 0 when stopped
static int playheadStep(int t) {
  return playing ? lastPlayedStep[t] : 0;
}
