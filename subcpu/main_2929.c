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
extern int  scd_cd_status(void);                           
extern int  scd_track_info(int track);                     
extern void scd_cdda_play(int track);                       
extern int  scd_bios_call(int fn, int d1, void *a0);       

/* 68K VBlank Tick Macro. Sub-CPU accesses Main CPU Comm Registers via 0xFF8000 */
#define GET_TICKS read_long(0xFF801C)

#define SUB_MAIN_FLAG  0xFF800E
#define SUB_SUB_FLAG   0xFF800F

static void fourcc(char *out, unsigned int v)
{
    int i, ok = 1;
    for (i = 0; i < 4; i++) {
        unsigned char c = (v >> (24 - 8 * i)) & 0xFF;
        if (c < 0x20 || c > 0x7E) ok = 0;
    }
    if (ok) {
        for (i = 0; i < 4; i++) out[i] = (v >> (24 - 8 * i)) & 0xFF;
        out[4] = 0;
    } else {
        sprintf(out, "%08X", v);
    }
}

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
#define MD_CMD_SAFE_CART_XFER 26   
#define MD_CMD_SAFE_CART_XFER_W 27     
#define MD_CMD_CHILLY_MEMCPY  32         
#define MD_CMD_CHILLY_RV_MEMCPY 33       
#define MD_CMD_BRAM_PUT       28         
#define MD_CMD_BRAM_TO_FB     29         
#define MD_CMD_BRAM_GET       30         
#define MD_CMD_SET_VDP_REG    34         /* KOBO K19: hw_md.s set_vdp_reg - one VDP register word */

/* KOBO K19 - GREEN DIAGNOSTIC TEXT.  1 = printed as before (C on the pad shows/hides it).
   0 = print_diag() prints nothing at all: every print_diag() call stays in the code, so
   setting 1 again brings all the text back.  (Chilly's white 32X start-up messages come
   from hw_md.s and are not affected.) */
#define KOBO_DIAG_TEXT        0          /* K20: off (dev request) - set 1 to debug */

/* K30 - BOOT TEXT.  0 = the Genesis display is switched OFF at the very start of main(), so
   neither the green text nor Chilly's white 32X start-up messages appear (dev request): the
   SEGA logo is followed by the splash.  The text is still written to video RAM, so setting 1
   (or pressing C in the game) shows it.  Set 1 when debugging a start-up problem. */
#define KOBO_BOOT_TEXT        0

#define LEGACY_68K_CART 0

static uint32_t saved_lba = 0;
/* K10: length of the file opened by CMD 38, kept here (like saved_lba) so CMD 50 never has
   to trust COMM8 - in K9 CMD 50 read a wrong length from COMM8 and read 0 sectors. */
static uint32_t saved_len = 0;
static int quiet = 0;      
#define SLIDESHOW_DEBUG 1

static const char *find_sh2_marker(const char *p, int len)
{
    int i;
    for (i = 0; i + 8 <= len; i++)
        if (p[i] == 'S' && p[i+1] == 'H' && p[i+2] == '2' && p[i+3] == '-' && p[i+4] == 'V')
            return p + i + 5;
    return 0;
}

void print_diag(char *temp, const char *msg, int x, int y) {
    if (quiet) return;
#if !KOBO_DIAG_TEXT
    return;                                   /* K19: green text switched off (see top) */
#endif
    strcpy(temp, msg);
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, x, y);
    switch_banks();
}

/* =====================================================================================
 * KOBO STORAGE MODE - must match d32xr-master/sh2_main.c KOBO_NOCART
 *
 *   1 = NO-CART (development / Fusion): the RAM cart is never touched - not even the
 *       start-up cart check (accessing a missing cart crashes the console).  The SH2 reads
 *       KOBOGFX.BIN straight from CD with CMD 38 (open) + CMD 50 (one 32 KB chunk into the
 *       frame buffer).  Runs in Fusion and on the console, cart plugged in or not.
 *   0 = CART (real hardware + MicroNut99 cart): KOBOGFX.BIN is stored in the cart at
 *       offset 2 MB with CMD 38 + CMD 46 (Chilly's rules, verified), the SH2 reads it at
 *       0x22000000.  Proven on hardware.
 * ===================================================================================== */
