"""[GT6 0183] Generate ps3recomp/libs/video/rsx_live_rec.inc: the D3D12 calls of the live-draw engine as records.

With LD_REC_THREAD on, the drawing thread (gt6-draw) makes no D3D12 command-list call itself: each call - and each
descriptor write the later calls read - becomes a record in a ring, and a thread of its own (gt6-draw-rec) makes the
calls, in the same order, on the same command list. Off, every wrapper is the call it replaces.

  python tools/gen_ld_rec.py            (writes the .inc; the spec is the table below)

A parameter is ('v', type, name) by value, ('arr', type, name, count) a pointer to <count> elements (NULL allowed),
('ptr', type, name) a pointer to one element (NULL allowed), ('fix', type, name, n) a pointer to n elements (NULL
allowed) or ('bytes', name, nbytes) a pointer to nbytes of data. The pointed-to data is copied into the record, so the
caller's arrays may go out of scope at once - which is what every call site does.
"""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / 'ps3recomp' / 'libs' / 'video' / 'rsx_live_rec.inc'

L = 'list'      # g.list
L9 = 'list9'    # g.list9
D = 'dev'       # g.dev
Q = 'queue'     # g.queue
A = 'alloc'     # the allocator passed as the first parameter

SPEC = [
    # name, object, return, params
    ('BeginQuery', L, 'void', [('v', 'ID3D12QueryHeap*', 'heap'), ('v', 'D3D12_QUERY_TYPE', 'type'), ('v', 'UINT', 'index')]),
    ('EndQuery', L, 'void', [('v', 'ID3D12QueryHeap*', 'heap'), ('v', 'D3D12_QUERY_TYPE', 'type'), ('v', 'UINT', 'index')]),
    ('ResolveQueryData', L, 'void', [('v', 'ID3D12QueryHeap*', 'heap'), ('v', 'D3D12_QUERY_TYPE', 'type'),
                                     ('v', 'UINT', 'start'), ('v', 'UINT', 'n'), ('v', 'ID3D12Resource*', 'dst'),
                                     ('v', 'UINT64', 'offset')]),
    ('ClearDepthStencilView', L, 'void', [('v', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'dsv'), ('v', 'D3D12_CLEAR_FLAGS', 'flags'),
                                          ('v', 'FLOAT', 'depth'), ('v', 'UINT8', 'stencil'), ('v', 'UINT', 'nrects'),
                                          ('arr', 'D3D12_RECT', 'rects', 'nrects')]),
    ('ClearRenderTargetView', L, 'void', [('v', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'rtv'), ('fix', 'FLOAT', 'color', 4),
                                          ('v', 'UINT', 'nrects'), ('arr', 'D3D12_RECT', 'rects', 'nrects')]),
    ('Close', L, 'HRESULT', []),
    ('CopyResource', L, 'void', [('v', 'ID3D12Resource*', 'dst'), ('v', 'ID3D12Resource*', 'src')]),
    ('CopyTextureRegion', L, 'void', [('ptr', 'D3D12_TEXTURE_COPY_LOCATION', 'dst'), ('v', 'UINT', 'x'), ('v', 'UINT', 'y'),
                                      ('v', 'UINT', 'z'), ('ptr', 'D3D12_TEXTURE_COPY_LOCATION', 'src'),
                                      ('ptr', 'D3D12_BOX', 'box')]),
    ('Dispatch', L, 'void', [('v', 'UINT', 'x'), ('v', 'UINT', 'y'), ('v', 'UINT', 'z')]),
    ('DrawIndexedInstanced', L, 'void', [('v', 'UINT', 'count'), ('v', 'UINT', 'instances'), ('v', 'UINT', 'start'),
                                         ('v', 'INT', 'base'), ('v', 'UINT', 'start_instance')]),
    ('DrawInstanced', L, 'void', [('v', 'UINT', 'count'), ('v', 'UINT', 'instances'), ('v', 'UINT', 'start'),
                                  ('v', 'UINT', 'start_instance')]),
    ('IASetIndexBuffer', L, 'void', [('ptr', 'D3D12_INDEX_BUFFER_VIEW', 'view')]),
    ('IASetPrimitiveTopology', L, 'void', [('v', 'D3D12_PRIMITIVE_TOPOLOGY', 'topology')]),
    ('IASetVertexBuffers', L, 'void', [('v', 'UINT', 'start'), ('v', 'UINT', 'n'),
                                       ('arr', 'D3D12_VERTEX_BUFFER_VIEW', 'views', 'n')]),
    ('OMSetBlendFactor', L, 'void', [('fix', 'FLOAT', 'factor', 4)]),
    ('OMSetRenderTargets', L, 'void', [('v', 'UINT', 'n'), ('arr', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'rtvs', '(single ? 1u : n)'),
                                       ('v', 'BOOL', 'single'), ('ptr', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'dsv')]),
    ('OMSetStencilRef', L, 'void', [('v', 'UINT', 'ref')]),
    ('RSSetScissorRects', L, 'void', [('v', 'UINT', 'n'), ('arr', 'D3D12_RECT', 'rects', 'n')]),
    ('RSSetViewports', L, 'void', [('v', 'UINT', 'n'), ('arr', 'D3D12_VIEWPORT', 'viewports', 'n')]),
    ('Reset', L, 'HRESULT', [('v', 'ID3D12CommandAllocator*', 'allocator'), ('v', 'ID3D12PipelineState*', 'pso')]),
    ('ResourceBarrier', L, 'void', [('v', 'UINT', 'n'), ('arr', 'D3D12_RESOURCE_BARRIER', 'barriers', 'n')]),
    ('SetComputeRoot32BitConstants', L, 'void', [('v', 'UINT', 'index'), ('v', 'UINT', 'n'), ('bytes', 'data', 'n * 4u'),
                                                 ('v', 'UINT', 'offset')]),
    ('SetComputeRootDescriptorTable', L, 'void', [('v', 'UINT', 'index'), ('v', 'D3D12_GPU_DESCRIPTOR_HANDLE', 'table')]),
    ('SetComputeRootSignature', L, 'void', [('v', 'ID3D12RootSignature*', 'rs')]),
    ('SetDescriptorHeaps', L, 'void', [('v', 'UINT', 'n'), ('arr', 'ID3D12DescriptorHeap*', 'heaps', 'n')]),
    ('SetGraphicsRoot32BitConstants', L, 'void', [('v', 'UINT', 'index'), ('v', 'UINT', 'n'), ('bytes', 'data', 'n * 4u'),
                                                  ('v', 'UINT', 'offset')]),
    ('SetGraphicsRootConstantBufferView', L, 'void', [('v', 'UINT', 'index'), ('v', 'D3D12_GPU_VIRTUAL_ADDRESS', 'va')]),
    ('SetGraphicsRootDescriptorTable', L, 'void', [('v', 'UINT', 'index'), ('v', 'D3D12_GPU_DESCRIPTOR_HANDLE', 'table')]),
    ('SetGraphicsRootSignature', L, 'void', [('v', 'ID3D12RootSignature*', 'rs')]),
    ('SetPipelineState', L, 'void', [('v', 'ID3D12PipelineState*', 'pso')]),
    ('RSSetDepthBias', L9, 'void', [('v', 'FLOAT', 'bias'), ('v', 'FLOAT', 'clamp'), ('v', 'FLOAT', 'slope')]),
    ('Reset', A, 'HRESULT', []),
    ('CopyDescriptors', D, 'void', [('v', 'UINT', 'nd'), ('arr', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'dst_starts', 'nd'),
                                    ('arr', 'UINT', 'dst_sizes', 'nd'), ('v', 'UINT', 'ns'),
                                    ('arr', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'src_starts', 'ns'),
                                    ('arr', 'UINT', 'src_sizes', 'ns'), ('v', 'D3D12_DESCRIPTOR_HEAP_TYPE', 'type')]),
    ('CopyDescriptorsSimple', D, 'void', [('v', 'UINT', 'n'), ('v', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'dst'),
                                          ('v', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'src'), ('v', 'D3D12_DESCRIPTOR_HEAP_TYPE', 'type')]),
    ('CreateDepthStencilView', D, 'void', [('v', 'ID3D12Resource*', 'res'), ('ptr', 'D3D12_DEPTH_STENCIL_VIEW_DESC', 'desc'),
                                           ('v', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'dst')]),
    ('CreateRenderTargetView', D, 'void', [('v', 'ID3D12Resource*', 'res'), ('ptr', 'D3D12_RENDER_TARGET_VIEW_DESC', 'desc'),
                                           ('v', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'dst')]),
    ('CreateSampler', D, 'void', [('ptr', 'D3D12_SAMPLER_DESC', 'desc'), ('v', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'dst')]),
    ('CreateShaderResourceView', D, 'void', [('v', 'ID3D12Resource*', 'res'), ('ptr', 'D3D12_SHADER_RESOURCE_VIEW_DESC', 'desc'),
                                             ('v', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'dst')]),
    ('CreateUnorderedAccessView', D, 'void', [('v', 'ID3D12Resource*', 'res'), ('v', 'ID3D12Resource*', 'counter'),
                                              ('ptr', 'D3D12_UNORDERED_ACCESS_VIEW_DESC', 'desc'),
                                              ('v', 'D3D12_CPU_DESCRIPTOR_HANDLE', 'dst')]),
    ('ExecuteCommandLists', Q, 'void', [('v', 'UINT', 'n'), ('arr', 'ID3D12CommandList*', 'lists', 'n')]),
    ('Signal', Q, 'HRESULT', [('v', 'ID3D12Fence*', 'fence'), ('v', 'UINT64', 'value')]),
]

