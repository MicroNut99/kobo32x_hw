#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw_md.h"
#include "files.h"

uint16_t gen_lvl2 = 0;

void write_byte(unsigned int dst, unsigned char val) { *(volatile unsigned char *)dst = val; }
void write_word(unsigned int dst, unsigned short val) { *(volatile unsigned short *)dst = val; }
void write_long(unsigned int dst, unsigned int val) { *(volatile unsigned int *)dst = val; }
unsigned char read_byte(unsigned int src) { gen_lvl2++; return *(volatile unsigned char *)src; }
unsigned short read_word(unsigned int src) { return *(volatile unsigned short *)src; }
unsigned int read_long(unsigned int src) { return *(volatile unsigned int *)src; }
void scd_init_pcm(void) {}
void bump_fm(void) { gen_lvl2++; }

int mystrlen(const char* string) {
    int rc = 0;
    while (*(string++)) rc++;
    return rc;
}

extern int sh2_app_length;
extern char sh2_app_start[];
extern char Sub_Start[];
extern char Sub_End[];
extern uint16_t InitCD(void);
extern volatile int scd_last_timeout_code;

extern int64_t scd_open_file(const char *name);
extern void scd_read_sectors(void *ptr, int lba, int len, void (*wait)(void));
extern void scd_stream_cmd(char cmd); 
extern void scd_sub_read(void *ptr, int lba, int len);
extern int  scd_cd_status(void);                           /* scd.c: BIOS CD status (see there) */
extern int  scd_track_info(int track);                     /* scd.c: BIOS TOC entry, MMSSFFTN */
extern void scd_cdda_play(int track);                    /* scd.c: BIOS CD-audio play, repeating */
extern void scd_cd_dump(unsigned short *out);            /* scd.c: first 10 words of the BIOS status table */
extern void scd_cdda_volume(int v);                      /* scd.c: BIOS_FDRSET, bit 15 = master volume */
extern void scd_cdda_stop(void);                         /* scd.c: MSCSTOP */
extern void scd_cdda_play_mode(int track, int cmd);      /* scd.c: 0x11 MSCPLAY, 0x12 MSCPLAY1, 0x13 MSCPLAYR */   

extern int do_md_cmd0(int cmd);
extern int do_md_cmd1(int cmd, int arg1);
extern int do_md_cmd2(int cmd, int arg1, int arg2);
extern int do_md_cmd3(int cmd, int arg1, int arg2, int arg3);

#define MD_CMD_CLEAR_B        4
#define MD_CMD_CLEAR_A        8
#define MD_CMD_READ_32X_16    19
#define MD_CMD_WRITE_32X_16   20
#define MD_CMD_READ_32X_32    21
#define MD_CMD_WRITE_32X_32   22
#define MD_CMD_COPY_FROM_CART 24
#define MD_CMD_COPY_WORDS     25

/* 1 = compile the OLD commands 40/42 that let the 68000 write/read the cart window at 0x600001.
   That window is not the cart on the real console, so they are compiled out. */
#define LEGACY_68K_CART 0

static uint32_t saved_lba = 0;
static int quiet = 0;      /* set after the first image is up: nothing more is drawn on the Genesis side */

static unsigned short text_on = 1;    /* Genesis text layer shown over the picture (START toggles) */
static unsigned short prev_pad = 0;

/* Sub-CPU gate array register 0xFF8034: the CDD fader, i.e. the CD audio volume the BIOS has set (layout not
   certain to me - this build only READS it, and in the AUDIO_TEST build START also writes candidate values) */
#define GA_FADER (*(volatile unsigned short *)0xFF8034)
static unsigned short fader_wrote = 0;
static unsigned short cur_pad = 0;         /* pad value as read now, shown on row 14 */
static unsigned short shown_pad = 0xFFFF;  /* value last drawn there */
static int cd_vol = 0x400;   /* CD audio level, 0..0x400, changed with UP / DOWN on pad 1 */
static int cd_track = 2;     /* track the music command started */
static int music_on = 0;     /* set when the SH2 has started the music (cmd 45) */

/* v2.9: reading a Main-CPU register from the Sub-CPU is a full round trip through the do_md_cmd protocol - much
   heavier than the reference project's own design, where the Main CPU's own idle loop just reads its registers
   directly, no round trip at all (see the comparison in hw_md.s's cmd_loop for the difference).  We cannot fully
   match that here without a bigger rewrite (disc reads must still go through the Sub-CPU), but every round trip
   this loop does NOT need is one less chance for whatever is causing the freeze.  COMM0 (whether the SH2 has a
   new command) is read every pass, since that has to be prompt.  The other three are for the on-screen row 10
   display only - nobody's logic depends on them - so they are now only actually re-read once every 16 passes,
   the rest of the time the last values are reused.  This alone does not fix the freeze; it lowers how much
   traffic is competing with the commands that matter. */