#define KOBO_NOCART     1

#define CART_TARGET     0              /* MicroNut99 cart layout (only used when KOBO_NOCART 0) */
#define KOBO_PACK_NAME  "KOBOGFX.BIN"
#define KOBO_PACK_SLOT  16             /* cart mode: 16 * CART_SLOT = 2 MB into the 4 MB cart */
#define CART_WINDOW_READS    1   
#define CART_VERIFY_READBACK 1   

#define CART_IMG_BASE   0x000000
#define CART_SLOT       0x20000
#define CMD_CHUNK_TO_CART 46
#define CMD_CART_TO_FB    47             
static int cur_img = 0;
static int g_nexc = 0;                   

#define XB(off)   (0x0C0100 + (off))       
#define XM(off)   (0x200100 + (off))       
#define PB(off)   (*(volatile uint8_t *)XB(off))

static void cx(int src, int dst, int len)  
{
    do_md_cmd3(MD_CMD_SAFE_CART_XFER, src, dst, len);
}

static int safe_cart_present(char *temp, char *tbuf)
{
    int ok;
#if CART_TARGET == 1
    uint8_t id, orig, rb;
    PB(0) = 0; PB(1) = 0; PB(2) = 0x5A; PB(3) = 0; PB(4) = 1; PB(5) = 0;
    switch_banks();                              
    cx(0x400001, XM(0), 1);                      
    cx(0x600001, XM(1), 1);                      
    cx(XM(4), 0x7FFFFF, 1);                      
    cx(XM(2), 0x600001, 1);                      
    cx(0x600001, XM(3), 1);                      
    cx(XM(1), 0x600001, 1);                      
    cx(XM(5), 0x7FFFFF, 1);                      
    switch_banks();                              
    id = PB(0); orig = PB(1); rb = PB(3);
    ok = (rb == 0x5A);
    sprintf(tbuf, "BRAM ID:%02X (%uKB) OLD:%02X        ", id, (id & 0x80) ? 0 : (0x2000u << (id & 7)) >> 10, orig);
    print_diag(temp, tbuf, 2, 23);
    sprintf(tbuf, "BRAM @600001: WROTE 5A READ %02X  ", rb);
#else
    uint16_t orig = 0, via9 = 0xFFFF, via4 = 0xFFFF;
    PB(0) = 0x5A; PB(1) = 0xA5; PB(2) = 0; PB(3) = 0;
#if CART_WINDOW_READS
    do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15104, 0);                       
    orig = (uint16_t)do_md_cmd1(MD_CMD_READ_32X_16, 0x900000);
#endif
    switch_banks();
    do_md_cmd3(MD_CMD_CHILLY_RV_MEMCPY, 0x400000, XM(0), 2);          
    switch_banks();                                                     
    via4 = 0xFFFF;                                                      
#if CART_WINDOW_READS
    via9 = (uint16_t)do_md_cmd1(MD_CMD_READ_32X_16, 0x900000);        
    PB(0) = orig >> 8; PB(1) = orig & 0xFF;
    switch_banks();
    do_md_cmd3(MD_CMD_CHILLY_RV_MEMCPY, 0x400000, XM(0), 2);          
    switch_banks();
#endif
#if !CART_WINDOW_READS && !CART_VERIFY_READBACK
    ok = 1;                                                             
#else
    ok = (via4 == 0x5AA5 || via9 == 0x5AA5);
#endif
    sprintf(tbuf, "OLD:%04X  VIA 400000:%04X      ", orig, via4);
    print_diag(temp, tbuf, 2, 23);
    sprintf(tbuf, "WROTE 5AA5  VIA 900000:%04X    ", via9);
#endif
    print_diag(temp, tbuf, 2, 24);
    print_diag(temp, ok ? "SAFE CART: FOUND               " : "SAFE CART: NOT FOUND           ", 2, 25);
    return ok;
}

static uint16_t cdda_track_word;       