PREFIX = {L: 'LDL_', L9: 'LDL9_', D: 'LDD_', Q: 'LDQ_', A: 'LDA_'}
OBJ = {L: 'g.list', L9: 'g.list9', D: 'g.dev', Q: 'g.queue'}
# a call the walker waits on soon after: publish at once (Signal: ld_flush then reads the fence)
PUBLISH_NOW = {('Signal', Q)}


def cparam(p):
    k = p[0]
    if k == 'v':
        return '%s %s' % (p[1], p[2])
    if k in ('arr', 'ptr', 'fix'):
        return '%s const* %s' % (p[1], p[2])     # "ID3D12DescriptorHeap* const*", as d3d12.h has it
    if k == 'bytes':
        return 'const void* %s' % p[1]
    raise ValueError(k)


def pname(p):
    return p[1] if p[0] == 'bytes' else p[2]


def gen():
    out = []
    w = out.append
    w('/* rsx_live_rec.inc - [GT6 0183] the D3D12 calls of the live-draw engine as records (LD_REC_THREAD).')
    w(' * Generated by tools/gen_ld_rec.py from its table - change the table, not this file.')
    w(' * Included by rsx_live_draw.c after its state g; the ring and the recording thread are there. */')
    w('')
    w('enum {')
    w('    LDR_PAD = 0, LDR_PRESENT, LDR_PRESENT_COPY, LDR_SYNC_NOTE,')
    for name, obj, ret, params in SPEC:
        w('    LDR_%s%s,' % (PREFIX[obj], name))
    w('    LDR_OPS')
    w('};')
    w('#define LDR_NULL 0xFFFFFFFFu')
    w('static u32 ldr_r8(u32 n) { return (n + 7u) & ~7u; }')
    w('')
    for name, obj, ret, params in SPEC:
        tag = PREFIX[obj] + name
        fields = []
        if obj == A:
            fields.append('ID3D12CommandAllocator* self;')
        for p in params:
            if p[0] == 'v':
                fields.append('%s %s;' % (p[1], p[2]))
            else:
                fields.append('u32 n_%s;' % pname(p))
        fields.append('u32 pad_;')
        w('typedef struct { %s } ldr_%s;' % (' '.join(fields), tag))
    w('')
    # wrappers
    for name, obj, ret, params in SPEC:
        tag = PREFIX[obj] + name
        args = []
        if obj == A:
            args.append('ID3D12CommandAllocator* self')
        args += [cparam(p) for p in params]
        sig = '%s %s(%s)' % ('static inline ' + ret, tag, ', '.join(args) if args else 'void')
        w(sig)
        w('{')
        callobj = 'self' if obj == A else OBJ[obj]
        direct_args = ', '.join([callobj] + [pname(p) for p in params])
        rkw = 'return ' if ret != 'void' else ''
        w('    if (!g_ld_rec_on) { %s%s->lpVtbl->%s(%s);%s }' % (rkw, callobj, name, direct_args, '' if ret != 'void' else ' return;'))
        # sizes
        size_terms = ['ldr_r8((u32)sizeof(ldr_%s))' % tag]
        for p in params:
            k = p[0]
            nm = pname(p)
            if k == 'arr':
                w('    const u32 c_%s = %s ? (u32)(%s) : LDR_NULL;' % (nm, nm, p[3]))
                size_terms.append('(c_%s == LDR_NULL ? 0u : ldr_r8(c_%s * (u32)sizeof(%s)))' % (nm, nm, p[1]))
            elif k == 'ptr':
                w('    const u32 c_%s = %s ? 1u : LDR_NULL;' % (nm, nm))
                size_terms.append('(c_%s == LDR_NULL ? 0u : ldr_r8((u32)sizeof(%s)))' % (nm, p[1]))
            elif k == 'fix':
                w('    const u32 c_%s = %s ? %du : LDR_NULL;' % (nm, nm, p[3]))
                size_terms.append('(c_%s == LDR_NULL ? 0u : ldr_r8(c_%s * (u32)sizeof(%s)))' % (nm, nm, p[1]))
            elif k == 'bytes':
                w('    const u32 c_%s = %s ? (u32)(%s) : LDR_NULL;' % (nm, nm, p[2]))
                size_terms.append('(c_%s == LDR_NULL ? 0u : ldr_r8(c_%s))' % (nm, nm))
        w('    ldr_%s* a = (ldr_%s*)ldr_begin(LDR_%s, %s);' % (tag, tag, tag, ' + '.join(size_terms)))
        if obj == A:
            w('    a->self = self;')
        w('    u8* t = (u8*)a + ldr_r8((u32)sizeof(ldr_%s));' % tag)
        for p in params:
            k = p[0]
            nm = pname(p)
            if k == 'v':
                w('    a->%s = %s;' % (nm, nm))
            elif k in ('arr', 'fix'):
                w('    a->n_%s = c_%s;' % (nm, nm))
                w('    if (c_%s != LDR_NULL) { memcpy(t, %s, (size_t)c_%s * sizeof(%s)); t += ldr_r8(c_%s * (u32)sizeof(%s)); }'
                  % (nm, nm, nm, p[1], nm, p[1]))
            elif k == 'ptr':
                w('    a->n_%s = c_%s;' % (nm, nm))
                w('    if (c_%s != LDR_NULL) { memcpy(t, %s, sizeof(%s)); t += ldr_r8((u32)sizeof(%s)); }' % (nm, nm, p[1], p[1]))
            elif k == 'bytes':
                w('    a->n_%s = c_%s;' % (nm, nm))
                w('    if (c_%s != LDR_NULL) { memcpy(t, %s, c_%s); t += ldr_r8(c_%s); }' % (nm, nm, nm, nm))
        w('    (void)t;')
        w('    ldr_end(%d);' % (1 if (name, obj) in PUBLISH_NOW else 0))
        if ret != 'void':
            w('    return S_OK;')
        w('}')
    w('')
    # executor
    w('static void ldr_exec_call(u32 op, const void* body)')
    w('{')
    w('    switch (op) {')
    for name, obj, ret, params in SPEC:
        tag = PREFIX[obj] + name
        w('    case LDR_%s: {' % tag)
        w('        const ldr_%s* a = (const ldr_%s*)body;' % (tag, tag))
        w('        const u8* t = (const u8*)a + ldr_r8((u32)sizeof(ldr_%s));' % tag)
        call_args = []
        for p in params:
            k = p[0]
            nm = pname(p)
            if k == 'v':
                call_args.append('a->%s' % nm)
            else:
                ty = 'void' if k == 'bytes' else p[1]
                w('        %s const* p_%s = a->n_%s == LDR_NULL ? NULL : (%s const*)t;' % (ty, nm, nm, ty))
                if k == 'bytes':
                    w('        if (a->n_%s != LDR_NULL) t += ldr_r8(a->n_%s);' % (nm, nm))
                elif k == 'ptr':
                    w('        if (a->n_%s != LDR_NULL) t += ldr_r8((u32)sizeof(%s));' % (nm, p[1]))
                else:
                    w('        if (a->n_%s != LDR_NULL) t += ldr_r8(a->n_%s * (u32)sizeof(%s));' % (nm, nm, p[1]))
                call_args.append('p_%s' % nm)
        callobj = 'a->self' if obj == A else OBJ[obj]
        allargs = ', '.join([callobj] + call_args)
        if ret == 'HRESULT':
            w('        const HRESULT hr = %s->lpVtbl->%s(%s);' % (callobj, name, allargs))
            w('        if (FAILED(hr)) ldr_failed("%s", hr);' % tag)
        else:
            w('        %s->lpVtbl->%s(%s);' % (callobj, name, allargs))
        w('        (void)t;')
        w('    } break;')
    w('    default: ldr_special(op, body); break;')
    w('    }')
    w('}')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    text = gen()
    if not OUT.exists() or OUT.read_text(encoding='utf-8') != text:
        OUT.write_text(text, encoding='utf-8', newline='\n')
    print(OUT, len(text.splitlines()), 'lines,', len(SPEC), 'calls')