static int diag_throttle = 0;
static uint16_t last_comm4 = 0;
static uint16_t last_sh2_state = 0;
static uint32_t last_ckpt = 0;
static unsigned int music_last = 0;   /* tick count at the last music (re)start */
static unsigned int disc_last = 0;    /* tick count when the SH2 last had the disc read (open file / piece) */


/* 1 = keep the status text on screen (shown over the picture) so you can see what the loader is doing.
   0 = clean slideshow: the text is wiped after the first picture.  Keep sh2_main.c DEBUG_OVERLAY the same. */
#define SLIDESHOW_DEBUG 1

/* 1 = AUDIO TEST build: no 32X at all.  Boots, starts CD audio track 2 by itself and then only runs the pad
   controls (A / B / C / UP / DOWN).  Load the disc in the emulator's plain SegaCD mode and listen.
   0 = the normal engine. */
#define AUDIO_TEST 0

/* 1 = start the CD music again after the LAST piece of every picture that is read from the disc while the music is
   running (reading the disc stops CD audio; the track starts over each time).  Only matters when the SH2 streams
   pictures from the disc (CART_BACKEND 0); with the RAM cart (CART_BACKEND 1) the music starts after all reads. */
#define MUSIC_RESUME 1

static const char *find_sh2_marker(const char *p, int len)
{
    int i;
    for (i = 0; i + 8 <= len; i++)
        if (p[i] == 'S' && p[i+1] == 'H' && p[i+2] == '2' && p[i+3] == '-' && p[i+4] == 'V')
            return p + i + 5;
    return 0;
}

void print_diag(char *temp, const char *msg, int x, int y) {
    if (quiet)
        return;
    strcpy(temp, msg);
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, x, y);
    switch_banks();
}

/* ---------------------------------------------------------------------------
   CART PROBE.  1 = at boot, instead of the slideshow, map what the cart window
   0x600000-0x7FFFFF really does.  0 = normal slideshow.
   One 16-bit word per 16 KB step is saved, overwritten with a pattern that is different at
   every step (so a mirrored block cannot be mistaken for a real one, and no byte is 00 or FF,
   which is what an empty bus reads), read back, and finally RESTORED.  Only the Main-CPU
   commands 19/20 (16-bit read/write at any address) are used.
   Map characters (one per 16 KB):
     #  both bytes of the word stuck        (16-bit wide RAM)
     o  only the low byte stuck             (odd-byte RAM, like the SRAM test)
     a  read back another step's pattern    (mirror / wrap-around: no new memory here)
     .  nothing stuck                       (no RAM)
   --------------------------------------------------------------------------- */
#define CART_PROBE    0
#define PROBE_BASE    0x600000
#define PROBE_STEP    0x4000
#define PROBE_POINTS  128

#if CART_PROBE
static uint16_t probe_pat(int i)
{
    return (uint16_t)(((0x40 + i) << 8) | (i + 1));
}

