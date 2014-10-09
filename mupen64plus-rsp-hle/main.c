/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - main.c                                          *
 *   Mupen64Plus homepage: http://code.google.com/p/mupen64plus/           *
 *   Copyright (C) 2012 Bobby Smiles                                       *
 *   Copyright (C) 2009 Richard Goedeken                                   *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_types.h"
#include "m64p_common.h"
#include "m64p_plugin.h"
#include "hle.h"
#include "alist.h"
#include "cicx105.h"
#include "jpeg.h"
#include "musyx.h"

#define min(a,b) (((a) < (b)) ? (a) : (b))

/* some rsp status flags */
#define RSP_STATUS_HALT             0x1
#define RSP_STATUS_BROKE            0x2
#define RSP_STATUS_INTR_ON_BREAK    0x40
#define RSP_STATUS_TASKDONE         0x200

/* some rdp status flags */
#define DP_STATUS_FREEZE            0x2

/* some mips interface interrupt flags */
#define MI_INTR_SP                  0x1


/* helper functions prototypes */
static unsigned int sum_bytes(const unsigned char *bytes, unsigned int size);
static void dump_binary(const char *const filename, const unsigned char *const bytes,
                        unsigned int size);
static void dump_task(const char *const filename);

static void handle_unknown_task(unsigned int sum);
static void handle_unknown_non_task(unsigned int sum);

/* global variables */
RSP_INFO rsp;

/* local variables */
static const int FORWARD_AUDIO = 0, FORWARD_GFX = 1;
static void (*l_DebugCallback)(void *, int, const char *) = NULL;
static void *l_DebugCallContext = NULL;
static int l_PluginInit = 0;

/* local functions */


/**
 * Try to figure if the RSP was launched using osSpTask* functions
 * and not run directly (in which case DMEM[0xfc0-0xfff] is meaningless).
 *
 * Previously, the ucode_size field was used to determine this,
 * but it is not robust enough (hi Pokemon Stadium !) because games could write anything
 * in this field : most ucode_boot discard the value and just use 0xf7f anyway.
 *
 * Using ucode_boot_size should be more robust in this regard.
 **/
static int is_task(void)
{
    return (*dmem_u32(TASK_UCODE_BOOT_SIZE) <= 0x1000);
}

static void rsp_break(unsigned int setbits)
{
    *rsp.SP_STATUS_REG |= setbits | RSP_STATUS_BROKE | RSP_STATUS_HALT;

    if ((*rsp.SP_STATUS_REG & RSP_STATUS_INTR_ON_BREAK)) {
        *rsp.MI_INTR_REG |= MI_INTR_SP;
        rsp.CheckInterrupts();
    }
}

static void forward_gfx_task(void)
{
    if (rsp.ProcessDlistList != NULL) {
        rsp.ProcessDlistList();
        *rsp.DPC_STATUS_REG &= ~DP_STATUS_FREEZE;
    }
}

static void forward_audio_task(void)
{
    if (rsp.ProcessAlistList != NULL)
        rsp.ProcessAlistList();
}

static void show_cfb(void)
{
    if (rsp.ShowCFB != NULL)
        rsp.ShowCFB();
}

