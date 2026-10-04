#include <stdint.h>
#include <string.h>
#include <stdio.h>

extern uint32_t vblank_vector;
extern uint16_t gen_lvl2;

extern uint32_t Sub_Start;
extern uint32_t Sub_End;

extern void Kos_Decomp(uint8_t *src, uint8_t *dst);

extern void write_byte(unsigned int dst, unsigned char val);
extern void write_word(unsigned int dst, unsigned short val);
extern void write_long(unsigned int dst, unsigned int val);
extern unsigned char read_byte(unsigned int src);
extern unsigned short read_word(unsigned int src);
extern unsigned int read_long(unsigned int src);

extern void switch_banks(void);
extern int do_md_cmd4(int cmd, int arg1, int arg2, int arg3, int arg4);
#define MD_CMD_PUT_STR 9
#define TEXT_GREEN 0x2000

static void scd_diag_print(const char *msg)
{
    char *temp = (char *)0x0C0000;
    int i = 0;
    while (msg[i] && i < 39) { temp[i] = msg[i]; i++; }
    temp[i] = 0;
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 16);
}

static void scd_timeout_print(const char *msg)
{
    char *temp = (char *)0x0C0000;
    int i = 0;
    while (msg[i] && i < 39) { temp[i] = msg[i]; i++; }
    temp[i] = 0;
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 2, 17);
}

static void init_diag_print(int row, const char *msg)
{
    char *temp = (char *)0x0C0000;
    int i = 0;
    while (msg[i] && i < 39) { temp[i] = msg[i]; i++; }
    temp[i] = 0;
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, TEXT_GREEN, 4, row);
}

void scd_delay(void) __attribute__((section(".data"), aligned(16)));
char wait_cmd_ack(void) __attribute__((section(".data"), aligned(16)));
void wait_do_cmd(char cmd) __attribute__((section(".data"), aligned(16)));

/* 
 * RAW BIOS FETCH PAYLOAD (100% Scratch Registers)
 * We will dynamically patch indices 12 and 13 with the true BIOS address!
 */
const uint16_t bios_fetch_code[] = {
    0x40C0,                         // 0: move.w %sr, %d0
    0x46FC, 0x2700,                 // 1, 2: move.w #0x2700, %sr     (Disable ints)
    0x3239, 0x00A1, 0x5100,         // 3, 4, 5: move.w 0xA15100, %d1    (Read Bank Reg)
    0x0881, 0x0000,                 // 6, 7: bclr #0, %d1            (Clear RV)
    0x33C1, 0x00A1, 0x5100,         // 8, 9, 10: move.w %d1, 0xA15100    (Write Bank Reg)
    0x207C, 0x0041, 0x5800,         // 11, 12, 13: movea.l #0x415800, %a0  <-- TO BE PATCHED
    0x227C, 0x0022, 0x0000,         // 14, 15, 16: movea.l #0x220000, %a1  (dst = Upper Word RAM)
    0x323C, 0x7FFF,                 // 17, 18: move.w #0x7FFF, %d1     (32768 iterations = 128KB)
    0x22D8,                         // 19: loop: move.l (%a0)+, (%a1)+
    0x51C9, 0xFFFC,                 // 20, 21: dbf %d1, loop
    0x3239, 0x00A1, 0x5100,         // 22, 23, 24: move.w 0xA15100, %d1    (Re-read Bank Reg)
    0x08C1, 0x0000,                 // 25, 26: bset #0, %d1            (Set RV)
    0x33C1, 0x00A1, 0x5100,         // 27, 28, 29: move.w %d1, 0xA15100    (Write Bank Reg)
    0x46C0,                         // 30: move.w %d0, %sr         (Restore ints)
    0x4E75                          // 31: rts
};

/* 
 * RAW PRG FLASH PAYLOAD (100% Scratch Registers)
 * Safely blasts the fully assembled 128KB image from lower Word RAM to PRG RAM.
 * Fixed: Uses the proven 0x420000 destination address.
 */
