#pragma once
/* Exact type-8 progressive movie conversion from the GT6 embedded SPU image
 * 3f8bd8e4cf635018... (leaf3980). Pixel work runs in a D3D11 compute shader;
 * upload/readback preserve the existing guest-memory/RSX texture contract.
 * Other geometries continue through the original task. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace gt6_movie_gpu {
using Microsoft::WRL::ComPtr;
static constexpr const char shader[] = R"(
ByteAddressBuffer source : register(t0);
RWByteAddressBuffer output : register(u0);
cbuffer Dimensions : register(b0) { uint width; uint height; uint sourceBytes; uint reserved; };
uint byteAt(uint offset) { return (source.Load(offset & ~3u) >> ((offset & 3u) * 8)) & 255u; }
[numthreads(16,16,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint x=id.x, y=id.y; if (x>=width || y>=height) return;
    uint lumaBytes=width*height, chromaBytes=lumaBytes/4;
    uint at=(y/2)*(width/2)+x/2;
    int u=byteAt(lumaBytes+at), v=byteAt(lumaBytes+chromaBytes+at);
    if (x&1u) {
        // The first chroma row of each of the four original tasks has no
        // extra DMA quadword; the zero scratch byte is observable at x=w-1.
        bool firstTail=(x==width-1) && ((y%(height/4))<2);
        int un=firstTail ? 0 : byteAt(lumaBytes+at+1);
        int vn=firstTail ? 0 : byteAt(lumaBytes+chromaBytes+at+1);
        u=(u+un+1)/2; v=(v+vn+1)/2;
    }
    u-=128; v-=128; int yy=int(byteAt(y*width+x))*256;
    uint r=uint(clamp((yy+402*v)>>8,0,255));
    uint g=uint(clamp((yy-48*u-120*v)>>8,0,255));
    uint b=uint(clamp((yy+474*u)>>8,0,255));
    output.Store((y*width+x)*4, r|(g<<8)|(b<<16)|0xff000000u);
}
)";
class Converter {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> program;
    ComPtr<ID3D11Buffer> input, result, staging, constants;
    ComPtr<ID3D11ShaderResourceView> inputView;
    ComPtr<ID3D11UnorderedAccessView> outputView;
    uint32_t sourceCapacity=0, outputCapacity=0;
    bool attempted=false, disabled=false, warp=false;
    HRESULT prepare(uint32_t sourceSize, uint32_t outputSize) {
        if (disabled) return E_FAIL;
        HRESULT hr=S_OK;
        if (!attempted) {
            attempted=true;
            D3D_FEATURE_LEVEL level;
            hr=D3D11CreateDevice(nullptr,warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
                nullptr,0,D3D11_SDK_VERSION,&device,&level,&context);
            if (SUCCEEDED(hr) && level<D3D_FEATURE_LEVEL_11_0) hr=E_FAIL;
            ComPtr<ID3DBlob> blob,errors;
            if (SUCCEEDED(hr)) hr=D3DCompile(shader,sizeof(shader)-1,"GT6 movie type8",nullptr,nullptr,"main","cs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
            if (FAILED(hr) && errors) std::fprintf(stderr,"[gt6-movie-gpu] shader: %s\n",(const char*)errors->GetBufferPointer());
            if (SUCCEEDED(hr)) hr=device->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&program);
            D3D11_BUFFER_DESC cb{}; cb.ByteWidth=16; cb.Usage=D3D11_USAGE_DYNAMIC;
            cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            if (SUCCEEDED(hr)) hr=device->CreateBuffer(&cb,nullptr,&constants);
            if (FAILED(hr)) { disabled=true; return hr; }
            std::fprintf(stderr,"[gt6-movie-gpu] type8 integer RGBA compute ready (%s); guest upload/readback retained\n",warp?"WARP fixture":"hardware");
        }
        if (sourceSize!=sourceCapacity) {
            inputView.Reset(); input.Reset(); sourceCapacity=0;
            D3D11_BUFFER_DESC desc{}; desc.ByteWidth=sourceSize; desc.Usage=D3D11_USAGE_DYNAMIC;
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE; desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            hr=device->CreateBuffer(&desc,nullptr,&input);
            D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX; view.BufferEx.NumElements=sourceSize/4;
            view.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
            if (SUCCEEDED(hr)) hr=device->CreateShaderResourceView(input.Get(),&view,&inputView);
            if (FAILED(hr)) return hr;
            sourceCapacity=sourceSize;
        }
        if (outputSize!=outputCapacity) {
            outputView.Reset(); result.Reset(); staging.Reset(); outputCapacity=0;
            D3D11_BUFFER_DESC desc{}; desc.ByteWidth=outputSize; desc.Usage=D3D11_USAGE_DEFAULT;
            desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS; desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            hr=device->CreateBuffer(&desc,nullptr,&result);
            D3D11_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER; view.Buffer.NumElements=outputSize/4;
            view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
            if (SUCCEEDED(hr)) hr=device->CreateUnorderedAccessView(result.Get(),&view,&outputView);
            desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=desc.MiscFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if (SUCCEEDED(hr)) hr=device->CreateBuffer(&desc,nullptr,&staging);
            if (FAILED(hr)) return hr;
            outputCapacity=outputSize;
        }
        return S_OK;
    }
public:
    explicit Converter(bool useWarp=false):warp(useWarp) {}
    static bool supports(uint32_t w,uint32_t h,uint32_t ow,uint32_t oh) {
        return w && h && w<=4096 && h<=2160 && !(w%64) && !(h%8) && w==ow && h==oh;
    }
    bool convert(const uint8_t* source, uint8_t* destination,uint32_t w,uint32_t h) {
        if (disabled || !supports(w,h,w,h)) return false;
        const uint32_t sourceSize=w*h*3/2+16, outputSize=w*h*4;
        HRESULT hr=prepare(sourceSize,outputSize);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(hr)) hr=context->Map(input.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map);
        if (SUCCEEDED(hr)) { std::memcpy(map.pData,source,sourceSize); context->Unmap(input.Get(),0); }
        if (SUCCEEDED(hr)) hr=context->Map(constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map);
        if (SUCCEEDED(hr)) {
            const uint32_t args[4]={w,h,sourceSize,0}; std::memcpy(map.pData,args,sizeof(args)); context->Unmap(constants.Get(),0);
            ID3D11ShaderResourceView* srv=inputView.Get(); ID3D11UnorderedAccessView* uav=outputView.Get();
            ID3D11Buffer* cb=constants.Get();
            context->CSSetShader(program.Get(),nullptr,0); context->CSSetShaderResources(0,1,&srv);
            context->CSSetUnorderedAccessViews(0,1,&uav,nullptr); context->CSSetConstantBuffers(0,1,&cb);
            context->Dispatch((w+15)/16,(h+15)/16,1);
            srv=nullptr; uav=nullptr;
            context->CSSetShaderResources(0,1,&srv); context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
            context->CopyResource(staging.Get(),result.Get());
            hr=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map);
            if (SUCCEEDED(hr)) { std::memcpy(destination,map.pData,outputSize); context->Unmap(staging.Get(),0); }
        }
        if (FAILED(hr)) {
            std::fprintf(stderr,"[gt6-movie-gpu] conversion failed %08lx; original SPU task retained\n",(unsigned long)hr);
            disabled=true;
        }
        return SUCCEEDED(hr);
    }
};
}