static int bios_wait_ready(unsigned int max_ticks)
{
    (void)max_ticks;
    while (scd_cd_status() & 0x80000000) {
        bump_fm();
    }
    return 1;
}

static int cdda_start(int track, int *st_out)
{
    int st = 0;
    bios_wait_ready(0);
    scd_bios_call(0x0089, 0, 0);            
    scd_bios_call(0x0002, 0, 0);            
    scd_bios_call(0x0085, 0x0400, 0);       
    cdda_track_word = (uint16_t)track;
    scd_bios_call(0x0013, 0, &cdda_track_word);  
    st = scd_cd_status();
    *st_out = st; 
    return 1;
}

int main(void)
{
    write_byte(0x8003, read_byte(0x8003) & ~1);
    while (read_byte(0x8003) & 2);
#if !KOBO_BOOT_TEXT
    do_md_cmd1(MD_CMD_SET_VDP_REG, 0x8134);   /* K30: Genesis display off (md_init_hw left it on) */
#endif

    char *temp = (char *)0x0C0000;
    char tbuf[64];
    uint32_t build_counter = 0;
    int cart_ok = 0;

    print_diag(temp, "BUILD: KOBO K30B MENU + PANEL       ", 2, 0);
    print_diag(temp, "LOADING 32XCD ENGINE...           ", 2, 2);

    {
        const char *m = find_sh2_marker(sh2_app_start, sh2_app_length);
        if (m) sprintf(tbuf, "32X PAYLOAD: %d BYTES SH2-V%c%c        ", sh2_app_length, m[0], m[1]);
        else   sprintf(tbuf, "32X PAYLOAD: %d BYTES SH2:NONE       ", sh2_app_length);
        print_diag(temp, tbuf, 2, 4);
    }

    memcpy(temp, sh2_app_start, sh2_app_length);
    switch_banks();
    {   
        int r32 = do_md_cmd2(MD_CMD_INIT_32X, 0x200000, sh2_app_length);
        g_nexc = 0;
        char a[12], b[12], c[12];
        fourcc(a, read_long(0xFF8014));   
        fourcc(b, read_long(0xFF8018));   
        fourcc(c, read_long(0xFF801C));   
        switch_banks();
        
        sprintf(tbuf, "INIT32X:%d 1ST:%s              ", r32, a);
        print_diag(temp, tbuf, 2, 19);
        sprintf(tbuf, "END C0:%s C4:%s          ", b, c);
        print_diag(temp, tbuf, 2, 20);
        if (r32 < 0) {
            print_diag(temp, "32X INIT FAILED - STOPPED       ", 2, 21);
            while (1) bump_fm();
        }

        do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15124, 0x1234);

        {
            uint16_t ready = 0;
            volatile int wait = 0;
            
            {
                unsigned int t0 = GET_TICKS;
                for (wait = 0; wait < 1000000; wait++) {
                    ready = (uint16_t)do_md_cmd1(MD_CMD_READ_32X_16, 0xA1512A);
                    if (ready == 0x0AAA) break;
                    if ((GET_TICKS - t0) >= 180) break;
                }
            }
            sprintf(tbuf, "SH2 READY FOR ISOLATED TEST: %04X    ", ready);
            print_diag(temp, tbuf, 2, 19);

            if (ready != 0x0AAA) {
                print_diag(temp, "SH2 NEVER SIGNALLED READY - STOPPED", 2, 20);
                do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA1512A, 0x0BBB);   
                while (1) bump_fm();
            }
        }

#if KOBO_NOCART
        cart_ok = 0;                                   /* no-cart build: never touch the cart */
        print_diag(temp, "CART: NOT USED (NO-CART BUILD)     ", 2, 25);
#else
        cart_ok = safe_cart_present(temp, tbuf);  
