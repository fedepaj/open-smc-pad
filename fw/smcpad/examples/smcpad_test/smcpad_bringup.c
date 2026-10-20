/*
 * SMC-PAD bring-up console, over the USB CDC serial (fw/smcpad/smcpad_dbg.c).
 *
 * The firmware boots fine now, but the button LEDs never react to anything we drive, and the
 * wiring we inferred from the stock firmware is only a guess: the 595 pins (PB6 clock, PB7
 * data, PB4 latch, PB5 /OE), the seven GPIO "rows" and the LED polarity are all unconfirmed.
 * Worse, a wrong *latch* pin looks exactly like a dead output stage -- you can shift data all
 * day and the 595 outputs never move.  Rather than one flash per hypothesis, this is a shell:
 * type a command, look at the board, type the next one.
 *
 *   ?                      list the commands
 *   m                      show the current pin assignment
 *   o <pin> / l <pin>      drive a pin high / low          e.g. "o b4"
 *   z <pin>                release a pin (hi-Z)
 *   r <pin>                read a pin's level
 *   w <hex>                shift 16 bits into the 595s and latch   e.g. "w ff00"
 *   c <clk> <dat> <lat>    reassign the 595 pins           e.g. "c b6 b7 b4"
 *   e <pin>                reassign the /OE pin
 *   s                      scan: drive every pin high, then low, one at a time
 *   p                      permutations: every clk/dat/lat triple out of PB4..PB7
 *   t                      run the LED matrix sweep once
 *   x                      release every pin
 *
 * Pins are written as <group><number>: a0..a8, b0..b8, c0..c5.  D+/D- are deliberately not
 * reachable -- they carry this console.  "s" is the safest way to find anything that has a
 * visible effect (one pin at a time, so no LED string can be over-driven); if a pin wedges
 * the chip, the port disappears from the host and the last line printed names the culprit.
 */
#include "system/includes.h"
#include "asm/gpio.h"
#include "asm/hwi.h"                     /* bit_clr_ie, IRQ_USB_*_IDX */
#include "asm/usb.h"                     /* USB_IO_CON0 bits */
#include "update.h"
#include "smcpad_usb_cfg.h"               /* SMCPAD_APP_DIR (fw/build.sh) */

extern void smcpad_trace(const char *tag);
extern int smcpad_dbg_ready(void);
extern void smcpad_dbg_puts(const char *s);
extern void smcpad_dbg_kv(const char *tag, u32 value);
extern void smcpad_dbg_hex16(const char *tag, u32 value);
extern u32 smcpad_dbg_read(char *buf, u32 len);
extern void clr_wdt(void);

/* The clock is whatever the boot left us; this is a factor-of-two estimate, which is all the
 * eye needs, and it never depends on the OS tick. */
#define LOOPS_PER_MS    4000u

static u32 sr_clk = IO_PORTB_06;
static u32 sr_dat = IO_PORTB_07;
static u32 sr_lat = IO_PORTB_04;
static u32 sr_oe  = IO_PORTB_05;

#define NUM_COLS    16

/*
 * Button LEDs, read out of the stock firmware's ISR at 0x1E23186 (see docs/hardware.md).
 * They have nothing to do with the 595s: it is a 3x4 multiplex of its own.
 *
 *   3 phases   PA3, PC2, PC3   high-side commons, active LOW (a PNP/P-FET feeds the rail),
 *                              exactly one low at a time.  The ISR writes phase 0 to PORTA
 *                              and phases 1/2 to `PORTA + 0x80`, which is PORTC: the port
 *                              blocks are 0x40 apart (PORTB is +0x40), so +0x80 is the third
 *                              one.  Reading that as PB2/PB3 lights nothing -- the segments
 *                              sink, but no rail is fed, so the LED just drains the local
 *                              capacitance and fades out.
 *   4 segments PA5, PA2, PA0, PA1   sinks, active LOW -- the 0x27 in `PORTA.OUT |= 0x27`
 *   led_map    flash 0x1E1659A, one byte per LED: (phase << 4) | segment bit
 *
 * So a LED is lit when its phase is low *and* its segment is low. Phases must be cycled to
 * show more than four at once, which is what btnled_multiplex() does.
 */
static const u8 led_phase_pins[3] = { IO_PORTA_03, IO_PORTC_02, IO_PORTC_03 };
static const u8 led_seg_pins[4] = { IO_PORTA_05, IO_PORTA_02, IO_PORTA_00, IO_PORTA_01 };
static const u8 led_map[10] = {
    0x10, 0x03, 0x12, 0x01, 0x11, 0x22, 0x21, 0x20, 0x02, 0x00,
};
#define NUM_LEDS    ARRAY_SIZE(led_map)

/* Which LED is which, read off the case (docs/hardware.md). Left to right the panel order is
 * bluetooth, pad bank, knob bank, left, right, play, pause, rec, shift, note repeat. */
enum {
    LED_LEFT = 0, LED_RIGHT, LED_PLAY, LED_PAUSE, LED_REC,
    LED_BT, LED_PAD_BANK, LED_KNOB_BANK, LED_SHIFT, LED_NOTE_REPEAT,
};

static const char *const led_names[NUM_LEDS] = {
    "left arrow", "right arrow", "play", "pause", "rec",
    "bluetooth", "pad bank", "knob bank", "shift", "note repeat",
};

/* Panel order, for sweeps that should look like they travel across the board. */
static const u8 led_panel_order[NUM_LEDS] = {
    LED_BT, LED_PAD_BANK, LED_KNOB_BANK, LED_LEFT, LED_RIGHT,
    LED_PLAY, LED_PAUSE, LED_REC, LED_SHIFT, LED_NOTE_REPEAT,
};

/*
 * Key matrix (stock firmware ISR 1e22fb6): the same two 595s drive 16 one-hot-LOW columns
 * (pattern table at flash 0x1E16914 = 0xFFFE, 0xFFFD, ... 0x7FFF) and two sense lines are
 * read back, PB0 and PC1, with diodes for anti-ghosting. 16 columns x 2 rows = 32 positions,
 * which is where the 8 encoders (2 contacts each) and the 10 buttons live.
 */
#define SENSE_0     IO_PORTB_00
#define SENSE_1     IO_PORTC_01

/*
 * Buttons sit on sense 1, one per column, columns 0..9 (measured; see docs/hardware.md).
 * Column order is not the panel order.
 */
static const char *const key_names[10] = {
    "shift", "note repeat", "bluetooth", "pad bank", "knob bank",
    "left arrow", "right arrow", "play", "rec", "pause",
};

/*
 * The eight encoders are on sense 0, each as a pair of adjacent columns -- 8 pairs x 2 = the
 * 16 sense-0 positions. They are incremental quadrature encoders with no detent (which is why
 * they feel like they have no clicks); the stock firmware decodes them the same way, in the
 * ISR at 1e23076, from pairs of matrix bits.
 */
#define NUM_ENCODERS    8

/* Quadrature: index by (previous << 2) | current, both as 2-bit (A,B) codes. The invalid
 * double transitions give 0, so noise and missed samples cost a step instead of a direction. */
static const signed char quad_step[16] = {
    0, +1, -1,  0,
   -1,  0,  0, +1,
   +1,  0,  0, -1,
    0, -1, +1,  0,
};

/* Every pin the console may touch: the chip's three GPIO groups as this package exposes them. */
static const u8 all_pins[] = {
    IO_PORTA_00, IO_PORTA_01, IO_PORTA_02, IO_PORTA_03, IO_PORTA_04,
    IO_PORTA_05, IO_PORTA_06, IO_PORTA_07, IO_PORTA_08,
    IO_PORTB_00, IO_PORTB_01, IO_PORTB_02, IO_PORTB_03, IO_PORTB_04,
    IO_PORTB_05, IO_PORTB_06, IO_PORTB_07, IO_PORTB_08,
    IO_PORTC_00, IO_PORTC_01, IO_PORTC_02, IO_PORTC_03, IO_PORTC_04, IO_PORTC_05,
};

void smcpad_delay_ms(u32 ms)
{
    while (ms--) {
        clr_wdt();
        for (volatile u32 i = 0; i < LOOPS_PER_MS; i++) {
            /* busy */
        }
    }
}

static void pin_output(u32 gpio, int value)
{
    gpio_set_pull_up(gpio, 0);
    gpio_set_pull_down(gpio, 0);
    gpio_direction_output(gpio, value);
}

static void pin_hiz(u32 gpio)
{
    gpio_set_pull_up(gpio, 0);
    gpio_set_pull_down(gpio, 0);
    gpio_set_direction(gpio, 1);
}

/* Input with the pull-up on: anything we probe by hand here is a switch or an open-drain
 * line, and a floating input just reports noise. */
static int pin_read(u32 gpio)
{
    gpio_set_pull_down(gpio, 0);
    gpio_set_pull_up(gpio, 1);
    gpio_set_direction(gpio, 1);
    gpio_set_die(gpio, 1);
    smcpad_delay_ms(1);
    return gpio_read(gpio);
}

/* ------------------------------------------------------------------ button LEDs */

/* Everything off: commons disabled, no sink anywhere. */
static void btnled_off(void)
{
    for (u32 i = 0; i < 3; i++) {
        pin_output(led_phase_pins[i], 1);
    }
    for (u32 i = 0; i < 4; i++) {
        pin_hiz(led_seg_pins[i]);
    }
}

/* Drive one phase and the segments it should light (bit per segment, 0..3).
 *
 * The inactive phases are driven high, which is what turns their high-side transistor off;
 * leaving them floating would let it conduct.  The inactive segments, though, go hi-Z rather
 * than high: driven high they push current into the LEDs of the *other* phases through
 * whatever parasitic path exists, which lights unrelated LEDs faintly (ghosting).  The stock
 * firmware drives them high (`PORTA.OUT |= 0x27`) and gets away with it because it re-arms a
 * different phase on every call; holding one phase steady, as `d <n>` does, does not.
 */
