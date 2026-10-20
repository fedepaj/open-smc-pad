/*
 * SMC-PAD hardware service: everything with timing, in the background (docs/hardware.md has the
 * map this is built on).
 *
 * TIMER3 at 4 kHz:
 *   - key matrix: two 74HC595 (bit-banged: PB6 clock, PB7 data, PB4 latch, /OE PB5) select one
 *     of 16 columns, one-hot low; PB0 senses the encoders, PC1 the buttons. Each tick reads the
 *     column latched by the previous one (250 us of settling) and latches the next: a whole
 *     pass every 4 ms, after which encoders are decoded and buttons debounced.
 *   - button LEDs: a 3x4 multiplex (phases PA3 / PC2 / PC3 active-low commons, segments PA5 /
 *     PA2 / PA0 / PA1 active-low sinks), one phase per tick. The phases are switched off before
 *     the sense lines are read: phase 2 (PC3) low drags PC1 down.
 * SAR ADC, interrupt-chained: the 16 pads through two HCF4051 (select PB2 / PB3 / PC4, COM PA4
 *   = channel 3 and PA6 = channel 4); each conversion's interrupt stores it, moves the mux, starts
 *   the next. Per pad: above the on threshold an attack window looks for the peak -> note on
 *   with a velocity; below the off threshold -> note off; in between the value is the pressure.
 * Pad RGB LEDs: WS2812-style chain on PB1 from SPI1 by DMA (frame as the stock firmware sends it).
 */
#include "system/includes.h"
#include "asm/gpio.h"
#include "asm/hwi.h"
#include "asm/adc_api.h"
#include "asm/clock.h"
#include "smcpad_rt.h"

#define REG32(a)            (*(volatile u32 *)(a))
#define PORT_OUT(b)         REG32((b) + 0x00)
#define PORT_IN(b)          REG32((b) + 0x04)
#define PORTA               0x1E5000u
#define PORTB               0x1E5040u
#define PORTC               0x1E5080u

/* ------------------------------------------------------------------ tables (docs/hardware.md) */

/* buttons, panel order -> matrix column on PC1 */
static const u8 button_col[SMCPAD_NUM_BUTTONS] = { 2, 3, 4, 5, 6, 7, 9, 8, 0, 1 };
/* button LEDs, panel order -> (phase << 4 | segment) */
static const u8 led_pos[SMCPAD_NUM_BUTTONS] = { 0x22, 0x21, 0x20, 0x10, 0x03, 0x12, 0x01, 0x11, 0x02, 0x00 };
/* encoders, knob 1..8 -> quadrature pair N = columns 2N, 2N+1 on PB0 */
static const u8 encoder_pair[SMCPAD_NUM_ENCODERS] = { 7, 5, 0, 4, 3, 6, 2, 1 };
/* ADC position (mux m on PA4 = m, on PA6 = 8 + m) -> pad (0 = bottom-left) */
static const u8 adc_to_pad[SMCPAD_NUM_PADS] = { 2, 0, 1, 3, 15, 14, 12, 13, 6, 4, 5, 7, 8, 9, 11, 10 };
/* pad -> position in the RGB chain */
static const u8 pad_to_led[SMCPAD_NUM_PADS] = { 11, 4, 2, 7, 10, 14, 6, 9, 0, 5, 15, 1, 13, 12, 8, 3 };

/* ------------------------------------------------------------------ events */

#define EVQ_LEN     64
static smcpad_event_t evq[EVQ_LEN];
static volatile u8 evq_head, evq_tail;

static void ev_push(u8 type, u8 index, s16 value)
{
    u8 next = (evq_head + 1) % EVQ_LEN;

    if (next == evq_tail) {
        return;                             /* full: drop the newest */
    }
    evq[evq_head].type = type;
    evq[evq_head].index = index;
    evq[evq_head].value = value;
    evq_head = next;
}

int smcpad_event_get(smcpad_event_t *ev)
{
    if (evq_tail == evq_head) {
        return 0;
    }
    *ev = evq[evq_tail];
    evq_tail = (evq_tail + 1) % EVQ_LEN;
    return 1;
}

/* ------------------------------------------------------------------ key matrix + button LEDs */