#endif
        (void)cart_ok; 

        do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA1512A, 0x0BBB);   
    }

    uint16_t init_err = InitCD();

    {
        unsigned char c = read_byte(SUB_MAIN_FLAG);
        sprintf(tbuf, "POST-INITCD COMM: %02X          ", c);
        print_diag(temp, tbuf, 2, 1);
    }

    if (init_err != 0) {
    } else {
        print_diag(temp, "CD DRIVE READY                ", 2, 5);
        print_diag(temp, "BOOTING SUB-CPU...            ", 2, 6);
    }

    {   
        int st = scd_cd_status();
        int t2 = scd_track_info(2);
        sprintf(tbuf, "CD TRACKS %02X-%02X  TRACK2 %08X      ", (st >> 8) & 0xFF, st & 0xFF, t2);
        print_diag(temp, tbuf, 2, 14);
    }

    memcpy(temp, Sub_Start, Sub_End - Sub_Start);

    {
        unsigned char c = read_byte(SUB_MAIN_FLAG);
        sprintf(tbuf, "POST-MEMCPY2 COMM: %02X         ", c);
        print_diag(temp, tbuf, 2, 3);
    }

    print_diag(temp, "INITIALIZING SUB-CPU Sfx...   ", 2, 7);
    send_md_cmd0('I'); 

    {
        unsigned char c = read_byte(SUB_MAIN_FLAG);
        sprintf(tbuf, "POST-SENDMD COMM: %02X          ", c);
        print_diag(temp, tbuf, 2, 18);
    }

    print_diag(temp, "68K LISTENER ACTIVE           ", 2, 8);

    {
        unsigned char boot_comm = read_byte(SUB_MAIN_FLAG);
        sprintf(tbuf, "BOOT COMM PORT: %02X            ", boot_comm);
        print_diag(temp, tbuf, 2, 9);
    }

    print_diag(temp, "(TEXT BLINKS TO SHOW SH2 IS ALIVE)", 2, 26);

    while (1) {
        build_counter++;

        if (build_counter == 1) {
            unsigned char first_iter_comm = read_byte(SUB_MAIN_FLAG);
            sprintf(tbuf, "ITER1 COMM PORT: %02X           ", first_iter_comm);
            print_diag(temp, tbuf, 2, 15);
        }

        bump_fm();

        uint16_t cmd = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15120);

        /* KOBO K5 - PAD FAST PATH.  WHY: the SH2 asks for the pad (CMD 48) every frame and
           only waits a short time.  The rest of this loop (3 more register reads + redrawing
           row 10, each a round trip through the 68K) took longer than that, so the SH2 gave
           up and scrolled with pad = 0.  Answer CMD 48 first and start the next pass. */
        if (cmd == 48) {
            uint16_t pad = GET_PAD(0);
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15128, pad);   /* reply in COMM8 */
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);     /* done           */
            continue;
        }

        uint16_t comm4 = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15124);
        uint16_t sh2_state = do_md_cmd1(MD_CMD_READ_32X_16, 0xA1512C);
        uint32_t ckpt = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128); 

        {   /* KOBO K5: redraw row 10 only when something changed (it used to cost a full
               68K text update on every pass) */
            static uint16_t l_cmd = 0xFFFF, l_c4 = 0xFFFF, l_st = 0xFFFF;
            static uint32_t l_ck = 0xFFFFFFFF;
            if (cmd != l_cmd || comm4 != l_c4 || sh2_state != l_st || ckpt != l_ck) {
                l_cmd = cmd; l_c4 = comm4; l_st = sh2_state; l_ck = ckpt;
                sprintf(tbuf, "32X:%X 32XReg:%X STATE:%X CKPT:%04X     ", cmd, comm4, sh2_state, (unsigned int)(ckpt >> 16));
                print_diag(temp, tbuf, 2, 10);
            }
        }

        if (comm4 == 0x0EEE) {
            print_diag(temp, "SH2 REACHED THE LDC LINE              ", 2, 18); 
        } else if (comm4 != 0x1234 && comm4 != 0x0000) {
            print_diag(temp, "SH2 NEVER EVEN REACHED THIS FILE      ", 2, 18); 
        }

        if (cmd == 38) { 
            int img_idx = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            char img_name[24];
            cur_img = img_idx < 0 ? 0 : img_idx;         
            /* KOBO M1: file number 16 = the Kobo graphics pack (tools/kobo_gfx.py).
               CMD 46 then stores it at cart offset 16 * CART_SLOT = 2 MB. */
            if (img_idx == 16) { strcpy(img_name, KOBO_PACK_NAME); cur_img = KOBO_PACK_SLOT; }
            else if (img_idx <= 0) strcpy(img_name, "IMAGE.RAW");
            else sprintf(img_name, "IMAGE%d.RAW", img_idx);

            sprintf(tbuf, "OPENING %s...              ", img_name);
            print_diag(temp, tbuf, 2, 11);

            unsigned char entry_comm = read_byte(SUB_MAIN_FLAG);
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
                saved_len = 0;
            } else {
                uint32_t length = (uint32_t)(file_info >> 32);
                saved_lba = (uint32_t)(file_info & 0xFFFFFFFF);
                saved_len = length;                                   /* K10: for CMD 50 */
                sprintf(tbuf, "%s %lu BYTES%s          ", img_name, (unsigned long)length,
                        length > 128000 ? " (TOO TALL)" : " OK");
                print_diag(temp, tbuf, 2, 12);
                do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15128, length); 
            }
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0); 
        }
        else if (cmd == CMD_CHUNK_TO_CART) {
            int chunk = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            int length = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128);
            int off = chunk * 32768;
            int n = 0, bytes, p, k, bad = 0;
            uint32_t coff = CART_IMG_BASE + (uint32_t)cur_img * CART_SLOT + (uint32_t)off;

            if (off < length) n = (length - off + 2047) / 2048;
            if (n > 16) n = 16;
            bytes = n * 2048;

