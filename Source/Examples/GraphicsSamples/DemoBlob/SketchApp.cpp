﻿#include <string>
#include <stdexcept>
#include <iostream>
#include <chrono>
#include <algorithm>

#include <wrl/client.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3dx12.h>
#include <DirectXMath.h>

#include "Launcher.h"
#include "ShadersVS.h"
#include "ShadersPS.h"

using Microsoft::WRL::ComPtr;

inline std::string HrToString(HRESULT hr, const std::string& context)
{
    char str[64] = {};
    sprintf_s(str, "HRESULT of 0x%08X", static_cast<unsigned int>(hr));
    std::string result(str);
    if (context.length() > 0)
    {
        result += ": ";
        result += context;
    }
    return result;
}

// Helper class for COM exceptions
// https://docs.microsoft.com/en-us/windows/win32/seccrypto/common-hresult-values
// https://docs.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-error
class HrException : public std::runtime_error
{
public:
    HrException(HRESULT hr, const std::string& context) : std::runtime_error(HrToString(hr, context)), result_(hr) {}
    HRESULT Error() const { return result_; }

private:
    const HRESULT result_;
};

// Helper utility converts D3D API failures into exceptions.
inline void ThrowIfFailed(HRESULT hr, const std::string& context = "")
{
    if (FAILED(hr))
    {
        throw HrException(hr, context);
    }
}

class DemoBlob : public sketch::SketchBase
{
    static const UINT kNumSwapChainBuffers = 2;

    struct Vertex
    {
        DirectX::XMFLOAT2 position;
        DirectX::XMFLOAT4 color;
        DirectX::XMFLOAT2 uv;
    };

    struct SceneConstantBuffer
    {
        DirectX::XMFLOAT2 center;
        float aspect;
        float padding[61]; // Padding so the constant buffer is 256-byte aligned.
    };
    static_assert((sizeof(SceneConstantBuffer) % 256) == 0, "Constant Buffer size must be 256-byte aligned");

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> commandQueue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> cbvSrvHeap_;
    UINT rtvDescriptorSize_;
    UINT cbvSrvDescriptorSize_;
    ComPtr<ID3D12Resource> swapChainBuffers_[kNumSwapChainBuffers];
    ComPtr<ID3D12CommandAllocator> commandAllocator_;
    ComPtr<ID3D12GraphicsCommandList> commandList_;
    ComPtr<ID3D12Fence> fence_;
    UINT64 fenceValue_;
    HANDLE fenceEventHandle_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipelineState_;
    ComPtr<ID3D12Resource> vertexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_;
    SceneConstantBuffer constantBufferData_;
    ComPtr<ID3D12Resource> constantBuffer_;
    UINT8* cbvDataBegin_;

    UINT fieldWidth_ = 480;
    UINT fieldHeight_ = 270;
    ComPtr<ID3D12Resource> vectorFieldBuffers[2];
    ComPtr<ID3D12RootSignature> vectorFieldRootSignature_;
    ComPtr<ID3D12PipelineState> vectorFieldPipelineState_;

public:
    virtual void OnInit() override
    {
        // Device, command queue, swap chain
        CreateInfrastructure();

        // Create fence
        CreateFence();

        // Descriptor heaps
        CreateRenderTargetDescriptorHeap();
        CreateConstantBufferDescriptorHeap();

        // Root Signature
        CreateRootSignature();

        // Pipeline state object
        CreatePipelineState();

        // Create the constant buffer
        CreateConstantBuffer();

        // Create the vertex buffer.
        CreateVertexBuffer();

        // Command allocator and list
        CreateCommandList();

        CreateVectorFieldBuffers();
        CreateVectorFieldRootSignature();
        CreateVectorFieldPipelineState();
    }

    virtual void OnUpdate() override
    {
        RenderToBackBuffer();

        PresentAndSwapBuffers();

        FlushCommandQueue();
    }

    virtual void OnQuit() override
    {
        FlushCommandQueue();
        constantBuffer_->Unmap(0, nullptr);
        CloseHandle(fenceEventHandle_);
    }
    