static int try_fast_audio_dispatching(void)
{
    /* identify audio ucode by using the content of ucode_data */
    uint32_t ucode_data = *dmem_u32(TASK_UCODE_DATA);
    uint32_t v;

    if (*dram_u32(ucode_data) == 0x00000001) {
        if (*dram_u32(ucode_data + 0x30) == 0xf0000f00) {
            v = *dram_u32(ucode_data + 0x28);
            switch(v)
            {
            case 0x1e24138c: /* audio ABI (most common) */
                alist_process_audio(); return 1;
            case 0x1dc8138c: /* GoldenEye */
                alist_process_audio_ge(); return 1;
            case 0x1e3c1390: /* BlastCorp, DiddyKongRacing */
                alist_process_audio_bc(); return 1;
            default:
                DebugMessage(M64MSG_WARNING, "ABI1 identification regression: v=%08x", v);
            }
        } else {
            v = *dram_u32(ucode_data + 0x10);
            switch(v)
            {
            case 0x11181350: /* MarioKart, WaveRace (E) */
                alist_process_mk(); return 1;
            case 0x111812e0: /* StarFox (J) */
                alist_process_sfj(); return 1;
            case 0x110412ac: /* WaveRace (J RevB) */
                alist_process_wrjb(); return 1;
            case 0x110412cc: /* StarFox/LylatWars (except J) */
                alist_process_sf(); return 1;
            case 0x1cd01250: /* FZeroX */
                alist_process_fz(); return 1;
            case 0x1f08122c: /* YoshisStory */
                alist_process_ys(); return 1;
            case 0x1f38122c: /* 1080° Snowboarding */
                alist_process_1080(); return 1;
            case 0x1f681230: /* Zelda OoT / Zelda MM (J, J RevA) */
                alist_process_oot(); return 1;
            case 0x1f801250: /* Zelda MM (except J, J RevA, E Beta), PokemonStadium 2 */
                alist_process_mm(); return 1;
            case 0x109411f8: /* Zelda MM (E Beta) */
                alist_process_mmb(); return 1;
            case 0x1eac11b8: /* AnimalCrossing */
                alist_process_ac(); return 1;

            case 0x00010010: /* MusyX (IndianaJones, BattleForNaboo) */
                musyx_task(); return 1;

            default:
                DebugMessage(M64MSG_WARNING, "ABI2 identification regression: v=%08x", v);
            }
        }
    } else {
        v = *dram_u32(ucode_data + 0x10);
        switch(v)
        {
        case 0x00000001: /* MusyX:
            RogueSquadron, ResidentEvil2, PolarisSnoCross,
            TheWorldIsNotEnough, RugratsInParis, NBAShowTime,
            HydroThunder, Tarzan, GauntletLegend, Rush2049 */
            musyx_task(); return 1;
        case 0x0000127c: /* naudio (many games) */
            alist_process_naudio(); return 1;
        case 0x00001280: /* BanjoKazooie */
            alist_process_naudio_bk(); return 1;
        case 0x1c58126c: /* DonkeyKong */
            alist_process_naudio_dk(); return 1;
        case 0x1ae8143c: /* BanjoTooie, JetForceGemini, MickeySpeedWayUSA, PerfectDark */
            alist_process_naudio_mp3(); return 1;
        case 0x1ab0140c: /* ConkerBadFurDay */
            alist_process_naudio_cbfd(); return 1;

        default:
            DebugMessage(M64MSG_WARNING, "ABI3 identification regression: v=%08x", v);
        }
    }

    return 0;
}

static int try_fast_task_dispatching(void)
{
    /* identify task ucode by its type */
    switch (*dmem_u32(TASK_TYPE)) {
    case 1:
        if (FORWARD_GFX) {
            forward_gfx_task();
            return 1;
        }
        break;

    case 2:
        if (FORWARD_AUDIO) {
            forward_audio_task();
            return 1;
        } else if (try_fast_audio_dispatching())
            return 1;
        break;

    case 7:
        show_cfb();
        return 1;
    }

    return 0;
}

static void normal_task_dispatching(void)
{
    const unsigned int sum =
        sum_bytes((void*)dram_u32(*dmem_u32(TASK_UCODE)), min(*dmem_u32(TASK_UCODE_SIZE), 0xf80) >> 1);

    switch (sum) {
    /* StoreVe12: found in Zelda Ocarina of Time [misleading task->type == 4] */
    case 0x278:
        /* Nothing to emulate */
        return;

    /* GFX: Twintris [misleading task->type == 0] */
    case 0x212ee:
        if (FORWARD_GFX) {
            forward_gfx_task();
            return;
        }
        break;

    /* JPEG: found in Pokemon Stadium J */
    case 0x2c85a:
        jpeg_decode_PS0();
        return;

    /* JPEG: found in Zelda Ocarina of Time, Pokemon Stadium 1, Pokemon Stadium 2 */
    case 0x2caa6:
        jpeg_decode_PS();
        return;

    /* JPEG: found in Ogre Battle, Bottom of the 9th */
    case 0x130de:
    case 0x278b0:
        jpeg_decode_OB();
        return;
    }

    handle_unknown_task(sum);
}

static void non_task_dispatching(void)
{
    const unsigned int sum = sum_bytes(rsp.IMEM, 0x1000 >> 1);

    switch (sum) {
    /* CIC x105 ucode (used during boot of CIC x105 games) */
    case 0x9e2: /* CIC 6105 */
    case 0x9f2: /* CIC 7105 */
        cicx105_ucode();
        return;
    }

    handle_unknown_non_task(sum);
}

