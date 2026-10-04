.text

| Standard MegaCD Sub-CPU Program Header (copied to 0x6000)

SPHeader:
        .asciz  "MAIN-SUBCPU"
        .word   0x0001,0x0000
        .long   0x00000000
        .long   0x00000000
        .long   SPHeaderOffsets-SPHeader
        .long   0x00000000

SPHeaderOffsets:
        .word   SPInit-SPHeaderOffsets
        .word   SPMain-SPHeaderOffsets
        .word   SPInt2-SPHeaderOffsets
        .word   SPNull-SPHeaderOffsets
        .word   0x0000

| Sub-CPU Program Initialization (VBlank not enabled yet)

SPInit:
        move.b  #'I,0x800F.w            /* sub comm port = INITIALIZING */
        andi.b  #0xE2,0x8003.w          /* Priority Mode = off, 2M mode, Sub-CPU has access */
        bset    #2,0x8003.w             /* switch to 1M mode */
        rts

| Sub-CPU Program Main Entry Point (VBlank now enabled)

SPMain:
        move.w  #0x0081,d0              /* CDBSTAT */
        jsr     0x5F22.w                /* call CDBIOS function */
        move.w  0(a0),d0                /* BIOS status word */
        bmi.b   1f                      /* not ready */
        lsr.w   #8,d0
        cmpi.b  #0x40,d0
        beq.b   9f                      /* open */
        cmpi.b  #0x10,d0
        beq.b   9f                      /* no disc */
1:
| Initialize Drive
        lea     drive_init_parms(pc),a0
        move.w  #0x0010,d0              /* DRVINIT */
        jsr     0x5F22.w                /* call CDBIOS function */

        move.w  #0x0085,d0              /* BIOS_FDRSET - set audio volume */
        move.w  #0x8400.w,d1            /* master volume (0 to 1024) */
        jsr     0x5F22.w                /* call CDBIOS function */

        move.w  #0x0089,d0              /* CDCSTOP - stop reading data */
        jsr     0x5F22.w                /* call CDBIOS function */
9:
| Initialize the ISO file system PVD parsing[cite: 15]
        lea     iso_pvd_magic(pc),a5[cite: 15]
        bsr     InitCD[cite: 15]

| Wait for Main CPU 'I' signal before opening comm port
        move.b  #0xFF,0x800F.w
10:
        cmpi.b  #'I,0x800E.w
        bne.b   10b
        move.b  #0,0x800E.w
        move.b  #0,0x800F.w             /* sub comm port = READY */[cite: 15]

| wait for command in main comm port
WaitCmd:
        tst.b   updates_suspend[cite: 15]
        bne     WaitCmdPostUpdate[cite: 15]

        jsr     S_Update[cite: 15]
        
WaitCmdPostUpdate:
        tst.b   0x800E.w[cite: 15]
        beq.b   WaitCmd[cite: 15]

        moveq   #0,d0[cite: 15]
        move.b  0x800E.w,d0[cite: 15]
        sub.b   #'A,d0[cite: 15]
        add.w   d0,d0[cite: 15]
        move.w  RequestTable(pc,d0.w),d0[cite: 15]
        jmp     RequestTable(pc,d0.w)[cite: 15]

        | from 'A' to '['
RequestTable:
        dc.w    SfxPlaySource - RequestTable[cite: 15]
        dc.w    SfxCopyBuffer - RequestTable[cite: 15]
        dc.w    CheckDisc - RequestTable[cite: 15]
        dc.w    GetDiscInfo - RequestTable[cite: 15]
        dc.w    SfxSuspendUpdates - RequestTable[cite: 15]
        dc.w    OpenFile - RequestTable[cite: 15]
        dc.w    SfxGetSourcePosition - RequestTable[cite: 15]
        dc.w    ReadSectors - RequestTable[cite: 15]
        dc.w    SfxInit - RequestTable[cite: 15]
        dc.w    SwitchToBank - RequestTable[cite: 15]
        dc.w    SfxCopyBuffersFromCDFile - RequestTable[cite: 15]
        dc.w    SfxClear - RequestTable[cite: 15]
        dc.w    ReadDir - RequestTable[cite: 15]
        dc.w    SfxPUnPSource - RequestTable[cite: 15]
        dc.w    SfxStopSource - RequestTable[cite: 15]
        dc.w    PlayTrack - RequestTable[cite: 15]
        dc.w    PlaySPCMTrack - RequestTable[cite: 15]
        dc.w    StopSPCMTrack - RequestTable[cite: 15]
        dc.w    StopPlayback  - RequestTable[cite: 15]
        dc.w    GetTrackInfo - RequestTable[cite: 15]
        dc.w    SfxUpdateSource - RequestTable[cite: 15]
        dc.w    SetVolume - RequestTable[cite: 15]
        dc.w    SfxRewindSource - RequestTable[cite: 15]
        dc.w    ResumeSPCMTrack - RequestTable[cite: 15]
        dc.w    OpenTray - RequestTable[cite: 15]
        dc.w    PauseResume - RequestTable[cite: 15]
        dc.w    StreamCD - RequestTable[cite: 15]

UknownCmd:
        move.b  #'E,0x800F.w            /* sub comm port = ERROR */[cite: 15]