static void btnled_phase(u32 phase, u8 segments)
{
    for (u32 i = 0; i < 4; i++) {
        pin_hiz(led_seg_pins[i]);                   /* drop all sinks before switching phase */
    }
    for (u32 i = 0; i < 3; i++) {
        pin_output(led_phase_pins[i], i != phase);
    }
    for (u32 i = 0; i < 4; i++) {
        if (segments & (1u << i)) {
            pin_output(led_seg_pins[i], 0);
        }
    }
}

/* Light exactly one LED, statically: no multiplexing needed for a single phase. */
static void btnled_one(u32 led)
{
    u8 entry = led_map[led];

    btnled_phase(entry >> 4, (u8)(1u << (entry & 0x0F)));
}

/* Show an arbitrary set of LEDs by cycling the phases for `ms` milliseconds. */
static void btnled_multiplex(u16 leds, u32 ms)
{
    u8 per_phase[3] = { 0, 0, 0 };

    for (u32 i = 0; i < NUM_LEDS; i++) {
        if (leds & (1u << i)) {
            per_phase[led_map[i] >> 4] |= (u8)(1u << (led_map[i] & 0x0F));
        }
    }

    while (ms) {
        for (u32 phase = 0; phase < 3 && ms; phase++) {
            btnled_phase(phase, per_phase[phase]);
            smcpad_delay_ms(2);
            ms = (ms > 2) ? ms - 2 : 0;
        }
    }
    btnled_off();
}

static void sr_pins_init(void)
{
    pin_output(sr_clk, 0);
    pin_output(sr_dat, 0);
    pin_output(sr_lat, 0);
    pin_output(sr_oe, 0);
}

static void sr_write(u16 value)
{
    for (int bit = 15; bit >= 0; bit--) {
        gpio_write(sr_dat, (value >> bit) & 1);
        gpio_write(sr_clk, 0);
        gpio_write(sr_clk, 1);
    }
    gpio_write(sr_clk, 0);
    gpio_write(sr_lat, 1);                  /* latch: shift register -> output register */
    gpio_write(sr_lat, 0);
}

/* ------------------------------------------------------------------ key matrix */

static void keys_init(void)
{
    sr_pins_init();
    for (u32 i = 0; i < 2; i++) {
        u32 pin = i ? SENSE_1 : SENSE_0;

        gpio_set_direction(pin, 1);
        gpio_set_die(pin, 1);
        gpio_set_pull_down(pin, 0);
        gpio_set_pull_up(pin, 1);           /* idle high; a selected column pulls it low */
    }
}

/* ~250us, enough for the pull-up to win against the line capacitance through the diodes,
 * and short enough that a full 16-column pass takes about 4ms: the encoders need the scan
 * to be faster than they can be turned by hand.
 *
 * It kicks the watchdog, like smcpad_delay_ms() does. Everything here busy-waits, so any
 * loop that stops passing through a delay stops feeding the watchdog and the chip resets a
 * moment later -- which is exactly what happened when this replaced the 1ms delay. */
static void keys_settle(void)
{
    clr_wdt();
    for (volatile u32 i = 0; i < LOOPS_PER_MS / 4; i++) {
        /* busy */
    }
}

/* One full pass: returns a bit per position, bit (col * 2 + sense) set when pressed. */
static u32 keys_scan(void)
{
    u32 state = 0;

    for (u32 col = 0; col < NUM_COLS; col++) {
        sr_write((u16)~(1u << col));
        keys_settle();
        if (!gpio_read(SENSE_0)) {
            state |= 1u << (col * 2);
        }
        if (!gpio_read(SENSE_1)) {
            state |= 1u << (col * 2 + 1);
        }
    }
    sr_write(0xFFFF);                       /* no column selected */
    return state;
}

/*
 * The matrix is never empty at rest: the encoders are quadrature contacts, so whichever
 * detent they are parked in leaves some of them closed. Everything below therefore works
 * against a baseline taken while nothing is touched, and reports only what is *added* to it.
 * (Measuring "any closed contact" instead is what made the first mapping run return the same
 * eight positions for all ten buttons.)
 */
static u32 keys_baseline(void)
{
    u32 baseline = keys_scan();

    for (u32 i = 0; i < 4; i++) {
        baseline |= keys_scan();            /* union, so a bouncing contact counts as resting */
    }
    return baseline;
}

/* What changed against the baseline.  Buttons are normally open, so for those we only care
 * about contacts that closed (`added`); an encoder's quadrature contacts are half closed at
 * any detent, so turning one also *opens* contacts, and those must count too (`both_ways`). */
static u32 keys_delta(u32 baseline, int both_ways)
{
    u32 now = keys_scan();

    return both_ways ? (now ^ baseline) : (now & ~baseline);
}

/* Wait for a stable change against the baseline. Returns it, or 0 on timeout. */
static u32 keys_wait_press(u32 baseline, u32 timeout_ms)
{
    u32 previous = 0;

    while (timeout_ms) {
        u32 now = keys_delta(baseline, 0);

        if (now && now == previous) {               /* same twice: past the contact bounce */
            return now;
        }
        previous = now;
        if (interrupted()) {
            return 0;
        }
        timeout_ms = (timeout_ms > 20) ? timeout_ms - 20 : 0;
    }
    return 0;
}

/*
 * Same, but blinking `led` so the user knows which button to press.
 *
 * The LEDs and the matrix do share a port: phase 2 is PC3 and the button sense is PC1, and
 * holding PC3 low drags PC1 low on every column — which is exactly why the three phase-2
 * buttons (bluetooth, pad bank, knob bank) first came back as "all columns at once".  So the
 * LED is only ever lit *between* scans, never during one.
 */
static u32 keys_wait_press_blink(u32 led, u32 baseline, u32 timeout_ms)
{
    u32 previous = 0;

    while (timeout_ms) {
        u32 now;

        btnled_one(led);
        smcpad_delay_ms(120);
        btnled_off();
        smcpad_delay_ms(3);                         /* let the rail settle before sampling */

        now = keys_delta(baseline, 0);
        if (now && now == previous) {
            return now;
        }
        previous = now;
        if (interrupted()) {
            return 0;
        }
        timeout_ms = (timeout_ms > 160) ? timeout_ms - 160 : 0;
    }
    return 0;
}

/* For encoders: wait for any contact to change state, in either direction. */
static u32 keys_wait_change(u32 baseline, u32 timeout_ms)
{
    while (timeout_ms) {
        u32 now = keys_delta(baseline, 1);

        if (now) {
            return now;
        }
        if (interrupted()) {
            return 0;
        }
        timeout_ms = (timeout_ms > 20) ? timeout_ms - 20 : 0;
    }
    return 0;
}

static void keys_wait_release(u32 baseline, u32 timeout_ms)
{
    while (timeout_ms) {
        if (!(keys_scan() & ~baseline) && !(keys_scan() & ~baseline)) {
            return;
        }
        timeout_ms = (timeout_ms > 20) ? timeout_ms - 20 : 0;
    }
}

/* Everything seen beyond the baseline during `ms`: encoders flicker through several
 * positions as they turn, and we want all of them. */
static u32 keys_collect(u32 baseline, u32 ms, int both_ways)
{
    u32 seen = 0;

    while (ms) {
        seen |= keys_delta(baseline, both_ways);
        ms = (ms > 20) ? ms - 20 : 0;
    }
    return seen;
}

/* Live decode of all eight encoders. Returns through the callback-less route: prints. */
static void cmd_encoders(void)
{
    u8 previous[NUM_ENCODERS];
    int count[NUM_ENCODERS];
    u32 state;

    smcpad_dbg_puts("encoder decode: turn any encoder.\r\n"
                    "pair N = columns 2N and 2N+1 on sense 0 (any key stops)\r\n");
    btnled_off();
    keys_init();

    state = keys_scan();
    for (u32 e = 0; e < NUM_ENCODERS; e++) {
        u32 a = (state >> ((2 * e) * 2)) & 1;           /* column 2e,   sense 0 */
        u32 b = (state >> ((2 * e + 1) * 2)) & 1;       /* column 2e+1, sense 0 */

        previous[e] = (u8)((a << 1) | b);
        count[e] = 0;
    }

    for (;;) {
        state = keys_scan();

        for (u32 e = 0; e < NUM_ENCODERS; e++) {
            u32 a = (state >> ((2 * e) * 2)) & 1;
            u32 b = (state >> ((2 * e + 1) * 2)) & 1;
            u8 now = (u8)((a << 1) | b);
            int step;

            if (now == previous[e]) {
                continue;
            }
            step = quad_step[(previous[e] << 2) | now];
            previous[e] = now;
            if (!step) {
                continue;                               /* skipped a sample, no direction */
            }
            count[e] += step;
            smcpad_dbg_puts(step > 0 ? "  pair" : "  pair");
            smcpad_dbg_kv(step > 0 ? " turned +1, index" : " turned -1, index", e);
            smcpad_dbg_kv("    steps so far", (u32)(count[e] < 0 ? -count[e] : count[e]));
        }

        clr_wdt();
        if (interrupted()) {
            break;
        }
    }

    sr_write(0xFFFF);
    smcpad_dbg_puts("encoder decode stopped\r\n");
}

/* ------------------------------------------------------------------ text helpers */

static void pin_name(u32 pin, char *out)
{
    static const char groups[] = "abc";
    u32 group = pin / 16, index = pin % 16;

    out[0] = (group < 3) ? groups[group] : '?';
    if (index >= 10) {
        out[1] = '1';
        out[2] = '0' + (index - 10);
        out[3] = 0;
    } else {
        out[1] = '0' + index;
        out[2] = 0;
    }
}

static void say_pin(const char *what, u32 pin)
{
    char line[24], name[4];
    u32 i = 0;

    pin_name(pin, name);
    while (*what) {
        line[i++] = *what++;
    }
    line[i++] = ' ';
    for (u32 j = 0; name[j]; j++) {
        line[i++] = name[j];
    }
    line[i++] = '\r';
    line[i++] = '\n';
    line[i] = 0;
    smcpad_dbg_puts(line);
}

