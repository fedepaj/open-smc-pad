// The pattern data: 16 tracks (one per pad) of up to 64 steps (docs/sequencer-design.md).
#pragma once
#include <Arduino.h>

static const int TRACKS = 16;
static const int MAX_STEPS = 64;
static const int PATTERNS = 16;
static const int LOCKS = 4;                 // p-lockable CCs per track
static const uint8_t NO_LOCK = 0xFF;

// step condition: 0 always; 1..99 probability %; then A:B conditions, fill, not fill
enum {
  COND_ALWAYS = 0,
  COND_PROB_MAX = 99,
  COND_1_2 = 100, COND_2_2, COND_1_3, COND_2_3, COND_3_3, COND_1_4, COND_2_4, COND_3_4, COND_4_4,
  COND_FILL, COND_NOT_FILL,
  COND_LAST = COND_NOT_FILL
};

enum { STEP_ON = 1 };

struct Step {
  uint8_t flags;              // STEP_ON
  uint8_t velocity;           // 1..127
  int8_t micro;               // -11..+11, in 1/24 of a step
  uint8_t cond;               // COND_*
  uint8_t ratchet;            // 1..4 hits in the step
  uint8_t lock[LOCKS];        // CC value for this step, or NO_LOCK
};

enum { SPEED_HALF = 0, SPEED_NORMAL, SPEED_DOUBLE };

struct Track {
  uint8_t channel;            // 0..15
  uint8_t note;
  uint8_t velocity;           // for live-recorded steps without velocity, and new steps
  uint8_t length;             // 1..64 steps
  uint8_t speed;              // SPEED_*
  uint8_t gate;               // note length, % of a step (10..100), 100+ = tie into the next
  uint8_t lockCC[LOCKS];      // the CCs p-locks set
  uint8_t lockBase[LOCKS];    // their value outside locked steps (set by the knobs)
  bool muted;
};

struct Pattern {
  uint8_t length;             // steps before a queued pattern takes over: 16, 32, 48, 64
  uint8_t swing;              // 0..4: odd steps later by 0, 8, 16, 24, 32 % of a step
  Track track[TRACKS];
  Step step[TRACKS][MAX_STEPS];
};

static void stepClear(Step &s) {
  s = Step{0, 100, 0, COND_ALWAYS, 1, {NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK}};
}

static bool stepHasLocks(const Step &s) {
  for (int i = 0; i < LOCKS; i++) if (s.lock[i] != NO_LOCK) return true;
  return false;
}

static bool stepIsDefault(const Step &s) {
  return !s.flags && s.velocity == 100 && !s.micro && s.cond == COND_ALWAYS && s.ratchet == 1 &&
         !stepHasLocks(s);
}

// a fresh pattern: a drum kit on channel 10, notes 36..51 (GM / MPC layout), 16 steps
static void patternClear(Pattern &p) {
  p.length = 16;
  p.swing = 0;
  for (int t = 0; t < TRACKS; t++) {
    p.track[t] = Track{9, (uint8_t)(36 + t), 100, 16, SPEED_NORMAL, 50, {70, 71, 72, 73},
                       {64, 64, 64, 64}, false};
    for (int s = 0; s < MAX_STEPS; s++) stepClear(p.step[t][s]);
  }
}

static void trackClearSteps(Pattern &p, int t) {
  for (int s = 0; s < MAX_STEPS; s++) stepClear(p.step[t][s]);
}

// the colour of each track (pad)
static const uint32_t trackColor[TRACKS] = {
  0xFF2000, 0xFF8000, 0xFFD000, 0x80FF00, 0x00FF40, 0x00FFC0, 0x00C0FF, 0x0050FF,
  0x4000FF, 0xA000FF, 0xFF00C0, 0xFF0050, 0xFF6060, 0x60FF90, 0x60A0FF, 0xE0E0E0,
};
