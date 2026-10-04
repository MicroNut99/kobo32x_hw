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

/* 68K VBlank Tick Macro & Pad Reader. Sub-CPU accesses Main CPU Comm Registers via 0xFF8000 */
#define GET_TICKS read_long(0xFF801C)
#define GET_PAD(n) read_word(0xFF8018 + (n)*2)

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
extern int do_md_cmd4(int cmd, int arg1, int arg2, int arg3, int arg4);

#define MD_CMD_CLEAR_B        4
#define MD_CMD_CLEAR_A        8
#define MD_CMD_PUT_STR        9
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
#define MD_CMD_INSTALL_EXC    31
#define MD_CMD_SET_VDP_REG    34         

#define KOBO_DIAG_TEXT  0
#define KOBO_BOOT_TEXT  1

#define KOBO_NOCART     0

#define LEGACY_68K_CART 0

static uint32_t saved_lba = 0;
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

static int text_visible = 1; 

void print_diag(char *temp, const char *msg, int x, int y) {
    if (quiet) return;
#if !KOBO_DIAG_TEXT
    return;
#endif
    if (!text_visible) return;
    strcpy(temp, msg);
    switch_banks();
    /* 0x2000 forces Palette 1 (Green text) */
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, 0x2000, x, y);
    switch_banks();
}

#define CART_TARGET     0              
#define KOBO_PACK_NAME  "KOBOGFX.BIN"
#define KOBO_PACK_SLOT  16             
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

/* =====================================================================================
 * K33 SOUND EFFECTS - Sega CD PCM chip (RF5C164), driven directly by the Sub-CPU.
 * ===================================================================================== */
#define PCM_ENV    0xFF0001
#define PCM_PAN    0xFF0003
#define PCM_FDL    0xFF0005
#define PCM_FDH    0xFF0007
#define PCM_LSL    0xFF0009
#define PCM_LSH    0xFF000B
#define PCM_ST     0xFF000D
#define PCM_CTRL   0xFF000F   
#define PCM_ONOFF  0xFF0011   
#define PCM_WAVE   0xFF2001
#define SFX_MAX    31
typedef struct { uint8_t st, vol; uint16_t fd, loop, len; } sfx_t;
static sfx_t sfx_tab[SFX_MAX + 1];            
static int sfx_count = 0;
static uint8_t pcm_onoff = 0xFF;              
static int pcm_next = 0;

static void pcm_play(int id) {
    const sfx_t *e;
    int ch;
    if (id < 1 || id > sfx_count) return;
    e = &sfx_tab[id];
    ch = (id == 1) ? 7 : pcm_next;            
    if (id != 1) pcm_next = pcm_next < 6 ? pcm_next + 1 : 0;
    pcm_onoff |= (uint8_t)(1 << ch);          
    write_byte(PCM_ONOFF, pcm_onoff);
    write_byte(PCM_CTRL, 0xC0 | ch);          
    write_byte(PCM_ENV, e->vol);
    write_byte(PCM_PAN, 0xFF);
    write_byte(PCM_FDL, e->fd & 0xFF);
    write_byte(PCM_FDH, e->fd >> 8);
    write_byte(PCM_LSL, e->loop & 0xFF);
    write_byte(PCM_LSH, e->loop >> 8);
    write_byte(PCM_ST, e->st);
    pcm_onoff &= (uint8_t)~(1 << ch);         
    write_byte(PCM_ONOFF, pcm_onoff);
}