#if CART_TARGET == 1
            if (n > 0) {
                volatile uint8_t *flag = (volatile uint8_t *)0x0DF000;   
                scd_sub_read((void *)0x0C0000, saved_lba + chunk * 16, n);
                flag[0] = 1; flag[1] = 0;
                switch_banks();                                   
                cx(0x21F000, 0x7FFFFF, 1);                        
                for (p = 0; p < bytes; p += 4096)
                    do_md_cmd3(MD_CMD_BRAM_PUT, 0x200000 + p, 0x600001 + 2 * (coff + p),
                               bytes - p < 4096 ? bytes - p : 4096);
                cx(0x21F001, 0x7FFFFF, 1);                        
                for (p = 0; p < bytes; p += 4096)                 
                    do_md_cmd3(MD_CMD_BRAM_GET, 0x600001 + 2 * (coff + p), 0x208000 + p,
                               bytes - p < 4096 ? bytes - p : 4096);
                switch_banks();                                   
                {
                    volatile uint8_t *disc = (volatile uint8_t *)0x0C0000;
                    volatile uint8_t *cart = (volatile uint8_t *)0x0C8000;
                    for (k = 0; k < bytes; k++) {
                        if (disc[k] != cart[k]) {
                            sprintf(tbuf, "MISMATCH C%d +%05X DISC %02X CART %02X ", chunk, k,
                                    disc[k], cart[k]);
                            print_diag(temp, tbuf, 2, 17);
                            bad = 1;
                            break;
                        }
                    }
                }
            }
#else
            if (n > 0) {
                scd_sub_read((void *)0x0C0000, saved_lba + chunk * 16, n);   
                switch_banks();                                   
                for (p = 0; p < bytes; p += 0x4000)
                    do_md_cmd3(MD_CMD_CHILLY_RV_MEMCPY, 0x400000 + coff + p, 0x200000 + p,
                               bytes - p < 0x4000 ? bytes - p : 0x4000);
#if CART_VERIFY_READBACK
                do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15104, (int)(coff >> 20) & 3);   
                for (p = 0; p < bytes; p += 0x4000)
                    do_md_cmd3(MD_CMD_CHILLY_MEMCPY, 0x208000 + p,
                               0x900000 + ((coff + p) & 0xFFFFF),
                               bytes - p < 0x4000 ? bytes - p : 0x4000);
#endif
                switch_banks();                                   
