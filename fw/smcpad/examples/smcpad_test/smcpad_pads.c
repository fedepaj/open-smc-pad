/*
 * Bring-up console: the 16 FSR pads and their 16 RGB LEDs.
 *
 * Pads (stock ISR 1e22d50): two HCF4051 muxes share the select lines PB2/PB3/PC4 (bit order
 * unconfirmed: we drive S0=PB2, S1=PB3, S2=PC4 and map positions empirically); mux #1's COM
 * goes to PA4 (ADC channel 3), mux #2's to PA6 (channel 4). Position p = mux index (0..7) for
 * PA4, 8 + mux index for PA6. Sampling goes through the SDK's ADC scan (2 ms timer cycling the
 * registered channels): set the mux, let two scan rounds pass, read both channels.
 *
 * RGB LEDs (stock 1e11cba / 1e10500): a WS2812-style chain on PB1, fed by SPI1 (port A, DO =
 * PB1) by DMA at ~6 MHz, one SPI byte per LED bit: 0xFC = 1, 0xC0 = 0, MSB first, colour word
 * G<<16 | R<<8 | B, two zero bytes in front and zeros after (the reset gap), 548 bytes in all.
 *
 * Stock tables, used as the hypothesis the wizard checks (flash 0x17CF0 / 0x17D00 of the dump):
 *   ADC position -> stock pad index, stock pad index -> LED chain index.
 */
#include "system/includes.h"
#include "asm/gpio.h"
#include "asm/adc_api.h"
#include "asm/clock.h"

extern void smcpad_dbg_puts(const char *s);
extern void smcpad_dbg_kv(const char *tag, u32 value);
extern u32 smcpad_dbg_read(char *buf, u32 len);
extern void smcpad_delay_ms(u32 ms);
extern void clr_wdt(void);

#define REG32(a)        (*(volatile u32 *)(a))
#define SPI1_CON        REG32(0x1E1D00)
#define SPI1_BAUD       REG32(0x1E1D04)
#define SPI1_ADR        REG32(0x1E1D0C)
#define SPI1_CNT        REG32(0x1E1D10)

#define NPADS           16
#define LED_BYTES       548             /* as the stock firmware sends it */
#define LED_SPI_HZ      6000000         /* SPI clock = clk / (2 * (BAUD + 1)) */

static const u8 stock_adc_to_pad[NPADS] = { 2, 0, 1, 3, 15, 14, 12, 13, 6, 4, 5, 7, 8, 9, 11, 10 };
static const u8 stock_pad_to_led[NPADS] = { 11, 4, 2, 7, 10, 14, 6, 9, 0, 5, 15, 1, 13, 12, 8, 3 };

/* ------------------------------------------------------------------ RGB LEDs */

static u32 led_rgb[NPADS];                              /* 0xRRGGBB per chain index */
static u8 led_buf[LED_BYTES] __attribute__((aligned(4)));
static u8 led_ready;

static void led_init(void)
{
    u32 clk = clk_get("spi");

    gpio_set_pull_up(IO_PORTB_01, 0);
    gpio_set_pull_down(IO_PORTB_01, 0);
    gpio_direction_output(IO_PORTB_01, 0);
    JL_IOMAP->CON1 &= ~BIT(4);                          /* SPI1 on port A: DO = PB1 */
    SPI1_CON = 0x4021;                                  /* as the stock firmware */
    SPI1_BAUD = clk / (2 * LED_SPI_HZ) ? clk / (2 * LED_SPI_HZ) - 1 : 0;
    led_ready = 1;
    smcpad_dbg_kv("rgb: spi clock", clk);
    smcpad_dbg_kv("rgb: baud", SPI1_BAUD);
}

static void led_show(void)
{
    if (!led_ready) {
        led_init();
    }
    {
        u32 clk = clk_get("spi");               /* BLE start (Y) changes the clocks */

        SPI1_BAUD = clk / (2 * LED_SPI_HZ) ? clk / (2 * LED_SPI_HZ) - 1 : 0;
    }
    memset(led_buf, 0, sizeof(led_buf));
    for (u32 i = 0; i < NPADS; i++) {
        u32 c = led_rgb[i];
        u32 grb = ((c >> 8) & 0xFF) << 16 | ((c >> 16) & 0xFF) << 8 | (c & 0xFF);
        u8 *p = led_buf + 2 + 24 * i;

        for (int b = 23; b >= 0; b--) {
            *p++ = (grb >> b) & 1 ? 0xFC : 0xC0;
        }
    }
    SPI1_ADR = (u32)led_buf;
    SPI1_CNT = LED_BYTES;
    for (u32 n = 0; !(SPI1_CON & BIT(15)) && n < 1000000; n++) {
    }
    SPI1_CON |= BIT(14);                                /* clear pending */
}

static void led_all(u32 rgb)
{
    for (u32 i = 0; i < NPADS; i++) {
        led_rgb[i] = rgb;
    }
}

/* n                 all off
 * n <i> <rrggbb>    chain LED i only
 * n a <rrggbb>      all of them */