WaitAck:
        tst.b   0x800E.w[cite: 15]
        bne.b   WaitAck                 /* wait for result acknowledged */[cite: 15]
        move.b  #0,0x800F.w             /* sub comm port = READY */[cite: 15]
        bra.w   WaitCmd[cite: 15]

GetDiscInfo:
        move.w  #0x0081,d0              /* CDBSTAT */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]
        move.w  0(a0),0x8020.w          /* BIOS status word */[cite: 15]
        move.w  16(a0),0x8022.w         /* First song number, Last song number */[cite: 15]
        move.w  18(a0),0x8024.w         /* Drive version, Flag */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

GetTrackInfo:
        move.w  0x8010.w,d1             /* track number */[cite: 15]
        move.w  #0x0083,d0              /* CDBTOCREAD */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]
        move.l  d0,0x8020.w             /* MMSSFFTN */[cite: 15]
        move.b  d1,0x8024.w             /* track type */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

PlayTrack:
        move.w  #0x0002,d0              /* MSCSTOP - stop playing */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        move.w  0x8010.w,d1             /* track number */[cite: 15]
        move.w  #0x0011,d0              /* MSCPLAY - play from track on */[cite: 15]
        move.b  0x8012.w,d2             /* flag */[cite: 15]
        bmi.b   2f[cite: 15]
        beq.b   1f[cite: 15]
        move.w  #0x0013,d0              /* MSCPLAYR - play with repeat */[cite: 15]
        bra.b   2f[cite: 15]
1:
        move.w  #0x0012,d0              /* MSCPLAY1 - play once */[cite: 15]