static void handle_unknown_task(unsigned int sum)
{
    char filename[256];
    uint32_t ucode = *dmem_u32(TASK_UCODE);
    uint32_t ucode_data = *dmem_u32(TASK_UCODE_DATA);
    uint32_t data_ptr = *dmem_u32(TASK_DATA_PTR);

    DebugMessage(M64MSG_WARNING, "unknown OSTask: sum %x PC:%x", sum, *rsp.SP_PC_REG);

    sprintf(&filename[0], "task_%x.log", sum);
    dump_task(filename);

    /* dump ucode_boot */
    sprintf(&filename[0], "ucode_boot_%x.bin", sum);
    dump_binary(filename, (void*)dram_u32(*dmem_u32(TASK_UCODE_BOOT)), *dmem_u32(TASK_UCODE_BOOT_SIZE));

    /* dump ucode */
    if (ucode != 0) {
        sprintf(&filename[0], "ucode_%x.bin", sum);
        dump_binary(filename, (void*)dram_u32(ucode), 0xf80);
    }

    /* dump ucode_data */
    if (ucode_data != 0) {
        sprintf(&filename[0], "ucode_data_%x.bin", sum);
        dump_binary(filename, (void*)dram_u32(ucode_data), *dmem_u32(TASK_UCODE_DATA_SIZE));
    }

    /* dump data */
    if (data_ptr != 0) {
        sprintf(&filename[0], "data_%x.bin", sum);
        dump_binary(filename, (void*)dram_u32(data_ptr), *dmem_u32(TASK_DATA_SIZE));
    }
}

static void handle_unknown_non_task(unsigned int sum)
{
    char filename[256];

    DebugMessage(M64MSG_WARNING, "unknown RSP code: sum: %x PC:%x", sum, *rsp.SP_PC_REG);

    /* dump IMEM & DMEM for further analysis */
    sprintf(&filename[0], "imem_%x.bin", sum);
    dump_binary(filename, rsp.IMEM, 0x1000);

    sprintf(&filename[0], "dmem_%x.bin", sum);
    dump_binary(filename, rsp.DMEM, 0x1000);
}


/* Global functions */
void DebugMessage(int level, const char *message, ...)
{
    char msgbuf[1024];
    va_list args;

    if (l_DebugCallback == NULL)
        return;

    va_start(args, message);
    vsprintf(msgbuf, message, args);

    (*l_DebugCallback)(l_DebugCallContext, level, msgbuf);

    va_end(args);
}

/* DLL-exported functions */
EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle CoreLibHandle, void *Context,
                                     void (*DebugCallback)(void *, int, const char *))
{
    if (l_PluginInit)
        return M64ERR_ALREADY_INIT;

    /* first thing is to set the callback function for debug info */
    l_DebugCallback = DebugCallback;
    l_DebugCallContext = Context;

    /* this plugin doesn't use any Core library functions (ex for Configuration), so no need to keep the CoreLibHandle */

    l_PluginInit = 1;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    if (!l_PluginInit)
        return M64ERR_NOT_INIT;

    /* reset some local variable */
    l_DebugCallback = NULL;
    l_DebugCallContext = NULL;

    l_PluginInit = 0;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion, int *APIVersion, const char **PluginNamePtr, int *Capabilities)
{
    /* set version info */
    if (PluginType != NULL)
        *PluginType = M64PLUGIN_RSP;

    if (PluginVersion != NULL)
        *PluginVersion = RSP_HLE_VERSION;

    if (APIVersion != NULL)
        *APIVersion = RSP_PLUGIN_API_VERSION;

    if (PluginNamePtr != NULL)
        *PluginNamePtr = "Hacktarux/Azimer High-Level Emulation RSP Plugin";

    if (Capabilities != NULL)
        *Capabilities = 0;

    return M64ERR_SUCCESS;
}

EXPORT unsigned int CALL DoRspCycles(unsigned int Cycles)
{
    if (is_task()) {
        if (!try_fast_task_dispatching())
            normal_task_dispatching();
        rsp_break(RSP_STATUS_TASKDONE);
    } else {
        non_task_dispatching();
        rsp_break(0);
    }

    return Cycles;
}

EXPORT void CALL InitiateRSP(RSP_INFO Rsp_Info, unsigned int *CycleCount)
{
    rsp = Rsp_Info;
}

EXPORT void CALL RomClosed(void)
{
    memset(rsp.DMEM, 0, 0x1000);
    memset(rsp.IMEM, 0, 0x1000);
}


/* local helper functions */
static unsigned int sum_bytes(const unsigned char *bytes, unsigned int size)
{
    unsigned int sum = 0;
    const unsigned char *const bytes_end = bytes + size;

    while (bytes != bytes_end)
        sum += *bytes++;

    return sum;
}