static volatile u32 matrix_now;             /* bit (col * 2 + sense): closed; sense 0 = PB0, 1 = PC1 */
static u32 matrix_acc;
static u8 scan_col;
static u8 led_phase;
static volatile u16 led_on;                 /* bit per button LED (panel order) */
static u8 led_seg_mask[3];                  /* per phase: segment bits to sink */
static u16 buttons_state, buttons_candidate;
static u8 buttons_stable;
static volatile s32 encoder_count[SMCPAD_NUM_ENCODERS];
static u8 encoder_ab[SMCPAD_NUM_ENCODERS];

static void sr_write(u16 v)
{
    for (int bit = 15; bit >= 0; bit--) {
        if ((v >> bit) & 1) {
            PORT_OUT(PORTB) |= 1u << 7;
        } else {
            PORT_OUT(PORTB) &= ~(1u << 7);
        }
        PORT_OUT(PORTB) &= ~(1u << 6);
        PORT_OUT(PORTB) |= 1u << 6;
    }
    PORT_OUT(PORTB) &= ~(1u << 6);
    PORT_OUT(PORTB) |= 1u << 4;
    PORT_OUT(PORTB) &= ~(1u << 4);
}

static void phases_off(void)
{
    PORT_OUT(PORTA) |= 1u << 3;
    PORT_OUT(PORTC) |= (1u << 2) | (1u << 3);
}

static void segments_set(u8 mask)           /* bit s = segment s sinks */
{
    static const u8 seg_pin[4] = { 5, 2, 0, 1 };
    for (int s = 0; s < 4; s++) {
        if (mask & (1u << s)) {
            PORT_OUT(PORTA) &= ~(1u << seg_pin[s]);
        } else {
            PORT_OUT(PORTA) |= 1u << seg_pin[s];
        }
    }
}

static void phase_on(u8 p)
{
    if (p == 0) {
        PORT_OUT(PORTA) &= ~(1u << 3);
    } else {
        PORT_OUT(PORTC) &= ~(1u << (p == 1 ? 2 : 3));
    }
}

/* Quadrature: (previous AB << 2 | new AB) -> step */
static const s8 quad_step[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };

static void matrix_pass_done(u32 m)
{
    u16 b = 0;

    matrix_now = m;
    for (int e = 0; e < SMCPAD_NUM_ENCODERS; e++) {
        u32 pair = encoder_pair[e];
        u8 ab = ((m >> (pair * 4)) & 1) | (((m >> (pair * 4 + 2)) & 1) << 1);
        s8 step = quad_step[(encoder_ab[e] << 2) | ab];

        encoder_ab[e] = ab;
        if (step) {
            encoder_count[e] += step;
            ev_push(SMCPAD_EV_ENCODER, (u8)e, step);
        }
    }
    for (int i = 0; i < SMCPAD_NUM_BUTTONS; i++) {
        if (m & (1u << (button_col[i] * 2 + 1))) {
            b |= 1u << i;
        }
    }
    if (b != buttons_candidate) {           /* debounce: the same for two passes (8 ms) */
        buttons_candidate = b;
        buttons_stable = 0;
    } else if (!buttons_stable) {
        u16 changed = b ^ buttons_state;

        buttons_stable = 1;
        for (int i = 0; i < SMCPAD_NUM_BUTTONS; i++) {
            if (changed & (1u << i)) {
                ev_push((b & (1u << i)) ? SMCPAD_EV_BUTTON_DOWN : SMCPAD_EV_BUTTON_UP, (u8)i, 0);
            }
        }
        buttons_state = b;
    }
}

static volatile u32 hw_ticks, hw_ticks_hi;  /* TIMER3 periods (250 us) since smcpad_hw_begin() */
static u8 hw_running;

___interrupt
static void hw_tick_isr(void)
{
    JL_TIMER3->CON |= BIT(14);              /* clear pending */
    if (++hw_ticks == 0) {
        hw_ticks_hi++;
    }

    phases_off();
    for (volatile int i = 0; i < 20; i++) {
    }                                       /* let PC1 recover from a phase-2 LED */
    if (!(PORT_IN(PORTB) & 1u)) {
        matrix_acc |= 1u << (scan_col * 2);
    }
    if (!(PORT_IN(PORTC) & (1u << 1))) {
        matrix_acc |= 1u << (scan_col * 2 + 1);
    }
    if (++scan_col == 16) {
        scan_col = 0;
        matrix_pass_done(matrix_acc);
        matrix_acc = 0;
    }
    sr_write((u16)~(1u << scan_col));

    led_phase = (led_phase + 1) % 3;
    if (led_seg_mask[led_phase]) {
        segments_set(led_seg_mask[led_phase]);
        phase_on(led_phase);
    }
}