static void cart_probe(char *temp, char *tbuf)
{
    uint16_t orig[PROBE_POINTS];
    char map[PROBE_POINTS + 1];
    char row[40];
    int i, j, n16 = 0, nodd = 0, nal = 0, nnone = 0, first = -1, last = -1;

    do_md_cmd0(MD_CMD_CLEAR_B);
    do_md_cmd0(MD_CMD_CLEAR_A);
    print_diag(temp, "CART PROBE  0x600000 - 0x7FFFFF", 2, 1);
    print_diag(temp, "CONTENTS ARE RESTORED AFTERWARDS", 2, 2);

    for (i = 0; i < PROBE_POINTS; i++)
        orig[i] = (uint16_t)do_md_cmd1(MD_CMD_READ_32X_16, PROBE_BASE + i * PROBE_STEP);

    for (i = 0; i < PROBE_POINTS; i++)
        do_md_cmd2(MD_CMD_WRITE_32X_16, PROBE_BASE + i * PROBE_STEP, probe_pat(i));

    for (i = 0; i < PROBE_POINTS; i++) {
        uint16_t v = (uint16_t)do_md_cmd1(MD_CMD_READ_32X_16, PROBE_BASE + i * PROBE_STEP);
        uint16_t p = probe_pat(i);
        char c = '.';

        if (v == p) {
            c = '#';
        } else if ((v & 0xFF) == (p & 0xFF)) {
            c = 'o';
        } else {
            for (j = 0; j < PROBE_POINTS; j++)
                if (j != i && (v & 0xFF) == (probe_pat(j) & 0xFF)) { c = 'a'; break; }
        }
        map[i] = c;
        if (c == '#') n16++;
        else if (c == 'o') nodd++;
        else if (c == 'a') nal++;
        else nnone++;
        if (c == '#' || c == 'o') {
            if (first < 0) first = i;
            last = i;
        }
    }

    for (i = 0; i < PROBE_POINTS; i++)              /* put everything back */
        do_md_cmd2(MD_CMD_WRITE_32X_16, PROBE_BASE + i * PROBE_STEP, orig[i]);

    map[PROBE_POINTS] = 0;
    for (i = 0; i < 4; i++) {
        memcpy(row, map + i * 32, 32);
        row[32] = 0;
        print_diag(temp, row, 2, 4 + i);
    }
    print_diag(temp, "ONE CHAR = 16 KB, ROW = 512 KB", 2, 9);
    print_diag(temp, "# 16-BIT   o ODD BYTE ONLY", 2, 10);
    print_diag(temp, "a MIRROR OF ANOTHER SPOT  . NONE", 2, 11);

    sprintf(tbuf, "16BIT:%d ODD:%d MIRROR:%d NONE:%d", n16, nodd, nal, nnone);
    print_diag(temp, tbuf, 2, 13);
    if (first >= 0) {
        sprintf(tbuf, "RAM FROM %06X TO %06X", PROBE_BASE + first * PROBE_STEP,
                PROBE_BASE + last * PROBE_STEP + PROBE_STEP - 1);
        print_diag(temp, tbuf, 2, 14);
    } else {
        print_diag(temp, "NO RAM FOUND IN THIS WINDOW", 2, 14);
    }
    print_diag(temp, "PROBE DONE - SET CART_PROBE 0", 2, 16);
}
#endif

#if AUDIO_TEST
/* row 18: the fader register as read now, and the last value START wrote into it */
static void show_fader(char *temp, char *tbuf)
{
    sprintf(tbuf, "FADER FF8034 %04X  WROTE %04X   ", GA_FADER, fader_wrote);
    print_diag(temp, tbuf, 2, 18);
}
#endif

/* row 14: first-last track from the BIOS, the CD audio level, and the pad value as this program reads it
   (A 0040  B 0010  C 0020  UP 0001  DOWN 0002  START 0080), so every button press is visible */
static void show_cd_line(char *temp, char *tbuf)
{
    int st = scd_cd_status();
    sprintf(tbuf, "CD %02X-%02X VOL %04X PAD %04X        ", (st >> 8) & 0xFF, st & 0xFF, cd_vol, cur_pad);
    print_diag(temp, tbuf, 2, 14);
    shown_pad = cur_pad;
#if AUDIO_TEST
    show_fader(temp, tbuf);
#endif
}

/* apply cd_vol to both the master and the plain volume */
static void apply_cd_vol(void)
{
    scd_cdda_volume(0x8000 | cd_vol);
    scd_cdda_volume(cd_vol);
}

/* after a (re)start: the BIOS status table now and 2 s later (rows 15 / 17: if the words change the drive is
   running; first byte 01 = playing) and the status line (row 27) */
static void cd_report(char *temp, char *tbuf, int track)
{
    unsigned short d0[10], d1[10];
    unsigned int t0;
    int st;

    scd_cd_dump(d0);
    t0 = GET_TICKS;
    {   /* limited by a pass count as well, so a stopped tick counter cannot hang the Sub-CPU here */
        unsigned long spins;
        for (spins = 0; spins < 400000UL && (GET_TICKS - t0) < 120; spins++)
            bump_fm();
    }
    scd_cd_dump(d1);
    st = scd_cd_status();
    sprintf(tbuf, "CDDA TRACK %d  STATUS %02X (01=PLAYING)", track, (st >> 24) & 0xFF);
    print_diag(temp, tbuf, 2, 27);
    sprintf(tbuf, "S0 %04X %04X %04X %04X %04X %04X", d0[0], d0[1], d0[2], d0[3], d0[4], d0[5]);
    print_diag(temp, tbuf, 2, 15);
    sprintf(tbuf, "S1 %04X %04X %04X %04X %04X %04X", d1[0], d1[1], d1[2], d1[3], d1[4], d1[5]);
    print_diag(temp, tbuf, 2, 17);
#if AUDIO_TEST
    show_fader(temp, tbuf);
#endif
}