const uint16_t prg_flash_code[] = {
    0x40C0,                         // move.w %sr, %d0
    0x46FC, 0x2700,                 // move.w #0x2700, %sr     (Disable ints)
    0x3239, 0x00A1, 0x5100,         // move.w 0xA15100, %d1    (Read Bank Reg)
    0x0881, 0x0000,                 // bclr #0, %d1            (Clear RV)
    0x33C1, 0x00A1, 0x5100,         // move.w %d1, 0xA15100    (Write Bank Reg)
    0x207C, 0x0020, 0x0000,         // movea.l #0x200000, %a0  (src = Lower Word RAM)
    0x227C, 0x0042, 0x0000,         // movea.l #0x420000, %a1  (dst = PRG RAM [0x420000])
    0x323C, 0x7FFF,                 // move.w #0x7FFF, %d1     (32768 iterations = 128KB)
    0x22D8,                         // loop: move.l (%a0)+, (%a1)+
    0x51C9, 0xFFFC,                 // dbf %d1, loop
    0x3239, 0x00A1, 0x5100,         // move.w 0xA15100, %d1    (Re-read Bank Reg)
    0x08C1, 0x0000,                 // bset #0, %d1            (Set RV)
    0x33C1, 0x00A1, 0x5100,         // move.w %d1, 0xA15100    (Write Bank Reg)
    0x46C0,                         // move.w %d0, %sr         (Restore ints)
    0x4E75                          // rts
};

// Safely allocate execution space in .bss (always in 68K Work RAM)
static uint16_t ram_flash_array[64];

/* Minimal test: same mechanism (memcpy into Work RAM, execute via
   function pointer) as bios_fetch_code/prg_flash_code, but does
   nothing except return immediately. If even this hangs on a given
   platform, the problem is the self-modifying-code execution
   mechanism itself (e.g. a stale instruction cache never invalidated
   after the memcpy), not anything specific inside the real routines. */
const uint16_t trivial_test_code[] = {
    0x4E75                          // rts
};

extern void scd_init_pcm(void);
extern void bump_fm(void);
extern int mystrlen(const char* string);

volatile int scd_last_timeout_code = 0;

void scd_delay(void)
{
    int cnt = 50;
    do {
        asm __volatile("nop");
    } while (--cnt);
}

char wait_cmd_ack(void)
{
    char ack = 0;
    int timeout = 0;

    do {
        scd_delay();
        bump_fm();
        ack = read_byte(0xA1200F); 
        timeout++;
        if (timeout > 20000) {
            scd_last_timeout_code = 2;
            return 0;
        }
    } while (!ack);

    return ack;
}

void wait_do_cmd(char cmd)
{
    int timeout = 0;

    while (read_byte(0xA1200F)) {
        scd_delay(); 
        bump_fm();
        timeout++;
        if (timeout > 20000) {
            scd_last_timeout_code = 1;
            
            char dmsg[40];
            sprintf(dmsg, "COMM:%02X CMD:%c TIMEOUT         ", read_byte(0xA1200F), cmd);
            scd_timeout_print(dmsg);
            
            return;
        }
    }
    write_byte(0xA1200E, cmd); 
}

int64_t scd_open_file(const char *name)
{
    int i;
    static int call_count = 0;
    char *scdfn = (char *)0x600000; 
    union {
        int32_t lo[2];
        int64_t value;
    } handle;

    call_count++;
    
    // Moved visible rows to 14-17 so they don't render off-screen!
    init_diag_print(14, "CKPT1: entered scd_open_file    ");
    
    /* 1M mode switch - Mandatory for the Sub-CPU to access Word RAM! */
    write_byte(0xA12003, (read_byte(0xA12003) | 0x04) & ~0x02);
    init_diag_print(15, "CKPT1b: 1M mode set!            ");

    if (read_byte(0xA1200F) == 0xFF) {
        write_byte(0xA1200E, 0x00);
        scd_delay();
    }

    write_byte(0xA12010, 1);
    wait_do_cmd('E');
    {
        char ack_val = wait_cmd_ack();
        static int already_shown = 0;
        if (!already_shown) {
            char abuf[40];
            sprintf(abuf, "E-ACK: %02X %s              ", (unsigned char)ack_val,
                    (ack_val == 'D') ? "(real)" : "(??)");
            init_diag_print(13, abuf);
            already_shown = 1;
        }
    }
    write_byte(0xA1200E, 0x00);

    init_diag_print(16, "CKPT2: mixer suspended          ");

    for (i = 0; name[i]; i++)
        *scdfn++ = name[i];
    *scdfn = 0;

    init_diag_print(17, "CKPT4: filename write done      ");

    write_long(0xA12010, 0x0C0000); 
    scd_last_timeout_code = 0;
    wait_do_cmd('F');
    if (scd_last_timeout_code == 1) {
        return -1;
    }
    if (!wait_cmd_ack()) {
        return -1;
    }
    handle.lo[0] = read_long(0xA12020);
    handle.lo[1] = read_long(0xA12024);
    write_byte(0xA1200E, 0x00);

    write_byte(0xA12010, 0);
    wait_do_cmd('E');
    wait_cmd_ack();
    write_byte(0xA1200E, 0x00);

    return handle.value;
}