static int pcm_load(void) {
    int64_t fi = scd_open_file("KOBOSFX.BIN");
    uint32_t len, lba, nsec, img, i, b;
    volatile uint8_t *buf = (volatile uint8_t *)0x0C0000;   
    if (fi < 0) return 0;
    len = (uint32_t)(fi >> 32); lba = (uint32_t)(fi & 0xFFFFFFFF);
    nsec = (len + 2047) / 2048;
    if (nsec > 48) nsec = 48;
    scd_sub_read((void *)0x0C0000, lba, nsec);
    if (buf[0] != 'K' || buf[1] != 'S' || buf[2] != 'F' || buf[3] != 'X') return 0;
    sfx_count = (buf[4] << 8) | buf[5];
    img = (uint32_t)((buf[6] << 8) | buf[7]);
    if (img == 0) img = 65536;
    if (sfx_count > SFX_MAX) sfx_count = SFX_MAX;
    for (i = 0; i < (uint32_t)sfx_count; i++) {
        volatile uint8_t *t = buf + 8 + i * 8;
        sfx_tab[i + 1].st = t[0]; sfx_tab[i + 1].vol = t[1];
        sfx_tab[i + 1].fd = (uint16_t)((t[2] << 8) | t[3]);
        sfx_tab[i + 1].loop = (uint16_t)((t[4] << 8) | t[5]);
        sfx_tab[i + 1].len = (uint16_t)((t[6] << 8) | t[7]);
    }
    pcm_onoff = 0xFF;
    write_byte(PCM_ONOFF, 0xFF);                               
    for (b = 0; b < 16; b++) {                                 
        write_byte(PCM_CTRL, (uint8_t)b);                      
        for (i = 0; i < 4096; i++) {
            uint32_t a = b * 4096 + i;
            write_byte(PCM_WAVE + 2 * i, a < img ? buf[256 + a] : 0xFF);   
        }
    }
    write_byte(PCM_CTRL, 0x80);                                
    return sfx_count;
}

/* =====================================================================================
 * K36 BACKUP RAM (Sega CD internal) 
 * ===================================================================================== */
#define BRAM_BYTES 128
static uint8_t bram_work[0x640];
static uint8_t bram_str[16];
static uint8_t bram_buf[BRAM_BYTES];
static const char bram_name[12] = "KOBODELUXE_";
static uint8_t bram_param[14];
static int bram_ready = 0;

static int bram_call(int fn, const void *pa0, void *pa1) {
    register int d0 __asm__("d0") = fn;
    register const void *a0 __asm__("a0") = pa0;
    register void *a1 __asm__("a1") = pa1;
    register int d1 __asm__("d1") = 0;
    __asm__ volatile (
        "movem.l %%d2-%%d7/%%a2-%%a6,-(%%sp)\n\t"
        "jsr 0x5F16.w\n\t"
        "scs %%d1\n\t"                          
        "movem.l (%%sp)+,%%d2-%%d7/%%a2-%%a6"
        : "+d"(d0), "+a"(a0), "+a"(a1), "+d"(d1) : : "cc", "memory");
    return (d1 & 0xFF) ? -1 : d0;
}
static int bram_init(void) {
    if (!bram_ready) bram_ready = bram_call(0, bram_work, bram_str) >= 0 ? 1 : -1;
    return bram_ready > 0;
}
static int bram_load(void) {                    
    int i;
    for (i = 0; i < BRAM_BYTES; i++) bram_buf[i] = 0;
    if (!bram_init()) return 0;
    return bram_call(3, bram_name, bram_buf) >= 0 ? 1 : 0;
}
static int bram_save(void) {
    int i;
    if (!bram_init()) return 0;
    for (i = 0; i < 11; i++) bram_param[i] = (uint8_t)bram_name[i];
    bram_param[11] = 0;                         
    bram_param[12] = 0; bram_param[13] = BRAM_BYTES / 64;   
    return bram_call(4, bram_param, bram_buf) >= 0 ? 1 : 0;
}