    virtual void OnResize(int width, int height) override
    {
        FlushCommandQueue();

        // Release the resources holding references to the swap chain (requirement of IDXGISwapChain::ResizeBuffers)
        for (UINT index = 0; index < kNumSwapChainBuffers; index++)
        {
            swapChainBuffers_[index].Reset();
        }

        // Resize the swap chain to the desired dimensions.
        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        swapChain_->GetDesc(&swapChainDesc);
        swapChain_->ResizeBuffers(kNumSwapChainBuffers, static_cast<UINT>(width), static_cast<UINT>(height), swapChainDesc.BufferDesc.Format, swapChainDesc.Flags);

        CreateSwapChainRTV();

        constantBufferData_.aspect = static_cast<float>(width) / static_cast<float>(height);
        memcpy(cbvDataBegin_, &constantBufferData_, sizeof(constantBufferData_));
    }

    virtual void OnMouseDrag(int x, int y, sketch::MouseButtonType buttonType) override
    {
        (void)buttonType;

        float xNormalized = static_cast<float>(x) / static_cast<float>(GetState().ViewportWidth);
        float yNormalized = static_cast<float>(y) / static_cast<float>(GetState().ViewportHeight);
        constantBufferData_.center = DirectX::XMFLOAT2(xNormalized, yNormalized);
        memcpy(cbvDataBegin_, &constantBufferData_, sizeof(constantBufferData_));
    }