/* v2.4  MUSIC WATCHDOG.  Reading the disc (a new picture, or a file that is not there) stops CD audio, and
   only some of those places start it again.  So whenever the command loop is idle and the music is meant to be
   on, look at the BIOS status: if the drive is not playing and the last (re)start is more than about 4 s ago,
   start the track again - but only once the SH2 has left the disc alone for about a second, so it never
   restarts in the middle of loading a picture.  Returns the BIOS status byte (01 = playing) for the live line on row 18. */
static int music_watchdog(void)
{
    int st = (scd_cd_status() >> 24) & 0xFF;
    if (music_on && st != 0x01 && (GET_TICKS - music_last) > 240 && (GET_TICKS - disc_last) > 60) {
        scd_cdda_play(cd_track);
        music_last = GET_TICKS;
    }
    return st;
}

/* pad 1.  START toggles the Genesis text (the SH2 applies it via COMM6).
   CD audio test controls:  A = (re)start the track (repeat)   B = send the volume again
   C = stop   UP / DOWN = level +/- 0x80
   LEFT = start with MSCPLAY (0x11)   RIGHT = start with MSCPLAY1 (0x12, once) */
static void pad_controls(char *temp, char *tbuf)
{
    unsigned short pad = GET_PAD(0);
    unsigned short press = pad & ~prev_pad;          /* buttons that just went down */
    cur_pad = pad;
    if (press & SEGA_CTRL_START) {
#if AUDIO_TEST
        {   /* no text layer to toggle in this build: START writes candidate values into the fader register */
            static const unsigned short cand[4] = { 0x4000, 0x0400, 0x7FF0, 0xFFF0 };
            static int ci = 0;
            fader_wrote = cand[ci];
            ci = (ci + 1) & 3;
            GA_FADER = fader_wrote;
            show_fader(temp, tbuf);
        }
#else
        text_on ^= 1;
        do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15126, 0xAA00 | text_on);
#endif
    }
    if (press & SEGA_CTRL_A) {
        scd_cdda_play(cd_track);
        music_on = 1;
        music_last = GET_TICKS;
        show_cd_line(temp, tbuf);
        cd_report(temp, tbuf, cd_track);
    }
    if (press & SEGA_CTRL_LEFT) {
        scd_cdda_play_mode(cd_track, 0x0011);
        music_on = 1;
        music_last = GET_TICKS;
        cd_report(temp, tbuf, cd_track);
    }
    if (press & SEGA_CTRL_RIGHT) {
        scd_cdda_play_mode(cd_track, 0x0012);
        music_on = 1;
        music_last = GET_TICKS;
        cd_report(temp, tbuf, cd_track);
    }
    if (press & SEGA_CTRL_B) {
        apply_cd_vol();
        show_cd_line(temp, tbuf);
    }
    if (press & SEGA_CTRL_C) {
        music_on = 0;                                   /* stopped on purpose: the watchdog leaves it alone */
        scd_cdda_stop();
    }
    if (press & SEGA_CTRL_UP) {
        cd_vol += 0x80;
        if (cd_vol > 0x400) cd_vol = 0x400;
        apply_cd_vol();
        show_cd_line(temp, tbuf);
    }
    if (press & SEGA_CTRL_DOWN) {
        cd_vol -= 0x80;
        if (cd_vol < 0) cd_vol = 0;
        apply_cd_vol();
        show_cd_line(temp, tbuf);
    }
    prev_pad = pad;
    if (pad != shown_pad)                            /* a button went down or up: show the raw pad value */
        show_cd_line(temp, tbuf);
}