void smcpad_led_set(int led, int on)
{
    if (led < 0 || led >= SMCPAD_NUM_BUTTONS) {
        return;
    }
    if (on) {
        led_on |= 1u << led;
    } else {
        led_on &= ~(1u << led);
    }
    u8 masks[3] = { 0, 0, 0 };
    for (int i = 0; i < SMCPAD_NUM_BUTTONS; i++) {
        if (led_on & (1u << i)) {
            masks[led_pos[i] >> 4] |= 1u << (led_pos[i] & 0xF);
        }
    }
    led_seg_mask[0] = masks[0];
    led_seg_mask[1] = masks[1];
    led_seg_mask[2] = masks[2];
}

int smcpad_button(int b)
{
    return b >= 0 && b < SMCPAD_NUM_BUTTONS && (buttons_state & (1u << b));
}

int32_t smcpad_encoder(int e)
{
    return e >= 0 && e < SMCPAD_NUM_ENCODERS ? encoder_count[e] : 0;
}

uint32_t smcpad_matrix_raw(void)
{
    return matrix_now;
}

/* ------------------------------------------------------------------ pads (ADC chain) */

#define PAD_ON          150                 /* the stock firmware's threshold */
#define PAD_OFF         80
#define ATTACK_ROUNDS   3                   /* rounds the peak is looked for after the threshold */

enum { PAD_IDLE, PAD_ATTACK, PAD_HELD };

static volatile u16 pad_raw[SMCPAD_NUM_PADS];   /* by pad */
static u8 pad_state[SMCPAD_NUM_PADS], pad_rounds[SMCPAD_NUM_PADS];
static u16 pad_peak[SMCPAD_NUM_PADS];
static u8 adc_pos;
static volatile u32 adc_rounds;

static void mux_select(u32 m)
{
    if (m & 1) PORT_OUT(PORTB) |= 1u << 2; else PORT_OUT(PORTB) &= ~(1u << 2);
    if (m & 2) PORT_OUT(PORTB) |= 1u << 3; else PORT_OUT(PORTB) &= ~(1u << 3);
    if (m & 4) PORT_OUT(PORTC) |= 1u << 4; else PORT_OUT(PORTC) &= ~(1u << 4);
}

static void adc_start(u32 pos)
{
    u32 con = 0b110;                        /* clock divider, as the SDK's adc_sample() */

    con |= 0x3u << 12;                      /* start delay (x 8 ADC clocks): the mux settles */
    con |= (pos < 8 ? AD_CH_PA4 : AD_CH_PA6) << 8;
    con |= BIT(3) | BIT(5) | BIT(6);        /* as adc_sample(): ?, interrupt enable, clear */
    JL_ADC->CON = con;
    JL_ADC->CON |= BIT(4);                  /* enable */
    JL_ADC->CON |= BIT(6);                  /* start */
}

static void pad_sample(u32 pad, u32 v)
{
    pad_raw[pad] = (u16)v;
    switch (pad_state[pad]) {
    case PAD_IDLE:
        if (v > PAD_ON) {
            pad_state[pad] = PAD_ATTACK;
            pad_peak[pad] = (u16)v;
            pad_rounds[pad] = 0;
        }
        break;
    case PAD_ATTACK:
        if (v > pad_peak[pad]) {
            pad_peak[pad] = (u16)v;
        }
        if (++pad_rounds[pad] >= ATTACK_ROUNDS) {
            u32 vel = (pad_peak[pad] - PAD_ON) * 127 / (1023 - PAD_ON);

            ev_push(SMCPAD_EV_PAD_ON, (u8)pad, (s16)(vel < 1 ? 1 : vel > 127 ? 127 : vel));
            pad_state[pad] = PAD_HELD;
        }
        break;
    case PAD_HELD:
        if (v < PAD_OFF) {
            ev_push(SMCPAD_EV_PAD_OFF, (u8)pad, 0);
            pad_state[pad] = PAD_IDLE;
        }
        break;
    }
}