/* "  play            col 5 sense 0  col 5 sense 1" — one line per mapped control, so the
 * saved log is already the table. */
static void say_positions(const char *label, u32 mask)
{
    char line[96];
    u32 i = 0;

    while (*label && i < 20) {
        line[i++] = *label++;
    }
    while (i < 20) {
        line[i++] = ' ';
    }

    if (!mask) {
        const char *none = "(nothing)";

        while (*none) {
            line[i++] = *none++;
        }
    }
    for (u32 pos = 0; pos < NUM_COLS * 2 && i < sizeof(line) - 20; pos++) {
        u32 col = pos / 2;

        if (!(mask & (1u << pos))) {
            continue;
        }
        line[i++] = ' ';
        line[i++] = 'c';
        if (col >= 10) {
            line[i++] = '1';
            line[i++] = (char)('0' + (col - 10));
        } else {
            line[i++] = (char)('0' + col);
        }
        line[i++] = '/';
        line[i++] = 's';
        line[i++] = (char)('0' + (pos % 2));
    }
    line[i++] = '\r';
    line[i++] = '\n';
    line[i] = 0;
    smcpad_dbg_puts(line);
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* "b4" / "c10" -> pin number; returns NULL on a bad pin. */
static const char *parse_pin(const char *s, u32 *pin)
{
    u32 group, index = 0;
    int digits = 0;

    s = skip_spaces(s);
    switch (*s) {
    case 'a':
        group = 0;
        break;
    case 'b':
        group = 1;
        break;
    case 'c':
        group = 2;
        break;
    default:
        return NULL;
    }
    s++;
    while (*s >= '0' && *s <= '9') {
        index = index * 10 + (u32)(*s++ - '0');
        digits++;
    }
    if (!digits || index > 15) {
        return NULL;
    }
    *pin = group * 16 + index;
    return s;
}

static const char *parse_hex(const char *s, u32 *value)
{
    u32 v = 0;
    int digits = 0;

    s = skip_spaces(s);
    for (;;) {
        char c = *s;
        u32 d;

        if (c >= '0' && c <= '9') {
            d = (u32)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (u32)(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = (u32)(c - 'A') + 10;
        } else {
            break;
        }
        v = (v << 4) | d;
        digits++;
        s++;
    }
    if (!digits) {
        return NULL;
    }
    *value = v;
    return s;
}

/* True if the host sent anything: used to break out of the long running commands. */
static int interrupted(void)
{
    char scratch[8];

    return smcpad_dbg_read(scratch, sizeof(scratch)) != 0;
}

/* ------------------------------------------------------------------ commands */

static void cmd_help(void)
{
    smcpad_dbg_puts(
        "\r\ncommands (pins are a0..a8 b0..b8 c0..c5):\r\n"
        "  m                 show pin assignment\r\n"
        "  o <pin>           drive high        l <pin>  drive low\r\n"
        "  z <pin>           release (hi-Z)    r <pin>  read level\r\n"
        "  w <hex>           shift 16 bits into the 595s and latch\r\n"
        "  c <clk> <dat> <lat>   reassign 595 pins   e <pin>  reassign /OE\r\n"
        "  s                 scan every pin high then low\r\n"
        "  p                 try every clk/dat/lat triple out of b4..b7\r\n"
        "button LEDs (3x4 multiplex, from the stock firmware):\r\n"
        "  t                 sweep the ten LEDs one at a time\r\n"
        "  d <0..9>          light one LED and hold it\r\n"
        "  k                 light all ten (multiplexed) for 5s\r\n"
        "  f <0..2>          drive one phase low, all segments low\r\n"
        "  g <phase> <seg>   light one raw position (phase 0..2, segment 0..3)\r\n"
        "keys:\r\n"
        "  b                 watch the key matrix (16 columns x 2 sense lines)\r\n"
        "  M                 guided mapping: press each lit button, then turn each encoder\r\n"
        "  E                 decode the eight encoders live (direction + step count)\r\n"
        "  x                 release every pin / LEDs off\r\n"
        "flash / flashing without the Pico (experimental):\r\n"
        "  R <addr>          dump 16 bytes of flash (read-only, safe)\r\n"
        "  F <sector>        erase+write+verify one free sector (0x27000..0x74000)\r\n"
        "  U [hex]           reset into bootloader update mode (default 5a00 = USB)\r\n"
        "  J                 drop off USB and jump to the mask ROM (download mode if keyed)\r\n"
        "  B                 reset into our bootloader's DFU mode (1200-baud touch does the same)\r\n"
        "pads and pad LEDs:\r\n"
        "  A                 live ADC values of the 16 pad positions\r\n"
        "  W                 pad wizard: press pads 1..16, maps ADC position -> pad -> LED\r\n"
        "  n [<i> <rrggbb>]  pad RGB: chain LED i only (n a <rrggbb> = all, n = off)\r\n"
        "  N                 walk the RGB chain one LED at a time\r\n"
        "bluetooth:\r\n"
        "  Y                 start BLE-MIDI (advertises as SMC-PAD); again = status\r\n"
        "  K                 play the pads over BLE-MIDI (notes 36..51, LEDs follow)\r\n"
        "  X <mode> [n] [sector]  flash-death bisection: 0 irq-off spin, 1 +SFC off/on,\r\n"
        "                    2 +status polls, 3 +WREN/WRDI, 4 +real erase\r\n"
        "  ub/uw/uv/uc       firmware update (host/update.py drives these)\r\n"
        "  Z                 plain reset\r\n");
}

static void cmd_map(void)
{
    smcpad_dbg_puts("595 pins:\r\n");
    say_pin("  clock  ", sr_clk);
    say_pin("  data   ", sr_dat);
    say_pin("  latch  ", sr_lat);
    say_pin("  /OE    ", sr_oe);
    smcpad_dbg_puts("button LED phases (active low, one at a time):\r\n");
    for (u32 i = 0; i < 3; i++) {
        say_pin("  phase  ", led_phase_pins[i]);
    }
    smcpad_dbg_puts("button LED segments (active low):\r\n");
    for (u32 i = 0; i < 4; i++) {
        say_pin("  segment", led_seg_pins[i]);
    }
}

static void cmd_release_all(void)
{
    for (u32 i = 0; i < ARRAY_SIZE(all_pins); i++) {
        pin_hiz(all_pins[i]);
    }
    smcpad_dbg_puts("all pins released\r\n");
}

/* One pin at a time, so nothing can be over-driven whatever the LEDs turn out to be. */
static void cmd_scan(void)
{
    smcpad_dbg_puts("scan: each pin high 1.2s, then low 1.2s, then released.\r\n"
                    "watch the board and note which pin changes anything "
                    "(send any key to stop)\r\n");
    cmd_release_all();

    for (u32 i = 0; i < ARRAY_SIZE(all_pins); i++) {
        u32 pin = all_pins[i];

        say_pin("  HIGH", pin);
        pin_output(pin, 1);
        smcpad_delay_ms(1200);

        say_pin("  LOW ", pin);
        pin_output(pin, 0);
        smcpad_delay_ms(1200);

        pin_hiz(pin);
        if (interrupted()) {
            smcpad_dbg_puts("scan stopped\r\n");
            return;
        }
    }
    smcpad_dbg_puts("scan done\r\n");
}

/* A wrong latch pin is indistinguishable from dead outputs, so try all the assignments. */
static void cmd_permutations(void)
{
    static const u8 cand[] = { IO_PORTB_04, IO_PORTB_05, IO_PORTB_06, IO_PORTB_07 };
    u32 saved_clk = sr_clk, saved_dat = sr_dat, saved_lat = sr_lat, saved_oe = sr_oe;

    smcpad_dbg_puts("permutations of b4..b7 as clock/data/latch (4th pin held low as /OE);\r\n"
                    "each one blinks all 595 outputs 3x -- say which one reacts "
                    "(any key stops)\r\n");
    btnled_off();

    for (u32 a = 0; a < 4; a++) {
        for (u32 b = 0; b < 4; b++) {
            for (u32 c = 0; c < 4; c++) {
                u32 d;

                if (a == b || a == c || b == c) {
                    continue;
                }
                d = 0 + 1 + 2 + 3 - a - b - c;       /* the pin left over */

                sr_clk = cand[a];
                sr_dat = cand[b];
                sr_lat = cand[c];
                sr_oe  = cand[d];
                sr_pins_init();

                smcpad_dbg_puts("\r\n");
                say_pin("  clk", sr_clk);
                say_pin("  dat", sr_dat);
                say_pin("  lat", sr_lat);

                for (int rep = 0; rep < 3; rep++) {
                    sr_write(0x0000);
                    smcpad_delay_ms(350);
                    sr_write(0xFFFF);
                    smcpad_delay_ms(350);
                }

                if (interrupted()) {
                    smcpad_dbg_puts("stopped; pin assignment left at the last triple\r\n");
                    return;
                }
            }
        }
    }

    sr_clk = saved_clk;
    sr_dat = saved_dat;
    sr_lat = saved_lat;
    sr_oe = saved_oe;
    sr_pins_init();
    smcpad_dbg_puts("permutations done, pins restored\r\n");
}

/* Walk the ten button LEDs in panel order, left to right across the case. */
static void cmd_sweep(void)
{
    smcpad_dbg_puts("button LED sweep, left to right across the panel (any key stops)\r\n");

    for (u32 i = 0; i < NUM_LEDS; i++) {
        u32 led = led_panel_order[i];

        btnled_one(led);
        smcpad_dbg_puts("  ");
        smcpad_dbg_puts(led_names[led]);
        smcpad_dbg_kv(" = led", led);
        smcpad_delay_ms(700);
        if (interrupted()) {
            btnled_off();
            smcpad_dbg_puts("sweep stopped\r\n");
            return;
        }
    }

    btnled_off();
    smcpad_dbg_puts("sweep done; 'k' lights all ten at once\r\n");
}

/* Watch the key matrix. This is also the first real test of our bit-banged 595 writes: the
 * LEDs never needed them, so if nothing is ever detected here, the shift path is the suspect
 * (try 'p' to find the right clock/data/latch triple). */
static void cmd_keys(void)
{
    u32 baseline, last = 0xFFFFFFFF;

    smcpad_dbg_puts("key matrix monitor. don't touch anything for a moment...\r\n");
    btnled_off();
    keys_init();
    baseline = keys_baseline();
    say_positions("at rest", baseline);
    smcpad_dbg_puts("now press buttons and turn the encoders; each change prints what is "
                    "added to the resting state (any key stops)\r\n");

    for (;;) {
        u32 now = keys_delta(baseline, 1);          /* both directions: encoders open too */

        if (now != last) {
            last = now;
            say_positions(now ? "changed" : "--", now);
            /* name it when it is a button: sense 1, one column each, columns 0..9 */
            for (u32 pos = 1; pos < NUM_COLS * 2; pos += 2) {
                if ((now & (1u << pos)) && (pos / 2) < 10) {
                    smcpad_dbg_puts("    = ");
                    smcpad_dbg_puts(key_names[pos / 2]);
                    smcpad_dbg_puts("\r\n");
                }
            }
        }
        clr_wdt();
        if (interrupted()) {
            break;
        }
    }

    sr_write(0xFFFF);
    smcpad_dbg_puts("monitor stopped\r\n");
}

/*
 * Guided mapping. The buttons each carry their own LED, so instead of naming them we light
 * the one to press: press the lit button, it records the matrix position, moves on. Then the
 * eight encoders, which have no LED, by position on the panel. The summary at the end is the
 * table to paste into docs/hardware.md.
 *
 * LEDs and the key matrix share no pins (LEDs on PA3/PC2/PC3 + PA5/PA2/PA0/PA1, matrix on
 * PB6/PB7/PB4 + PB0/PC1), so a LED can stay lit while we scan.
 */
static void cmd_map_wizard(void)
{
    u32 button_pos[NUM_LEDS];
    u32 encoder_pos[8];
    u32 baseline;

    smcpad_dbg_puts("\r\n=== guided mapping ===\r\n"
                    "keep your hands off the pad for a moment while I read the resting "
                    "state...\r\n");
    keys_init();
    btnled_off();
    baseline = keys_baseline();
    say_positions("at rest", baseline);

    smcpad_dbg_puts("\r\npart 1: ten buttons. PRESS THE BUTTON THAT IS LIT, then release.\r\n"
                    "        send any character to skip the current one.\r\n");

    for (u32 i = 0; i < NUM_LEDS; i++) {
        u32 led = led_panel_order[i];

        button_pos[led] = 0;
        smcpad_dbg_puts("  press the blinking button: ");
        smcpad_dbg_puts(led_names[led]);
        smcpad_dbg_puts("\r\n");

        button_pos[led] = keys_wait_press_blink(led, baseline, 15000);
        say_positions(led_names[led], button_pos[led]);
        keys_wait_release(baseline, 3000);
        btnled_off();
        smcpad_delay_ms(150);
    }

    smcpad_dbg_puts("\r\npart 2: eight encoders, leftmost first.\r\n"
                    "        TURN the encoder a few steps; it records for 2.5s once it sees "
                    "movement.\r\n");

    /* Turning the encoders parks them in new detents, so the resting state has moved. */
    baseline = keys_baseline();
    say_positions("at rest now", baseline);

    for (u32 e = 0; e < 8; e++) {
        smcpad_dbg_kv("  turn encoder (1 = leftmost)", e + 1);
        encoder_pos[e] = keys_wait_change(baseline, 15000);
        if (encoder_pos[e]) {
            encoder_pos[e] |= keys_collect(baseline, 2500, 1);
        }
        say_positions("  encoder", encoder_pos[e]);
        keys_wait_release(baseline, 2000);
        baseline = keys_baseline();         /* it has probably stopped somewhere new */
        smcpad_delay_ms(150);
    }

    smcpad_dbg_puts("\r\n=== mapping summary ===\r\nbuttons (panel order):\r\n");
    for (u32 i = 0; i < NUM_LEDS; i++) {
        u32 led = led_panel_order[i];

        say_positions(led_names[led], button_pos[led]);
    }
    smcpad_dbg_puts("encoders (left to right):\r\n");
    for (u32 e = 0; e < 8; e++) {
        char label[12] = "encoder N";

        label[8] = (char)('1' + e);
        say_positions(label, encoder_pos[e]);
    }
    smcpad_dbg_puts("=== end ===\r\n");
}

/*
 * Internal flash, through our own driver (fw/smcpad/smcpad_flash.c). The SDK's sfc_erase /
 * sfc_write occasionally hang with interrupts masked until the 8 s watchdog resets the chip,
 * so nothing here uses them any more. Our driver bounds every wait and retries instead.
 *
 * Reads do not need the flash driver at all: dropping the cache lines and loading from the
 * XIP window gives what the CPU sees (decrypted below the VM start, as stored above it).
 */
extern int smcpad_flash_erase_sector(u32 addr, int attempts);
extern int smcpad_flash_program(u32 addr, const u8 *data, u32 len, int attempts);
extern u32 smcpad_flash_sfc_con(void);
extern const char *smcpad_flash_strerror(int err);
extern u32 smcpad_flash_error_count(int err);
extern void smcpad_flash_get_diag(u8 id[3], u8 sr[4], u32 *polls);
extern u32 smcpad_flash_take_persisted_diag(void);
extern void smcpad_flash_set_variant(u32 v);
extern int smcpad_flash_experiment(u32 mode, u32 addr);
extern void smcpad_flash_get_probe(u32 *before, u32 *after, u32 *spins);
extern void smcpad_flash_get_sums(u32 *base_bad, u32 *after_bad);

static void say_byte(const char *label, u8 value);
extern int smcpad_fault_get(u32 regs[5], u32 *vector);

/*
 * Exception post-mortem from the previous boot (fw/smcpad/smcpad_fault.c): which vector fired
 * (0 emulator exception, 1 exception) and the special registers at that moment. reti/retx/rete
 * hold the interrupted or faulting instruction, rets the return address of the function it was
 * in. 0x1Exxxxx is flash (XIP), 0x1xxxx is RAM.
 */
extern int smcpad_pcsample_get(u32 out[16]);
extern int smcpad_reset_record(u32 out[28]);
extern int smcpad_coarse_get(u32 pc[64], u32 stat[64]);
extern void smcpad_usb_isr_wrap(u8 priority);
extern void smcpad_usb_irq_demote(void);
extern void smcpad_pcsample_live(u32 out[16]);

/* `n` words in hex, six per line. */
static void say_words(const u32 *w, int n)
{
    static const char hexdigits[] = "0123456789abcdef";
    char line[6 * 9 + 4];
    u32 len = 0;

    for (int i = 0; i < n; i++) {
        line[len++] = ' ';
        for (int shift = 28; shift >= 0; shift -= 4) {
            line[len++] = hexdigits[(w[i] >> shift) & 0xF];
        }
        if ((i % 6) == 5 || i == n - 1) {
            line[len++] = '\r';
            line[len++] = '\n';
            line[len] = 0;
            smcpad_dbg_puts(line);
            len = 0;
        }
    }
}

/* Where the code was (timer-interrupt samples, every 2 ms) during the last 32 ms before the
 * last reset, and the registers if it died in the exception handler. */
extern void smcpad_reset_source_get(u32 *pmu, u32 *sys);

static void pcsample_report(void)
{
    u32 pcs[16], rec[28], pmu, sys;

    smcpad_reset_source_get(&pmu, &sys);
    smcpad_dbg_hex16("reset source: pmu", pmu);
    smcpad_dbg_hex16("reset source: sys", sys);

    if (smcpad_pcsample_get(pcs)) {
        smcpad_dbg_puts("PCs sampled before this boot (oldest first, 2 ms apart):\r\n");
        say_words(pcs, 16);
    }
    {
        static u32 cpc[64], cst[64];

        if (smcpad_coarse_get(cpc, cst)) {
            smcpad_dbg_puts("every 100 ms before this boot (oldest first): pc, samples<<16|rx bytes\r\n");
            for (int i = 0; i < 64; i += 3) {
                u32 w[6];
                int k = 0;

                for (int j = i; j < i + 3 && j < 64; j++) {
                    w[k++] = cpc[j];
                    w[k++] = cst[j];
                }
                say_words(w, k);
            }
        }
    }
    if (smcpad_reset_record(rec)) {
        smcpad_dbg_puts("!! the last boot ended in chip_reset(): caller (rets), r0, r1, sp\r\n");
        say_words(rec, 4);
        smcpad_dbg_puts("   words at r0:\r\n");
        say_words(rec + 4, 24);
    }
}

static void cmd_pcsample_live(void)
{
    u32 pcs[16];

    smcpad_pcsample_live(pcs);
    smcpad_dbg_puts("last 16 PCs sampled (oldest first, every 2 ms):\r\n");
    say_words(pcs, 16);
}

static void fault_report(void)
{
    static const char *const names[5] = { "reti", "rete", "retx", "rets", "psr " };
    u32 regs[5], vector;

    pcsample_report();
    if (!smcpad_fault_get(regs, &vector)) {
        return;
    }
    smcpad_dbg_kv("!! last CPU exception before this boot, vector", vector);
    for (int i = 0; i < 5; i++) {
        smcpad_dbg_puts("     ");
        smcpad_dbg_puts(names[i]);
        smcpad_dbg_hex16("  hi", regs[i] >> 16);
        smcpad_dbg_hex16("           lo", regs[i] & 0xFFFF);
    }
}
extern int smcpad_flash_drop_cache(u32 addr, u32 len);  /* smcpad_flash.c */

/* Free flash on this board: after the stock VM region, before BTIF. Nothing we care about
 * lives here, so it is where write tests and staging images go. */
#define FREE_FLASH_BEGIN    0x27000
#define FREE_FLASH_END      0x74000

static u32 flash_to_cpu(u32 addr)
{
    return 0x1E00000 + addr - 0x1000;       /* flash 0x1000 is mapped at 0x1E00000 */
}

static void flash_read_mapped(u32 addr, u8 *buf, u32 len)
{
    const volatile u8 *src = (const volatile u8 *)flash_to_cpu(addr);

    smcpad_flash_drop_cache(addr, len);     /* never sfc_drop_cache() from here: see there */
    for (u32 i = 0; i < len; i++) {
        buf[i] = src[i];
    }
}

/*
 * Breadcrumb for operations that can reset the chip.
 *
 * A fault or a watchdog reset restarts the CPU but does not clear SRAM, and the crt0 only
 * zeroes .bss -- so a word parked in the SDK's reserved block (the 0x80 bytes below the mask
 * ROM's memory, which also holds the update flag) survives and tells us, on the next boot,
 * what we were in the middle of. Without it every crashing experiment costs a reflash to
 * find out how far it got.
 */
/* At +0, i.e. BOOT_STATUS_ADDR's eight reserved bytes. NOT at +0x40: that is inside the
 * UPDATA_PARM struct, which update_result_get() memsets to zero during app_main -- the first
 * version of this breadcrumb was wiped before it could ever be printed. */
extern u32 UPDATA_BEG;
#define CRUMB           ((volatile u32 *)((u32)&UPDATA_BEG))
#define CRUMB_MAGIC     0x5CAD0000
#define CRUMB_NONE          0
#define CRUMB_ERASE         1
#define CRUMB_WRITE         2
#define CRUMB_TRIAL_BASE    0x10        /* +n while running trial n */
/* A silent run (USB interrupts off) reports through a reset: clean << 8 | failed. */
#define CRUMB_SILENT_MAGIC  0x5CAE0000
#define CRUMB_EXP_MAGIC     0x5CB00000          /* | mode << 12 | iteration */
#define UPDATE_DONE_MAGIC   0x5CD00000u         /* smcpad_flash.c: | mismatches << 8 | failures */
static void update_report(u32 value);

static void crumb_set(u32 step)
{
    *CRUMB = step ? (CRUMB_MAGIC | step) : 0;
}

static void crumb_report(void)
{
    u32 value = *CRUMB;
    u32 saved = smcpad_flash_take_persisted_diag();

    /* What the flash driver recorded right before handing the flash back to the SFC on the
     * last flash operation -- the one we may have died after. */
    if (saved) {
        say_byte("!! last flash op before reset: variant ", (u8)((saved >> 24) & 0x3F));
        if ((saved & 0xFFFF) == 0xFFFF) {
            smcpad_dbg_puts("  -- died before the post-resume check finished\r\n");
        } else {
            smcpad_dbg_puts((saved & (1u << 31))
                            ? ((saved & (1u << 30)) ? "  probe WRONG, never recovered"
                                                    : "  probe wrong, then recovered")
                            : "  probe right");
            smcpad_dbg_kv(";  code checksum rounds wrong after resume (of 4)",
                          (saved >> 16) & 0xF);
        }
        if (saved & (1u << 23)) {
            smcpad_dbg_puts("    and the two checksums BEFORE the op already disagreed\r\n");
        }
    }
    if ((value & 0xFFFF0000) == CRUMB_SILENT_MAGIC) {
        *CRUMB = 0;
        smcpad_dbg_puts("!! silent flash run (USB interrupts off) finished:\r\n");
        smcpad_dbg_kv("     first-try successes", (value >> 8) & 0xFF);
        smcpad_dbg_kv("     failures", value & 0xFF);
        smcpad_dbg_puts("     (all 32 trials completed -- no reset during the run)\r\n");
        return;
    }
    if ((value & 0xFFFF0000) == UPDATE_DONE_MAGIC) {
        *CRUMB = 0;
        update_report(value);
        return;
    }
    if (value == 0x5CC10000) {                  /* smcpad_fault.c: USB_MARK_VALUE */
        *CRUMB = 0;
        smcpad_dbg_puts("!! last boot hung INSIDE the USB interrupt (usb_isr)\r\n");
        return;
    }
    if ((value & 0xFFF00000) == CRUMB_EXP_MAGIC) {
        *CRUMB = 0;
        smcpad_dbg_kv("!! last boot died in flash experiment, mode", (value >> 12) & 0xF);
        smcpad_dbg_kv("     at iteration", value & 0xFFF);
        return;
    }
    if ((value & 0xFFFF0000) != CRUMB_MAGIC) {
        return;
    }
    *CRUMB = 0;
    switch (value & 0xFFFF) {
    case CRUMB_ERASE:
        smcpad_dbg_puts("!! last boot ended inside a flash erase\r\n");
        break;
    case CRUMB_WRITE:
        smcpad_dbg_puts("!! last boot ended inside a flash program\r\n");
        break;
    default:
        if ((value & 0xFFFF) >= CRUMB_TRIAL_BASE) {
            smcpad_dbg_kv("!! last boot died in flash trial",
                          (value & 0xFFFF) - CRUMB_TRIAL_BASE);
        } else {
            smcpad_dbg_kv("!! last boot ended at step", value & 0xFFFF);
        }
        break;
    }
}

static void say_hex_dump(u32 addr, const u8 *buf, u32 len)
{
    static const char hexdigits[] = "0123456789abcdef";
    char line[80];
    u32 i = 0;

    line[i++] = ' ';
    line[i++] = ' ';
    for (int shift = 20; shift >= 0; shift -= 4) {
        line[i++] = hexdigits[(addr >> shift) & 0xF];
    }
    line[i++] = ':';
    for (u32 b = 0; b < len && i < sizeof(line) - 4; b++) {
        line[i++] = ' ';
        line[i++] = hexdigits[buf[b] >> 4];
        line[i++] = hexdigits[buf[b] & 0xF];
    }
    line[i++] = '\r';
    line[i++] = '\n';
    line[i] = 0;
    smcpad_dbg_puts(line);
}

/* Read-only and safe anywhere in the mapped range (flash 0x1000 and up). */
static void cmd_flash_read(u32 addr)
{
    u8 buf[16];

    if (addr < 0x1000) {
        smcpad_dbg_puts("sector 0 is not in the memory map; read it in download mode\r\n");
        return;
    }
    flash_read_mapped(addr, buf, sizeof(buf));
    say_hex_dump(addr, buf, sizeof(buf));
}

static void say_byte(const char *label, u8 value)
{
    static const char hexdigits[] = "0123456789abcdef";
    char text[72];
    u32 i = 0;

    while (*label && i < sizeof(text) - 4) {
        text[i++] = *label++;
    }
    text[i++] = hexdigits[value >> 4];
    text[i++] = hexdigits[value & 0xF];
    text[i] = 0;
    smcpad_dbg_puts(text);
}

/* What the flash itself reported during the last operation. Expected: id cd 70 13, status
 * 00 before, WEL (02) after WREN, WIP (03) right after the command, 00 at the end. */
static void say_flash_diag(void)
{
    u8 id[3], sr[4];
    u32 polls;

    smcpad_flash_get_diag(id, sr, &polls);
    say_byte("    flash: id ", id[0]);
    say_byte(" ", id[1]);
    say_byte(" ", id[2]);
    say_byte("  sr before ", sr[0]);
    say_byte(" after-wren ", sr[1]);
    say_byte(" first ", sr[2]);
    say_byte(" last ", sr[3]);
    smcpad_dbg_kv("  polls", polls);
    {
        u32 before, after, spins;

        smcpad_flash_get_probe(&before, &after, &spins);
        smcpad_dbg_hex16("    XIP probe before (low half)", before & 0xFFFF);
        smcpad_dbg_hex16("    XIP probe after  (low half)", after & 0xFFFF);
        smcpad_dbg_kv("    re-reads until it matched", spins);
    }
    {
        u32 base_bad, after_bad;

        smcpad_flash_get_sums(&base_bad, &after_bad);
        smcpad_dbg_kv("    8 KB code checksum: baseline pair disagreed", base_bad);
        smcpad_dbg_kv("    8 KB code checksum: rounds wrong after resume (of 4)", after_bad);
    }
}

static void say_result(const char *what, int result)
{
    smcpad_dbg_puts(what);
    if (result > 0) {
        smcpad_dbg_kv(" ok, attempts", (u32)result);
    } else {
        smcpad_dbg_puts(" FAILED: ");
        smcpad_dbg_puts(smcpad_flash_strerror(result));
        smcpad_dbg_puts("\r\n");
    }
}

#define FLASH_TRIALS    32

/*
 * The same 32 trials with both USB interrupts (SOF and control) masked for the whole run, no
 * printing, results reported through a reset. Every flash operation masks *all* interrupts for
 * 5-15 ms; the deaths happen right when they are re-enabled, so the question is whether it is
 * the USB stack that does not survive being kept waiting. If this completes 32/32 while the
 * normal run dies, it is.
 */
static void cmd_flash_silent(u32 addr)
{
    static const char pattern_rodata[] = "SMCPAD-flash-ok";
    u8 pattern[16], buf[16];
    u32 clean = 0, failed = 0;

    smcpad_dbg_puts("silent run: USB interrupts off for 32 trials, no output; the pad then\r\n"
                    "resets by itself and reports. Expect the port to drop for a few seconds.\r\n");
    smcpad_delay_ms(400);                   /* let the text above reach the host */
    memcpy(pattern, pattern_rodata, sizeof(pattern));

    bit_clr_ie(IRQ_USB_SOF_IDX);
    bit_clr_ie(IRQ_USB_CTRL_IDX);

    for (u32 trial = 0; trial < FLASH_TRIALS; trial++) {
        int e, w;

        crumb_set(CRUMB_TRIAL_BASE + trial);
        pattern[15] = (u8)('A' + (trial % 26));
        e = smcpad_flash_erase_sector(addr, 5);
        w = (e > 0) ? smcpad_flash_program(addr, pattern, sizeof(pattern), 5) : e;
        flash_read_mapped(addr, buf, sizeof(buf));
        if (e == 1 && w == 1 && !memcmp(buf, pattern, sizeof(pattern))) {
            clean++;
        } else if (!(e > 0 && w > 0 && !memcmp(buf, pattern, sizeof(pattern)))) {
            failed++;
        }
    }
    smcpad_flash_erase_sector(addr, 5);

    *CRUMB = CRUMB_SILENT_MAGIC | ((clean & 0xFF) << 8) | (failed & 0xFF);
    cpu_reset();
}

/* ------------------------------------------------------------------ firmware update */

/*
 * Update over the console, driven by host/update.py:
 *   ub <size>              erase the staging area for `size` bytes       -> "ub ok"
 *   uw <offset> <len>      -> "uw ready", then exactly `len` raw bytes (<= 1024), programmed
 *                          into staging at `offset`                       -> "uw ok <crc32>"
 *   uv <size> <crc32>      CRC32 of the whole staging copy                -> "uv ok" / "uv bad"
 *   uc <size> <crc32>      checks again, then copies staging over the app area from RAM and
 *                          resets; the next boot reports how it went
 * The image is what host/flash_fw.sh writes at 0x1000 (work/flash/fw_area_raw.bin): raw
 * physical bytes, app directory first.
 */
#define UPDATE_STAGING      0x40000
#define UPDATE_DST          SMCPAD_APP_DIR
#define UPDATE_AREA_LEN     0x25000             /* as much as flash_fw.sh erases at 0x1000 */
#define UPDATE_CHUNK_MAX    1024

extern int smcpad_flash_copy_and_reset(u32 dst, u32 src, u32 len, u32 erase_len);

static u8 update_chunk[UPDATE_CHUNK_MAX] __attribute__((aligned(4)));

static u32 crc32_update(u32 crc, const u8 *p, u32 len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

static void say_kv_hex(const char *tag, u32 value)
{
    static const char hexdigits[] = "0123456789abcdef";
    char line[48];
    u32 n = 0;

    while (*tag && n < sizeof(line) - 12) {
        line[n++] = *tag++;
    }
    line[n++] = ' ';
    for (int shift = 28; shift >= 0; shift -= 4) {
        line[n++] = hexdigits[(value >> shift) & 0xF];
    }
    line[n++] = '\r';
    line[n++] = '\n';
    line[n] = 0;
    smcpad_dbg_puts(line);
}

static int update_size_ok(u32 size)
{
    return size > 0x100 && size <= UPDATE_AREA_LEN;
}

static u32 staging_crc(u32 size)
{
    u8 buf[64];
    u32 crc = 0;

    for (u32 off = 0; off < size; off += sizeof(buf)) {
        u32 n = size - off < sizeof(buf) ? size - off : sizeof(buf);

        flash_read_mapped(UPDATE_STAGING + off, buf, n);
        crc = crc32_update(crc, buf, n);
        if ((off & 0xFFF) == 0) {
            clr_wdt();
        }
    }
    return crc;
}

static void cmd_update(const char *args)
{
    char sub = *args++;
    u32 a = 0, b = 0;
    const char *rest = parse_hex(args, &a);

    if (rest) {
        parse_hex(rest, &b);
    }
    if (sub == 'b' && rest && update_size_ok(a)) {
        for (u32 off = 0; off < a; off += 0x1000) {
            if (smcpad_flash_erase_sector(UPDATE_STAGING + off, 3) <= 0) {
                smcpad_dbg_puts("ub fail\r\n");
                return;
            }
            clr_wdt();
        }
        smcpad_dbg_puts("ub ok\r\n");
    } else if (sub == 'w' && rest && b > 0 && b <= UPDATE_CHUNK_MAX && a + b <= UPDATE_AREA_LEN) {
        u32 got = 0, idle = 0;

        smcpad_dbg_puts("uw ready\r\n");
        while (got < b && idle < 2000) {        /* 2 s without a byte: give up */
            u32 n = smcpad_dbg_read((char *)update_chunk + got, b - got);

            got += n;
            if (n) {
                idle = 0;
            } else {
                idle++;
                smcpad_delay_ms(1);
            }
        }
        if (got < b) {
            say_kv_hex("uw timeout, bytes received", got);
            return;
        }
        for (u32 off = 0; off < b;) {
            u32 addr = UPDATE_STAGING + a + off;
            u32 n = 256 - (addr & 0xFF);

            if (n > b - off) {
                n = b - off;
            }
            if (smcpad_flash_program(addr, update_chunk + off, n, 3) <= 0) {
                smcpad_dbg_puts("uw fail\r\n");
                return;
            }
            off += n;
        }
        say_kv_hex("uw ok", crc32_update(0, update_chunk, b));
    } else if ((sub == 'v' || sub == 'c') && rest && update_size_ok(a)) {
        u32 crc = staging_crc(a);

        if (crc != b) {
            say_kv_hex(sub == 'v' ? "uv bad" : "uc bad", crc);
            return;
        }
        if (sub == 'v') {
            smcpad_dbg_puts("uv ok\r\n");
            return;
        }
        smcpad_dbg_puts("uc copying, back in a few seconds\r\n");
        smcpad_delay_ms(300);                   /* let that reach the host */
        smcpad_flash_copy_and_reset(UPDATE_DST, UPDATE_STAGING, a, UPDATE_AREA_LEN);
        smcpad_dbg_puts("uc refused\r\n");    /* only on bad arguments */
    } else {
        smcpad_dbg_puts("u?: ub <size> | uw <off> <len> | uv <size> <crc32> | uc <size> <crc32>\r\n");
    }
}

/* At boot: did the last boot end in an update copy? */
static void update_report(u32 value)
{
    if (value & 0xFFFF) {
        smcpad_dbg_kv("!! update copy had problems: failed ops", value & 0xFF);
        smcpad_dbg_kv("   pages that did not match staging", (value >> 8) & 0xFF);
    } else {
        smcpad_dbg_puts("update applied: copy verified, this is the new firmware\r\n");
    }
}

/*
 * Bisection of the death after flash operations (see smcpad_flash_experiment): `count`
 * iterations of one mode, interrupts back on for a few ms between them, nothing printed until
 * the end. If the chip dies, the next boot names the mode and iteration.
 */
static void cmd_flash_experiment(u32 mode, u32 count, u32 addr)
{
    u32 failed = 0;
    int last = 1;

    if (count == 0 || count > 0xFFF) {
        count = 200;
    }
    if (mode == 4 && (addr < FREE_FLASH_BEGIN || addr >= FREE_FLASH_END || (addr & 0xFFF))) {
        smcpad_dbg_puts("refusing: mode 4 erases, give a free sector (27000..73000)\r\n");
        return;
    }
    smcpad_dbg_kv("experiment mode", mode);
    smcpad_dbg_kv("  iterations", count);
    smcpad_delay_ms(300);                   /* let that reach the host */
    for (u32 i = 0; i < count; i++) {
        *CRUMB = CRUMB_EXP_MAGIC | ((mode & 0xF) << 12) | (i & 0xFFF);
        last = smcpad_flash_experiment(mode, addr);
        if (last < 0) {
            failed++;
        }
        smcpad_delay_ms(5);
    }
    *CRUMB = 0;
    smcpad_dbg_kv("experiment survived, mode", mode);
    smcpad_dbg_kv("  iterations", count);
    smcpad_dbg_kv("  with a driver error", failed);
}

/*
 * Erase / program / verify on one sector of free flash, then the same 32 times to measure
 * how often an operation needs a retry. A retry is our driver giving up on a stuck wait and
 * trying again; before this driver, each of those was a watchdog reset.
 */
static void cmd_flash_test(u32 addr)
{
    static const char pattern_rodata[] = "SMCPAD-flash-ok";
    u8 pattern[16];                     /* RAM: the flash is unreadable while it programs */
    u8 buf[16];
    u32 clean = 0, retried = 0, failed = 0;
    u32 erase_never_busy = 0, program_never_busy = 0, polls;
    u32 xip_wrong = 0, xip_spins_max = 0, pb, pa, ps;
    u32 sum_rounds_bad = 0, sum_base_bad = 0, sb, sa;
    u8 id[3], sr[4];
    int r;

    if (addr < FREE_FLASH_BEGIN || addr >= FREE_FLASH_END || (addr & 0xFFF)) {
        smcpad_dbg_puts("refusing: must be a 4KB-aligned sector in free flash "
                        "(0x27000..0x74000)\r\n");
        return;
    }
    memcpy(pattern, pattern_rodata, sizeof(pattern));
    smcpad_dbg_hex16("SFC CON low half", smcpad_flash_sfc_con() & 0xFFFF);
    smcpad_dbg_kv("SFC read mode (CON bits 8..11)", (smcpad_flash_sfc_con() >> 8) & 0xF);

    crumb_set(CRUMB_ERASE);
    r = smcpad_flash_erase_sector(addr, 3);
    crumb_set(CRUMB_NONE);
    say_result("erase:", r);
    say_flash_diag();
    flash_read_mapped(addr, buf, sizeof(buf));
    say_hex_dump(addr, buf, sizeof(buf));

    crumb_set(CRUMB_WRITE);
    r = smcpad_flash_program(addr, pattern, sizeof(pattern), 3);
    crumb_set(CRUMB_NONE);
    say_result("program:", r);
    say_flash_diag();
    flash_read_mapped(addr, buf, sizeof(buf));
    say_hex_dump(addr, buf, sizeof(buf));
    smcpad_dbg_puts(memcmp(buf, pattern, sizeof(pattern)) ? "  readback does NOT match\r\n"
                                                           : "  WRITE WORKS\r\n");

    smcpad_dbg_kv("now erase+program+verify, trials", FLASH_TRIALS);
    for (u32 trial = 0; trial < FLASH_TRIALS; trial++) {
        int e, w;

        crumb_set(CRUMB_TRIAL_BASE + trial);
        pattern[15] = (u8)('A' + (trial % 26));
        e = smcpad_flash_erase_sector(addr, 5);
        smcpad_flash_get_diag(id, sr, &polls);
        if (e > 0 && !(sr[2] & 1)) {
            erase_never_busy++;
        }
        smcpad_flash_get_probe(&pb, &pa, &ps);
        if (pa != pb) {
            xip_wrong++;
        }
        xip_spins_max = ps > xip_spins_max ? ps : xip_spins_max;
        smcpad_flash_get_sums(&sb, &sa);
        sum_rounds_bad += sa;
        sum_base_bad += sb;
        w = (e > 0) ? smcpad_flash_program(addr, pattern, sizeof(pattern), 5) : e;
        smcpad_flash_get_diag(id, sr, &polls);
        if (w > 0 && !(sr[2] & 1)) {
            program_never_busy++;
        }
        smcpad_flash_get_probe(&pb, &pa, &ps);
        if (pa != pb) {
            xip_wrong++;
        }
        xip_spins_max = ps > xip_spins_max ? ps : xip_spins_max;
        smcpad_flash_get_sums(&sb, &sa);
        sum_rounds_bad += sa;
        sum_base_bad += sb;
        flash_read_mapped(addr, buf, sizeof(buf));

        if (e > 0 && w > 0 && !memcmp(buf, pattern, sizeof(pattern))) {
            if (e == 1 && w == 1) {
                clean++;
            } else {
                retried++;
            }
        } else {
            failed++;
            if (failed <= 3) {
                smcpad_dbg_kv("  trial failed", trial);
                say_result("    erase:", e);
                say_result("    program:", w);
            }
        }
        if (e > 1) {
            smcpad_dbg_kv("  trial needed erase attempts, trial", trial);
        }
        if (w > 1) {
            smcpad_dbg_kv("  trial needed program attempts, trial", trial);
        }
    }
    crumb_set(CRUMB_NONE);
    smcpad_dbg_kv("  first-try successes", clean);
    smcpad_dbg_kv("  successes after a retry", retried);
    smcpad_dbg_kv("  failures", failed);
    /* An erase or program the flash never reported as busy means its status register cannot
     * be trusted, and "done" cannot be decided by polling it. */
    smcpad_dbg_kv("  erases whose status never showed busy", erase_never_busy);
    smcpad_dbg_kv("  programs whose status never showed busy", program_never_busy);
    smcpad_dbg_kv("  ops whose first XIP read after resume was wrong", xip_wrong);
    smcpad_dbg_kv("  most re-reads before XIP came right", xip_spins_max);
    smcpad_dbg_kv("  code checksum rounds wrong after a resume (of 4 per op x 64 ops)", sum_rounds_bad);
    smcpad_dbg_kv("  ops whose BASELINE checksums already disagreed", sum_base_bad);

    /* Which wait our driver had to abandon, and how often: this names the SDK's hang. */
    smcpad_dbg_puts("waits abandoned since boot:\r\n");
    for (int err = -1; err >= -4; err--) {
        smcpad_dbg_puts("  ");
        smcpad_dbg_puts(smcpad_flash_strerror(err));
        smcpad_dbg_kv("", smcpad_flash_error_count(err));
    }

    r = smcpad_flash_erase_sector(addr, 5);
    say_result("final erase (sector left blank):", r);
}

/*
 * Back into the ROM's USB download mode without touching the power switch.
 *
 * __JUMP_TO_MASKROM (SDK, RAM) re-enters the mask ROM at its reset entry, 0x110000. The ROM
 * enters download mode when it sees the USB_KEY on D+/D- as it starts, so if the Pico is
 * clocking the key at that moment we land in UBOOT exactly as after a power cycle with the key.
 * Order matters: the Pico takes "D+ is high" as the chip's acknowledgement, and while our USB
 * device is attached its 1.5k pull-up keeps D+ high -- so we first drop off the bus (pins to
 * GPIO, no pulls), give the host time to start the key, and only then jump.
 * host/flash_fw.sh does its half: 'J' here, then 'r' to the Pico.
 */
extern void __JUMP_TO_MASKROM(void);

#include "smcpad_usb_cfg.h"
#if SMCPAD_TINYUSB
/*
 * USB bring-up safety net for the TinyUSB build: the host has not configured the device
 * within 25 s, so USB does not work and this firmware cannot be told anything. Save the driver's event log
 * in free flash (USB_LOG_SECTOR: magic, count, then the entries -- host/flash_fw.sh reads the
 * whole flash back, so it lands in work/flash/verify_last.bin) and jump to the mask ROM the
 * way `J` does, so an armed USB_KEY catches the chip without a hand on the switch.
 */
#include "tusb.h"
#define USB_LOG_SECTOR      0x70000
#define USB_LOG_MAGIC       0x55534C47u            /* "USLG" */
extern uint32_t smcpad_usb_log(uint32_t *out, uint32_t max);
static void cmd_jump_to_rom(void);

static void usb_bringup_give_up(void)
{
    static u32 page[64] __attribute__((aligned(4)));
    u32 n = smcpad_usb_log(page + 2, 62);

    page[0] = USB_LOG_MAGIC;
    page[1] = n;
    if (smcpad_flash_erase_sector(USB_LOG_SECTOR, 3) > 0) {
        smcpad_flash_program(USB_LOG_SECTOR, (const u8 *)page, sizeof(page), 3);
    }
    cmd_jump_to_rom();
}
#endif

static void cmd_jump_to_rom(void)
{
    smcpad_dbg_puts("detaching from USB, jumping to the mask ROM in 2 s\r\n");
    smcpad_delay_ms(200);                   /* let that line reach the host */

    local_irq_disable();
    /* usb_iomode() + gpio pulls alone leave the PHY's 1.5k on D+: the host drops the device,
     * but the USB_KEY sender reads D+ high and takes it for the ROM's acknowledgement. */
    JL_USB->CON0 = 0;                       /* PHY off, SIE in reset */
    JL_USB_IO->CON0 &= ~(BIT(DPPU) | BIT(DMPU) | BIT(DPPD) | BIT(DMPD));
    usb_iomode(1);
    gpio_set_pull_up(IO_PORT_DP, 0);
    gpio_set_pull_down(IO_PORT_DP, 0);
    gpio_set_direction(IO_PORT_DP, 1);
    gpio_set_pull_up(IO_PORT_DM, 0);
    gpio_set_pull_down(IO_PORT_DM, 0);
    gpio_set_direction(IO_PORT_DM, 1);

    smcpad_delay_ms(2000);                  /* the host starts clocking the key meanwhile */
    __JUMP_TO_MASKROM();
}

/* Our bootloader (fw/boot): reset into its DFU mode (smcpad_rt.c). */
#include "smcpad_rt.h"

static void cmd_enter_bootloader(void)
{
#if SMCPAD_APP_DIR == 0x1000
    smcpad_dbg_puts("no bootloader in this layout (built for 0x1000)\r\n");
#else
    smcpad_dbg_puts("resetting into the bootloader (DFU)\r\n");
    smcpad_delay_ms(200);
    smcpad_enter_bootloader();
#endif
}

/*
 * Ask the bootloader to come up in update mode instead of running us, then reset.
 *
 * The SDK passes this through a reserved RAM block: an UPDATA_PARM whose parm_type is one of
 * UPDATA_TYPE (USB_UPDATA = 0x5A00, SD0, SD1, PC_UPDATA, UART_UPDATA, ... USB_HID_UPDATA),
 * protected by a CRC16 over everything after the crc field itself -- same shape as
 * update_result_set() in apps/common/update/update.c.  The factory isd_config.ini carries
 * UPDATE_JUMP, which the SDK documents as "jump to the mask ROM", so the hope is that the
 * uboot hands us to the ROM's USB download mode: that would let us reflash over plain USB,
 * with no Pico and no USB_KEY.
 *
 * Safe to try: the flag lives in RAM, so a power cycle undoes whatever it decides to do.
 */
static void cmd_update_mode(u32 type)
{
    UPDATA_PARM *p = UPDATA_FLAG_ADDR;

    smcpad_dbg_hex16("asking the bootloader for update mode, type", type);
    smcpad_dbg_puts("resetting now; if the pad comes back as a mass-storage device\r\n"
                    "(JieLi 0x4C4A, no serial port) we can flash it without the Pico.\r\n"
                    "if nothing happens, power-cycle it: the flag is in RAM.\r\n");
    smcpad_delay_ms(400);                   /* let the host read the lines above */

    memset(p, 0x00, sizeof(UPDATA_PARM));
    p->parm_type = (u16)type;
    p->parm_crc = CRC16(((u8 *)p) + 2, sizeof(UPDATA_PARM) - 2);

    local_irq_disable();
    cpu_reset();
}

/* smcpad_pads.c */
extern void smcpad_cmd_pads_live(void);
extern void smcpad_cmd_pads_wizard(void);
extern void smcpad_cmd_rgb(int all, int idx, u32 rgb);
extern void smcpad_cmd_rgb_walk(void);

static void run_command(const char *line)
{
    u32 pin, value;
    const char *rest;

    line = skip_spaces(line);
    switch (*line) {
    case 0:
        break;
    case '?':
    case 'h':
        cmd_help();
        break;
    case 'm':
        cmd_map();
        break;
    case 'o':
    case 'l':
        if (parse_pin(line + 1, &pin)) {
            pin_output(pin, *line == 'o');
            say_pin(*line == 'o' ? "high" : "low ", pin);
        } else {
            smcpad_dbg_puts("bad pin\r\n");
        }
        break;
    case 'z':
        if (parse_pin(line + 1, &pin)) {
            pin_hiz(pin);
            say_pin("released", pin);
        } else {
            smcpad_dbg_puts("bad pin\r\n");
        }
        break;
    case 'r':
        if (parse_pin(line + 1, &pin)) {
            smcpad_dbg_kv(pin_read(pin) ? "level high, pin" : "level low, pin", pin);
        } else {
            smcpad_dbg_puts("bad pin\r\n");
        }
        break;
    case 'w':
        if (parse_hex(line + 1, &value)) {
            sr_pins_init();
            sr_write((u16)value);
            smcpad_dbg_hex16("latched", value & 0xFFFF);
        } else {
            smcpad_dbg_puts("bad hex\r\n");
        }
        break;
    case 'c':
        rest = parse_pin(line + 1, &pin);
        if (rest) {
            u32 dat, lat;

            rest = parse_pin(rest, &dat);
            if (rest && parse_pin(rest, &lat)) {
                sr_clk = pin;
                sr_dat = dat;
                sr_lat = lat;
                sr_pins_init();
                cmd_map();
                break;
            }
        }
        smcpad_dbg_puts("usage: c <clk> <dat> <lat>\r\n");
        break;
    case 'e':
        if (parse_pin(line + 1, &pin)) {
            sr_oe = pin;
            pin_output(sr_oe, 0);
            say_pin("/OE is now", sr_oe);
        } else {
            smcpad_dbg_puts("bad pin\r\n");
        }
        break;
    case 's':
        cmd_scan();
        break;
    case 'p':
        cmd_permutations();
        break;
    case 't':
        cmd_sweep();
        break;
    case 'd':
        if (parse_hex(line + 1, &value) && value < NUM_LEDS) {
            btnled_one(value);
            smcpad_dbg_kv("holding led", value);
        } else {
            smcpad_dbg_puts("usage: d <0..9>\r\n");
        }
        break;
    case 'k':
        smcpad_dbg_puts("all ten LEDs, multiplexed, 5s\r\n");
        btnled_multiplex(0x03FF, 5000);
        break;
    case 'b':
        cmd_keys();
        break;
    case 'M':
        cmd_map_wizard();
        break;
    case 'E':
        cmd_encoders();
        break;
    case 'R':
        if (parse_hex(line + 1, &value)) {
            cmd_flash_read(value);
        } else {
            smcpad_dbg_puts("usage: R <flash addr hex>   e.g. R 1120\r\n");
        }
        break;
    case 'F':
        rest = parse_hex(line + 1, &value);
        if (rest) {
            u32 v = 0;

            parse_hex(rest, &v);                /* optional variant, default 0 */
            smcpad_flash_set_variant(v);
            smcpad_dbg_kv("flash variant (bit0 drop code cache, bit1 own resume, bit2 silent/USB off)", v);
            if (v & 4) {
                if (value < FREE_FLASH_BEGIN || value >= FREE_FLASH_END || (value & 0xFFF)) {
                    smcpad_dbg_puts("refusing: not a free sector\r\n");
                } else {
                    cmd_flash_silent(value);
                }
            } else {
                cmd_flash_test(value);
            }
        } else {
            smcpad_dbg_puts("usage: F <free sector hex> [variant]  e.g. F 30000 1\r\n");
        }
        break;
    case 'P':
        cmd_pcsample_live();
        break;
    case 'T': {
        /* USB test pattern: one 200-byte write, then ten 20-byte writes. */
        static char pat[201];

        for (int i = 0; i < 200; i++) {
            pat[i] = (char)('A' + (i % 26));
        }
        pat[200] = 0;
        smcpad_dbg_puts("<");
        smcpad_dbg_puts(pat);
        smcpad_dbg_puts(">\r\n");
        for (int k = 0; k < 10; k++) {
            char line[21];

            for (int i = 0; i < 18; i++) {
                line[i] = (char)('0' + k);
            }
            line[18] = '\r';
            line[19] = '\n';
            line[20] = 0;
            smcpad_dbg_puts(line);
        }
        break;
    }
#if SMCPAD_TINYUSB
    case 'I': {
        /* what the uboot handed over (boot_info_init copies it from the struct in r0) */
        extern u32 boot_info[];
        static const char *const names[] = { "vm.align", "vm.saddr", "vm.res", "vm.size",
                                             "sfc_base_addr", "app_addr", "flash_size",
                                             "chip_id|trim<<16", "mac0", "mac1" };

        for (int i = 0; i < 10; i++) {
            say_kv_hex(names[i], boot_info[i]);
        }
        say_kv_hex("SFC BASE_ADR reg", *(volatile u32 *)0x1F020C);
        /* what the bootloader's last DFU session left (fw/boot/boot.h struct boot_diag) */
        static const char *const dnames[] = { "boot.magic", "boot.ms", "boot.polls", "boot.usb_con0",
                                              "boot.clk_con1", "boot.pll_con", "boot.events",
                                              "boot.blocks", "boot.last_error" };
        for (int i = 0; i < 9; i++) say_kv_hex(dnames[i], ((volatile u32 *)0x31AA0)[i]);
        break;
    }
    case 'G': {
        /* per 64-byte block of the last uw chunk: first byte, last byte, bytes != first */
        for (int blk = 0; blk < 16; blk++) {
            u8 *b = update_chunk + blk * 64;
            u32 diff = 0;

            for (int i = 0; i < 64; i++) {
                diff += b[i] != b[0];
            }
            say_kv_hex("blk", ((u32)blk << 24) | ((u32)b[0] << 16) | ((u32)b[63] << 8) | diff);
        }
        break;
    }
    case 'L': {
        static u32 ev[128];
        u32 n = smcpad_usb_log(ev, 128);

        smcpad_dbg_kv("usb driver events", n);
        say_words(ev, (int)n);
        break;
    }
#endif
    case 'u':
        cmd_update(line + 1);
        break;
    case 'X':
        rest = parse_hex(line + 1, &value);
        if (rest && value <= 4) {
            u32 count = 200, addr = 0x30000;

            rest = parse_hex(rest, &count);
            if (rest) {
                parse_hex(rest, &addr);
            }
            cmd_flash_experiment(value, count, addr);
        } else {
            smcpad_dbg_puts("usage: X <mode 0..4> [count hex, default c8] [sector, default 30000]\r\n");
        }
        break;
    case 'U':
        cmd_update_mode(parse_hex(line + 1, &value) ? value : USB_UPDATA);
        break;
    case 'J':
        cmd_jump_to_rom();
        break;
    case 'B':
        cmd_enter_bootloader();
        break;
    case 'K': {
        extern void smcpad_cmd_pads_midi(void);
        smcpad_cmd_pads_midi();
        break;
    }
    case 'Y': {
        extern void blemidi_start(void);
        blemidi_start();
        break;
    }
    case 'S': {
        extern int smcpad_flash_read_sr(u8 *sr1, u8 *sr2);
        u8 sr1 = 0, sr2 = 0;

        if (smcpad_flash_read_sr(&sr1, &sr2)) {
            smcpad_dbg_puts("status register read failed\r\n");
        } else {
            smcpad_dbg_hex16("flash SR1 (BP = 0x7C)", sr1);
            smcpad_dbg_hex16("flash SR2 (CMP = 0x40, QE = 0x02)", sr2);
        }
        break;
    }
    case 'A':
        smcpad_cmd_pads_live();
        break;
    case 'W':
        smcpad_cmd_pads_wizard();
        break;
    case 'N':
        smcpad_cmd_rgb_walk();
        break;
    case 'n':
        rest = skip_spaces(line + 1);
        if (!*rest) {
            smcpad_cmd_rgb(0, -1, 0);
        } else if (*rest == 'a' && parse_hex(rest + 1, &value)) {
            smcpad_cmd_rgb(1, 0, value);
        } else if ((rest = parse_hex(rest, &pin)) && parse_hex(rest, &value) && pin < 16) {
            smcpad_cmd_rgb(0, (int)pin, value);
        } else {
            smcpad_dbg_puts("usage: n | n <0..f> <rrggbb> | n a <rrggbb>\r\n");
        }
        break;
    case 'Z':
        smcpad_dbg_puts("resetting\r\n");
        smcpad_delay_ms(300);
        cpu_reset();
        break;
    case 'f':
        if (parse_hex(line + 1, &value) && value < 3) {
            btnled_phase(value, 0x0F);
            smcpad_dbg_kv("phase low, all segments low", value);
        } else {
            smcpad_dbg_puts("usage: f <0..2>\r\n");
        }
        break;
    case 'g':
        rest = parse_hex(line + 1, &value);
        if (rest && value < 3) {
            u32 seg;

            if (parse_hex(rest, &seg) && seg < 4) {
                btnled_phase(value, (u8)(1u << seg));
                smcpad_dbg_kv("phase", value);
                smcpad_dbg_kv("  segment", seg);
                break;
            }
        }
        smcpad_dbg_puts("usage: g <phase 0..2> <segment 0..3>\r\n");
        break;
    case 'x':
        btnled_off();
        cmd_release_all();
        break;
    default:
        smcpad_dbg_puts("unknown command, '?' for help\r\n");
        break;
    }
    smcpad_dbg_puts("> ");
}

void smcpad_bringup_start(void)
{
    char line[48];
    u32 len = 0, uptime_ticks = 0;

    smcpad_trace("bringup: console start");

    /* Wait for the host to open the port; the boot trace goes out on the first success. */
    for (int i = 0; i < (SMCPAD_TINYUSB ? 250 : 100); i++) {
        char none;

        if (smcpad_dbg_ready()) {
            break;
        }
        smcpad_dbg_read(&none, 0);              /* honours a 1200-baud touch meanwhile */
        smcpad_delay_ms(100);
    }
#if SMCPAD_TINYUSB
    if (!tud_mounted()) {
        usb_bringup_give_up();          /* never configured by the host: USB is broken */
    }
#endif

    smcpad_dbg_puts("\r\n=== SMC-PAD open firmware: bring-up console ===\r\n");
    smcpad_dbg_puts("build " __DATE__ " " __TIME__ "\r\n");
    fault_report();
    crumb_report();
    cmd_help();
    cmd_map();
    smcpad_dbg_puts("> ");

    for (;;) {
        char chunk[32];
        u32 got = smcpad_dbg_read(chunk, sizeof(chunk));

#if SMCPAD_TINYUSB
#endif

        for (u32 i = 0; i < got; i++) {
            char c = chunk[i];

            if (c == '\r' || c == '\n') {
                line[len] = 0;
                if (len) {
                    run_command(line);
                } else {
                    smcpad_dbg_puts("> ");
                }
                len = 0;
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
            }
        }
        clr_wdt();
        smcpad_delay_ms(got ? 1 : 10);
        if (++uptime_ticks == 300) {
            smcpad_boot_ok();                   /* ~3 s up: not a crash loop */
        }
    }
}