#if CART_VERIFY_READBACK
                {   
                    volatile uint8_t *disc = (volatile uint8_t *)0x0C0000;
                    volatile uint8_t *cart = (volatile uint8_t *)0x0C8000;
                    for (k = 0; k < bytes; k++) {
                        if (disc[k] != cart[k]) {
                            sprintf(tbuf, "MISMATCH C%d +%05X DISC %02X CART %02X ", chunk, k,
                                    disc[k], cart[k]);
                            print_diag(temp, tbuf, 2, 17);
                            bad = 1;
                            break;
                        }
                    }
                }
#endif
#if CART_WINDOW_READS
                {   
                    int wbad = 0;
                    uint16_t want = 0, got = 0;
                    uint32_t o = 0;
                    do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15104, (int)(coff >> 20) & 3);   
                    for (k = 0; k < 4 && !wbad; k++) {
                        o = (uint32_t)(((bytes - 2) / 3 * k) & ~1);
                        want = *(volatile uint16_t *)(0x0C0000 + o);
                        got  = (uint16_t)do_md_cmd1(MD_CMD_READ_32X_16, 0x900000 + ((coff + o) & 0xFFFFF));
                        if (got != want) wbad = 1;
                    }
                    if (wbad)
                        sprintf(tbuf, "32X WIN C%d +%05lX W%04X R%04X   ", chunk, (unsigned long)o, want, got);
                    else
                        sprintf(tbuf, "32X WIN C%d 900000: OK            ", chunk);
                    print_diag(temp, tbuf, 2, 16);
                }
#else
                print_diag(temp, "32X WIN: OFF (V72 TEST)          ", 2, 16);
#endif
            }
