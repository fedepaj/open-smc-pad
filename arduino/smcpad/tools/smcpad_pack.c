/*
 * SMC-PAD sketch packager: ELF -> app.bin -> app area for flash 0x9000 (what the bootloader's DFU
 * alternate 0 takes). The C version of smcpad_pack.py, so the Arduino platform needs no Python;
 * the two produce the same bytes.
 *
 *   smcpad_pack <sketch.elf> <out.bin> --template <app_template.bin> [--size]
 *
 * app.bin is what fw/build.sh makes: .text, .data, .data_code, then the overlays with their
 * trailing zero bytes removed (JieLi's remove_tailing_zeros) -- the crt0 copies .data and
 * .data_code from absolute flash addresses, so the order and sizes must be exactly these.
 * The app area is the JieLi directory (9 entries; the special ones -- VM, PRCT, BTIF ... -- from
 * the template), app.bin, cfg_tool.bin (the template's tail).
 *
 * Build: cc -O2 -o smcpad_pack smcpad_pack.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENTRY       0x1E08120u              /* app.bin at flash 0x9120 (application area 0x9000) */
#define AREA_MAX    (0x71000u - 0x9000u)
#define DIR_SIZE    0x120u

static const char *const overlays[] = {"aec", "wav", "ape", "flac", "m4a", "amr", "dts", "fm", "mp3", "wma"};
static const char *const known[] = {".text", ".mmu_tlb", ".boot_info", ".irq_stack", ".data", ".bss",
                                    ".data_code", ".vir_timer"};

typedef struct { uint8_t *p; size_t n; } buf_t;

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "smcpad_pack: %s%s\n", msg, arg ? arg : "");
    exit(1);
}

