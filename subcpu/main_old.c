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

#define LEGACY_68K_CART 0

static uint32_t saved_lba = 0;
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
    strcpy(temp, msg);
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, x, y);
    switch_banks();
}

#define CART_TEST_ADDR  0x400001

/* =========================================================================
 * HARDWARE ARBITRATED CART ACCESS
 * =========================================================================
 * The Backup RAM Cart is an 8-bit device living strictly on ODD addresses 
 * within the 0x400000 - 0x4FFFFF window. 
 * We must use the hardware RV bit arbitration (MD_CMD_SAFE_CART_XFER) for 
 * BOTH reading and writing to prevent 68K/SH2 bus collisions.
 * ========================================================================= */
static int safe_cart_present(char *temp, char *tbuf)
{
    /* Use 8-bit pointers to ensure we only transfer bytes */
    volatile uint8_t *pattern = (volatile uint8_t *)0x0C0100;
    volatile uint8_t *back    = (volatile uint8_t *)0x0C0101;
    *pattern = 0x5A;
    *back = 0;

    /* Write 1 byte to odd address 0x400001 natively via 0x400000 bus */
    do_md_cmd3(MD_CMD_SAFE_CART_XFER, 0x200100, CART_TEST_ADDR, 1);

    /* Read 1 byte back from the exact same bus block using RV arbitration */
    do_md_cmd3(MD_CMD_SAFE_CART_XFER, CART_TEST_ADDR, 0x200101, 1);

    int ok = (*back == 0x5A);
    sprintf(tbuf, "SAFE CART @400001: WROTE 5A READ %02X  ", *back);
    print_diag(temp, tbuf, 2, 24);
    print_diag(temp, ok ? "SAFE CART: FOUND               " : "SAFE CART: NOT FOUND           ", 2, 25);
    return ok;
}

#define CART_PROBE       0
#define PROBE_BASE_ADDR  0x400000
#define PROBE_STEP       0x4000
#define PROBE_POINTS     128

#if CART_PROBE
static uint8_t probe_pat(int i) { return (uint8_t)(0x40 + i); }
static void cart_probe(char *temp, char *tbuf)
{
    volatile uint8_t *xfer_buf = (volatile uint8_t *)0x0C0104;
    uint8_t orig[PROBE_POINTS];
    char map[PROBE_POINTS + 1];
    char row[40];
    int i, j, n8 = 0, nal = 0, nnone = 0, first = -1, last = -1;

    do_md_cmd0(MD_CMD_CLEAR_B); do_md_cmd0(MD_CMD_CLEAR_A);
    print_diag(temp, "CART PROBE  0x400001 - 0x5FFFFF", 2, 1);
    print_diag(temp, "CONTENTS ARE RESTORED AFTERWARDS", 2, 2);

    for (i = 0; i < PROBE_POINTS; i++) {
        *xfer_buf = 0;
        do_md_cmd3(MD_CMD_SAFE_CART_XFER, PROBE_BASE_ADDR + i * PROBE_STEP + 1, 0x200104, 1);
        orig[i] = *xfer_buf;
    }

    for (i = 0; i < PROBE_POINTS; i++) {
        *xfer_buf = probe_pat(i);
        do_md_cmd3(MD_CMD_SAFE_CART_XFER, 0x200104, PROBE_BASE_ADDR + i * PROBE_STEP + 1, 1);
    }

    for (i = 0; i < PROBE_POINTS; i++) {
        *xfer_buf = 0;
        do_md_cmd3(MD_CMD_SAFE_CART_XFER, PROBE_BASE_ADDR + i * PROBE_STEP + 1, 0x200104, 1);
        uint8_t v = *xfer_buf;
        uint8_t p = probe_pat(i);
        char c = '.';

        if (v == p) { c = '#'; } 
        else {
            for (j = 0; j < PROBE_POINTS; j++)
                if (j != i && v == probe_pat(j)) { c = 'a'; break; }
        }
        map[i] = c;
        if (c == '#') n8++; else if (c == 'a') nal++; else nnone++;
        if (c == '#') { if (first < 0) first = i; last = i; }
    }

    for (i = 0; i < PROBE_POINTS; i++) {
        *xfer_buf = orig[i];
        do_md_cmd3(MD_CMD_SAFE_CART_XFER, 0x200104, PROBE_BASE_ADDR + i * PROBE_STEP + 1, 1);
    }

    map[PROBE_POINTS] = 0;
    for (i = 0; i < 4; i++) { memcpy(row, map + i * 32, 32); row[32] = 0; print_diag(temp, row, 2, 4 + i); }
    print_diag(temp, "ONE CHAR = 16 KB, ROW = 512 KB", 2, 9);
    print_diag(temp, "# 8-BIT OK   a MIRROR   . NONE", 2, 10);

    sprintf(tbuf, "8BIT:%d MIRROR:%d NONE:%d", n8, nal, nnone);
    print_diag(temp, tbuf, 2, 13);
    if (first >= 0) {
        sprintf(tbuf, "RAM FROM %06X TO %06X", PROBE_BASE_ADDR + first * PROBE_STEP + 1, PROBE_BASE_ADDR + last * PROBE_STEP + PROBE_STEP + 1);
        print_diag(temp, tbuf, 2, 14);
    } else { print_diag(temp, "NO RAM FOUND IN THIS WINDOW", 2, 14); }
    print_diag(temp, "PROBE DONE - SET CART_PROBE 0", 2, 16);
}
#endif