int64_t scd_read_directory(char *buf)
{
    int i, numwords;
    char *scdWordRam = (char *)0x600000; 
    union {
        int32_t ln[2]; 
        int64_t value;
    } res;

    memcpy(scdWordRam, buf, mystrlen(buf)+1);

    write_long(0xA12010, 0x0C0000); 
    wait_do_cmd('M'); 
    wait_cmd_ack();
    res.ln[0] = read_long(0xA12020);
    res.ln[1] = read_long(0xA12024);
    write_byte(0xA1200E, 0x00); 

    if (res.value < 0)
        return res.value;

#if 1
    numwords = (res.ln[0] + 1)/2;
    for (i = 0; i < numwords; i++) {
        ((volatile int16_t *)buf)[i] = ((volatile int16_t *)scdWordRam)[i];
    }
#else
    memcpy(buf, scdWordRam, res.ln[0]);
#endif

    return res.value;
}

void scd_read_sectors(void *ptr, int lba, int len, void (*wait)(void))
{
    char ack = 0;
    if (!wait)
        wait = scd_delay;

    write_long(0xA12010, (uintptr_t)ptr);
    write_long(0xA12014, lba);
    write_long(0xA12018, len);
    wait_do_cmd('H');
    do {
        wait();
        ack = read_byte(0xA1200F); 
    } while (!ack);
    write_byte(0xA1200E, 0x00); 
}

void scd_switch_to_bank(int bank)
{
    write_byte(0xA12010, bank);
    wait_do_cmd('J');
    wait_cmd_ack();
    write_byte(0xA1200E, 0x00); 
}

void scd_play_spcm_track(const char *name, int repeat)
{
    char *scdWordRam = (char *)0x600000; 

    memcpy(scdWordRam, name, mystrlen(name)+1);
    write_long(0xA12010, 0x0C0000); 
    write_long(0xA12014, repeat);
    wait_do_cmd('Q'); 
    wait_cmd_ack();
    write_byte(0xA1200E, 0x00); 
}

void scd_stop_spcm_track(void)
{
    wait_do_cmd('R'); 
    wait_cmd_ack();
    write_byte(0xA1200E, 0x00); 
}

void scd_resume_spcm_track(void)
{
    wait_do_cmd('X'); 
    wait_cmd_ack();
    write_byte(0xA1200E, 0x00); 
}

int scd_get_spcm_playback_status(void)
{
    return read_byte(0xA1202E);
}

void scd_set_volume(int volume)
{
    write_word(0xA12010, volume);
    wait_do_cmd('V'); 
    wait_cmd_ack();
    write_byte(0xA1200E, 0x00); 
}

int64_t scd_get_disc_info(void)
{
    union {
        int16_t lo[4];
        int64_t value;
    } res;

    wait_do_cmd('D'); 
    wait_cmd_ack();
    res.lo[0] = read_word(0xA12020); 
    res.lo[1] = read_word(0xA12022); 
    res.lo[2] = read_word(0xA12024); 
    res.lo[3] = 0;
    write_byte(0xA1200E, 0x00); 

    return res.value;
}

int64_t scd_get_track_info(int track)
{
    union {
        int32_t lo[2];
        int64_t value;
    } res;

    write_word(0xA12010, track);
    wait_do_cmd('T'); 
    wait_cmd_ack();
    res.lo[0] = read_long(0xA12020); 
    res.lo[1] = read_byte(0xA12024) & 0xff; 
    write_byte(0xA1200E, 0x00); 

    return res.value;
}

void scd_play_cdda_track(int track, int repeat)
{
    write_word(0xA12010, track);
    write_byte(0xA12012, repeat);
    wait_do_cmd('P'); 
    wait_cmd_ack();
    write_byte(0xA1200E, 0x00); 
}

void scd_stop_cdda_playback(void)
{
    wait_do_cmd('S'); 
    wait_cmd_ack();
    write_byte(0xA1200E, 0x00); 
}

void scd_toggle_cdda_pause(void)
{
    wait_do_cmd('Z'); 
    wait_cmd_ack();
    write_byte(0xA1200E, 0x00); 
}

void scd_open_tray(void)
{
    wait_do_cmd('Y'); 
    wait_cmd_ack();
    write_byte(0xA1200E, 0x00); 
}

int scd_stream_cd(int lba, int len)
{
    int wram_ofs;
    write_long(0xA12010, lba);
    write_long(0xA12014, len);
    wait_do_cmd('['); 
    wait_cmd_ack();
    wram_ofs = read_long(0xA12020);
    write_byte(0xA1200E, 0x00); 
    return wram_ofs;
}

void scd_stream_cmd(char cmd)
{
    wait_do_cmd(cmd);
    wait_cmd_ack();
    write_byte(0xA1200E, 0x00); 
}