static void dump_binary(const char *const filename, const unsigned char *const bytes,
                        unsigned int size)
{
    FILE *f;

    /* if file already exists, do nothing */
    f = fopen(filename, "r");
    if (f == NULL) {
        /* else we write bytes to the file */
        f = fopen(filename, "wb");
        if (f != NULL) {
            if (fwrite(bytes, 1, size, f) != size)
                DebugMessage(M64MSG_ERROR, "Writing error on %s", filename);
            fclose(f);
        } else
            DebugMessage(M64MSG_ERROR, "Couldn't open %s for writing !", filename);
    } else
        fclose(f);
}

static void dump_task(const char *const filename)
{
    FILE *f;

    f = fopen(filename, "r");
    if (f == NULL) {
        f = fopen(filename, "w");
        fprintf(f,
                "type = %d\n"
                "flags = %d\n"
                "ucode_boot  = %#08x size  = %#x\n"
                "ucode       = %#08x size  = %#x\n"
                "ucode_data  = %#08x size  = %#x\n"
                "dram_stack  = %#08x size  = %#x\n"
                "output_buff = %#08x *size = %#x\n"
                "data        = %#08x size  = %#x\n"
                "yield_data  = %#08x size  = %#x\n",
                *dmem_u32(TASK_TYPE),
                *dmem_u32(TASK_FLAGS),
                *dmem_u32(TASK_UCODE_BOOT),     *dmem_u32(TASK_UCODE_BOOT_SIZE),
                *dmem_u32(TASK_UCODE),          *dmem_u32(TASK_UCODE_SIZE),
                *dmem_u32(TASK_UCODE_DATA),     *dmem_u32(TASK_UCODE_DATA_SIZE),
                *dmem_u32(TASK_DRAM_STACK),     *dmem_u32(TASK_DRAM_STACK_SIZE),
                *dmem_u32(TASK_OUTPUT_BUFF),    *dmem_u32(TASK_OUTPUT_BUFF_SIZE),
                *dmem_u32(TASK_DATA_PTR),       *dmem_u32(TASK_DATA_SIZE),
                *dmem_u32(TASK_YIELD_DATA_PTR), *dmem_u32(TASK_YIELD_DATA_SIZE));
        fclose(f);
    } else
        fclose(f);
}


/* memory access helper functions */
void dmem_load_u8 (uint8_t*  dst, uint16_t address, size_t count)
{
    while (count != 0) {
        *(dst++) = *dmem_u8(address);
        address += 1;
        --count;
    }
}

void dmem_load_u16(uint16_t* dst, uint16_t address, size_t count)
{
    while (count != 0) {
        *(dst++) = *dmem_u16(address);
        address += 2;
        --count;
    }
}

void dmem_load_u32(uint32_t* dst, uint16_t address, size_t count)
{
    /* Optimization for uint32_t */
    memcpy(dst, dmem_u32(address), count * sizeof(uint32_t));
}

void dmem_store_u8 (const uint8_t*  src, uint16_t address, size_t count)
{
    while (count != 0) {
        *dmem_u8(address) = *(src++);
        address += 1;
        --count;
    }
}

void dmem_store_u16(const uint16_t* src, uint16_t address, size_t count)
{
    while (count != 0) {
        *dmem_u16(address) = *(src++);
        address += 2;
        --count;
    }
}

void dmem_store_u32(const uint32_t* src, uint16_t address, size_t count)
{
    /* Optimization for uint32_t */
    memcpy(dmem_u32(address), src, count * sizeof(uint32_t));
}


void dram_load_u8 (uint8_t*  dst, uint32_t address, size_t count)
{
    while (count != 0) {
        *(dst++) = *dram_u8(address);
        address += 1;
        --count;
    }
}

void dram_load_u16(uint16_t* dst, uint32_t address, size_t count)
{
    while (count != 0) {
        *(dst++) = *dram_u16(address);
        address += 2;
        --count;
    }
}

void dram_load_u32(uint32_t* dst, uint32_t address, size_t count)
{
    /* Optimization for uint32_t */
    memcpy(dst, dram_u32(address), count * sizeof(uint32_t));
}

void dram_store_u8 (const uint8_t*  src, uint32_t address, size_t count)
{
    while (count != 0) {
        *dram_u8(address) = *(src++);
        address += 1;
        --count;
    }
}

void dram_store_u16(const uint16_t* src, uint32_t address, size_t count)
{
    while (count != 0) {
        *dram_u16(address) = *(src++);
        address += 2;
        --count;
    }
}

void dram_store_u32(const uint32_t* src, uint32_t address, size_t count)
{
    /* Optimization for uint32_t */
    memcpy(dram_u32(address), src, count * sizeof(uint32_t));
}