static uint16_t cdda_track_word;       

static int bios_wait_ready(unsigned int max_ticks)
{
    unsigned int t0 = GET_TICKS;
    while (scd_cd_status() & 0x80000000) {
        if ((GET_TICKS - t0) >= max_ticks) return 0;
        bump_fm();
    }
    return 1;
}

static int cdda_start(int track, int *st_out)
{
    int attempt, st = 0;
    for (attempt = 1; attempt <= 3; attempt++) {
        bios_wait_ready(300);
        scd_bios_call(0x0089, 0, 0);            
        bios_wait_ready(300);
        scd_bios_call(0x0002, 0, 0);            
        bios_wait_ready(300);
        scd_bios_call(0x0085, 0x0400, 0);       
        cdda_track_word = (uint16_t)track;
        scd_bios_call(0x0013, 0, &cdda_track_word);  
        {
            unsigned int t0 = GET_TICKS;
            while ((GET_TICKS - t0) < 240) {    
                st = scd_cd_status();
                if (((st >> 24) & 0xFF) == 0x01) { *st_out = st; return attempt; }
                bump_fm();
            }
        }
    }
    *st_out = st; return 0;
}

int main(void)
{
    write_byte(0x8003, read_byte(0x8003) & ~1);
    while (read_byte(0x8003) & 2);

    char *temp = (char *)0x0C0000;
    char tbuf[64];
    uint32_t build_counter = 0;
    unsigned short text_on = 1;         
    unsigned short prev_pad = 0;
    int cart_ok = 0;

    print_diag(temp, "BUILD: WRAM-OWNERSHIP-V59-8BIT      ", 2, 0);
    print_diag(temp, "LOADING 32XCD ENGINE...", 2, 2);

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

        /* =========================================================================
         * SH2 ISOLATION & BOOT VERIFICATION
         * =========================================================================
         * We confirm the SH2 processors have successfully exited their internal
         * BIOS bootloader, unpacked our C payload, and are actively executing it.
         * 
         * 1. 68K writes 0x1234 to COMM4 (0xA15124).
         * 2. SH2 payload spins waiting for COMM4 == 0x1234.
         * 3. SH2 unmasks its interrupts, then writes 0x0AAA to COMM10 (0xA1512A).
         * 4. 68K waits for 0x0AAA on COMM10 to definitively prove SH2s are alive.
         * ========================================================================= */
        do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15124, 0x1234);

        {
            uint16_t ready = 0;
            volatile int wait = 0;
            
            /* Simple native wait loop ensures we don't rely on Main CPU ticking */
            for (wait = 0; wait < 1000000; wait++) {            
                ready = (uint16_t)do_md_cmd1(MD_CMD_READ_32X_16, 0xA1512A);   
                if (ready == 0x0AAA) break;
            }
            sprintf(tbuf, "SH2 READY FOR ISOLATED TEST: %04X    ", ready);
            print_diag(temp, tbuf, 2, 19);

            if (ready != 0x0AAA) {
                print_diag(temp, "SH2 NEVER SIGNALLED READY - STOPPED", 2, 20);
                do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA1512A, 0x0BBB);   
                while (1) bump_fm();
            }
        }

        cart_ok = safe_cart_present(temp, tbuf);  
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

    print_diag(temp, "START = SHOW / HIDE THIS TEXT     ", 2, 26);

#if CART_PROBE
    if (cart_ok) {
        cart_probe(temp, tbuf);
    } else {
        print_diag(temp, "CART PROBE ABORTED: CART NOT FOUND  ", 2, 16);
    }
    while (1) { bump_fm(); }
