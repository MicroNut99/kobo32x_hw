# Kobo 32X (RAM Cartridge Edition)
This is a specialized build of the Kobo Deluxe port for the Sega 32X. Unlike standard Sega CD / 32XCD releases, 
this version is engineered specifically to execute entirely the Sega CD hardware for audio and Sub-CPU processing.
Game assets are stored on the RAM cartridge while still fully interfacing with SEGA CD for CDDA audio.

## Reverse Engineering the Boot Sequence
Getting a 32XCD title to boot and stream assets cleanly from a CD requires understanding and writing standard Sega CD32X boot procedures. 
To accomplish this, the initialization sequence from the original Sega CD32X release of *Night Trap* was heavily analyzed and reverse-engineered.
This effort was a collaborative human-AI undertaking. Both **Gemini** and **Claude** were utilized extensively to analyze the raw disassembly, 
trace the 68000/SH2 handoffs, document the *Night Trap* boot process, and write a custom initialization sequence that stabilizes the hardware.

## Technical Documentation
The findings, memory maps, and technical breakdowns from the AI-assisted reverse engineering process have been compiled 
into the two reference documents included in this repository. 

If you are developing homebrew for the 32XCD or trying to understand Sub-CPU and SH2 communication during boot, please refer to:
* **`KOBO_CD32X_BOOT_PROCESS.pdf`**
  A comprehensive overview of the CD to 32X handoff, memory allocation strategies, 
  and the specific initialization stages required to boot successfully from a cartridge environment.
* **`KOBO-Sega 32XCD Boot Sequence and Sub-CPU Command Ledger.pdf`**
  A granular reference containing the exact Motorola 68000 / Sub-CPU command ledger, expected file-handling routines, timing requirements, 
  and hardware register states mapped during the *CD32X Night Trap* analysis.

## Build Requirements
To compile this project from source, you must use **Chilly Willy's Sega MD/CD/32X devkit** (sh-elf + m68k-elf GCC). 

You can download the required toolchain from the 32XDK releases page:
[https://github.com/viciious/32XDK/releases](https://github.com/viciious/32XDK/releases)

Ensure the toolchain is correctly extracted to `sega-toolchain-12.1/sega/kobo32x_hw`) before building. 

Compile using the provided `make` files. 
