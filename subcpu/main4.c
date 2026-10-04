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

// Safe wrappers from scd.c for the Main CPU to command the Sub-CPU
extern int64_t scd_open_file(const char *name);
extern void scd_read_sectors(void *ptr, int lba, int len, void (*wait)(void));
extern void scd_stream_cmd(char cmd); 

extern int do_md_cmd1(int cmd, int arg1);
extern int do_md_cmd2(int cmd, int arg1, int arg2);

#define MD_CMD_READ_32X_16  19
#define MD_CMD_WRITE_32X_16 20
#define MD_CMD_READ_32X_32  21
#define MD_CMD_WRITE_32X_32 22

static uint32_t saved_lba = 0;

int main(void)
{
    char *temp = (char *)0x0C0000;
    uint32_t build_counter = 0;

    strcpy(temp, "BUILD: WRAM-OWNERSHIP-V2      ");
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 0);

    strcpy(temp, "LOADING 32XCD ENGINE...");
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 2);

    sprintf(temp, "32X PAYLOAD SIZE: %d BYTES    ", sh2_app_length);
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 4);

    memcpy(temp, sh2_app_start, sh2_app_length);
    switch_banks();
    do_md_cmd2(MD_CMD_INIT_32X, 0x200000, sh2_app_length);

    uint16_t init_err = InitCD();

    {
        unsigned char c = read_byte(0xA1200F);
        sprintf(temp, "POST-INITCD COMM: %02X          ", c);
        switch_banks();
        do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 1);
    }

    if (init_err != 0) {
        // error handling
    } else {
        strcpy(temp, "CD DRIVE READY                ");
        switch_banks();
        do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 5);
        
        strcpy(temp, "BOOTING SUB-CPU...            ");
        switch_banks();
        do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 6);
    }

    memcpy(temp, Sub_Start, Sub_End - Sub_Start);
    switch_banks();

    {
        unsigned char c = read_byte(0xA1200F);
        sprintf(temp, "POST-MEMCPY2 COMM: %02X         ", c);
        switch_banks();
        do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 3);
    }

    strcpy(temp, "INITIALIZING SUB-CPU Sfx...   ");
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 7);

    send_md_cmd0('I'); 

    {
        unsigned char c = read_byte(0xA1200F);
        sprintf(temp, "POST-SENDMD COMM: %02X          ", c);
        switch_banks();
        do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 18);
    }

    strcpy(temp, "68K LISTENER ACTIVE           ");
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 8);

    /* Check the comm port at the earliest possible moment, before the
       polling loop even starts, to pin down exactly when it gets to
       0xFF - immediately at boot, or only after some delay. */
    {
        unsigned char boot_comm = read_byte(0xA1200F);
        sprintf(temp, "BOOT COMM PORT: %02X            ", boot_comm);
        switch_banks();
        do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 9);
    }

    do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15124, 0x1234);

    while (1) {
        build_counter++;

        /* Check again on the very first loop iteration only, to see if
           it corrupts within a single pass or needs more time/iterations. */
        if (build_counter == 1) {
            unsigned char first_iter_comm = read_byte(0xA1200F);
            sprintf(temp, "ITER1 COMM PORT: %02X           ", first_iter_comm);
            switch_banks();
            do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 15);
        }

        bump_fm();

        uint16_t cmd = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15120);
        uint16_t comm4 = do_md_cmd1(MD_CMD_READ_32X_16, 0xA15124);
        uint16_t sh2_state = do_md_cmd1(MD_CMD_READ_32X_16, 0xA1512C);
        uint32_t ckpt = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128); 
        
        sprintf(temp, "32X:%X 32XReg:%X STATE:%X CKPT:%lu     ", cmd, comm4, sh2_state, (unsigned long)ckpt);
        switch_banks();
        do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 10);

        if (cmd == 38) { 
            strcpy(temp, "OPENING IMAGE.RAW...          ");
            switch_banks();
            do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 11);

            // Instant pre-check: what is the comm port doing right when cmd 38 arrives?
            unsigned char entry_comm = read_byte(0xA1200F);
            if (entry_comm != 0) {
                sprintf(temp, "ENTRY COMM FAULT: %02X        ", entry_comm);
                switch_banks();
                do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 12);
            }

            int64_t file_info = scd_open_file("IMAGE.RAW");
            
            if (file_info < 0) {
                strcpy(temp, "ERR: IMAGE.RAW NOT FOUND      ");
                switch_banks();
                do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 12);

                sprintf(temp, "TIMEOUT: %d (0=none 1=cmd 2=ack 3=wram)", scd_last_timeout_code);
                switch_banks();
                do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 13);

                do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15128, 0); 
            } else {
                strcpy(temp, "IMAGE.RAW FOUND OK            ");
                switch_banks();
                do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 12);

                uint32_t length = (uint32_t)(file_info >> 32);
                saved_lba = (uint32_t)(file_info & 0xFFFFFFFF);

                do_md_cmd2(MD_CMD_WRITE_32X_32, 0xA15128, length); 
            }
            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0); 
        }
        else if (cmd == 40) { 
            strcpy(temp, "READING IMAGE.RAW DATA...     ");
            switch_banks();
            do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 13);

            int length = do_md_cmd1(MD_CMD_READ_32X_32, 0xA15128);
            uint32_t sh2_dest = do_md_cmd1(MD_CMD_READ_32X_32, 0xA1512C);
            
            uint32_t m68k_dest = (sh2_dest & 0x00FFFFFF) | 0x00400000;
            int sectors = (length + 2047) / 2048;
            
            switch_banks();
            
            scd_read_sectors((void*)m68k_dest, saved_lba, sectors, NULL);

            strcpy(temp, "IMAGE.RAW STREAM COMPLETE     ");
            switch_banks();
            do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 14);

            do_md_cmd2(MD_CMD_WRITE_32X_16, 0xA15120, 0); 
        }
    }
    return 0;
}