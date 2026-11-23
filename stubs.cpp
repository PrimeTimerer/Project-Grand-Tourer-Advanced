/*
 * GT6 native port -- game-specific hooks and overrides (ps3recomp project).
 *
 * main.cpp is the runner. This file answers what is specific to Gran Turismo 6
 * (BCES01893, update 1.22, main executable EMAIN.SELF):
 *   - the firmware PRX modules it runs as real code (LLE);
 *   - the cellGame answers for a disc game booted with its HDD update.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ps3recomp/runtime/prx/prx_loader.h"
#include "ps3recomp/runtime/ppu/ppu_context.h"

#include <atomic>

/* ---------------------------------------------------------------------------
 * 1.22 patch boot.
 *
 * The console boots /dev_hdd0/game/BCES01893/USRDIR/EBOOT.BIN (Category GD,
 * APP_VER 01.22); that loader hands over to EMAIN.SELF with
 * sceNpDrmProcessExitSpawn2. The port starts EMAIN directly. EMAIN's own calls:
 *   cellGameBootCheck -> GetParamString 100/101/106 -> ContentPermit
 *   ... cellGameDataCheck(type=3 GAMEDATA, "BCES01893") -> ContentPermit
 * There is no cellGamePatchCheck import in EMAIN.
 * -----------------------------------------------------------------------*/

static const char kTitleId[] = "BCES01893";
static const uint32_t kNidBootCheck = 0xF52639EAu;

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" void cellGame_init_from_paramsfo(const char* sfo_path);
extern "C" s32  cellGameBootCheck(void* type, void* attributes, void* size, void* dirName);

static void guest_be32(uint32_t ea, uint32_t v)
{
    if (!ea) return;
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void gt6_cellGameBootCheck(ppu_context* ctx)
{
    const uint32_t type = (uint32_t)ctx->gpr[3], attr = (uint32_t)ctx->gpr[4];
    const uint32_t size = (uint32_t)ctx->gpr[5], dir  = (uint32_t)ctx->gpr[6];
    s32 rc = cellGameBootCheck((void*)(uintptr_t)type, (void*)(uintptr_t)attr,
                               (void*)(uintptr_t)size, (void*)(uintptr_t)dir);
    if (rc == 0) {
        /* cellGameBootCheck for category "GD": disc game type, PATCH attribute. */
        guest_be32(type, 1);        /* CELL_GAME_GAMETYPE_DISC */
        guest_be32(attr, 0x1);      /* CELL_GAME_ATTRIBUTE_PATCH */
        if (size) {
            guest_be32(size + 0, 40u * 1024u * 1024u - 256u);   /* hddFreeSizeKB */
            guest_be32(size + 4, 0xFFFFFFFFu);                  /* sizeKB = NOTCALC */
            guest_be32(size + 8, 4);                            /* sysSizeKB */
        }
        fprintf(stderr, "[gt6] cellGameBootCheck -> DISC + PATCH (1.22 patch boot)\n");
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

/* cellVideoOutGetScreenSize: CELL_VIDEO_OUT_ERROR_VALUE_IS_NOT_SET (0x8002B228), the answer on an ordinary 2D
 * screen (no 3D display size configured). */
static void gt6_cellVideoOutGetScreenSize(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8002B228;
}

static void gt6_register_patch_boot(void)
{
    ps3_hle_register_ctx(0xFAA275A4u, "cellVideoOutGetScreenSize", gt6_cellVideoOutGetScreenSize);
    /* Title id, title and version come from the update's PARAM.SFO (GD boot). */
    char sfo[1024];
    const char* hdd0 = getenv("PS3_HDD0_ROOT");
    snprintf(sfo, sizeof sfo, "%s/game/%s/PARAM.SFO",
             (hdd0 && hdd0[0]) ? hdd0 : "game/dev_hdd0", kTitleId);
    cellGame_init_from_paramsfo(sfo);
    ps3_hle_register_ctx(kNidBootCheck, "cellGameBootCheck", gt6_cellGameBootCheck);
}

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * libsre (cellSpurs + cellSync) runs as LLE: EMAIN uses SPURS v2 tasksets and
 * job chains that the HLE library does not implement. The image is the player's
 * FW 4.93 libsre, relocated at 0x30000000 and lifted for the Sakatsuku port
 * (same firmware, so the same artifact: prx_src/libsre, game/firmware).
 * GT6_PRX_DIR overrides the image folder.
 * -----------------------------------------------------------------------*/
extern "C" const prx_func_entry libsre_function_table[];
extern "C" const uint64_t        libsre_function_table_count;
extern "C" const prx_export      libsre_exports[];
extern "C" const uint32_t        libsre_export_count;

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(void*));

static void prx_register(uint32_t guest_addr, void (*host)(void*))
{
    ppu_register_function(guest_addr, host);
}

static void load_prx(const char* dir, const char* name, uint32_t base,
                     const prx_func_entry* funcs, uint64_t nfuncs,
                     const prx_export* exports, uint32_t nexports)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s_image.bin", dir, name);
    uint32_t size = 0;
    uint8_t* img = prx_image_load_file(path, &size);
    if (!img) {
        fprintf(stderr, "[prx] %s: image not found at %s -- its imports stay on HLE\n", name, path);
        return;
    }
    prx_module m;
    memset(&m, 0, sizeof(m));
    m.name         = name;
    m.base         = base;      /* must match the lift_prx.py / ppu_lifter --base */
    m.image        = img;
    m.image_size   = size;
    m.funcs        = funcs;
    m.func_count   = nfuncs;
    m.exports      = exports;
    m.export_count = nexports;
    prx_load_result r = prx_load_module(&m, prx_register);
    fprintf(stderr, "[prx] %s: ok=%d base=0x%08X size=0x%X funcs=%u exports=%u\n",
            name, r.ok, base, size, r.funcs_registered, nexports);
    free(img);   /* the loader copied the bytes into guest RAM */
}

extern "C" void ps3_load_prx_modules(void)
{
    gt6_register_patch_boot();   /* before ppu_hle_init: ctx handlers win over the table */

    const char* dir = getenv("GT6_PRX_DIR");
    if (!dir || !dir[0]) dir = "game/firmware";
    const char* lle = getenv("GT6_LLE_SRE");
    if (!(lle && *lle == '0'))
        load_prx(dir, "libsre", 0x30000000u, libsre_function_table, libsre_function_table_count,
                 libsre_exports, libsre_export_count);
}