___interrupt
static void hw_adc_isr(void)
{
    u32 v = JL_ADC->RES;
    u32 pos = adc_pos;

    JL_ADC->CON = BIT(6);                   /* clear pending, stop */
    JL_ADC->CON = 0;
    pad_sample(adc_to_pad[pos], v);
    pos = (pos + 1) % SMCPAD_NUM_PADS;
    if (pos == 0) {
        adc_rounds++;
    }
    adc_pos = (u8)pos;
    mux_select(pos & 7);
    adc_start(pos);
}

uint16_t smcpad_pad_raw(int pad)
{
    return pad >= 0 && pad < SMCPAD_NUM_PADS ? pad_raw[pad] : 0;
}

uint32_t smcpad_pad_rounds(void)
{
    return adc_rounds;
}

/* ------------------------------------------------------------------ pad RGB */

#define SPI1_CON        REG32(0x1E1D00)
#define SPI1_BAUD       REG32(0x1E1D04)
#define SPI1_ADR        REG32(0x1E1D0C)
#define SPI1_CNT        REG32(0x1E1D10)
#define LED_BYTES       548

static u32 rgb[SMCPAD_NUM_PADS];            /* by chain position, 0xRRGGBB */
static u8 rgb_buf[LED_BYTES] __attribute__((aligned(4)));

void smcpad_rgb_set(int pad, uint32_t color)
{
    if (pad >= 0 && pad < SMCPAD_NUM_PADS) {
        rgb[pad_to_led[pad]] = color & 0xFFFFFF;
    }
}

void smcpad_rgb_show(void)
{
    static u8 sending;
    u32 clk = clk_get("spi");

    /* the previous frame still going out (~0.75 ms): until its pending bit; SPI1_CNT does not
     * read back the bytes left */
    for (u32 n = 0; sending && (SPI1_CON & BIT(15)) == 0 && n < 1000000; n++) {
    }
    sending = 1;
    memset(rgb_buf, 0, sizeof(rgb_buf));
    for (u32 i = 0; i < SMCPAD_NUM_PADS; i++) {
        u32 c = rgb[i];
        u32 grb = ((c >> 8) & 0xFF) << 16 | ((c >> 16) & 0xFF) << 8 | (c & 0xFF);
        u8 *p = rgb_buf + 2 + 24 * i;

        for (int b = 23; b >= 0; b--) {
            *p++ = (grb >> b) & 1 ? 0xFC : 0xC0;
        }
    }
    SPI1_BAUD = clk / 12000000 ? clk / 12000000 - 1 : 0;     /* SPI clock = clk / (2 * (BAUD + 1)) */
    SPI1_CON |= BIT(14);                    /* clear pending */
    SPI1_ADR = (u32)rgb_buf;
    SPI1_CNT = LED_BYTES;                   /* DMA: returns at once */
}

/* ------------------------------------------------------------------ microseconds
 * TIMER3 counts the 24 MHz oscillator up to PRD (one 250 us tick), so micros = ticks * 250 +
 * CNT / 24. A wrap whose interrupt has not run yet (interrupts masked, or a higher priority
 * one running) shows as the pending bit with a small CNT. Before the timer runs: jiffies. */
#define TICK_US     250u
#define TICK_CNT    (24000000u / 4000u)

/* millis: ticks / 4 over the 64-bit count (wraps after 49.7 days, as on Arduino) */
uint32_t smcpad_millis(void)
{
    extern unsigned long jiffies_half_msec(void);
    u32 lo, hi;

    if (!hw_running) {
        return jiffies_half_msec() >> 1;
    }
    do {
        hi = hw_ticks_hi;
        lo = hw_ticks;
    } while (hi != hw_ticks_hi);
    return lo >> 2 | hi << 30;
}

uint32_t smcpad_micros(void)
{
    extern unsigned long jiffies_half_msec(void);
    u32 t, c, pend;

    if (!hw_running) {
        return jiffies_half_msec() * 500u;
    }
    do {
        t = hw_ticks;
        c = JL_TIMER3->CNT;
        pend = JL_TIMER3->CON & BIT(15);
    } while (t != hw_ticks);
    if (pend && c < TICK_CNT / 2) {
        t++;
    }
    return t * TICK_US + c / 24u;
}

/* Short waits (bit-banging) run with interrupts off, so they are exact; longer ones can be
 * stretched by interrupts and higher-priority tasks, as on any RTOS-based core. */