int main(void)
{
    write_byte(0x8003, read_byte(0x8003) & ~1);
    while (read_byte(0x8003) & 2);
#if !KOBO_BOOT_TEXT
    do_md_cmd1(MD_CMD_SET_VDP_REG, 0x8134);   
#endif

    char *temp = (char *)0x0C0000;
    char tbuf[64];
    uint32_t build_counter = 0;
    int cart_ok = 0;

    print_diag(temp, "BUILD: KOBO K37D FULL AUDIO & SFX   ", 2, 0);
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
        do_md_cmd0(MD_CMD_INSTALL_EXC);
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
        cart_ok = 0;                                   
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

    print_diag(temp, "SOLID TEXT MODE - PRESS C TO TOGGLE", 2, 26);

    while (1) {
        build_counter++;

        if (build_counter == 1) {
            unsigned char first_iter_comm = read_byte(SUB_MAIN_FLAG);
            sprintf(tbuf, "ITER1 COMM PORT: %02X           ", first_iter_comm);
            print_diag(temp, tbuf, 2, 15);
        }

        bump_fm();

        // 1. Controller Polling Fast Path (Zero-Overhead Read)
        uint16_t current_pad = GET_PAD(0);
        static int c_was_pressed = 0;
        int c_pressed = (current_pad & 0x0020); // SEGA_CTRL_C
        
        if (c_pressed && !c_was_pressed) {
            text_visible = !text_visible;
            if (!text_visible) {
                do_md_cmd1(MD_CMD_SET_VDP_REG, 0x8134); 
                do_md_cmd0(MD_CMD_CLEAR_B);
                do_md_cmd0(MD_CMD_CLEAR_A);
            } else {
                do_md_cmd1(MD_CMD_SET_VDP_REG, 0x8174); 
                print_diag(temp, "SOLID TEXT MODE - PRESS C TO TOGGLE", 2, 26);
            }
        }
        c_was_pressed = c_pressed;

        uint16_t cmd = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15120);

        if ((cmd & 0xFF) == 48) {
            if (cmd >> 8) pcm_play(cmd >> 8);           
            
            do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15128, ((uint32_t)current_pad << 16) | (GET_TICKS & 0xFFFF));
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);    
            continue;
        }

        uint16_t comm4 = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15124);
        uint16_t sh2_state = do_md_cmd1(MD_CMD_READ_32X_16, 0xA1512C);
        uint32_t ckpt = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128); 

        {   
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
                saved_len = length;                                   
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
            int chunk = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            int length = (int)saved_len;           
            int sec = cmd == 51 ? 16 : 8;                   
            int off = chunk * sec * 2048;
            int n = 0;
            if (off < length) n = (length - off + 2047) / 2048;
            if (n > sec) n = sec;

            if (n > 0) {
                scd_sub_read((void *)0x0C0000, saved_lba + chunk * sec, n);  
                switch_banks();                                               
                do_md_cmd4(MD_CMD_COPY_WORDS, 0x200000,
                           cmd == 51 ? 0x840200 + off : 0x852000, n * 2048, 0);
                switch_banks();                                               
            }

            {
                uint16_t wr = (n > 0) ? *(volatile uint16_t *)(0x0C0000 + 0x210) : 0xEEEE;
                do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA1512C, (chunk << 24) | (n << 16) | wr);
            }
            sprintf(tbuf, "CD->FB CHUNK %d  %d SECTORS          ", chunk, n);
            print_diag(temp, tbuf, 2, 16);
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 54) {
            scd_bios_call(0x0002, 0, 0);
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 56 || cmd == 59) {
            int ok = cmd == 56 ? bram_load() : bram_save();
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA1512C, ok);
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 57 || cmd == 58) {
            int wi = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122) & 63;
            if (cmd == 58) {
                int v = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15128);
                bram_buf[wi * 2] = (uint8_t)(v >> 8); bram_buf[wi * 2 + 1] = (uint8_t)v;
            } else {
                do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15128, (bram_buf[wi * 2] << 8) | bram_buf[wi * 2 + 1]);
            }
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 53) {
            int nfx = pcm_load();
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA1512C, nfx);
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
        else if (cmd == 52) {
            int on = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122) & 1;
            do_md_cmd1(MD_CMD_SET_VDP_REG, on ? 0x8174 : 0x8134);
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
                while ((GET_TICKS - t0) < 90)
                    bump_fm();
                st = scd_cd_status();
                sprintf(tbuf, "CDDA TRACK %d  STATUS %02X (01=PLAYING)", track, (st >> 24) & 0xFF);
                print_diag(temp, tbuf, 2, 27);
            }
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0);
        }
    }
    return 0;
}