    void CreateInfrastructure()
    {
        UINT dxgiFactoryFlag = 0;

#ifndef NDEBUG
        // Enable the debug layer (requires the Graphics Tools "optional feature").
        // NOTE: Enabling the debug layer after device_ creation will invalidate the active device_.
        // When the debug layer is enabled, D3D will send debug messages to the Visual Studio output window like the following :
        // D3D12 ERROR : ID3D12CommandList::Reset : Reset fails because the command list was not closed.
        {
            ComPtr<ID3D12Debug> debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
            {
                debugController->EnableDebugLayer();

                // Enable additional debug layers
                dxgiFactoryFlag |= DXGI_CREATE_FACTORY_DEBUG;
            }
        }
#endif // !NDEBUG

        // Factory
        ComPtr<IDXGIFactory6> dxgiFactory6;
        ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlag, IID_PPV_ARGS(&dxgiFactory6)), "CreateDXGIFactory2");

        // Adapter
        ComPtr<IDXGIAdapter> adapter;
        ThrowIfFailed(dxgiFactory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)), "EnumAdapterByGpuPreference");

        // Device
        ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)), "D3D12CreateDevice");

        // MSAA support
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qualityLevels = {};
        qualityLevels.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        qualityLevels.SampleCount = 4;
        qualityLevels.NumQualityLevels = 0;
        ThrowIfFailed(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qualityLevels, sizeof(qualityLevels)), "CheckFeatureSupport");

        // Command queue
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        ThrowIfFailed(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue_)), "CreateCommandQueue");

        // Swap chain
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = GetConfig().Width;
        swapChainDesc.Height = GetConfig().Height;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.Scaling = DXGI_SCALING_NONE;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = kNumSwapChainBuffers;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        // Support Vsync off
        // https://docs.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
        BOOL allowTearing = FALSE;
        ThrowIfFailed(dxgiFactory6->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing)), "CheckFeatureSupport");
        swapChainDesc.Flags = allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        SetFeature([allowTearing](Feature& feature) { feature.Tearing = allowTearing; });

        ComPtr<IDXGISwapChain1> swapChain;
        ThrowIfFailed(dxgiFactory6->CreateSwapChainForHwnd(commandQueue_.Get(), launcher::GetMainWindow(), &swapChainDesc, nullptr, nullptr, swapChain.GetAddressOf()), "CreateSwapChainForHwnd");
        ThrowIfFailed(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain_)), "QueryInterface");

        // Disable Alt+Enter fullscreen transitions offered by IDXGIFactory
        //
        // https://docs.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgifactory-makewindowassociation?redirectedfrom=MSDN#remarks
        // Applications that want to handle mode changes or Alt+Enter themselves should call 
        // MakeWindowAssociation with the DXGI_MWA_NO_WINDOW_CHANGES flag **AFTER** swap chain creation.
        // Ensures that DXGI will not interfere with application's handling of window mode changes or Alt+Enter.
        ThrowIfFailed(dxgiFactory6->MakeWindowAssociation(launcher::GetMainWindow(), DXGI_MWA_NO_ALT_ENTER));
    }

    void CreateRenderTargetDescriptorHeap()
    {
        // Describe and create a render target view (RTV) descriptor heap
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = kNumSwapChainBuffers + 2;  // + 2 vector field generation buffers

        ThrowIfFailed(device_->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap_)), "CreateDescriptorHeap");

        // Descriptor size
        rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    void CreateConstantBufferDescriptorHeap()
    {
        // Describe and create a constant buffer view (CBV) descriptor heap
        D3D12_DESCRIPTOR_HEAP_DESC cbvSrvHeapDesc = {};
        cbvSrvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        cbvSrvHeapDesc.NumDescriptors = 1 + 2;  // One CBV + 2 SRV for vector field
        cbvSrvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        ThrowIfFailed(device_->CreateDescriptorHeap(&cbvSrvHeapDesc, IID_PPV_ARGS(&cbvSrvHeap_)));

        // Descriptor Size
        cbvSrvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void CreateRootSignature()
    {
        // Root Signagure, consisting of a descriptor table with a single CBV
        CD3DX12_DESCRIPTOR_RANGE1 ranges[] = { CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC) };
        CD3DX12_ROOT_PARAMETER1 rootParameters[] = { CD3DX12_ROOT_PARAMETER1() };
        rootParameters[0].InitAsDescriptorTable(_countof(ranges), ranges, D3D12_SHADER_VISIBILITY_PIXEL);

        // Allow input layout and deny uneccessary access to certain pipeline stages.
        D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr, rootSignatureFlags);

        ComPtr<ID3DBlob> signature;
        ThrowIfFailed(D3D12SerializeVersionedRootSignature(&rootSignatureDesc, signature.GetAddressOf(), nullptr), "D3D12SerializeVersionedRootSignature");
        ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature_)), "CreateRootSignature");
    }

    void CreatePipelineState()
    {
        // Define the vertex input layout.
        D3D12_INPUT_ELEMENT_DESC inputElementDesc[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        D3D12_INPUT_LAYOUT_DESC inputLayoutDesc{ inputElementDesc, _countof(inputElementDesc) };

        // Pipeline state object
        // Describe and create the graphics pipeline state object (PSO).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = inputLayoutDesc;
        psoDesc.pRootSignature = rootSignature_.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(g_Shaders_VSMain, sizeof(g_Shaders_VSMain));
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(g_Shaders_PSMain, sizeof(g_Shaders_PSMain));
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        ThrowIfFailed(device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState_)), "CreateGraphicsPipelineState");
    }

    void CreateConstantBuffer()
    {
        // Create the constant buffer
        const UINT constantBufferSize = sizeof(SceneConstantBuffer);

        CD3DX12_HEAP_PROPERTIES uploadPropety(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC constantBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);
        ThrowIfFailed(device_->CreateCommittedResource(&uploadPropety, D3D12_HEAP_FLAG_NONE, &constantBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constantBuffer_)));

        // Describe and create a constant buffer view (CBV)
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = constantBuffer_->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes = constantBufferSize;

        CD3DX12_CPU_DESCRIPTOR_HANDLE cbvHandle(cbvSrvHeap_->GetCPUDescriptorHandleForHeapStart());
        device_->CreateConstantBufferView(&cbvDesc, cbvHandle);

        // Map and initialize the constant buffer. We don't unmap this until the
        // app closes. Keeping things mapped for the lifetime of the resource is okay.
        CD3DX12_RANGE readRange(0, 0);        // We do not intend to read from this resource on the CPU.
        ThrowIfFailed(constantBuffer_->Map(0, &readRange, reinterpret_cast<void**>(&cbvDataBegin_)));
        constantBufferData_.center = DirectX::XMFLOAT2(0.5f, 0.5f);
        constantBufferData_.aspect = 1.0f;
        memcpy(cbvDataBegin_, &constantBufferData_, constantBufferSize);
    }

    void CreateFence()
    {
        // Fence
        UINT64 initialFenceValue = 0;
        ThrowIfFailed(device_->CreateFence(initialFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        fenceValue_ = initialFenceValue + 1;
        fenceEventHandle_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    void CreateVertexBuffer()
    {
        // Define the geometry for a quad.
        Vertex quadVertices[] =
        {
            { { -1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 1.0f }, {0.0f, 0.0f} },
            { { 1.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f }, {1.0f, 0.0f} },
            { { -1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f }, {0.0f, 1.0f} },
            { { 1.0f, -1.0f }, { 1.0f, 1.0f, 0.0f, 1.0f }, {1.0f, 1.0f} }
        };

        const UINT vertexBufferSize = sizeof(quadVertices);

        // Note: using upload heaps to transfer static data like vert buffers is not recommended.
        // Every time the GPU needs it, the upload heap will be marshalled over. Use default heaps instead.
        CD3DX12_RESOURCE_DESC vertexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

        CD3DX12_HEAP_PROPERTIES defaultProperty(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(device_->CreateCommittedResource(&defaultProperty, D3D12_HEAP_FLAG_NONE, &vertexBufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&vertexBuffer_)), "CreateCommittedResource");

        ComPtr<ID3D12Resource> vertexBufferUpload;
        CD3DX12_HEAP_PROPERTIES uploadPropety(D3D12_HEAP_TYPE_UPLOAD);
        ThrowIfFailed(device_->CreateCommittedResource(&uploadPropety, D3D12_HEAP_FLAG_NONE, &vertexBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexBufferUpload)), "CreateCommittedResource");

        // Copy the triangle data to the vertex buffer in upload heap.
        UINT8* vertexDataBegin;
        CD3DX12_RANGE readRange(0, 0);        // We do not intend to read from this resource on the CPU.
        ThrowIfFailed(vertexBufferUpload->Map(0, &readRange, reinterpret_cast<void**>(&vertexDataBegin)), "Map");
        memcpy(vertexDataBegin, quadVertices, sizeof(quadVertices));
        vertexBufferUpload->Unmap(0, nullptr);

        ComPtr<ID3D12CommandAllocator> copyCommandAllocator;
        ComPtr<ID3D12GraphicsCommandList> copyCommandList;
        // Command allocator
        ThrowIfFailed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copyCommandAllocator)), "CreateCommandAllocator");
        ThrowIfFailed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copyCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&copyCommandList)), "CreateCommandList");

        // 将顶点数据由 upload heap 拷贝至 default heap
        // 这里只是记录指令，实际执行在本函数的末尾。因此，必须保证 upload heap 中分配的资源在指令执行时是有效的。
        copyCommandList->CopyBufferRegion(vertexBuffer_.Get(), 0, vertexBufferUpload.Get(), 0, vertexBufferSize);

        // 切换顶点缓冲的状态
        CD3DX12_RESOURCE_BARRIER toVertexBufferBarrier = CD3DX12_RESOURCE_BARRIER::Transition(vertexBuffer_.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        copyCommandList->ResourceBarrier(1, &toVertexBufferBarrier);

        // Initialize the vertex buffer view.
        vertexBufferView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
        vertexBufferView_.StrideInBytes = sizeof(Vertex);
        vertexBufferView_.SizeInBytes = vertexBufferSize;

        // Command lists are created in the recording state.
        // Close the resource creation command list and execute it to begin the vertex buffer copy into the default heap.
        ThrowIfFailed(copyCommandList->Close(), "Close command list");
        ID3D12CommandList* commandLists[] = { copyCommandList.Get() };
        commandQueue_->ExecuteCommandLists(_countof(commandLists), commandLists);

        FlushCommandQueue();
    }

    void CreateCommandList()
    {
        // Command allocator
        ThrowIfFailed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator_)), "CreateCommandAllocator");

        // Command list
        ThrowIfFailed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator_.Get(), pipelineState_.Get(), IID_PPV_ARGS(&commandList_)), "CreateCommandList");
        ThrowIfFailed(commandList_->Close(), "Close command list when initializing");
    }

    void RenderToBackBuffer()
    {
        // Command list allocators can only be reset when the associated command lists have finished execution on the GPU.
        // Apps shoud use fences to determin GPU execution progress, which we will do at the end of this function.
        ThrowIfFailed(commandAllocator_->Reset(), "Reset command allocator");

        // After ExecuteCommandList() has been called on a particular command list,
        // that command list can then be reset at any time before re-recoding.
        ThrowIfFailed(commandList_->Reset(commandAllocator_.Get(), pipelineState_.Get()), "Reset command list");

        // Indicate the the back buffer will be used as a render target.
        const UINT backBufferIndex = swapChain_->GetCurrentBackBufferIndex();
        CD3DX12_RESOURCE_BARRIER toRenderBarrier = CD3DX12_RESOURCE_BARRIER::Transition(swapChainBuffers_[backBufferIndex].Get(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList_->ResourceBarrier(1, &toRenderBarrier);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap_->GetCPUDescriptorHandleForHeapStart(), backBufferIndex, rtvDescriptorSize_);

        commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        // Set necessary state.
        commandList_->SetGraphicsRootSignature(rootSignature_.Get());
        // 绑定数据
        ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvHeap_.Get() };
        commandList_->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
        commandList_->SetGraphicsRootDescriptorTable(0, cbvSrvHeap_->GetGPUDescriptorHandleForHeapStart());

        CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(GetState().ViewportWidth), static_cast<float>(GetState().ViewportHeight));
        CD3DX12_RECT scissorRect(0, 0, static_cast<LONG>(GetState().ViewportWidth), static_cast<LONG>(GetState().ViewportHeight));
        commandList_->RSSetViewports(1, &viewport);
        commandList_->RSSetScissorRects(1, &scissorRect);

        // Record commands
        const FLOAT clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
        commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        commandList_->IASetVertexBuffers(0, 1, &vertexBufferView_);
        commandList_->DrawInstanced(4, 1, 0, 0);

        // Indicate that the back buffer will now be used to present.
        CD3DX12_RESOURCE_BARRIER toPresentBarrier = CD3DX12_RESOURCE_BARRIER::Transition(swapChainBuffers_[backBufferIndex].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        commandList_->ResourceBarrier(1, &toPresentBarrier);

        // Command list is expected to be closed before calling Reset again.
        ThrowIfFailed(commandList_->Close(), "Clost command list");

        // Execulte the command list
        ID3D12CommandList* commandLists[] = { commandList_.Get() };
        commandQueue_->ExecuteCommandLists(_countof(commandLists), commandLists);
    }

    void PresentAndSwapBuffers()
    {
        // Present and swap buffers
        if (GetConfig().Vsync)
        {
            ThrowIfFailed(swapChain_->Present(1, 0), "Present");
        }
        else
        {
            ThrowIfFailed(swapChain_->Present(0, GetFeature().Tearing ? DXGI_PRESENT_ALLOW_TEARING : 0), "Present");
        }
    }

    void CreateSwapChainRTV()
    {
        // Create RTV for each back buffer, RTV for back buffers are stored at the start of render target descriptor heap.
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap_->GetCPUDescriptorHandleForHeapStart());
        for (UINT index = 0; index < kNumSwapChainBuffers; index++)
        {
            // Save pointers to back buffers in swapChainBuffers.
            ThrowIfFailed(swapChain_->GetBuffer(index, IID_PPV_ARGS(&swapChainBuffers_[index])), "GetBuffer");

            device_->CreateRenderTargetView(swapChainBuffers_[index].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, rtvDescriptorSize_);
        }
    }

    void CreateVectorFieldBuffers()
    {
        CD3DX12_HEAP_PROPERTIES defaultProperty(D3D12_HEAP_TYPE_DEFAULT);
        CD3DX12_RESOURCE_DESC renderTargetDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8G8B8A8_UNORM,
            fieldWidth_, fieldHeight_,
            1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap_->GetCPUDescriptorHandleForHeapStart(), kNumSwapChainBuffers, rtvDescriptorSize_);
        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(cbvSrvHeap_->GetCPUDescriptorHandleForHeapStart(), 1, cbvSrvDescriptorSize_);
        for (int i = 0; i < 2; i++)
        {
            ThrowIfFailed(device_->CreateCommittedResource(&defaultProperty, D3D12_HEAP_FLAG_NONE, &renderTargetDesc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&vectorFieldBuffers[i])), "Create vector field buffer");

            // null pDesc argument will inherit the resource format and dimension (if not typeless) and RTVs target the first mip and all array slices.
            device_->CreateRenderTargetView(vectorFieldBuffers[i].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, rtvDescriptorSize_);

            // null pDesc argument will inherit the resource format and dimension (if not typeless) and for buffers SRVs target a full buffer and are typed (not raw or structured), 
            // and for textures SRVs target a full texture, all mips and all array slices.
            device_->CreateShaderResourceView(vectorFieldBuffers[i].Get(), nullptr, srvHandle);
            srvHandle.Offset(1, cbvSrvDescriptorSize_);
        }
    }

    void CreateVectorFieldRootSignature()
    {
        CD3DX12_DESCRIPTOR_RANGE1 ranges[] = { CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0) };
        CD3DX12_ROOT_PARAMETER1 rootParameters[] = { CD3DX12_ROOT_PARAMETER1() };
        rootParameters[0].InitAsDescriptorTable(_countof(ranges), ranges, D3D12_SHADER_VISIBILITY_PIXEL);

        D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags = 
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        // Create a sampler
        D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 1, &samplerDesc, rootSignatureFlags);
        ComPtr<ID3DBlob> signature;
        ThrowIfFailed(D3D12SerializeVersionedRootSignature(&rootSignatureDesc, &signature, nullptr), "D3D12SerializeVersionedRootSignature for vector field");
        ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&vectorFieldRootSignature_)), "CreateRootSignature for vector field");
    }

    void CreateVectorFieldPipelineState()
    {
        D3D12_INPUT_ELEMENT_DESC inputElementDesc[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        D3D12_INPUT_LAYOUT_DESC inputLayoutDesc{ inputElementDesc, _countof(inputElementDesc) };
    }

    void FlushCommandQueue()
    {
        // Add an instruction to the command queue to set a new fence point by instructing 'fence_' to wait for the 'fenceValue_'.
        // `fence_` value won't be set by GPU until it finishes processing all the commands prior to this `Signal()`.
        const UINT64 fenceValueToWaitFor = fenceValue_;
        ThrowIfFailed(commandQueue_->Signal(fence_.Get(), fenceValueToWaitFor), "Signal");
        fenceValue_++;

        // Wait until the GPU has completed commands up to this fence point.
        if (fence_->GetCompletedValue() < fenceValueToWaitFor)
        {
            // Fire event when GPU hits current fence.
            ThrowIfFailed(fence_->SetEventOnCompletion(fenceValueToWaitFor, fenceEventHandle_), "SetEventOnCompletion");
            // Wait until the created event is fired
            WaitForSingleObject(fenceEventHandle_, INFINITE);
        }
    }
};

CREATE_SKETCH(DemoBlob,
    [](sketch::SketchBase::Config& config)
    {
        config.Width = 800;
        config.Height = 450;
        //config.Vsync = false;
        config.WindowModeSwitch = true;
        //config.Fullscreen = true;
    }
)