void smcpad_cmd_rgb(int all, int idx, u32 rgb)
{
    led_all(0);
    if (all) {
        led_all(rgb);
    } else if (idx >= 0 && idx < NPADS) {
        led_rgb[idx] = rgb;
    }
    led_show();
}

/* N: walk the chain, one LED at a time, 1 s each, in a colour per index group. */
void smcpad_cmd_rgb_walk(void)
{
    static const u32 colours[4] = { 0x400000, 0x004000, 0x000040, 0x303000 };

    for (u32 i = 0; i < NPADS; i++) {
        led_all(0);
        led_rgb[i] = colours[i / 4];
        led_show();
        smcpad_dbg_kv("chain LED (red 0-3, green 4-7, blue 8-11, yellow 12-15)", i);
        for (u32 t = 0; t < 10; t++) {
            char c;

            smcpad_delay_ms(100);
            if (smcpad_dbg_read(&c, 1)) {
                led_all(0);
                led_show();
                return;
            }
        }
    }
    led_all(0);
    led_show();
}

/* ------------------------------------------------------------------ pads */

static u8 pads_ready;

static void pads_init(void)
{
    static const u32 sel[3] = { IO_PORTB_02, IO_PORTB_03, IO_PORTC_04 };
    static const u32 com[2] = { IO_PORTA_04, IO_PORTA_06 };

    for (u32 i = 0; i < 3; i++) {
        gpio_set_pull_up(sel[i], 0);
        gpio_set_pull_down(sel[i], 0);
        gpio_direction_output(sel[i], 0);
    }
    gpio_direction_output(IO_PORTB_05, 0);             /* INH / OE low, as the stock init ends */
    for (u32 i = 0; i < 2; i++) {
        gpio_set_pull_up(com[i], 0);
        gpio_set_pull_down(com[i], 0);
        gpio_set_direction(com[i], 1);
        gpio_set_die(com[i], 0);                        /* analog */
    }
    adc_add_sample_ch(AD_CH_PA4);
    adc_add_sample_ch(AD_CH_PA6);
    pads_ready = 1;
}

static void mux_select(u32 m)
{
    gpio_write(IO_PORTB_02, m & 1);
    gpio_write(IO_PORTB_03, (m >> 1) & 1);
    gpio_write(IO_PORTC_04, (m >> 2) & 1);
}

/* All 16 positions: out[m] = PA4 through mux m, out[8 + m] = PA6 through mux m. */
static void pads_read(u16 *out)
{
    if (!pads_ready) {
        pads_init();
    }
    for (u32 m = 0; m < 8; m++) {
        mux_select(m);
        smcpad_delay_ms(12);                            /* two rounds of the 2 ms ADC scan */
        out[m] = adc_get_value(AD_CH_PA4);
        out[8 + m] = adc_get_value(AD_CH_PA6);
    }
}

