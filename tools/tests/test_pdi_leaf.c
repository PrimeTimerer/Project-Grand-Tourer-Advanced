/* Disposable CPU differential fixture. No game, audio device, VM or user data. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu_interp.c"

SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
int g_spu_ls_watch_n;
spu_context* trace_ctx;
uint64_t trace_hash;
unsigned trace_calls;
void spu_spurs_taskset_syscall(spu_context* ctx) { (void)ctx; abort(); }
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t pc) { (void)ctx; (void)pc; return NULL; }
void spu_indirect_branch(spu_context* ctx) { (void)ctx; abort(); }
u128 spu_rdch(spu_context* ctx, uint32_t channel) { (void)ctx; (void)channel; abort(); }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) { (void)ctx; (void)channel; (void)value; abort(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel) { (void)ctx; (void)channel; abort(); }
static void sentinel(spu_context* ctx) { (void)ctx; abort(); }

#include "pdi_leaf_cases.inc"

static unsigned checks, pairs, oracle_pairs, prefix_pairs;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
static spu_context initial_ctx, old_ctx, new_ctx;
static uint32_t rng = 0x14548C0Du;
static uint32_t random_word(void) { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static const uint32_t edges[] = {
    0,0x80000000u,1,0x80000001u,0x007fffffu,0x807fffffu,0x00800000u,0x80800000u,
    0x3f800000u,0xbf800000u,0x3f7fffffu,0xbf7fffffu,0x7f7fffffu,0xff7fffffu,
    0x7f800000u,0xff800000u,0x7fc00000u,0xffc00000u,0x7fffffffu,0xffffffffu,
    0x40000000u,0xc0000000u,0x3eaaaaabu,0xbeaaaaabu,0x7f000000u,0xff000000u
};
static uint32_t edge(unsigned n) { return edges[n % (sizeof edges / sizeof edges[0])]; }
static void copy_ctx(spu_context* to, const spu_context* from) { memcpy(to,from,sizeof *to); to->ls=to->ls_store; }
static void seed(unsigned id, unsigned count)
{
    memset(&initial_ctx,0,sizeof initial_ctx); initial_ctx.ls=initial_ctx.ls_store;
    initial_ctx.status=SPU_STATUS_RUNNING; initial_ctx.pc=0x14548;
    initial_ctx.image_id=249; initial_ctx.spu_id=0x2000u+(id%5);
    initial_ctx.host_depth=(id%3)?1u+(id%17):0;
    initial_ctx.event_status=random_word(); initial_ctx.event_mask=random_word();
    initial_ctx.mfc_lsa=random_word(); initial_ctx.mfc_size=random_word();
    initial_ctx.mfc_tag_status=random_word(); initial_ctx.resv_ea=random_word();
    initial_ctx.resv_valid=1; initial_ctx.dcache_id=0x5a5a5a5a;
    initial_ctx.dcache_residency=3; initial_ctx.resident_code_count=2;
    for(unsigned i=0;i<2;i++) {
        initial_ctx.resident_code[i].lsa=i*0x20000;
        initial_ctx.resident_code[i].size=0x20000;
        initial_ctx.resident_code[i].image_id=249;
        initial_ctx.resident_code[i].verify_state=1;
        initial_ctx.resident_code[i].source_ea=0x143f280;
    }
    for(unsigned r=0;r<128;r++) for(unsigned k=0;k<4;k++)
        initial_ctx.gpr[r]._u32[k]=(id&1)?random_word():edge(r*4+k+id);
    for(unsigned p=0;p<SPU_LS_SIZE;p+=4) {
        uint32_t v=(id&1)?random_word():edge(p/4+id);
        initial_ctx.ls[p]=(uint8_t)(v>>24); initial_ctx.ls[p+1]=(uint8_t)(v>>16);
        initial_ctx.ls[p+2]=(uint8_t)(v>>8); initial_ctx.ls[p+3]=(uint8_t)v;
    }
    static const uint32_t pointers[]={0,1,0x3fff0u,0x3fffcu,0xfffffff8u,0x1fff0u,0x20004u,0x4000fu};
    static const uint32_t steps[]={16,32,0xfffffff0u,0xffffffe0u,0,0x40010u,1,0xfffffffcu};
    initial_ctx.gpr[0]._u32[0]=0x3210;
    initial_ctx.gpr[3]._u32[0]=pointers[id%8];
    initial_ctx.gpr[30]._u32[0]=pointers[(id+3)%8];
    initial_ctx.gpr[5]._u32[0]=steps[id%8];
    initial_ctx.gpr[8]._u32[0]=count;
    if(id%4==0) initial_ctx.gpr[30]._u32[0]=initial_ctx.gpr[3]._u32[0];
}
static void configure(spu_context* ctx,unsigned rz,unsigned flags)
{
    _mm_setcsr((rz?SPU_MXCSR_RZFZ:(0x1f80u|((flags/16)*0x2000u)))|(flags&63));
    spu__rz_mark((int)rz);
    g_spu_ls_probe=g_spu_smc_watch=g_spu_wws_probe=0;
    g_spu_ls_watch_n=g_spu_ls_rdiag=g_spu_ls_wdiag=(int)(flags&1);
    if(flags&1) memset(ctx->resident_code_pages,255,sizeof ctx->resident_code_pages);
    trace_ctx=ctx; trace_hash=UINT64_C(1469598103934665603); trace_calls=0;
    g_spu_trampoline_fn=(flags&2)?sentinel:NULL;
}
static unsigned short control_word(void) { unsigned short cw; __asm__("fnstcw %0" : "=m"(cw)); return cw; }
static void compare_state(unsigned old_fp,unsigned old_cw,unsigned old_calls,uint64_t old_hash,void (*old_trampoline)(spu_context*))
{
    CHECK(old_fp==_mm_getcsr()); CHECK(old_cw==control_word());
    CHECK(old_calls==trace_calls); CHECK(old_hash==trace_hash);
    CHECK(old_trampoline==g_spu_trampoline_fn);
    CHECK(!memcmp(old_ctx.gpr,new_ctx.gpr,sizeof old_ctx.gpr));
    CHECK(!memcmp(old_ctx.ls,new_ctx.ls,SPU_LS_SIZE));
    old_ctx.ls=new_ctx.ls=NULL;
    CHECK(!memcmp(&old_ctx,&new_ctx,sizeof old_ctx));
    old_ctx.ls=old_ctx.ls_store;new_ctx.ls=new_ctx.ls_store;
}
static void compare_one(unsigned rz,unsigned flags,unsigned budget)
{
    copy_ctx(&old_ctx,&initial_ctx); configure(&old_ctx,rz,flags);
    if(budget) original_prefix_14548(&old_ctx,budget); else original_14548(&old_ctx);
    unsigned old_fp=_mm_getcsr(),old_cw=control_word(),old_calls=trace_calls;
    uint64_t old_hash=trace_hash;void (*old_trampoline)(spu_context*)=g_spu_trampoline_fn;
    copy_ctx(&new_ctx,&initial_ctx); configure(&new_ctx,rz,flags);
    if(budget) native_prefix_14548(&new_ctx,budget); else native_14548(&new_ctx);
    compare_state(old_fp,old_cw,old_calls,old_hash,old_trampoline);
    CHECK(new_ctx.pc==initial_ctx.gpr[0]._u32[0]);
    CHECK(new_ctx.host_depth==initial_ctx.host_depth);
    CHECK(g_spu_trampoline_fn==(initial_ctx.host_depth?((flags&2)?sentinel:NULL):spu_indirect_branch));
    if(budget) { CHECK(new_ctx.gpr[8]._u32[0]==initial_ctx.gpr[8]._u32[0]-budget);prefix_pairs++; }
    else { CHECK(new_ctx.gpr[8]._u32[0]==0); pairs++; }
}
static DWORD WINAPI first_use_thread(void* value)
{
    (void)value;
    (void)gt6_pdi_14548_enabled(); /* A contending first call may use fallback. */
    return 0;
}
static void gate_tests(void)
{
    static const char* settings[]={"","0","1","01","true","1 ","-1","2"};
    for(unsigned i=0;i<sizeof settings/sizeof settings[0];i++) {
        CHECK(_putenv_s("GT6_NATIVE_PDI_LEAVES",settings[i])==0);
        atomic_store(&gt6_pdi_14548_state,0);
        CHECK(gt6_pdi_14548_enabled()==(!strcmp(settings[i],"1")));
        CHECK(atomic_load(&gt6_pdi_14548_state)==(!strcmp(settings[i],"1")?2:1));
        seed(i,3);compare_one(1,i,0);
    }
    CHECK(_putenv_s("GT6_NATIVE_PDI_LEAVES","1")==0);
    atomic_store(&gt6_pdi_14548_state,-1);
    CHECK(gt6_pdi_14548_enabled()==0); CHECK(atomic_load(&gt6_pdi_14548_state)==-1);
    atomic_store(&gt6_pdi_14548_state,0);
    HANDLE threads[16];
    for(unsigned i=0;i<16;i++) { threads[i]=CreateThread(NULL,0,first_use_thread,NULL,0,NULL);CHECK(threads[i]!=NULL); }
    CHECK(WaitForMultipleObjects(16,threads,TRUE,5000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<16;i++)CloseHandle(threads[i]);
    CHECK(gt6_pdi_14548_enabled()==1); CHECK(atomic_load(&gt6_pdi_14548_state)==2);
    CHECK(_putenv_s("GT6_NATIVE_PDI_LEAVES","0")==0);
    CHECK(gt6_pdi_14548_enabled()==1); /* immutable process setting */
    CHECK(_putenv_s("GT6_NATIVE_PDI_LEAVES","1")==0);
}
static void opcode_oracle(void)
{
    for(unsigned id=0;id<32;id++) for(unsigned rz=0;rz<2;rz++) {
        unsigned count=1+id%16;seed(id,count);
        initial_ctx.gpr[3]._u32[0]=0x20000;initial_ctx.gpr[30]._u32[0]=0x30000;initial_ctx.gpr[5]._u32[0]=16;
        memcpy(initial_ctx.ls+0x14548,original_opcodes,sizeof original_opcodes);
        copy_ctx(&old_ctx,&initial_ctx);configure(&old_ctx,rz,0);original_14548(&old_ctx);
        unsigned fp=_mm_getcsr();
        copy_ctx(&new_ctx,&initial_ctx);configure(&new_ctx,rz,0);
        unsigned steps=0;
        while(new_ctx.pc!=0x3210) { CHECK(spu_step(&new_ctx)==0);CHECK(++steps<=count*24+1); }
        CHECK(steps==count*24+1);CHECK(fp==_mm_getcsr());
        CHECK(!memcmp(old_ctx.gpr,new_ctx.gpr,sizeof old_ctx.gpr));
        CHECK(!memcmp(old_ctx.ls,new_ctx.ls,SPU_LS_SIZE));
        CHECK(old_ctx.pc==new_ctx.pc);oracle_pairs++;
    }
}
typedef void (*leaf_fn)(spu_context*);
static double bench(leaf_fn fn,unsigned reps)
{
    LARGE_INTEGER start,end,freq;QueryPerformanceFrequency(&freq);long long ticks=0;
    for(unsigned r=0;r<reps;r++) {
        copy_ctx(&new_ctx,&initial_ctx);configure(&new_ctx,1,0);
        QueryPerformanceCounter(&start);fn(&new_ctx);QueryPerformanceCounter(&end);
        ticks+=end.QuadPart-start.QuadPart;
    }
    return ticks*1e9/freq.QuadPart/reps;
}
static void benchmark(void)
{
    for(unsigned count=16;count<=256;count*=4) {
        seed(17,count);
        /* finite, representative arithmetic, while keeping real LS helpers */
        for(unsigned r=0;r<128;r++)for(unsigned k=0;k<4;k++)initial_ctx.gpr[r]._f32[k]=0.0625f;
        initial_ctx.gpr[0]._u32[0]=0x3210;initial_ctx.gpr[3]._u32[0]=0x20000;
        initial_ctx.gpr[30]._u32[0]=0x30000;initial_ctx.gpr[5]._u32[0]=16;initial_ctx.gpr[8]._u32[0]=count;
        for(unsigned p=0;p<SPU_LS_SIZE;p+=4) { initial_ctx.ls[p]=0x3d;initial_ctx.ls[p+1]=0x80;initial_ctx.ls[p+2]=initial_ctx.ls[p+3]=0; }
        for(unsigned round=0;round<6;round++) {
            double a,b;if(round&1){b=bench(native_14548,600);a=bench(original_14548,600);}
            else {a=bench(original_14548,600);b=bench(native_14548,600);}
            printf("BENCH count=%u round=%u original_ns=%.1f native_ns=%.1f ratio=%.4f\n",count,round,a,b,a/b);
        }
    }
}
int main(int argc,char** argv)
{
    unsigned saved=_mm_getcsr();unsigned short saved_cw=control_word();
    gate_tests();
    static const unsigned lengths[]={1,2,3,7,16,31,64,129,257};
    for(unsigned id=0;id<64;id++) {
        seed(id,lengths[id%(sizeof lengths/sizeof lengths[0])]);
        for(unsigned rz=0;rz<2;rz++)for(unsigned flags=0;flags<64;flags++)compare_one(rz,flags,0);
    }
    for(unsigned id=0;id<8;id++) {
        seed(id,(id&1)?0xffffffffu:0);
        for(unsigned rz=0;rz<2;rz++)for(unsigned flags=0;flags<64;flags++)compare_one(rz,flags,1+id*7);
    }
    opcode_oracle();
    if(argc>1 && !strcmp(argv[1],"--bench"))benchmark();
    _mm_setcsr(saved);__asm__ volatile("fldcw %0" :: "m"(saved_cw));
    printf("PASS checks=%u differential_pairs=%u prefix_pairs=%u opcode_oracle_pairs=%u full_GPR_LS_context_PC_trampoline_MXCSR_x87_callbacks=exact\n",checks,pairs,prefix_pairs,oracle_pairs);
    return 0;
}
