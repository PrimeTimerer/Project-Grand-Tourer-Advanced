#pragma once
/* BCES01893 1.22, FD63C8: replace four proven movie color-conversion tasks
 * with their complete GPU-computed RGBA result. Unknown descriptors or GPU
 * failure retain the original SPU path. Optional capture records two frames.
 * This header is included only in the lifted movie-consumer TU. */
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <mutex>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "gt6_movie_gpu.h"

struct Gt6MoviePostCapture {
    const char* directory = nullptr;
    unsigned frame = 0, selected = 0;
    uint32_t src = 0, dst = 0, source_bytes = 0, output_bytes = 0;
};
static Gt6MoviePostCapture& gt6_movie_post_capture_state()
{
    static Gt6MoviePostCapture state{std::getenv("GT6_MOVIE_POST_CAPTURE")};
    return state;
}
static bool gt6_movie_post_readable(uint32_t ea, uint32_t length, bool write=false)
{
    if (ea < 0x10000 || !length || uint64_t(ea) + length > 0x100000000ull) return false;
#ifdef _WIN32
    for (uintptr_t p = uintptr_t(vm_base) + ea, end = p + length; p < end;) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(p), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        if (write && !(region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        const uintptr_t next = uintptr_t(region.BaseAddress) + region.RegionSize;
        if (next <= p) return false;
        p = next;
    }
#endif
    return true;
}
static void gt6_movie_post_dump(const char* suffix, uint32_t ea, uint32_t bytes)
{
    auto& state = gt6_movie_post_capture_state();
    if (!gt6_movie_post_readable(ea, bytes)) {
        std::fprintf(stderr, "[gt6-movie-post] capture rejected unreadable %s ea=%08X size=%u\n", suffix, ea, bytes);
        return;
    }
    char file[2048];
    const int n = std::snprintf(file, sizeof(file), "%s/post_%04u_%s.bin", state.directory, state.frame, suffix);
    if (n <= 0 || size_t(n) >= sizeof(file)) return;
    FILE* f = std::fopen(file, "wb");
    if (!f) { std::fprintf(stderr, "[gt6-movie-post] cannot write %s\n", file); return; }
    const size_t written = std::fwrite(vm_base + ea, 1, bytes, f);
    std::fclose(f);
    std::fprintf(stderr, "[gt6-movie-post] capture %s bytes=%zu/%u\n", file, written, bytes);
}
static void gt6_movie_post_capture_before()
{
    auto& state = gt6_movie_post_capture_state();
    if (!state.directory || !*state.directory) return;
    ++state.frame;
    state.selected = 0;
    if (state.frame != 32 && state.frame != 160) return;
    constexpr uint32_t descriptor = 0x016EDA00;
    if (!gt6_movie_post_readable(descriptor, 0x200)) return;
    const uint32_t w = vm_read32(descriptor + 0x10), h = vm_read32(descriptor + 0x14);
    const uint32_t ow = vm_read32(descriptor + 0x18), oh = vm_read32(descriptor + 0x1C);
    const uint32_t flags = vm_read32(descriptor + 4), align = flags & 0xff;
    if (vm_read32(descriptor) != 8 || !w || !h || !ow || !oh ||
        w > 4096 || h > 2160 || ow > 4096 || oh > 2160 ||
        !align || align > 128 || (align & (align - 1))) return;
    const uint32_t stride = (w + align - 1) & ~(align - 1);
    const uint32_t cstride = ((w + 1) / 2 + align - 1) & ~(align - 1);
    state.src = vm_read32(descriptor + 8); state.dst = vm_read32(descriptor + 12);
    state.source_bytes = stride * h + 2 * cstride * ((h + 1) / 2);
    state.output_bytes = ow * oh * 4;
    state.selected = 1;
    std::fprintf(stderr, "[gt6-movie-post] frame=%u type=8 flags=%08X src=%08X dst=%08X source=%ux%u output=%ux%u\n",
        state.frame, flags, state.src, state.dst, w, h, ow, oh);
    gt6_movie_post_dump("descriptor", descriptor, 0x200);
    gt6_movie_post_dump("input", state.src, state.source_bytes);
}
static bool gt6_movie_post_before()
{
    gt6_movie_post_capture_before();
    static const bool enabled=[] {
        const char* mode=std::getenv("GT6_MOVIE_POST");
        return !mode || (std::strcmp(mode,"spu") && std::strcmp(mode,"0"));
    }();
    if (!enabled) return false;
    constexpr uint32_t descriptor=0x016EDA00;
    const uint32_t src=vm_read32(descriptor+8),dst=vm_read32(descriptor+12);
    const uint32_t w=vm_read32(descriptor+16),h=vm_read32(descriptor+20);
    const uint32_t ow=vm_read32(descriptor+24),oh=vm_read32(descriptor+28);
    if (!gt6_movie_gpu::Converter::supports(w,h,ow,oh)) return false;
    // This exact four-way descriptor is the proven progressive RGBA path.
    for (uint32_t j=0;j<4;++j) {
        const uint32_t p=descriptor+j*128;
        if (vm_read32(p)!=8 || vm_read32(p+4)!=(16|(j<<24)) ||
            vm_read32(p+8)!=src || vm_read32(p+12)!=dst ||
            vm_read32(p+16)!=w || vm_read32(p+20)!=h ||
            vm_read32(p+24)!=ow || vm_read32(p+28)!=oh) return false;
    }
    const uint32_t sourceBytes=w*h*3/2+16, outputBytes=w*h*4;
    if ((uint64_t(src)<uint64_t(dst)+outputBytes && uint64_t(dst)<uint64_t(src)+sourceBytes) ||
        !gt6_movie_post_readable(src,sourceBytes) || !gt6_movie_post_readable(dst,outputBytes,true)) return false;
    static std::mutex mutex;
    std::lock_guard<std::mutex> guard(mutex);
    static gt6_movie_gpu::Converter converter;
    LARGE_INTEGER begin{},end{},frequency{};
    QueryPerformanceCounter(&begin);
    const bool complete=converter.convert(vm_base+src,vm_base+dst,w,h);
    QueryPerformanceCounter(&end); QueryPerformanceFrequency(&frequency);
    static uint64_t count=0,total_us=0,last_ms=0;
    if (complete) {
        ++count; total_us+=uint64_t(end.QuadPart-begin.QuadPart)*1000000/uint64_t(frequency.QuadPart);
        const uint64_t now=GetTickCount64();
        if (count<=3 || now-last_ms>=5000) {
            last_ms=now;
            std::fprintf(stderr,"[gt6-movie-gpu] converted=%llu %ux%u avg_upload_compute_readback_us=%llu\n",
                (unsigned long long)count,w,h,(unsigned long long)(total_us/count));
        }
    }
    return complete;
}
static void gt6_movie_post_after()
{
    auto& state = gt6_movie_post_capture_state();
    if (!state.selected) return;
    gt6_movie_post_dump("output", state.dst, state.output_bytes);
    state.selected = 0;
}