#endif
            sprintf(tbuf, "CART I%d C%d @%06lX %s          ", cur_img, chunk,
#if CART_TARGET == 1
                    (unsigned long)(0x600001 + 2 * coff),      
#else
                    (unsigned long)(0x400000 + coff),          
#endif
                    n == 0 ? "EMPTY" : bad ? "BAD" : (CART_VERIFY_READBACK ? "OK" : "NOVFY"));
            print_diag(temp, tbuf, 2, 13);
            do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA1512C, bad);
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == CMD_CART_TO_FB) {
            int slot_chunk = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            int slot = slot_chunk >> 8;
            int chunk = slot_chunk & 0xFF;
            int length = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128);
            int off = chunk * 32768, bytes = length - off, p;
            uint32_t coff = CART_IMG_BASE + (uint32_t)slot * CART_SLOT + (uint32_t)off;
            
            if (bytes > 32768) bytes = 32768;
            for (p = 0; p < bytes; p += 4096)
                do_md_cmd3(MD_CMD_BRAM_TO_FB, 0x600001 + 2 * (coff + p), 0x840200 + off + p,
                           bytes - p < 4096 ? bytes - p : 4096);
            
            sprintf(tbuf, "CART->FB S%d C%d %d BYTES          ", slot, chunk, bytes > 0 ? bytes : 0);
            print_diag(temp, tbuf, 2, 16);
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 44) {
#if !SLIDESHOW_DEBUG
            if (!quiet) { do_md_cmd0(MD_CMD_CLEAR_B); do_md_cmd0(MD_CMD_CLEAR_A); quiet = 1; }
#endif
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 50 || cmd == 51) {
            /* K15: CMD 51 = same as CMD 50, but chunk c goes to frame buffer byte
               0x200 + c * 32 KB (a whole picture in order - the splash screen). */
            /* KOBO NO-CART - CMD 50 "file chunk -> frame buffer (raw)".
               COMM2 = chunk number (32 KB each).  File length: saved_len from CMD 38 (K10 -
               no longer read from COMM8).  CD -> Word RAM (Sub-CPU) -> frame buffer at 68K 0x840200 via MD
               command 25 copy_words with flag 0 = plain copy (flag 1 would set bit 15 on every
               word, which is only right for 15-bit pictures).  The SH2 has handed the frame
               buffer to the 68K (FM=0) before sending this. */
            int chunk = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            int length = (int)saved_len;           /* K10: from CMD 38, NOT from COMM8 */
            int off = chunk * 32768;
            int n = 0;
            if (off < length) n = (length - off + 2047) / 2048;   /* sectors, max 16 = 32 KB */
            if (n > 16) n = 16;
            uint16_t w0 = 0;
            /* (K13 cleared Word RAM priority mode here; K13 showed it was never on - register 05
               before and after - so the write is removed in K15: no needless pokes at the Word
               RAM mode register on real hardware.  The real K12 fault was zero words not landing
               in the frame buffer - fixed on the SH2 side by clearing the area first.) */
            if (n > 0) {
                scd_sub_read((void *)0x0C0000, saved_lba + chunk * 16, n);   /* CD -> Word RAM */
                switch_banks();                                               /* -> 68K side    */
                do_md_cmd4(MD_CMD_COPY_WORDS, 0x200000,
                           cmd == 51 ? 0x840200 + off : 0x852000, n * 2048, 0);
                /* K30: CMD 50 now lands OFF-SCREEN (frame buffer byte 0x12000, past the
                   visible 320x224 picture which ends at 0x11A00): loading never overwrites
                   what is on screen (the panel stays intact).  SH2 side: FB_CHUNK_BUF. */
                switch_banks();                                               /* -> Sub side    */
                w0 = *(volatile uint16_t *)0x0C0000;   /* first word read from CD (read it
                                                          BEFORE print_diag reuses this buffer) */
            }
            /* K11 reply for the SH2's diagnostic strip (lines 1-3): chunk | sectors | length>>8.
               Written before COMM0 is cleared; the SH2 reads it only after that. */
            /* K14 reply: chunk | sectors | the word in WORD RAM at offset 0x210 of this chunk, read
               after the copy (the Sub-CPU's buffer at 0x0C0000).  For chunk 0 that is the high
               word of the tile set offset and must be 0000.  Correct here but wrong in the
               frame buffer (strip line T) = the 68K -> frame buffer copy loses zero data.
               (K13: priority mode was never on - register read 05 before and after.)        */
            {
                uint16_t wr = (n > 0) ? *(volatile uint16_t *)(0x0C0000 + 0x210) : 0xEEEE;
                do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA1512C, (chunk << 24) | (n << 16) | wr);
            }
            (void)w0;
            sprintf(tbuf, "CD->FB CHUNK %d  %d SECTORS          ", chunk, n);
            print_diag(temp, tbuf, 2, 16);
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 52) {
            /* KOBO K19 - CMD 52 "Genesis text layer on/off": COMM2 = 1 on, 0 off.
               Switches the Genesis display itself (VDP register 1 bit 6) - off blanks ALL
               Genesis text (green diagnostics and Chilly's white messages) to black and the
               32X picture shows alone; on brings everything back (it stays in video RAM).
               WHY: the 32X priority bit alone did not hide the text reliably (K17/K18). */
            int on = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122) & 1;
            do_md_cmd1(MD_CMD_SET_VDP_REG, on ? 0x8174 : 0x8134);
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 48) {
            /* (K5: normally handled by the fast path at the top of the loop)
               KOBO M1 - CMD 48 "read pad": reply = pad 1 bits (SEGA_CTRL_*) in COMM8.
               WHY a command: the SH2 must never poll a comm register the 68K writes
               (32X manual p.69, and the COMM6 crash).  GET_PAD(0) is the copy the 68K's
               vblank handler keeps in its comm word - no extra 68K work. */
            uint16_t pad = GET_PAD(0);
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15128, pad);
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 45) {
            int track = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            sprintf(tbuf, "CDDA: PLAY TRACK %d (REPEAT)        ", track);
            print_diag(temp, tbuf, 2, 27);
            
            {
                int st = 0;
                int tries = cdda_start(track, &st);
                sprintf(tbuf, "CDDA T%d ST %02X TRY %d%s          ", track, (st >> 24) & 0xFF,
                        tries, tries ? "" : " FAILED");
                print_diag(temp, tbuf, 2, 27);
            }
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
    }
    return 0;
}