static void put_num(char **p, u32 v, int width)
{
    char d[10];
    int n = 0;

    do {
        d[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (width-- > n) {
        *(*p)++ = ' ';
    }
    while (n) {
        *(*p)++ = d[--n];
    }
}

/* A: the 16 raw values, one line about every 100 ms, until the host sends something. */
void smcpad_cmd_pads_live(void)
{
    u16 v[NPADS];
    char line[NPADS * 5 + 4], *p;
    char c;

    smcpad_dbg_puts("pad ADC positions: PA4 mux 0..7 | PA6 mux 0..7  (any key stops)\r\n");
    while (!smcpad_dbg_read(&c, 1)) {
        pads_read(v);
        p = line;
        for (u32 i = 0; i < NPADS; i++) {
            put_num(&p, v[i], 5);
            if (i == 7) {
                *p++ = ' ';
                *p++ = '|';
            }
        }
        *p++ = '\r';
        *p++ = '\n';
        *p = 0;
        smcpad_dbg_puts(line);
        clr_wdt();
    }
}

/* W: press the pads in order; for each, which ADC position moved, and light the LED the stock
 * tables put under it. Pad order: 1 = top-left, row by row, left to right. */
void smcpad_cmd_pads_wizard(void)
{
    /* rows top to bottom: red, green, blue, yellow */
    static const u32 row_colour[4] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFC000 };
    u16 base[NPADS], v[NPADS], peak[NPADS];
    u8 found[NPADS];
    u32 acc[NPADS] = { 0 };
    char c;

    led_all(0);
    led_show();
    smcpad_dbg_puts("pad wizard: hands off the pads, measuring the rest values...\r\n");
    for (u32 r = 0; r < 4; r++) {
        pads_read(v);
        for (u32 i = 0; i < NPADS; i++) {
            acc[i] += v[i];
        }
    }
    for (u32 i = 0; i < NPADS; i++) {
        base[i] = acc[i] / 4;
    }
    smcpad_dbg_puts("press each pad firmly, then let go: 1 = top-left, row by row, left to right\r\n"
                    "(any key aborts)\r\n");

    for (u32 k = 0; k < NPADS; k++) {
        int best = -1;
        u32 best_d = 0;

        smcpad_dbg_kv("press pad", k + 1);
        while (best < 0) {
            if (smcpad_dbg_read(&c, 1)) {
                led_all(0);
                led_show();
                return;
            }
            pads_read(v);
            for (u32 i = 0; i < NPADS; i++) {
                u32 d = v[i] > base[i] ? v[i] - base[i] : base[i] - v[i];

                if (d > 120 && d > best_d) {
                    best_d = d;
                    best = (int)i;
                }
            }
        }
        found[k] = (u8)best;
        {
            u32 pad = stock_adc_to_pad[best], led = stock_pad_to_led[pad];

            smcpad_dbg_kv("  ADC position", best);
            smcpad_dbg_kv("    rest", base[best]);
            smcpad_dbg_kv("    pressed", v[best]);
            smcpad_dbg_kv("    stock pad index", pad);
            smcpad_dbg_kv("    lighting chain LED (should be under your finger)", led);
            /* the pads done so far stay on, dim, in their row's colour: the grid fills up in
             * order, and a LED in the wrong place stands out */
            for (u32 j = 0; j < k; j++) {
                led_rgb[stock_pad_to_led[stock_adc_to_pad[found[j]]]] = row_colour[j / 4] >> 3 & 0x1F1F1F;
            }
            led_rgb[led] = row_colour[k / 4];
            led_show();
        }
        /* wait for the release, keeping the peak */
        peak[k] = v[best];
        for (u32 quiet = 0; quiet < 3; ) {
            pads_read(v);
            u32 d = v[best] > base[best] ? v[best] - base[best] : base[best] - v[best];

            if (v[best] > peak[k]) {
                peak[k] = v[best];
            }
            /* released: back near the rest value, or under a quarter of the peak (a resting
             * silicone pad keeps some pressure on the FSR) */
            quiet = (d < 60 || v[best] < peak[k] / 4) ? quiet + 1 : 0;
            if (smcpad_dbg_read(&c, 1)) {
                return;
            }
        }
    }

    smcpad_dbg_puts("summary: pad (1 = top-left) -> ADC position, stock pad, chain LED, rest, peak\r\n");
    for (u32 k = 0; k < NPADS; k++) {
        char line[64], *p = line;

        put_num(&p, k + 1, 3);
        *p++ = ' '; *p++ = '-'; *p++ = '>';
        put_num(&p, found[k], 4);
        put_num(&p, stock_adc_to_pad[found[k]], 4);
        put_num(&p, stock_pad_to_led[stock_adc_to_pad[found[k]]], 4);
        put_num(&p, base[found[k]], 6);
        put_num(&p, peak[k], 6);
        *p++ = '\r'; *p++ = '\n'; *p = 0;
        smcpad_dbg_puts(line);
    }
    /* the whole grid in row colours for 5 s: one last look */
    for (u32 j = 0; j < NPADS; j++) {
        led_rgb[stock_pad_to_led[stock_adc_to_pad[found[j]]]] = row_colour[j / 4];
    }
    led_show();
    smcpad_delay_ms(5000);
    led_all(0);
    led_show();
}

/* K: play the pads over BLE-MIDI (console Y first). Note 36 + stock pad index (bottom-left =
 * 0, MPC style), velocity from the ADC value, the pad's LED lit while it is held. Slow -- a
 * full scan takes ~100 ms through the SDK's ADC scan -- but enough to test the chain. */
extern int blemidi_send(const u8 *msg, u32 len);

void smcpad_cmd_pads_midi(void)
{
    u16 v[NPADS];
    u8 on[NPADS] = { 0 };
    char c;

    smcpad_dbg_puts("pads -> BLE-MIDI notes 36..51 (any key stops)\r\n");
    led_all(0);
    led_show();
    while (!smcpad_dbg_read(&c, 1)) {
        int changed = 0;

        pads_read(v);
        for (u32 pos = 0; pos < NPADS; pos++) {
            u32 pad = stock_adc_to_pad[pos], led = stock_pad_to_led[pad];
            u8 msg[3];

            if (!on[pos] && v[pos] > 150) {
                u32 vel = v[pos] / 7;

                msg[0] = 0x90;
                msg[1] = (u8)(36 + pad);
                msg[2] = (u8)(vel > 127 ? 127 : vel < 1 ? 1 : vel);
                blemidi_send(msg, 3);
                on[pos] = 1;
                led_rgb[led] = 0x00FF40;
                changed = 1;
                smcpad_dbg_kv("note on", 36 + pad);
            } else if (on[pos] && v[pos] < 80) {
                msg[0] = 0x80;
                msg[1] = (u8)(36 + pad);
                msg[2] = 0;
                blemidi_send(msg, 3);
                on[pos] = 0;
                led_rgb[led] = 0;
                changed = 1;
            }
        }
        if (changed) {
            led_show();
        }
        clr_wdt();
    }
    led_all(0);
    led_show();
}