int main(void)
{
    write_byte(0x8003, read_byte(0x8003) & ~1);
    while (read_byte(0x8003) & 2);

    char *temp = (char *)0x0C0000;
    char tbuf[64];
    uint32_t build_counter = 0;

    print_diag(temp, "32XCD BOOT ENGINE  V2.9           ", 2, 0);
    print_diag(temp, "LOADING 32XCD ENGINE...", 2, 2);

    {
        const char *m = find_sh2_marker(sh2_app_start, sh2_app_length);
        if (m)
            sprintf(tbuf, "32X PAYLOAD: %d BYTES SH2-V%.5s        ", sh2_app_length, m);
        else
            sprintf(tbuf, "32X PAYLOAD: %d BYTES SH2:NONE       ", sh2_app_length);
        print_diag(temp, tbuf, 2, 4);
    }

#if !AUDIO_TEST
    memcpy(temp, sh2_app_start, sh2_app_length);
    switch_banks();
    {
        int r32 = do_md_cmd2(MD_CMD_INIT_32X, 0x200000, sh2_app_length);
        switch_banks();
        if (r32 < 0) {
            /* the 32X did not come up (r32 > 0 only means an SH2 did not write M_OK / S_OK - see hw_md.s v2.0 - and is not a failure).  Say why, and keep showing what the SH2s left in the COMM registers
               (5F43445F = "_CD_" is what the Main CPU wrote; 4D5F4F4B = "M_OK" / 535F4F4B = "S_OK" are the answers) */
            const char *why = (r32 == -2) ? "NO 32X (MARS ID)     " :
                              (r32 == -3) ? "SH2 SDRAM ERROR      " :
                              (r32 == -4) ? "MASTER SH2 NO M_OK   " :
                              (r32 == -5) ? "SLAVE SH2 NO S_OK    " : "UNKNOWN              ";
            int go = 0;
            sprintf(tbuf, "32X INIT FAILED %d  %s", r32, why);
            print_diag(temp, tbuf, 2, 8);
            print_diag(temp, "-4 MASTER -5 SLAVE -2 NO32X -3 SDRAM", 2, 15);
            print_diag(temp, "A SEND _CD_ AGAIN   B CLEAR COMM0/4  ", 2, 16);
            print_diag(temp, "C CONTINUE ANYWAY                    ", 2, 17);
            while (!go) {
                unsigned int t0;
                sprintf(tbuf, "COMM0  %08lX  (5F43445F = _CD_)   ", (unsigned long)do_md_cmd1(MD_CMD_READ_32X_32, 0xA15120));
                print_diag(temp, tbuf, 2, 9);
                sprintf(tbuf, "COMM4  %08lX                       ", (unsigned long)do_md_cmd1(MD_CMD_READ_32X_32, 0xA15124));
                print_diag(temp, tbuf, 2, 10);
                sprintf(tbuf, "COMM8  %08lX                       ", (unsigned long)do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128));
                print_diag(temp, tbuf, 2, 11);
                sprintf(tbuf, "COMM12 %08lX                       ", (unsigned long)do_md_cmd1(MD_CMD_READ_32X_32, 0xA1512C));
                print_diag(temp, tbuf, 2, 12);
                sprintf(tbuf, "A100 %04X  A102 %04X  A104 %04X     ",
                        (unsigned)do_md_cmd1(MD_CMD_READ_32X_16, 0xA15100), (unsigned)do_md_cmd1(MD_CMD_READ_32X_16, 0xA15102),
                        (unsigned)do_md_cmd1(MD_CMD_READ_32X_16, 0xA15104));
                print_diag(temp, tbuf, 2, 13);
                sprintf(tbuf, "A106 %04X  A180 %04X  A18A %04X     ",
                        (unsigned)do_md_cmd1(MD_CMD_READ_32X_16, 0xA15106), (unsigned)do_md_cmd1(MD_CMD_READ_32X_16, 0xA15180),
                        (unsigned)do_md_cmd1(MD_CMD_READ_32X_16, 0xA1518A));
                print_diag(temp, tbuf, 2, 14);
                t0 = GET_TICKS;
                while (!go && (GET_TICKS - t0) < 30) {         /* about half a second, then the numbers are read again */
                    unsigned short pad = GET_PAD(0);
                    unsigned short press = pad & ~prev_pad;
                    prev_pad = pad;
                    bump_fm();
                    if (press & SEGA_CTRL_A)                    /* offer the SH2s the "_CD_" handshake once more */
                        do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15120, 0x5F43445F);
                    if (press & SEGA_CTRL_B) {                  /* what a good init does last: clear COMM0 / COMM4 */
                        do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15120, 0);
                        do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15124, 0);
                    }
                    if (press & SEGA_CTRL_C) {                  /* ... and carry on as if the init had worked */
                        do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15120, 0);
                        do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15124, 0);
                        go = 1;
                    }
                }
            }
        }
    }
#endif

    uint16_t init_err = InitCD();

    {
        unsigned char c = read_byte(0xA1200F);
        sprintf(tbuf, "POST-INITCD COMM: %02X          ", c);
        print_diag(temp, tbuf, 2, 1);
    }

    if (init_err != 0) {
        // error handling
    } else {
        print_diag(temp, "CD DRIVE READY                ", 2, 5);
        print_diag(temp, "BOOTING SUB-CPU...            ", 2, 6);
    }

    show_cd_line(temp, tbuf);      /* first-last track (a disc with an audio track 2 shows 01-02) and the level */

