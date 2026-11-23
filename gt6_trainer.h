#pragma once
#include <stdint.h>

struct ppu_context;

/* Native-only ABI. The original RPCS3 control pages overlap GT6's main-memory
 * render targets in this port; these pages live beyond stacks and raw SPUs. */
#define GT6_TRAINER_PAGE  0xE1000000u
#define GT6_TRAINER_PAGE2 0xE1010000u
#define GT6_TRAINER_WINDOW_SIZE 0x00220000u
#ifdef _WIN32
#define GT6_TRAINER_API __declspec(dllexport)
#else
#define GT6_TRAINER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif
typedef struct gt6_trainer_bridge_data {
    char magic[8];
    uint32_t abi, size;
    uint64_t guest_base;
    uint32_t page, page2, capabilities, ready;
    uint64_t image_marker, boot_tick, reserved;
} gt6_trainer_bridge_data;

extern GT6_TRAINER_API gt6_trainer_bridge_data gt6_trainer_bridge;
GT6_TRAINER_API int gt6_trainer_init(void);
GT6_TRAINER_API void gt6_trainer_after(struct ppu_context* ctx, uint32_t site);
GT6_TRAINER_API int gt6_trainer_bypass(struct ppu_context* ctx, uint32_t site);
#ifdef __cplusplus
}
#endif