2:
        lea     track_number(pc),a0[cite: 15]
        move.w  d1,(a0)[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

StopPlayback:
        move.w  #0x0002,d0              /* MSCSTOP - stop playing */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

SetVolume:
        move.w  #0x0085,d0              /* BIOS_FDRSET - set audio volume */[cite: 15]
        move.w  0x8010.w,d1             /* cd volume (0 to 1024) */[cite: 15]
        move.w  d1,CDA_VOLUME[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

PauseResume:
        move.w  #0x0081,d0              /* CDBSTAT */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]
        move.b  (a0),d0[cite: 15]
        cmpi.b  #1,d0[cite: 15]
        beq.b   1f                      /* playing - pause playback */[cite: 15]
        cmpi.b  #5,d0[cite: 15]
        beq.b   2f                      /* paused - resume playback */[cite: 15]

        move.b  #'E,0x800F.w            /* sub comm port = ERROR */[cite: 15]
        bra     WaitAck[cite: 15]
1:
        move.w  #0x0003,d0              /* MSCPAUSEON - pause playback */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]
2:
        move.w  #0x0004,d0              /* MSCPAUSEOFF - resume playback */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

CheckDisc:
        lea     drive_init_parms(pc),a0[cite: 15]
        move.w  #0x0010,d0              /* DRVINIT */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        move.w  #0x0089,d0              /* CDCSTOP - stop reading data */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

SfxInit:
        jsr     S_Init[cite: 15]
        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

SfxClear:
        jsr     S_Clear[cite: 15]
        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

SfxCopyBuffer:
        move.b  0x8013.w,d1             /* flags */[cite: 15]
        move.l  0x8018.w,d0             /* length */[cite: 15]
        move.l  d0,-(sp)[cite: 15]
        move.l  0x8014.w,d0             /* address in RAM */[cite: 15]
        move.l  d0,-(sp)[cite: 15]
        moveq   #0,d0[cite: 15]
        move.w  0x8010.w,d0             /* buffer id */[cite: 15]
        move.l  d0,-(sp)[cite: 15]
        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
SfxCopyBufferWaitAck:
        tst.b   0x800E.w[cite: 15]
        bne.b   SfxCopyBufferWaitAck    /* wait for result acknowledged */[cite: 15]
        move.b  #0,0x800F.w             /* sub comm port = READY */[cite: 15]

        btst.b  #0,d1[cite: 15]
        bne.b   SfxSetBufferPtr[cite: 15]

        jsr     switch_banks[cite: 15]
        jsr     S_CopyBufferData        /* copy the buffer data in the background */[cite: 15]
        lea     12(sp),sp               /* clear the stack */[cite: 15]
        bra.w   WaitCmd[cite: 15]
SfxSetBufferPtr:
        jsr     S_SetBufferData         /* update the buffer data pointer in the background */[cite: 15]
        lea     12(sp),sp               /* clear the stack */[cite: 15]
        bra.w   WaitCmd[cite: 15]

SfxCopyBuffersFromCDFile:
        jsr     S_PauseSPCMTrack[cite: 15]

        jsr     switch_banks[cite: 15]

        move.l  #0x0C0000,d0            /* file name + offsets */[cite: 15]
        move.l  d0,-(sp)[cite: 15]
        moveq   #0,d0[cite: 15]
        move.w  0x8012.w,d0             /* num sfx */[cite: 15]
        move.l  d0,-(sp)[cite: 15]
        move.w  0x8010.w,d0             /* start buffer id */[cite: 15]
        move.l  d0,-(sp)[cite: 15]
        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
SfxCopyBuffersFromCDFileWaitAck:
        tst.b   0x800E.w[cite: 15]
        bne.b   SfxCopyBuffersFromCDFileWaitAck  /* wait for result acknowledged */[cite: 15]
        move.b  #0,0x800F.w             /* sub comm port = READY */[cite: 15]
        jsr     S_LoadCDBuffers         /* copy the buffer data in the background */[cite: 15]
        lea     12(sp),sp               /* clear the stack */[cite: 15]

        |jsr     S_SPCM_Unsuspend[cite: 15]

        bra.w   WaitCmd[cite: 15]

PlaySPCMTrack:
        jsr     switch_banks[cite: 15]

        moveq   #0,d0[cite: 15]

        move.l  0x8014.w,d0             /* repeat/autoloop flag */[cite: 15]
        move.l  d0,-(sp)[cite: 15]
        move.l  0x8010.w,d0             /* file name */[cite: 15]
        move.l  d0,-(sp)[cite: 15]

        jsr     S_PlaySPCMTrack[cite: 15]
        lea     8(sp),sp                /* clear the stack */[cite: 15]

        move.w  d0,0x8020.w             /* 0 if the playback failed */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

StopSPCMTrack:
        jsr     S_StopSPCMTrack[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

ResumeSPCMTrack:
        jsr     S_UnpauseSPCMTrack[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

OpenTray:
        lea     0x18A,a0                /* ROM version */[cite: 15]
        move.b  (a0),d0                 /* refer to MEGA CD TECHNICAL BULLETIN #8 */[cite: 15]
        cmpi.b  #'1, d0                 /* '1' for Model 1 */[cite: 15]
        bne.b   1f[cite: 15]

        move.w  #0x000A,d0              /* DRVOPEN */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]
1:
        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

StreamCD:
        jsr     S_GetMemBankPtr[cite: 15]

        move.l  d0,-(sp)                /* re-use the main sound buffer */[cite: 15]
        move.l  0x8014.w,d0[cite: 15]
        move.l  d0,-(sp)                /* length */[cite: 15]
        move.l  0x8010.w,d0[cite: 15]
        move.l  d0,-(sp)                /* start sector */[cite: 15]

        jsr     stream_cd[cite: 15]
        lea     12(sp),sp                /* clear the stack */[cite: 15]

        |jsr     S_ClearBuffersMem[cite: 15]

        move.l  #-1,CURR_OFFSET[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

SfxPlaySource:
| uint8_t S_PlaySource(uint8_t src_id, uint16_t buf_id, uint16_t freq, uint8_t pan, uint8_t vol, uint8_t autoloop);
        moveq   #0,d0[cite: 15]

        move.b  0x801b.w,d0[cite: 15]
        move.l  d0,-(sp)                /* autoloop */[cite: 15]
        move.b  0x8019.w,d0[cite: 15]
        move.l  d0,-(sp)                /* vol */[cite: 15]

        move.b  0x8017.w,d0[cite: 15]
        move.l  d0,-(sp)                /* pan */[cite: 15]
        move.w  0x8014.w,d0[cite: 15]
        move.l  d0,-(sp)                /* freq */[cite: 15]

        move.w  0x8012.w,d0[cite: 15]
        move.l  d0,-(sp)                /* buf_id */[cite: 15]
        move.w  0x8010.w,d0[cite: 15]
        move.l  d0,-(sp)                /* src_id */[cite: 15]

        jsr     S_PlaySource[cite: 15]
        lea     24(sp),sp               /* clear the stack */[cite: 15]

        move.b  d0,0x8020.w             /* src_id */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

SfxPUnPSource:
| void S_PUnPSource(uint8_t src_id, uint8_t pause);
        moveq   #0,d0[cite: 15]

        move.b  0x8013.w,d0[cite: 15]
        move.l  d0,-(sp)                /* paused */[cite: 15]
        move.w  0x8010.w,d0[cite: 15]
        move.l  d0,-(sp)                /* src_id */[cite: 15]

        jsr     S_PUnPSource[cite: 15]
        lea     8(sp),sp                /* clear the stack */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

SfxRewindSource:
| void S_RewindSource(uint8_t src_id);
        moveq   #0,d0[cite: 15]

        move.w  0x8010.w,d0[cite: 15]
        move.l  d0,-(sp)                /* src_id */[cite: 15]

        jsr     S_RewindSource[cite: 15]
        lea     4(sp),sp                /* clear the stack */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

SfxStopSource:
| void S_StopSource(uint8_t src_id);
        moveq   #0,d0[cite: 15]

        move.w  0x8010.w,d0[cite: 15]
        move.l  d0,-(sp)                /* src_id */[cite: 15]

        jsr     S_StopSource[cite: 15]
        lea     4(sp),sp                /* clear the stack */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

SfxUpdateSource:
| void S_UpdateSource(uint8_t src_id, uint16_t freq, uint8_t pan, uint8_t vol, uint8_t autoloop);
        moveq   #0,d0[cite: 15]

        move.b  0x801b.w,d0[cite: 15]
        move.l  d0,-(sp)                /* autoloop */[cite: 15]
        move.b  0x8019.w,d0[cite: 15]
        move.l  d0,-(sp)                /* vol */[cite: 15]

        move.b  0x8017.w,d0[cite: 15]
        move.l  d0,-(sp)                /* pan */[cite: 15]
        move.w  0x8014.w,d0[cite: 15]
        move.l  d0,-(sp)                /* freq */[cite: 15]

        move.w  0x8010.w,d0[cite: 15]
        move.l  d0,-(sp)                /* src_id */[cite: 15]

        jsr     S_UpdateSource[cite: 15]
        lea     20(sp),sp               /* clear the stack */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

SfxGetSourcePosition:
| void S_GetSourcePosition(uint8_t src_id);
        moveq   #0,d0[cite: 15]
        move.w  0x8010.w,d0[cite: 15]
        move.l  d0,-(sp)                /* src_id */[cite: 15]

        jsr     S_GetSourcePosition[cite: 15]
        lea     4(sp),sp                /* clear the stack */[cite: 15]

        move.w  d0,0x8020.w             /* position */[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

SfxSuspendUpdates:
        move.b  0x8010.w,updates_suspend[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

OpenFile:
        jsr     S_PauseSPCMTrack[cite: 15]

        jsr     switch_banks[cite: 15]

        move.l  0x8010.w,d0             /* name */[cite: 15]
        move.l  d0,-(sp)[cite: 15]
        jsr     open_file[cite: 15]
        lea     4(sp),sp                /* clear the stack */[cite: 15]
        move.l  d0,0x8020.w             /* length */[cite: 15]
        move.l  d1,0x8024.w             /* offset */[cite: 15]
        jsr     switch_banks[cite: 15]

        |jsr     S_SPCM_Unsuspend[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

ReadDir:
        jsr     S_PauseSPCMTrack[cite: 15]

        jsr     switch_banks[cite: 15]

        move.l  0x8010.w,d0             /* path */[cite: 15]
        move.l  d0,-(sp)[cite: 15]
        jsr     read_directory[cite: 15]
        lea     4(sp),sp                /* clear the stack */[cite: 15]
        move.l  d0,0x8020.w             /* buffer length */[cite: 15]
        move.l  d1,0x8024.w             /* num entries */[cite: 15]
        jsr     switch_banks[cite: 15]

        |jsr     S_SPCM_Unsuspend[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

ReadSectors:
        jsr     S_PauseSPCMTrack[cite: 15]

        move.l  0x8018.w,d0             /* length */[cite: 15]
        move.l  d0,-(sp)[cite: 15]
        move.l  0x8014.w,d0             /* lba */[cite: 15]
        move.l  d0,-(sp)[cite: 15]
        move.l  0x8010.w,d0             /* address in RAM */[cite: 15]
        move.l  d0,-(sp)[cite: 15]
        jsr     read_sectors[cite: 15]
        lea     12(sp),sp               /* clear the stack */[cite: 15]
        jsr     switch_banks[cite: 15]

        |jsr     S_SPCM_Unsuspend[cite: 15]

        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

SwitchToBank:
        btst    #0,0x8010.w[cite: 15]
        bne.b   1f[cite: 15]

        bclr    #0,0x8003.w             /* switch banks */[cite: 15]
0:
        btst    #1,0x8003.w[cite: 15]
        bne.b   0b                      /* bank switch not finished */[cite: 15]
        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

1:
        bset    #0,0x8003.w             /* switch banks */[cite: 15]
11:
        btst    #1,0x8003.w[cite: 15]
        bne.b   11b                     /* bank switch not finished */[cite: 15]
        move.b  #'D,0x800F.w            /* sub comm port = DONE */[cite: 15]
        bra     WaitAck[cite: 15]

| void switch_banks(void);
| Switch 1M Banks
        .global switch_banks
switch_banks:
        bchg    #0,0x8003.w             /* switch banks */[cite: 15]
0:
        btst    #1,0x8003.w[cite: 15]
        bne.b   0b                      /* bank switch not finished */[cite: 15]
        rts

| Sub-CPU Program VBlank (INT02) Service Handler

SPInt2:
        rts

| Sub-CPU program Reserved Function

SPNull:
        rts


|-----------------------------------------------------------------------|
|                            Sub-CPU code                               |
|-----------------------------------------------------------------------|


| ISO directory offsets (big-endian where applicable)
        .equ    RECORD_LENGTH,  0
        .equ    EXTENT,         6
        .equ    FILE_LENGTH,    14
        .equ    FILE_FLAGS,     25
        .equ    FILE_NAME_LEN,  32
        .equ    FILE_NAME,      33

| Primary Volume Descriptor offset
        .equ    PVD_ROOT, 0x9C

| CDFS Error codes
        .equ    ERR_READ_FAILED,    -2
        .equ    ERR_NO_PVD,         -3
        .equ    ERR_NO_MORE_ENTRIES,-4
        .equ    ERR_BAD_ENTRY,      -5
        .equ    ERR_NAME_NOT_FOUND, -6
        .equ    ERR_NO_DISC,        -7

| XCMD Error codes
        .equ    ERR_UNKNOWN_CMD,    -1
        .equ    ERR_CMDDONE_TIMEOUT,-2
        .equ    ERR_CMDACK_TIMEOUT, -3

|-----------------------------------------------------------------------
| Global functions
|-----------------------------------------------------------------------

| int init_cd(void);
        .global init_cd
init_cd:
        movem.l d2-d7/a2-a6,-(sp)[cite: 15]
        lea     iso_pvd_magic,a5[cite: 15]
        jsr     InitCD[cite: 15]
        movem.l (sp)+,d2-d7/a2-a6[cite: 15]
        rts

| int seek_cd(int lba);
        .global seek_cd
seek_cd:
        move.l  4(sp),d0                /* lba */[cite: 15]
        movem.l d2-d7/a2-a6,-(sp)[cite: 15]
        jsr     SeekCD[cite: 15]
        movem.l (sp)+,d2-d7/a2-a6[cite: 15]
        rts

| int begin_read_cd(int lba, int len);
        .global begin_read_cd
begin_read_cd:
        move.l  4(sp),d0                /* lba */[cite: 15]
        move.l  8(sp),d1                /* length */[cite: 15]
        movem.l d2-d7/a2-a6,-(sp)[cite: 15]
        jsr     BeginReadCD[cite: 15]
        movem.l (sp)+,d2-d7/a2-a6[cite: 15]
        rts

| int dma_cd_sector_pcm(void *buffer);
        .global dma_cd_sector_pcm
dma_cd_sector_pcm:
        move.w  #0x4,d0                 /* set CDC Mode destination device to PCM DMA */[cite: 15]
        movea.l 4(sp),a0                /* buffer */[cite: 15]
        movem.l d2-d7/a2-a6,-(sp)[cite: 15]
        jsr     ReadSectorDMA[cite: 15]
        movem.l (sp)+,d2-d7/a2-a6[cite: 15]
        rts

| int dma_cd_sector_prg(void *buffer);
        .global dma_cd_sector_prg
dma_cd_sector_prg:
        move.w  #0x5,d0                 /* set CDC Mode destination device to PRG RAM DMA */[cite: 15]
        movea.l 4(sp),a0                /* buffer */[cite: 15]
        movem.l d2-d7/a2-a6,-(sp)[cite: 15]
        jsr     ReadSectorDMA[cite: 15]
        movem.l (sp)+,d2-d7/a2-a6[cite: 15]
        rts

| int dma_cd_sector_wram(void *buffer);
        .global dma_cd_sector_wram
dma_cd_sector_wram:
        move.w  #0x7,d0                 /* set CDC Mode destination device to PRG RAM DMA */[cite: 15]
        movea.l 4(sp),a0                /* buffer */[cite: 15]
        movem.l d2-d7/a2-a6,-(sp)[cite: 15]
        jsr     ReadSectorDMA[cite: 15]
        movem.l (sp)+,d2-d7/a2-a6[cite: 15]
        rts

| int stop_read_cd();
        .global stop_read_cd
stop_read_cd:
        move.l  4(sp),d0                /* lba */[cite: 15]
        move.l  8(sp),d1                /* length */[cite: 15]
        movem.l d2-d7/a2-a6,-(sp)[cite: 15]
        jsr     StopReadCD[cite: 15]
        movem.l (sp)+,d2-d7/a2-a6[cite: 15]
        rts

| int set_cwd(char *path);
        .global set_cwd
set_cwd:
        movea.l 4(sp),a0                /* path */[cite: 15]
        movem.l d2-d7/a2-a6,-(sp)[cite: 15]
        jsr     SetCWD[cite: 15]
        movem.l (sp)+,d2-d7/a2-a6[cite: 15]
        rts

| int first_dir_sec(void);
        .global first_dir_sec
first_dir_sec:
        movem.l d2-d7/a2-a6,-(sp)[cite: 15]
        jsr     FirstDirSector[cite: 15]
        movem.l (sp)+,d2-d7/a2-a6[cite: 15]
        rts

| int next_dir_sec(void);
        .global next_dir_sec
next_dir_sec:
        movem.l d2-d7/a2-a6,-(sp)[cite: 15]
        jsr     NextDirSector[cite: 15]
        movem.l (sp)+,d2-d7/a2-a6[cite: 15]
        rts

| int find_dir_entry(char *name);
        .global find_dir_entry
find_dir_entry:
        movea.l 4(sp),a0[cite: 15]
        movem.l d2-d7/a2-a6,-(sp)[cite: 15]
        jsr     FindDirEntry[cite: 15]
        movem.l (sp)+,d2-d7/a2-a6[cite: 15]
        rts

| int next_dir_entry(void)
        .global next_dir_entry
next_dir_entry:
        movem.l d2-d7/a2-a6,-(sp)[cite: 15]
        jsr     NextDirEntry[cite: 15]
        movem.l (sp)+,d2-d7/a2-a6[cite: 15]
        rts

|----------------------------------------------------------------------|
|                      File System Support Code                        |
|----------------------------------------------------------------------|

| Initialize CD - pass PVD Magic to look for in a5

InitCD:
        lea     drive_init_parms,a0[cite: 15]
        move.w  #0x0010,d0              /* DRVINIT */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        move.w  #0,d1                   /* Mode 1 (CD-ROM with full error correction) */[cite: 15]
        move.w  #0x0096,d0              /* CDCSETMODE */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        moveq   #-1,d0[cite: 15]
        move.l  d0,ROOT_OFFSET[cite: 15]
        move.l  d0,ROOT_LENGTH[cite: 15]
        move.l  d0,CURR_OFFSET[cite: 15]
        move.l  d0,CURR_LENGTH[cite: 15]
        move.w  d0,DISC_TYPE            /* no disc/not recognized */[cite: 15]

| find Primary Volume Descriptor

        moveq   #16,d2                  /* starting sector when searching for PVD */[cite: 15]
0:
        lea     DISC_BUFFER,a0          /* buffer */[cite: 15]
        move.l  d2,d0                   /* sector */[cite: 15]
        moveq   #1,d1                   /* # sectors */[cite: 15]
        move.w  d2,-(sp)[cite: 15]
        bsr     ReadSectorsSUB[cite: 15]
        move.w  (sp)+,d2[cite: 15]
        tst.l   d0[cite: 15]
        bmi.b   9f                      /* error */[cite: 15]
        lea     DISC_BUFFER,a0[cite: 15]
        movea.l a5,a1                   /* PVD magic */[cite: 15]
        cmpm.l  (a0)+,(a1)+[cite: 15]
        bne.b   1f                      /* next sector */[cite: 15]
        cmpm.l  (a0)+,(a1)+[cite: 15]
        bne.b   1f                      /* next sector */[cite: 15]
        /* found PVD */
        move.l  DISC_BUFFER+PVD_ROOT+EXTENT,ROOT_OFFSET[cite: 15]
        move.l  DISC_BUFFER+PVD_ROOT+FILE_LENGTH,ROOT_LENGTH[cite: 15]
        move.w  #0,DISC_TYPE            /* found PVD */[cite: 15]
        moveq   #0,d0[cite: 15]
        rts
1:
        addq.w  #1,d2[cite: 15]
        cmpi.w  #32,d2[cite: 15]
        bne.b   0b                      /* check next sector */[cite: 15]

| No PVD found

        moveq   #ERR_NO_PVD,d0[cite: 15]
9:
        rts

| Set directory entry variables to next entry of directory in disc buffer

NextDirEntry:
        lea     DISC_BUFFER,a0[cite: 15]
        move.w  DIR_ENTRY,d2[cite: 15]
        cmpi.w  #2048,d2[cite: 15]
        blo.b   1f[cite: 15]
        moveq   #ERR_NO_MORE_ENTRIES,d0[cite: 15]
        rts
1:
        tst.b   (a0,d2.w)               /* record length */[cite: 15]
        bne.b   2f[cite: 15]
        move.w  #2048,DIR_ENTRY[cite: 15]
        moveq   #ERR_NO_MORE_ENTRIES,d0[cite: 15]
        rts
2:
        lea     (a0,d2.w),a1            /* entry */[cite: 15]
        moveq   #0,d0[cite: 15]
        move.b  (a1),d0                 /* record length */[cite: 15]
        add.w   d0,d2[cite: 15]
        move.w  d2,DIR_ENTRY            /* next entry */[cite: 15]
        cmpi.w  #2048,d2[cite: 15]
        bls.b   3f[cite: 15]
        moveq   #ERR_NO_MORE_ENTRIES,d0 /* entries should NEVER cross a sector boundary */[cite: 15]
        rts
3:
        tst.b   FILE_NAME_LEN(a1)[cite: 15]
        bne.b   4f[cite: 15]
        moveq   #ERR_BAD_ENTRY,d0[cite: 15]
        rts
4:
        move.b  FILE_NAME_LEN(a1),d0[cite: 15]
        subq.w  #1,d0[cite: 15]
        lea     FILE_NAME(a1),a2[cite: 15]
        lea     DENTRY_NAME,a3[cite: 15]
5:
        move.b  (a2)+,(a3)+[cite: 15]
        dbeq    d0,5b[cite: 15]
        move.b  #0,(a3)                 /* make sure is null-terminated */[cite: 15]

        lea     DENTRY_NAME,a2[cite: 15]
        /* check for special case 0 */
        cmpi.b  #0,(a2)[cite: 15]
        bne.b   9f[cite: 15]
        move.l  #0x2E000000,(a2)        /* "." */[cite: 15]
        bra.b   10f[cite: 15]
9:
        /* check for special case 1 */
        cmpi.b  #1,(a2)[cite: 15]
        bne.b   10f[cite: 15]
        move.l  #0x2E2E0000,(a2)        /* ".." */[cite: 15]
10:
        cmpi.b  #0x3B,(a2)+             /* look for ";" */[cite: 15]
        beq.b   11f[cite: 15]
        tst.b   (a2)[cite: 15]
        bne     10b[cite: 15]
        bra.b   12f[cite: 15]
11:
        move.b  #0,-(a2)                /* apply Rockridge correction to name */[cite: 15]
12:
        move.l  EXTENT(a1),DENTRY_OFFSET[cite: 15]
        move.l  FILE_LENGTH(a1),DENTRY_LENGTH[cite: 15]
        move.b  FILE_FLAGS(a1),DENTRY_FLAGS[cite: 15]

        moveq   #0,d0[cite: 15]
        rts

| Find entry in directory in the disc buffer using name in a0

FindDirEntry:
        move.w  DIR_ENTRY,d0[cite: 15]
        cmpi.w  #2048,d0[cite: 15]
        blo.b   1f[cite: 15]
0:
        moveq   #ERR_NAME_NOT_FOUND,d0[cite: 15]
        rts
1:
        move.l  a0,-(sp)[cite: 15]
        bsr     NextDirEntry[cite: 15]
        movea.l (sp)+,a0[cite: 15]
        bmi.b   FindDirEntry[cite: 15]
| got an entry, check the name
        lea     DENTRY_NAME,a1[cite: 15]
        movea.l a0,a2[cite: 15]
2:
        cmpm.b  (a1)+,(a2)+[cite: 15]
        bne.b   FindDirEntry[cite: 15]
        tst.b   -1(a1)[cite: 15]
        bne.b   2b[cite: 15]
        /* dentry holds match */
        moveq   #0,d0[cite: 15]
        rts

| Read first sector in CWD

FirstDirSector:
        move.l  CWD_OFFSET,d0[cite: 15]
        cmp.l   CURR_OFFSET,d0[cite: 15]
        beq.b   0f                      /* already loaded, just reset length */[cite: 15]
        moveq   #1,d1[cite: 15]
        lea     DISC_BUFFER,a0          /* buffer */[cite: 15]
        bsr     ReadSectorsSUB[cite: 15]
        bmi.b   1f[cite: 15]
        /* disc buffer holds first sector of dir */
        move.l  CWD_OFFSET,CURR_OFFSET[cite: 15]
0:
        clr.l   CURR_LENGTH[cite: 15]
        clr.w   DIR_ENTRY[cite: 15]
        moveq   #0,d0[cite: 15]
1:
        rts

| Read next sector in CWD

NextDirSector:
        addq.l  #1,CURR_OFFSET[cite: 15]
        addi.l  #2048,CURR_LENGTH[cite: 15]
        move.l  CWD_LENGTH,d0[cite: 15]
        cmp.l   CURR_LENGTH,d0[cite: 15]
        bhi.b   0f[cite: 15]
        moveq   #ERR_NO_MORE_ENTRIES,d0[cite: 15]
        rts
0:
        move.l  CURR_OFFSET,d0[cite: 15]
        moveq   #1,d1[cite: 15]
        lea     DISC_BUFFER,a0          /* buffer */[cite: 15]
        bsr     ReadSectorsSUB[cite: 15]
        bmi     1f[cite: 15]
        /* disc buffer holds next sector of dir */
        clr.w   DIR_ENTRY[cite: 15]
        moveq   #0,d0[cite: 15]
1:
        rts

| Set current working directory using path at a0

SetCWD:
        jsr     S_PauseSPCMTrack[cite: 15]

        cmpi.b  #0x2F,(a0)              /* check for leading "/" */[cite: 15]
        bne.b   0f                      /* relative to cwd */[cite: 15]
        /* start at root dir */
        addq.l  #1,a0                   /* skip over "/" */[cite: 15]
        move.l  ROOT_OFFSET,CWD_OFFSET[cite: 15]
        move.l  ROOT_LENGTH,CWD_LENGTH[cite: 15]
0:
        move.l  a0,-(sp)[cite: 15]
        bsr     FirstDirSector          /* disc buffer holds first sector of dir */[cite: 15]
        movea.l (sp)+,a0[cite: 15]
        bmi.b   2f[cite: 15]
        /* check if done */
        tst.b   (a0)[cite: 15]
        bne.b   3f[cite: 15]
1:
        moveq   #0,d0[cite: 15]
2:
        rts
3:
        tst.b   (a0)[cite: 15]
        beq.b   1b                      /* done */[cite: 15]

        /* copy next part of path to temp */
        lea     TEMP_NAME,a1[cite: 15]
4:
        move.b  (a0)+,(a1)+[cite: 15]
        beq.b   5f[cite: 15]
        cmpi.b  #0x2F,-1(a0)            /* check for "/" */[cite: 15]
        bne.b   4b[cite: 15]
5:
        clr.b   -1(a1)                  /* null terminate string in temp */[cite: 15]
        subq.l  #1,a0[cite: 15]
6:
        /* check current directory sector for entry */
        move.l  a0,-(sp)[cite: 15]
        lea     TEMP_NAME,a0[cite: 15]
        bsr     FindDirEntry[cite: 15]
        movea.l (sp)+,a0[cite: 15]
        bmi.b   7f[cite: 15]
        /* found this part of path */
        move.l  DENTRY_OFFSET,CWD_OFFSET[cite: 15]
        move.l  DENTRY_LENGTH,CWD_LENGTH[cite: 15]
        bra.b   0b                      /* read first sector of dir and check if done */[cite: 15]
7:
        /* not found, try next sector */
        move.l  a0,-(sp)[cite: 15]
        bsr     NextDirSector[cite: 15]
        movea.l (sp)+,a0[cite: 15]
        beq.b   6b[cite: 15]
        moveq   #ERR_NAME_NOT_FOUND,d0[cite: 15]
        rts

| Seek to the designated logical sector

SeekCD:
        movem.l d0-d1/a0-a1,-(sp)[cite: 15]
0:
        movea.l sp,a0                   /* ptr to 32 bit sector start */[cite: 15]
        move.w  #0x0018,d0              /* ROMSEEK */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        lea     16(sp),sp               /* cleanup stack */[cite: 15]
        rts

| Begin reading d1 sectors starting at d0

BeginReadCD:
        movem.l d0-d1/a0-a1,-(sp)[cite: 15]
0:
        move.w  #0x0089,d0              /* CDCSTOP */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        movea.l sp,a0                   /* ptr to 32 bit sector start and 32 bit sector count */[cite: 15]
        move.w  #0x0020,d0              /* ROMREADN */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        lea     16(sp),sp               /* cleanup stack */[cite: 15]
        rts

| Read d1 sectors starting at d0 into buffer in a0 (using Sub-CPU)

        .global ReadSectorsSUB
ReadSectorsSUB:
        movem.l d0-d1/a0-a1,-(sp)[cite: 15]
0:
        move.w  #0x0089,d0              /* CDCSTOP */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        movea.l sp,a0                   /* ptr to 32 bit sector start and 32 bit sector count */[cite: 15]
        move.w  #0x0020,d0              /* ROMREADN */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]
1:
        move.w  #0x008A,d0              /* CDCSTAT */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]
        bcs.b   1b                      /* no sectors in CD buffer */[cite: 15]

        /* set CDC Mode destination device to Sub-CPU */
        andi.w  #0xF8FF,0x8004.w[cite: 15]
        ori.w   #0x0300,0x8004.w[cite: 15]
2:
        move.w  #0x008B,d0              /* CDCREAD */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]
        bcs.b   2b                      /* not ready to xfer data */[cite: 15]

        movea.l 8(sp),a0                /* data buffer */[cite: 15]
        lea     12(sp),a1               /* header address */[cite: 15]
        move.w  #0x008C,d0              /* CDCTRN */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]
        bcs.b   0b                      /* failed, retry */[cite: 15]

        move.w  #0x008D,d0              /* CDCACK */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        addq.l  #1,(sp)                 /* next sector */[cite: 15]
        addi.l  #2048,8(sp)             /* inc buffer ptr */[cite: 15]
        subq.l  #1,4(sp)                /* dec sector count */[cite: 15]
        bne.b   1b[cite: 15]

        lea     16(sp),sp               /* cleanup stack */[cite: 15]
        rts

| Read 1 sector into buffer in a0 (using DMA mode specified pecified in d0)

ReadSectorDMA:
        movem.l d0-d1/a0-a1,-(sp)[cite: 15]

        move.b  d0,0x8004.w[cite: 15]
        move.l  8(sp),d0[cite: 15]
        lsr.l   #3,d0[cite: 15]
        move.w  d0,0x800A.w             /* DMA destination address */[cite: 15]

        move.w  #0x008B,d0              /* CDCREAD */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]
        bcc.b   1f                      /* ready to xfer data */[cite: 15]

        lea     16(sp),sp               /* cleanup stack */[cite: 15]
        moveq   #0,d0[cite: 15]
        rts

1:
        /* check for EDT (end of data transfer) to be set */
        btst    #0x7,0x8004.w[cite: 15]
        beq.s   1b[cite: 15]

        move.w  #0x008D,d0              /* CDCACK */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        lea     16(sp),sp               /* cleanup stack */[cite: 15]
        moveq   #1,d0[cite: 15]
        rts

StopReadCD:
        movem.l d0-d1/a0-a1,-(sp)[cite: 15]

        move.w  #0x0089,d0              /* CDCSTOP */[cite: 15]
        jsr     0x5F22.w                /* call CDBIOS function */[cite: 15]

        lea     16(sp),sp               /* cleanup stack */[cite: 15]
        rts

| Sub-CPU variables

        .align  4
int3_callback:
        .long   0[cite: 15]

int3_cntr:
        .word   0[cite: 15]


        .align  2
track_number:
        .word   0[cite: 15]

updates_suspend:
        .byte   0[cite: 15]

        .align  2
root_dirname:
        .asciz  "/"[cite: 15]

        .align  2
iso_pvd_magic:
        .asciz  "\1CD001\1"[cite: 15]

        .align  2
drive_init_parms:
        .byte   0x01, 0xFF              /* first track (1), last track (all) */[cite: 15]

        .align  4
DISC_TYPE:
        .word   0[cite: 15]
DIR_ENTRY:
        .word   0[cite: 15]
CWD_OFFSET:
        .long   0[cite: 15]
CWD_LENGTH:
        .long   0[cite: 15]
CURR_OFFSET:
        .long   0[cite: 15]
        .global CURR_OFFSET[cite: 15]
CURR_LENGTH:
        .long   0[cite: 15]
ROOT_OFFSET:
        .long   0[cite: 15]
ROOT_LENGTH:
        .long   0[cite: 15]
        .global DENTRY_OFFSET[cite: 15]
DENTRY_OFFSET:
        .long   0[cite: 15]
        .global DENTRY_LENGTH[cite: 15]
DENTRY_LENGTH:
        .long   0[cite: 15]
DENTRY_FLAGS:
        .byte   0[cite: 15]
        .global DENTRY_FLAGS[cite: 15]

        .align  2
CDA_VOLUME:
        .global CDA_VOLUME[cite: 15]
        .word   0[cite: 15]

        .global _start
_start:

        .bss
        .align  8
        .global DISC_BUFFER
DISC_BUFFER:
        .skip  2048*4[cite: 15]

        .align  2
DENTRY_NAME:
        .skip  256[cite: 15]
        .global DENTRY_NAME[cite: 15]
TEMP_NAME:
        .skip  256[cite: 15]
