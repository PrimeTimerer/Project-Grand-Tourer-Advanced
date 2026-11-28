"""[GT6 0183] Route the live-draw engine's D3D12 command-list, descriptor and submit calls through the record
wrappers of rsx_live_rec.inc (tools/gen_ld_rec.py): "g.list->lpVtbl->X(g.list, ..." -> "LDL_X(...", and so on.

  python tools/ld_rec_convert.py <rsx_live_draw.c>      (rewrites the file in place; prints what it changed)

Left alone on purpose: QueryInterface and SetName (made once at start, before any record), the swap chain (the
present and the back-buffer index are handled by hand in rsx_live_draw.c), the fence (thread-safe; the waits stay on
the drawing thread). Calls inside the ldr_* block itself are the recording thread's and stay as they are.
"""
import re, sys, collections

path = sys.argv[1]
src = open(path, encoding='utf-8', newline='').read()      # the file's own line ends (CRLF) are kept
LIST = {'BeginQuery', 'EndQuery', 'ResolveQueryData', 'ClearDepthStencilView', 'ClearRenderTargetView', 'Close',
        'CopyResource', 'CopyTextureRegion', 'Dispatch', 'DrawIndexedInstanced', 'DrawInstanced', 'IASetIndexBuffer',
        'IASetPrimitiveTopology', 'IASetVertexBuffers', 'OMSetBlendFactor', 'OMSetRenderTargets', 'OMSetStencilRef',
        'RSSetScissorRects', 'RSSetViewports', 'Reset', 'ResourceBarrier', 'SetComputeRoot32BitConstants',
        'SetComputeRootDescriptorTable', 'SetComputeRootSignature', 'SetDescriptorHeaps', 'SetGraphicsRoot32BitConstants',
        'SetGraphicsRootConstantBufferView', 'SetGraphicsRootDescriptorTable', 'SetGraphicsRootSignature', 'SetPipelineState'}
DEV = {'CopyDescriptors', 'CopyDescriptorsSimple', 'CreateDepthStencilView', 'CreateRenderTargetView', 'CreateSampler',
       'CreateShaderResourceView', 'CreateUnorderedAccessView'}
QUEUE = {'ExecuteCommandLists', 'Signal'}

# the recording thread's own block (between these two markers) is not rewritten
start = re.search(r'static void ldr_special\(u32 op, const void\* body\)\r?\n\{', src).start()
end = src.index('/* every record made so far has been made into a call')
head, block, tail = src[:start], src[start:end], src[end:]
n = collections.Counter()


def sub(text):
    def rl(m):
        meth = m.group(1)
        if meth not in LIST:
            return m.group(0)
        n['LDL_' + meth] += 1
        return 'LDL_%s(%s' % (meth, ')' if m.group(2) == ')' else '')
    text = re.sub(r'\bg\.list->lpVtbl->([A-Za-z0-9]+)\(\s*g\.list\s*(,\s*|\))', rl, text)

    def r9(m):
        n['LDL9_' + m.group(1)] += 1
        return 'LDL9_%s(' % m.group(1)
    text = re.sub(r'\bg\.list9->lpVtbl->(RSSetDepthBias)\(\s*g\.list9\s*,\s*', r9, text)

    def rd(m):
        meth = m.group(1)
        if meth not in DEV:
            return m.group(0)
        n['LDD_' + meth] += 1
        return 'LDD_%s(' % meth
    text = re.sub(r'\bg\.dev->lpVtbl->([A-Za-z0-9]+)\(\s*g\.dev\s*,\s*', rd, text)

    def rq(m):
        meth = m.group(1)
        if meth not in QUEUE:
            return m.group(0)
        n['LDQ_' + meth] += 1
        return 'LDQ_%s(' % meth
    text = re.sub(r'\bg\.queue->lpVtbl->([A-Za-z0-9]+)\(\s*g\.queue\s*,\s*', rq, text)

    def ra(m):
        n['LDA_Reset'] += 1
        return 'LDA_Reset(g.alloc)'
    text = re.sub(r'\bg\.alloc->lpVtbl->Reset\(\s*g\.alloc\s*\)', ra, text)
    return text


head2, tail2 = sub(head), sub(tail)
out = head2 + block + tail2
open(path, 'w', encoding='utf-8', newline='').write(out)
for k, v in sorted(n.items()):
    print('%-44s %d' % (k, v))
print('total', sum(n.values()))
left = re.findall(r'\bg\.(list9?|dev|queue|alloc)->lpVtbl->([A-Za-z0-9]+)\(', head2 + tail2)
print('left as they were:', collections.Counter('%s.%s' % x for x in left))