static buf_t slurp(const char *path)
{
    buf_t b = {0, 0};
    FILE *f = fopen(path, "rb");

    if (!f) die("cannot read ", path);
    fseek(f, 0, SEEK_END);
    b.n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b.p = malloc(b.n ? b.n : 1);
    if (fread(b.p, 1, b.n, f) != b.n) die("short read: ", path);
    fclose(f);
    return b;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* CRC16-CCITT, poly 0x1021, init 0, not reflected (the JLFS one) */
static uint16_t crc16(const uint8_t *d, size_t n)
{
    uint16_t crc = 0;

    while (n--) {
        crc ^= (uint16_t)(*d++ << 8);
        for (int i = 0; i < 8; i++) crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ---------------------------------------------------------------- ELF sections */

typedef struct { const char *name; uint32_t type, flags, addr; const uint8_t *data; uint32_t size; } section_t;

static section_t *sections;
static int nsections;

static void read_sections(const buf_t *elf)
{
    if (elf->n < 0x34 || memcmp(elf->p, "\x7f" "ELF", 4)) die("not an ELF file", 0);
    uint32_t shoff = le32(elf->p + 0x20);
    uint16_t shentsize = le16(elf->p + 0x2E), shnum = le16(elf->p + 0x30), shstrndx = le16(elf->p + 0x32);
    const uint8_t *strtab = elf->p + le32(elf->p + shoff + shstrndx * shentsize + 16);

    nsections = shnum;
    sections = calloc(shnum, sizeof(section_t));
    for (int i = 0; i < shnum; i++) {
        const uint8_t *h = elf->p + shoff + i * shentsize;
        section_t *s = &sections[i];

        s->name = (const char *)strtab + le32(h);
        s->type = le32(h + 4);
        s->flags = le32(h + 8);
        s->addr = le32(h + 12);
        s->size = le32(h + 20);
        s->data = s->type == 8 ? 0 : elf->p + le32(h + 16);     /* SHT_NOBITS */
    }
}

static section_t *section(const char *name)
{
    for (int i = 0; i < nsections; i++)
        if (!strcmp(sections[i].name, name)) return &sections[i];
    return 0;
}

static void append(buf_t *b, const uint8_t *d, size_t n)
{
    b->p = realloc(b->p, b->n + n + 1);
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}

static buf_t app_bin(void)
{
    buf_t app = {0, 0};
    static const char *const main_parts[] = {".text", ".data", ".data_code"};

    for (int i = 0; i < 3; i++) {
        section_t *s = section(main_parts[i]);

        if (!s) die("missing section ", main_parts[i]);
        if (s->data) append(&app, s->data, s->size);
    }
    for (size_t i = 0; i < sizeof(overlays) / sizeof(overlays[0]); i++) {
        char name[32];
        section_t *s;
        uint32_t n;

        snprintf(name, sizeof(name), ".overlay_%s", overlays[i]);
        s = section(name);
        if (!s || !s->data) continue;
        n = s->size;
        while (n && s->data[n - 1] == 0) n--;   /* remove_tailing_zeros: trailing 0x00 only */
        append(&app, s->data, n);
    }
    return app;
}

/* anything allocated that the core's linker script does not place is an orphan: in RAM past
 * bss_end, never cleared or loaded (a sketch global left uninitialised) */
static void check_orphans(void)
{
    int bad = 0;

    for (int i = 0; i < nsections; i++) {
        const section_t *s = &sections[i];
        int ok = !(s->flags & 2) || !strncmp(s->name, ".overlay_", 9);

        for (size_t k = 0; !ok && k < sizeof(known) / sizeof(known[0]); k++) ok = !strcmp(s->name, known[k]);
        if (!ok) {
            if (!bad++) fprintf(stderr, "sections the core's linker script does not place:");
            fprintf(stderr, " %s", s->name);
        }
    }
    if (bad) {
        fprintf(stderr, "\n");
        exit(1);
    }
}

/* ---------------------------------------------------------------- the app area */

static void entry_pack(uint8_t *out, uint16_t data_crc, uint32_t offset, uint32_t size, const uint8_t *rest)
{
    put16(out + 2, data_crc);
    put32(out + 4, offset);
    put32(out + 8, size);
    memcpy(out + 12, rest, 20);
    put16(out, crc16(out + 2, 30));
}

static buf_t app_area(const buf_t *app, const buf_t *tpl)
{
    const uint8_t *dir = tpl->p, *cfg = tpl->p + DIR_SIZE;
    size_t cfg_n = tpl->n - DIR_SIZE, app_off = DIR_SIZE, cfg_off = app_off + app->n, total;
    buf_t area;

    cfg_off += (4 - cfg_off % 4) % 4;
    total = cfg_off + cfg_n;
    area.n = total;
    area.p = malloc(total);
    memset(area.p, 0xFF, total);
    memcpy(area.p + 0x40, dir + 0x40, DIR_SIZE - 0x40);                 /* cfg placeholder + specials */
    entry_pack(area.p + 0x20, crc16(app->p, app->n), (uint32_t)app_off, (uint32_t)app->n, dir + 0x20 + 12);
    entry_pack(area.p + 0x40, crc16(cfg, cfg_n), (uint32_t)cfg_off, (uint32_t)cfg_n, dir + 0x40 + 12);
    memcpy(area.p + app_off, app->p, app->n);
    memcpy(area.p + cfg_off, cfg, cfg_n);
    entry_pack(area.p, crc16(area.p + 0x20, total - 0x20), ENTRY, (uint32_t)total, dir + 12);
    return area;
}

int main(int argc, char **argv)
{
    const char *in = 0, *out = 0, *tpl_path = 0;
    int size = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size")) size = 1;
        else if (!strncmp(argv[i], "--template=", 11)) tpl_path = argv[i] + 11;
        else if (!strcmp(argv[i], "--template") && i + 1 < argc) tpl_path = argv[++i];
        else if (!in) in = argv[i];
        else if (!out) out = argv[i];
        else die("unexpected argument ", argv[i]);
    }
    if (!in || !out || !tpl_path) die("usage: smcpad_pack <sketch.elf> <out.bin> --template <app_template.bin> [--size]", 0);

    buf_t elf = slurp(in), tpl = slurp(tpl_path);
    if (tpl.n <= DIR_SIZE) die("template too short: ", tpl_path);
    read_sections(&elf);
    section_t *text = section(".text");
    if (!text) die("no .text", 0);
    if (text->addr != ENTRY) {
        fprintf(stderr, "smcpad_pack: .text is linked at %#x, the bootloader starts applications at %#x\n",
                text->addr, ENTRY);
        return 1;
    }
    check_orphans();
    buf_t app = app_bin(), area = app_area(&app, &tpl);
    if (area.n > AREA_MAX) {
        fprintf(stderr, "smcpad_pack: sketch too big: %zu bytes, the application area holds %u\n", area.n, AREA_MAX);
        return 1;
    }
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(area.p, 1, area.n, f) != area.n) die("cannot write ", out);
    fclose(f);
    if (size)
        printf("Sketch uses %zu bytes (%zu%%) of program storage space. Maximum is %u bytes.\n", area.n,
               area.n * 100 / AREA_MAX, AREA_MAX);
    return 0;
}