#if AUDIO_TEST
    print_diag(temp, "AUDIO TEST - NO 32X                  ", 2, 8);
    print_diag(temp, "A PLAY  B VOL  C STOP  UP/DN LEVEL  ", 2, 25);
    print_diag(temp, "LEFT PLAY-ON  RIGHT PLAY-ONCE       ", 2, 24);
    print_diag(temp, "START  WRITE VALUES TO THE FADER    ", 2, 23);
    cd_track = 2;
    scd_cdda_play(cd_track);
    cd_report(temp, tbuf, cd_track);
    while (1) {
        bump_fm();
        pad_controls(temp, tbuf);
    }
#endif

    memcpy(temp, Sub_Start, Sub_End - Sub_Start);

    {
        unsigned char c = read_byte(0xA1200F);
        sprintf(tbuf, "POST-MEMCPY2 COMM: %02X         ", c);
        print_diag(temp, tbuf, 2, 3);
    }

    print_diag(temp, "INITIALIZING SUB-CPU Sfx...   ", 2, 7);

    send_md_cmd0('I'); 

    {
        unsigned char c = read_byte(0xA1200F);
        sprintf(tbuf, "POST-SENDMD COMM: %02X          ", c);
        print_diag(temp, tbuf, 2, 18);
    }

    print_diag(temp, "68K LISTENER ACTIVE           ", 2, 8);

    {
        unsigned char boot_comm = read_byte(0xA1200F);
        sprintf(tbuf, "BOOT COMM PORT: %02X            ", boot_comm);
        print_diag(temp, tbuf, 2, 9);
    }

    print_diag(temp, "START = SHOW / HIDE THIS TEXT     ", 2, 26);

#if CART_PROBE
    cart_probe(temp, tbuf);
    while (1) {
        bump_fm();
    }
#endif

    do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15124, 0x1234);
    do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15126, 0xAA00 | text_on);   /* COMM6: 0xAA01 = text shown, 0xAA00 = hidden */

    while (1) {
        build_counter++;

        /* ==============================================================
         * FIXED DELAY TEST (Guess: 10,000 iterations)
         * Simulates the time eaten by button-mashing round-trips.
         * ============================================================== */
        {
            volatile int delay_cnt;
            for (delay_cnt = 0; delay_cnt < 10000; delay_cnt++) {
                __asm__ __volatile__("nop");
            }
        }

        if (build_counter == 1) {
            unsigned char first_iter_comm = read_byte(0xA1200F);
            sprintf(tbuf, "ITER1 COMM PORT: %02X           ", first_iter_comm);
            print_diag(temp, tbuf, 2, 15);
        }

        bump_fm();

        pad_controls(temp, tbuf);

        {   /* v2.4 (row 18): TICKS must keep counting, PAD must follow the buttons (A 0040  B 0010  C 0020  UP 0001
               DOWN 0002  START 0080), CD is the BIOS drive status (01 = the music is playing).  The music watchdog
               runs here too. */
            int cdst = music_watchdog();
            sprintf(tbuf, "TICKS %08lX PAD %04X CD %02X   ", (unsigned long)GET_TICKS, (unsigned)GET_PAD(0), cdst);
            print_diag(temp, tbuf, 2, 18);
            /* v2.6: v2.5's HBEAT (0xFF8020) was a bad idea - that address is the Sub-CPU's own comm-flags
               argument register (the "arg0" slot every do_md_cmd2/3/4 writes before a command), not free RAM,
               and it is NOT shared with the Main CPU at all.  It just showed the last argument value the
               SUB-CPU itself wrote there (0x00200000, from the MD_CMD_INIT_32X call at the very start) and
               could never have proven anything about the Main CPU.  Sorry - my mistake.
               Real test: build_counter (declared above, already incremented once per pass of THIS loop, purely
               in Sub-CPU RAM) vs the do_md_cmd1 round trip a few lines below, which is a genuine Main-CPU call.
               If build_counter freezes at the SAME point as TICKS/PAD, the freeze is inside this loop, at or
               before that round trip (most likely: the Main CPU has stopped answering it). */
            sprintf(tbuf, "LOOP %08lX                     ", (unsigned long)build_counter);
            print_diag(temp, tbuf, 2, 19);
        }

        uint16_t cmd = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15120);      /* every pass: this one has to be prompt */
        uint16_t comm4 = last_comm4;
        uint16_t sh2_state = last_sh2_state;
        uint32_t ckpt = last_ckpt;
        if (++diag_throttle >= 16) {                                   /* v2.9: display-only, so throttled */
            diag_throttle = 0;
            comm4 = last_comm4 = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15124);
            sh2_state = last_sh2_state = do_md_cmd1(MD_CMD_READ_32X_16, 0xA1512C);
            ckpt = last_ckpt = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128);
        }

        sprintf(tbuf, "32X:%X 32XReg:%X STATE:%X CKPT:%lu     ", cmd, comm4, sh2_state, (unsigned long)ckpt);
        print_diag(temp, tbuf, 2, 10);

        if (cmd == 38) { 
            /* The SH2 puts the file number in COMM2: 0 = IMAGE.RAW, n = IMAGEn.RAW */
            int img_idx = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            char img_name[24];
            if (img_idx <= 0)
                strcpy(img_name, "IMAGE.RAW");
            else
                sprintf(img_name, "IMAGE%d.RAW", img_idx);

            sprintf(tbuf, "OPENING %s...              ", img_name);
            print_diag(temp, tbuf, 2, 11);

            unsigned char entry_comm = read_byte(0xA1200F);
            if (entry_comm != 0) {
                sprintf(tbuf, "ENTRY COMM FAULT: %02X        ", entry_comm);
                print_diag(temp, tbuf, 2, 12);
            }

            int64_t file_info = scd_open_file(img_name);
            
            if (file_info < 0) {
                sprintf(tbuf, "ERR: %s NOT FOUND          ", img_name);
                print_diag(temp, tbuf, 2, 12);

                sprintf(tbuf, "TIMEOUT: %d (0=none 1=cmd 2=ack)", scd_last_timeout_code);
                print_diag(temp, tbuf, 2, 13);

                do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15128, 0); 
            } else {
                uint32_t length = (uint32_t)(file_info >> 32);
                saved_lba = (uint32_t)(file_info & 0xFFFFFFFF);

                /* a picture is 320 x 200 x 2 = 128000 bytes; anything longer is cut after 200 rows */
                sprintf(tbuf, "%s %lu BYTES%s          ", img_name, (unsigned long)length,
                        length > 128000 ? " (TOO TALL)" : " OK");
                print_diag(temp, tbuf, 2, 12);

                do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15128, length); 
            }
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0); 
            disc_last = GET_TICKS;
        }