#endif

    do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15126, 0xAA00 | text_on);   

    while (1) {
        build_counter++;

        if (build_counter == 1) {
            unsigned char first_iter_comm = read_byte(SUB_MAIN_FLAG);
            sprintf(tbuf, "ITER1 COMM PORT: %02X           ", first_iter_comm);
            print_diag(temp, tbuf, 2, 15);
        }

        bump_fm();

        {   
            unsigned short pad = GET_PAD(0);
            if ((pad & SEGA_CTRL_START) && !(prev_pad & SEGA_CTRL_START)) {
                text_on ^= 1;
                do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15126, 0xAA00 | text_on);
            }
            prev_pad = pad;
        }

        uint16_t cmd = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15120);
        uint16_t comm4 = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15124);
        uint16_t sh2_state = do_md_cmd1(MD_CMD_READ_32X_16, 0xA1512C);
        uint32_t ckpt = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128); 
        
        sprintf(tbuf, "32X:%X 32XReg:%X STATE:%X CKPT:%04X     ", cmd, comm4, sh2_state, (unsigned int)(ckpt >> 16));
        print_diag(temp, tbuf, 2, 10);

        if (comm4 == 0x0EEE) {
            print_diag(temp, "SH2 REACHED THE LDC LINE              ", 2, 18); 
        } else if (comm4 != 0x1234 && comm4 != 0x0000) {
            print_diag(temp, "SH2 NEVER EVEN REACHED THIS FILE      ", 2, 18); 
        }

        if (cmd == 38) { 
            int img_idx = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            char img_name[24];
            if (img_idx <= 0) strcpy(img_name, "IMAGE.RAW");
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
            } else {
                uint32_t length = (uint32_t)(file_info >> 32);
                saved_lba = (uint32_t)(file_info & 0xFFFFFFFF);
                sprintf(tbuf, "%s %lu BYTES%s          ", img_name, (unsigned long)length,
                        length > 128000 ? " (TOO TALL)" : " OK");
                print_diag(temp, tbuf, 2, 12);
                do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15128, length); 
            }
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0); 
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

            if (!quiet) {
                volatile uint8_t *chk = (volatile uint8_t *)0x0C0100;
                uint32_t base_even = m68k_dest - 1;
                uint32_t total = (uint32_t)length;
                int s, k, ok = 0, first_bad = -1;
                uint8_t bad_cart0 = 0, bad_cart1 = 0, bad_disc0 = 0, bad_disc1 = 0;

                for (s = 0; s < 16; s++) {
                    uint32_t pos = (((total - 8) / 15) * s) & ~1u;
                    uint8_t cv[8];
                    int same = 1;
                    for (k = 0; k < 8; k++) cv[k] = do_md_cmd1(MD_CMD_READ_32X_16, base_even + 2 * (pos + k)) & 0xFF;
                    scd_sub_read((void *)0x0C0100, saved_lba + pos / 2048, 2);
                    for (k = 0; k < 8; k++) if (chk[(pos % 2048) + k] != cv[k]) same = 0;

                    if (same) { ok++; } else if (first_bad < 0) {
                        first_bad = s; bad_cart0 = cv[0]; bad_cart1 = cv[1];
                        bad_disc0 = chk[pos % 2048]; bad_disc1 = chk[(pos % 2048) + 1];
                    }
                }
                sprintf(tbuf, "VERIFY %d/16 OK  1STBAD:%d          ", ok, first_bad);
                print_diag(temp, tbuf, 2, 17);
                if (first_bad >= 0) {
                    sprintf(tbuf, "BAD S%d CART=%02X%02X DISC=%02X%02X        ", first_bad, bad_cart0, bad_cart1, bad_disc0, bad_disc1);
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
            
#if !SLIDESHOW_DEBUG
            if (!quiet) { do_md_cmd0(MD_CMD_CLEAR_B); do_md_cmd0(MD_CMD_CLEAR_A); quiet = 1; }
#endif
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0); 
        }
#endif
        else if (cmd == 43) {
            int chunk = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            int length = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128);
            int direct = do_md_cmd1(MD_CMD_READ_32X_32, 0xA1512C);   
            int off = chunk * 32768;
            int n = 0;

            if (off < length) n = (length - off + 2047) / 2048;
            if (n > 16) n = 16;

            sprintf(tbuf, "CHUNK %d: %d SECTORS               ", chunk, n);
            print_diag(temp, tbuf, 2, 13);

            if (n > 0) {
                scd_sub_read((void *)0x0C0000, saved_lba + chunk * 16, n);
                switch_banks();
                do_md_cmd4(MD_CMD_COPY_WORDS, 0x200000, 0x840200 + (direct ? chunk * 32768 : 0), n * 2048, direct ? 1 : 0);
            }
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 44) {
#if !SLIDESHOW_DEBUG
            if (!quiet) { do_md_cmd0(MD_CMD_CLEAR_B); do_md_cmd0(MD_CMD_CLEAR_A); quiet = 1; }
#endif
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 45) {
            int track = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            sprintf(tbuf, "CDDA: PLAY TRACK %d (REPEAT)        ", track);
            print_diag(temp, tbuf, 2, 27);
            
            scd_cdda_play(track);
            
            {   
                unsigned int t0 = GET_TICKS;
                int st;
                while ((GET_TICKS - t0) < 90) bump_fm();
                st = scd_cd_status();
                sprintf(tbuf, "CDDA TRACK %d  STATUS %02X (01=PLAYING)", track, (st >> 24) & 0xFF);
                print_diag(temp, tbuf, 2, 27);
            }
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
    }
    return 0;
}