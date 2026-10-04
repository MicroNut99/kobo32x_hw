#include "doomdef.h"
#include "mars.h"
#include "32x.h"

/* ===========================================================================
 * Matches the real, working main.c's actual protocol exactly (not the
 * file-index redesign from earlier tonight, which was a different,
 * divergent design):
 *   - COMM4 = 0x1234 is the boot/start signal from Main CPU.
 *   - cmd=38 on COMM0 asks Main to open "IMAGE.RAW" (hardcoded on the
 *     Main side - this SH2 code doesn't need to send a filename).
 *     Main clears cmd to 0 when done, and reports length via COMM8 (32-bit).
 *   - cmd=40 asks Main to stream the file. This SH2 supplies ITS OWN
 *     destination pointer via COMM12 (32-bit) before sending cmd=40;
 *     Main computes the real Main-CPU address as
 *     (sh2_dest & 0xFFFFFF) | 0x400000 - so the low 24 bits of whatever
 *     this code writes to COMM12 become the byte offset into the cart.
 *   - Real cart address mapping (confirmed against the real "SRAM Test
 *     for 32X" project): SH2 address = 0x22000000 + Main-CPU address.
 * ===========================================================================
 */

void pri_vbi_handler(void) {}
void pri_cmd_handler(void) {}
void sec_cmd_handler(void) {}
void sec_dma1_handler(void) {}
void secondary(void) { while(1); }

volatile unsigned mars_pwdt_ovf_count = 0;
volatile unsigned mars_swdt_ovf_count = 0;

uint32_t _text_end;
uint32_t _data_size;
uint32_t _bss_start;
uint32_t _bss_end;

int main(void);
void _main(void) { main(); }

/* 32-bit comm registers, repurposed for our own protocol needs (their
   "official" meaning elsewhere - controller/vcount data - doesn't apply
   in this CD-streaming context). Reading/writing as a long spans two
   adjacent 16-bit comm registers, same pattern crt0.s itself uses. */
#define SH2_COMM8_32  (*(volatile uint32_t*)0x20004028)
#define SH2_COMM12_32 (*(volatile uint32_t*)0x2000402C)

#define CMD_OPEN_FILE   38
#define CMD_STREAM_FILE 40

/* Destination offset into the cart, our own choice - 0 = start of the
   4MB window at Main-CPU 0x400000 / SH2 0x22400000. */
#define CART_DEST_OFFSET 0x000000
#define CART_BASE_SH2    0x22400000

int main(void) {
    MARS_SYS_COMM0 = 0;

    /* Wait for Main CPU's boot/start signal. */
    while (MARS_SYS_COMM4 != 0x1234) {
        /* hard spin - Main CPU retries this write repeatedly */
    }
    MARS_SYS_COMM4 = 0;

    /* Request IMAGE.RAW be opened (hardcoded on the Main CPU side). */
    MARS_SYS_COMM0 = CMD_OPEN_FILE;
    while (MARS_SYS_COMM0 != 0) {
        /* hard spin - Main CPU clears cmd when the open completes */
    }

    uint32_t file_length = SH2_COMM8_32;
    if ((int32_t)file_length <= 0) {
        while (1) {}  /* open failed */
    }

    /* Supply our destination pointer, then request the stream. */
    SH2_COMM12_32 = CART_DEST_OFFSET;
    MARS_SYS_COMM0 = CMD_STREAM_FILE;
    while (MARS_SYS_COMM0 != 0) {
        /* hard spin - streaming may take a while, no timeout needed */
    }

    /* Data is now sitting in the real cart at this address. */
    volatile uint8_t *cart_data = (volatile uint8_t *)(CART_BASE_SH2 + CART_DEST_OFFSET);
    (void)cart_data;  /* display/further processing is the next step */

    while (1) {}
    return 0;
}
