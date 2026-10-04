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

static uint32_t saved_lba = 0;
static int quiet = 0;      /* set after the first image is up: nothing more is drawn on the Genesis side */

/* 1 = keep the status text on screen (shown over the picture) so you can see what the loader is doing.
   0 = clean slideshow: the text is wiped after the first picture.  Keep sh2_main.c DEBUG_OVERLAY the same. */
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
    if (quiet)
        return;
    strcpy(temp, msg);
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, x, y);
    switch_banks();
}

int main(void)
{
    write_byte(0x8003, read_byte(0x8003) & ~1);
    while (read_byte(0x8003) & 2);

    char *temp = (char *)0x0C0000;
    char tbuf[64];
    uint32_t build_counter = 0;

    print_diag(temp, "BUILD: WRAM-OWNERSHIP-V38     ", 2, 0);
    print_diag(temp, "LOADING 32XCD ENGINE...", 2, 2);

    {
        const char *m = find_sh2_marker(sh2_app_start, sh2_app_length);
        if (m)
            sprintf(tbuf, "32X PAYLOAD: %d BYTES SH2-V%c%c        ", sh2_app_length, m[0], m[1]);
        else
            sprintf(tbuf, "32X PAYLOAD: %d BYTES SH2:NONE       ", sh2_app_length);
        print_diag(temp, tbuf, 2, 4);
    }

    memcpy(temp, sh2_app_start, sh2_app_length);
    switch_banks();
    do_md_cmd2(MD_CMD_INIT_32X, 0x200000, sh2_app_length);
    switch_banks(); 

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

    do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15124, 0x1234);

    while (1) {
        build_counter++;

        if (build_counter == 1) {
            unsigned char first_iter_comm = read_byte(0xA1200F);
            sprintf(tbuf, "ITER1 COMM PORT: %02X           ", first_iter_comm);
            print_diag(temp, tbuf, 2, 15);
        }

        bump_fm();

        uint16_t cmd = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15120);
        uint16_t comm4 = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15124);
        uint16_t sh2_state = do_md_cmd1(MD_CMD_READ_32X_16, 0xA1512C);
        uint32_t ckpt = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128); 
        
        sprintf(tbuf, "32X:%X 32XReg:%X STATE:%X CKPT:%lu     ", cmd, comm4, sh2_state, (unsigned long)ckpt);
        print_diag(temp, tbuf, 2, 10);

        if (cmd == 38) { 
            /* The SH2 puts the file number in COMM2: 0 = IMAGE.RAW, n = IMAGEn.RAW */
            int img_idx = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15122);
            char img_name[16];
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
                sprintf(tbuf, "%s FOUND OK              ", img_name);
                print_diag(temp, tbuf, 2, 12);

                uint32_t length = (uint32_t)(file_info >> 32);
                saved_lba = (uint32_t)(file_info & 0xFFFFFFFF);

                do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15128, length); 
            }
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0); 
        }
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
    }
    return 0;
}