uint16_t InitCD(void)
{
    uint32_t b_addr;
    uint8_t *bios;

    init_diag_print(18, "Init 32XCD Engine               ");

    {
        memcpy(ram_flash_array, trivial_test_code, sizeof(trivial_test_code));
        void (*trivial_func)(void) = (void (*)(void))ram_flash_array;
        trivial_func();
        init_diag_print(10, "TRIVIAL RTS: returned OK        ");
    }

    /* Scan for Sega CD BIOS safely using byte pointer */
    uint8_t *base = (uint8_t *)0x400000;
    if (base[0x1586D] == 'S') bios = (uint8_t *)0x415800;
    else if (base[0x1606D] == 'S') bios = (uint8_t *)0x416000;
    else if (base[0x1AD6D] == 'W') bios = (uint8_t *)0x41AD00;
    else if (base[0x0D56D] == 'S') bios = (uint8_t *)0x40D500;
    else bios = (uint8_t *)0x415800; 
    b_addr = (uint32_t)bios;

    write_word(0xA12002, 0xFF00);
    write_byte(0xA12001, 0x03);
    write_byte(0xA12001, 0x02);
    write_byte(0xA12001, 0x00);

    write_byte(0xA12001, 0x02);
    while (!(read_byte(0xA12001) & 2)) write_byte(0xA12001, 0x02); 

    init_diag_print(19, "Bus Requested                   ");

    write_word(0xA12002, 0x0002); // Give Word RAM to Main CPU

    /* 1. DYNAMIC EXECUTION: Patch array and fetch true BIOS block into upper Word RAM */
    memcpy(ram_flash_array, bios_fetch_code, sizeof(bios_fetch_code));
    
    // Patch indices 12 and 13 with the exact BIOS source pointer!
    ram_flash_array[12] = (uint16_t)(b_addr >> 16);
    ram_flash_array[13] = (uint16_t)(b_addr & 0xFFFF);
    
    void (*fetch_func)(void) = (void (*)(void))ram_flash_array;
    fetch_func();

    /* 2. Decompress the BIOS from UPPER Word RAM into LOWER Word RAM natively. */
    *(volatile uint32_t *)0x21FFFC = 0xCAFEBABE;
    {
        uint32_t precheck = *(volatile uint32_t *)0x21FFFC;
        char pbuf[40];
        sprintf(pbuf, "PRE-CHECK: %08lX %s        ", (unsigned long)precheck,
                (precheck == 0xCAFEBABE) ? "(OK)" : "(BAD ADDR)");
        init_diag_print(11, pbuf);
    }

    Kos_Decomp((uint8_t *)0x220000, (uint8_t *)0x200000);

    {
        uint32_t canary = *(volatile uint32_t *)0x21FFFC;
        char cbuf[40];
        sprintf(cbuf, "CANARY: %08lX %s        ", (unsigned long)canary,
                (canary == 0xCAFEBABE) ? "(OK)" : "(HIT!)");
        init_diag_print(12, cbuf);
    }

    /* 3. Inject Sub-CPU Payload into the newly uncompressed BIOS image inside Word RAM */
    memcpy((char *)0x206000, (char *)&Sub_Start, (int)&Sub_End - (int)&Sub_Start);

    init_diag_print(20, "Assembled Payload in Word RAM   ");

    /* 4. Direct copy from Word RAM to PRG RAM - matching the real
       reference implementation exactly. The hand-assembled RV-toggle
       flash routine that was here has been dropped: it stalled on
       both emulators AND real hardware with no working fix found, and
       none of that machinery exists in the actual working reference
       this project is based on - it was added to solve a different,
       unrelated problem (BIOS-address detection) and was never
       confirmed necessary for this step. */
    memcpy((char *)0x420000, (char *)0x200000, 0x20000);

    init_diag_print(21, "Flashed PRG RAM (direct)        ");

    write_byte(0xA1200E, 0x00); 
    write_byte(0xA12002, 0x2A); 
    write_byte(0xA12001, 0x01); 
    while (!(read_byte(0xA12001) & 1)) write_byte(0xA12001, 0x01); 

    init_diag_print(22, "Sub-CPU Un-Halted               ");

    gen_lvl2 = 1; 

    int timeout = 0;
    while (read_byte(0xA1200F) != 'I')
    {
        scd_delay(); 
        timeout++;
        if (timeout > 200000) 
        {
            gen_lvl2 = 0;
            return 0; 
        }
    }

    init_diag_print(23, "32X Handshake OK                ");

    while (read_byte(0xA1200F) != 0x00) ;

    scd_init_pcm();

    init_diag_print(24, "32X GO!                         ");

    return 0x1; 
}