void smcpad_delay_us(uint32_t us)
{
    uint32_t start;

    if (us <= 50 && hw_running) {
        /* count the timer directly: 24 counts per us, wrapping at TICK_CNT */
        u32 last, elapsed = 0, target = us * 24;

        local_irq_disable();
        last = JL_TIMER3->CNT;
        while (elapsed < target) {
            u32 c = JL_TIMER3->CNT;

            elapsed += c >= last ? c - last : c + TICK_CNT - last;
            last = c;
        }
        local_irq_enable();
        return;
    }
    start = smcpad_micros();
    while (smcpad_micros() - start < us) {
    }
}

/* ------------------------------------------------------------------ start */

void smcpad_hw_begin(void)
{
    static u8 started;
    static const u32 outputs_b[] = { IO_PORTB_02, IO_PORTB_03, IO_PORTB_04, IO_PORTB_05, IO_PORTB_06, IO_PORTB_07 };
    static const u32 led_pins[] = { IO_PORTA_03, IO_PORTC_02, IO_PORTC_03,
                                    IO_PORTA_05, IO_PORTA_02, IO_PORTA_00, IO_PORTA_01 };

    if (started) {
        return;
    }
    started = 1;

    /* 595s and 4051 select, all low (/OE on) */
    for (u32 i = 0; i < ARRAY_SIZE(outputs_b); i++) {
        gpio_set_pull_up(outputs_b[i], 0);
        gpio_set_pull_down(outputs_b[i], 0);
        gpio_direction_output(outputs_b[i], 0);
    }
    gpio_set_pull_up(IO_PORTC_04, 0);
    gpio_set_pull_down(IO_PORTC_04, 0);
    gpio_direction_output(IO_PORTC_04, 0);
    /* sense lines: inputs, pull-up */
    gpio_set_direction(IO_PORTB_00, 1); gpio_set_die(IO_PORTB_00, 1); gpio_set_pull_up(IO_PORTB_00, 1);
    gpio_set_direction(IO_PORTC_01, 1); gpio_set_die(IO_PORTC_01, 1); gpio_set_pull_up(IO_PORTC_01, 1);
    /* button LEDs: everything off (high) */
    for (u32 i = 0; i < ARRAY_SIZE(led_pins); i++) {
        gpio_set_pull_up(led_pins[i], 0);
        gpio_set_pull_down(led_pins[i], 0);
        gpio_direction_output(led_pins[i], 1);
    }
    /* pad COMs: analog */
    gpio_set_direction(IO_PORTA_04, 1); gpio_set_die(IO_PORTA_04, 0);
    gpio_set_pull_up(IO_PORTA_04, 0); gpio_set_pull_down(IO_PORTA_04, 0);
    gpio_set_direction(IO_PORTA_06, 1); gpio_set_die(IO_PORTA_06, 0);
    gpio_set_pull_up(IO_PORTA_06, 0); gpio_set_pull_down(IO_PORTA_06, 0);
    /* RGB: PB1 output low, SPI1 on port A as the stock firmware */
    gpio_set_pull_up(IO_PORTB_01, 0);
    gpio_set_pull_down(IO_PORTB_01, 0);
    gpio_direction_output(IO_PORTB_01, 0);
    JL_IOMAP->CON1 &= ~BIT(4);
    SPI1_CON = 0x4021;
    smcpad_rgb_show();                      /* all off */

    /* the matrix / LED tick: TIMER3, 24 MHz oscillator, 250 us */
    sr_write(0xFFFF);
    request_irq(IRQ_TIME3_IDX, 2, hw_tick_isr, 0);
    JL_TIMER3->CON = BIT(14);
    JL_TIMER3->CNT = 0;
    JL_TIMER3->PRD = TICK_CNT;
    JL_TIMER3->CON = BIT(0) | BIT(3) | BIT(14);
    hw_running = 1;

    /* the pads: take the ADC over from the SDK's scan (occupy mode stops it), then chain */
    adc_enter_occupy_mode(AD_CH_PA4);
    for (u32 n = 0; (JL_ADC->CON & BIT(4)) && n < 100000; n++) {
    }
    request_irq(IRQ_SARADC_IDX, 2, hw_adc_isr, 0);
    adc_pos = 0;
    mux_select(0);
    adc_start(0);
}