#if LEGACY_68K_CART
        else if (cmd == 40) { 
            print_diag(temp, "READING IMAGE.RAW DATA...     ", 2, 13);

            int length = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128);
            uint32_t sh2_dest = do_md_cmd1(MD_CMD_READ_32X_32, 0xA1512C);
            
            uint32_t m68k_dest = 0x600001 + ((sh2_dest & 0x00FFFFFF) << 1);
            int sectors = (length + 2047) / 2048;
            
            scd_read_sectors((void*)m68k_dest, saved_lba, sectors, NULL);

            print_diag(temp, "IMAGE.RAW STREAM COMPLETE     ", 2, 14);

            if (!quiet)
            {
                /* VERIFY: 16 sample points spread over the WHOLE file, 8 bytes each.  Cart contents
                   (read back through Main-CPU cmd 19) are compared with a fresh read of the same
                   place on the disc.  Row 17 = how many of the 16 match and which one failed first;
                   row 24 = what the cart and the disc held at that first bad point. */
                volatile uint8_t *chk = (volatile uint8_t *)0x0C0100;
                uint32_t base_even = m68k_dest - 1;
                uint32_t total = (uint32_t)length;
                int s, k, ok = 0, first_bad = -1;
                uint8_t bad_cart0 = 0, bad_cart1 = 0, bad_disc0 = 0, bad_disc1 = 0;

                for (s = 0; s < 16; s++) {
                    uint32_t pos = (((total - 8) / 15) * s) & ~1u;
                    uint8_t cv[8];
                    int same = 1;

                    for (k = 0; k < 8; k++)
                        cv[k] = do_md_cmd1(MD_CMD_READ_32X_16, base_even + 2 * (pos + k)) & 0xFF;

                    scd_sub_read((void *)0x0C0100, saved_lba + pos / 2048, 2);
                    for (k = 0; k < 8; k++)
                        if (chk[(pos % 2048) + k] != cv[k]) same = 0;

                    if (same) {
                        ok++;
                    } else if (first_bad < 0) {
                        first_bad = s;
                        bad_cart0 = cv[0]; bad_cart1 = cv[1];
                        bad_disc0 = chk[pos % 2048]; bad_disc1 = chk[(pos % 2048) + 1];
                    }
                }
                sprintf(tbuf, "VERIFY %d/16 OK  1STBAD:%d          ", ok, first_bad);
                print_diag(temp, tbuf, 2, 17);
                if (first_bad >= 0) {
                    sprintf(tbuf, "BAD S%d CART=%02X%02X DISC=%02X%02X        ",
                            first_bad, bad_cart0, bad_cart1, bad_disc0, bad_disc1);
                    print_diag(temp, tbuf, 2, 24);
                }
            }

            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0); 
        }
        else if (cmd == 42) {
            print_diag(temp, "UNPACKING CART TO FRAMEBUF... ", 2, 15);

            int length = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128);
            uint32_t sh2_src_offset = do_md_cmd1(MD_CMD_READ_32X_32, 0xA1512C);
            uint32_t m68k_src = 0x600001 + ((sh2_src_offset & 0x00FFFFFF) << 1);
            
            do_md_cmd3(MD_CMD_COPY_FROM_CART, m68k_src, 0x840200, length);
            
            /* First image only: wipe the Genesis text layers, then stay quiet so nothing is
               drawn on the Genesis side while the slideshow runs. */
#if !SLIDESHOW_DEBUG
            if (!quiet) {
                do_md_cmd0(MD_CMD_CLEAR_B);
                do_md_cmd0(MD_CMD_CLEAR_A);
                quiet = 1;
            }
#endif

            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0); 
        }
#endif
        else if (cmd == 43) {
            /* The SH2 asks for one 32 KB piece of the opened file (COMM2 = piece number,
               COMM8 = file length): disc -> Word RAM -> frame buffer, hidden frame, from word 0x100.
               The SH2 then writes that piece into the RAM cart itself. */
            int chunk = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            int length = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128);
            int direct = do_md_cmd1(MD_CMD_READ_32X_32, 0xA1512C);   /* COMM12: 1 = piece goes straight to its
                                                                        place in the frame buffer, 0 = staging area */
            int off = chunk * 32768;
            int n = 0;

            if (off < length)
                n = (length - off + 2047) / 2048;
            if (n > 16)
                n = 16;

            sprintf(tbuf, "CHUNK %d: %d SECTORS               ", chunk, n);
            print_diag(temp, tbuf, 2, 13);

            /* v2.2: row 17 shows how far each piece got (the last letter on screen is where it stopped):
               A = piece asked for, B = disc read done, C = Word RAM handed over, D = copied into the frame buffer,
               E = answered to the SH2 (COMM0 cleared) */
            sprintf(tbuf, "CH%d A ASKED  %d SEC          ", chunk, n);
            print_diag(temp, tbuf, 2, 17);
            if (n > 0) {
                scd_sub_read((void *)0x0C0000, saved_lba + chunk * 16, n);
                sprintf(tbuf, "CH%d B DISC READ DONE       ", chunk);
                print_diag(temp, tbuf, 2, 17);
                switch_banks();
                sprintf(tbuf, "CH%d C WORD RAM HANDED OVER ", chunk);
                print_diag(temp, tbuf, 2, 17);
                do_md_cmd4(MD_CMD_COPY_WORDS, 0x200000, 0x840200 + (direct ? chunk * 32768 : 0), n * 2048,
                           direct ? 1 : 0);   /* direct pieces are made opaque here (bit 15) */
                sprintf(tbuf, "CH%d D COPIED TO FRAME       ", chunk);
                print_diag(temp, tbuf, 2, 17);
            }

            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
            sprintf(tbuf, "CH%d E ANSWERED SH2          ", chunk);
            print_diag(temp, tbuf, 2, 17);
            disc_last = GET_TICKS;
#if MUSIC_RESUME
            /* the disc was read, so CD audio has stopped: after the last piece of a picture start it again */
            if (music_on && (chunk + 1) * 32768 >= length) {
                scd_cdda_play(cd_track);
                music_last = GET_TICKS;
            }
#endif
        }
        else if (cmd == 44) {
            /* The SH2 is about to show the first picture: wipe the Genesis text (clean mode only) */
#if !SLIDESHOW_DEBUG
            if (!quiet) {
                do_md_cmd0(MD_CMD_CLEAR_B);
                do_md_cmd0(MD_CMD_CLEAR_A);
                quiet = 1;
            }
#endif
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 45) {
            /* The SH2 has every picture in the cart: start the music (COMM2 = track number).
               From here on nothing reads the disc, so the music is not interrupted. */
            int track = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            cd_track = track;
            music_on = 1;
            music_last = GET_TICKS;
            print_diag(temp, "A PLAY  B VOL  C STOP  UP/DN LEVEL  ", 2, 25);
            print_diag(temp, "LEFT PLAY-ON  RIGHT PLAY-ONCE       ", 2, 24);
            sprintf(tbuf, "CDDA: PLAY TRACK %d (REPEAT)        ", track);
            print_diag(temp, tbuf, 2, 27);
            scd_cdda_play(track);
            cd_report(temp, tbuf, track);
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
    }
    return 0;
}