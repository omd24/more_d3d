#include <windows.h>
#include <d3d12.h>

#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <dxgidebug.h>

#include <stdio.h>

#if !defined(NDEBUG) && !defined(_DEBUG)
#error "Define at least one."
#elif defined(NDEBUG) && defined(_DEBUG)
#error "Define at most one."
#endif

#define SUCCEEDED(hr)   (((HRESULT)(hr)) >= 0)
//#define SUCCEEDED_OPERATION(hr)   (((HRESULT)(hr)) == S_OK)
#define FAILED(hr)      (((HRESULT)(hr)) < 0)
//#define FAILED_OPERATION(hr)      (((HRESULT)(hr)) != S_OK)
#define CHECK_AND_FAIL(hr)                          \
    if (FAILED(hr)) {                               \
        ::printf("[ERROR] " #hr "() failed at line %d. \n", __LINE__);   \
        ::abort();                                  \
    }                                               \

#if defined(_DEBUG)
#define ENABLE_DEBUG_LAYER 1
#else
#define ENABLE_DEBUG_LAYER 0
#endif

bool global_running;

#define ARRAY_COUNT(arr)            sizeof(arr)/sizeof(arr[0])
#define SIMPLE_ASSERT(exp) if(!(exp))  {*(int *)0 = 0;}

// In this sample we overload the meaning of FrameCount to mean both the maximum
// number of frames that will be queued to the GPU at a time, as well as the number
// of back buffers in the DXGI swap chain. For the majority of applications, this
// is convenient and works well. However, there will be certain cases where an
// application may want to queue up more frames than there are back buffers
// available.
// It should be noted that excessive buffering of frames dependent on user input
// may result in noticeable latency in your app.
#define FRAME_COUNT 2

struct SceneConstantBuffer {
    DirectX::XMFLOAT4 offset;
    float padding [60];             // Padding so the constant buffer is 256-byte aligned
};
static_assert(256 == sizeof(SceneConstantBuffer), "Constant buffer size must be 256b aligned");
struct D3DRenderContext {
    
    // Display data
    UINT width;
    UINT height;
    float aspect_ratio;

    // Pipeline stuff
    D3D12_VIEWPORT                  viewport;
    D3D12_RECT                      scissor_rect;
    IDXGISwapChain3 *               swapchain3;
    IDXGISwapChain *                swapchain;
    ID3D12Device *                  device;
    ID3D12Resource *                render_targets [FRAME_COUNT];
    ID3D12CommandAllocator *        cmd_allocator [FRAME_COUNT];
    ID3D12CommandAllocator *        bundle_allocator;
    ID3D12CommandQueue *            cmd_queue;
    ID3D12RootSignature *           root_signature;
    ID3D12PipelineState *           pso;
    ID3D12GraphicsCommandList *     direct_cmd_list;
    ID3D12GraphicsCommandList *     bundle;
    UINT                            rtv_descriptor_size;
    UINT                            srv_cbv_descriptor_size;

    ID3D12DescriptorHeap *          rtv_heap;
    // NOTE(omid): Instead of separate descriptor heap use one for both srv and cbv 
    ID3D12DescriptorHeap *          srv_cbv_heap;
    //ID3D12DescriptorHeap *          srv_heap;
    //ID3D12DescriptorHeap *          cbv_heap;

    // App resources
    ID3D12Resource *                texture;
    ID3D12Resource *                vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW        vb_view;
    ID3D12Resource *                constant_buffer;
    SceneConstantBuffer             constant_buffer_data;
    uint8_t *                       cbv_data_begin_ptr;

    // Synchronization stuff
    UINT                            frame_index;
    HANDLE                          fence_event;
    ID3D12Fence *                   fence;
    UINT64                          fence_value [FRAME_COUNT];

};
struct ColorVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT4 color;
};
struct TextuVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT2 uv;
};
static HRESULT
move_to_next_frame (D3DRenderContext * render_ctx) {
    HRESULT ret = E_FAIL;

    // -- 1. schedule a signal command in the queue
    UINT64 const current_fence_value = render_ctx->fence_value[render_ctx->frame_index];
    ret = render_ctx->cmd_queue->Signal(render_ctx->fence, current_fence_value);
    CHECK_AND_FAIL(ret);

    // -- 2. update frame index
    render_ctx->frame_index = render_ctx->swapchain3->GetCurrentBackBufferIndex();

    // -- 3. if the next frame is not ready to be rendered yet, wait until it is ready
    if (render_ctx->fence->GetCompletedValue() < render_ctx->fence_value[render_ctx->frame_index]) {
        ret = render_ctx->fence->SetEventOnCompletion(render_ctx->fence_value[render_ctx->frame_index], render_ctx->fence_event);
        CHECK_AND_FAIL(ret);
        WaitForSingleObjectEx(render_ctx->fence_event, INFINITE /*return only when the object is signaled*/, false);
    }

    // -- 3. set the fence value for the next frame
    render_ctx->fence_value[render_ctx->frame_index] = current_fence_value + 1;

    return ret;
}
static HRESULT
wait_for_gpu (D3DRenderContext * render_ctx) {
    HRESULT ret = E_FAIL;

    // -- 1. schedule a signal command in the queue
    ret = render_ctx->cmd_queue->Signal(render_ctx->fence, render_ctx->fence_value[render_ctx->frame_index]);
    CHECK_AND_FAIL(ret);

    // -- 2. wait until the fence has been processed
    ret = render_ctx->fence->SetEventOnCompletion(render_ctx->fence_value[render_ctx->frame_index], render_ctx->fence_event);
    CHECK_AND_FAIL(ret);
    WaitForSingleObjectEx(render_ctx->fence_event, INFINITE /*return only when the object is signaled*/, false);

    // -- 3. increment fence value for the current frame
    ++render_ctx->fence_value[render_ctx->frame_index];

    return ret;
}
static void
update_constant_buffer(D3DRenderContext * render_ctx) {
    const float translation_speed = 0.003f;
    const float offset_bounds = 1.3f;

    render_ctx->constant_buffer_data.offset.x -= translation_speed;
    if (render_ctx->constant_buffer_data.offset.x < -offset_bounds) {
        render_ctx->constant_buffer_data.offset.x = offset_bounds;
    }
    memcpy(render_ctx->cbv_data_begin_ptr, &render_ctx->constant_buffer_data, sizeof(render_ctx->constant_buffer_data));
}
static HRESULT
render_stuff (D3DRenderContext * render_ctx) {
    
    HRESULT ret = E_FAIL;

    // Populate command list
    
    // -- reset cmd_allocator and cmd_list
    
    // Command list allocators can only be reset when the associated 
    // command lists have finished execution on the GPU; apps should use 
    // fences to determine GPU execution progress.
    CHECK_AND_FAIL(render_ctx->cmd_allocator[render_ctx->frame_index]->Reset());
    
    // However, when ExecuteCommandList() is called on a particular command 
    // list, that command list can then be reset at any time and must be before 
    // re-recording.
    ret = render_ctx->direct_cmd_list->Reset(render_ctx->cmd_allocator[render_ctx->frame_index], render_ctx->pso);
    CHECK_AND_FAIL(ret);

    // -- set root_signature, viewport and scissor
    render_ctx->direct_cmd_list->SetGraphicsRootSignature(render_ctx->root_signature);
    render_ctx->direct_cmd_list->RSSetViewports(1, &render_ctx->viewport);
    render_ctx->direct_cmd_list->RSSetScissorRects(1, &render_ctx->scissor_rect);

    // -- set descriptor heaps and root descriptor table (index 0 for srv, and index 1 for cbv)
    ID3D12DescriptorHeap * heaps [] = {render_ctx->srv_cbv_heap};
    render_ctx->direct_cmd_list->SetDescriptorHeaps(ARRAY_COUNT(heaps), heaps);
    render_ctx->direct_cmd_list->SetGraphicsRootDescriptorTable(0, render_ctx->srv_cbv_heap->GetGPUDescriptorHandleForHeapStart());
    
    SIMPLE_ASSERT(render_ctx->srv_cbv_descriptor_size > 0);
    D3D12_GPU_DESCRIPTOR_HANDLE cbv_gpu_handle = {};
    cbv_gpu_handle.ptr = render_ctx->srv_cbv_heap->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)render_ctx->srv_cbv_descriptor_size;
    render_ctx->direct_cmd_list->SetGraphicsRootDescriptorTable(1, cbv_gpu_handle);

    // -- indicate that the backbuffer will be used as the render target
    D3D12_RESOURCE_BARRIER barrier1 = {};
    barrier1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier1.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier1.Transition.pResource = render_ctx->render_targets[render_ctx->frame_index];
    barrier1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier1.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier1.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    render_ctx->direct_cmd_list->ResourceBarrier(1, &barrier1);
    
    // -- get CPU descriptor handle that represents the start of the rtv heap
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = render_ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    // -- apply initial offset
    rtv_handle.ptr = SIZE_T(INT64(rtv_handle.ptr) + INT64(render_ctx->frame_index) * INT64(render_ctx->rtv_descriptor_size));
    render_ctx->direct_cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
    
    // -- record command(s)
    
    // NOTE(omid): We can't use any "clear" method with bundles, so we use the command-list itself to clear rtv
    float clear_colors [] = {0.5f, 0.4f, 0.2f, 1.0f};
    render_ctx->direct_cmd_list->ClearRenderTargetView(rtv_handle, clear_colors, 0, nullptr);

    // -- execute bundle commands
    render_ctx->direct_cmd_list->ExecuteBundle(render_ctx->bundle);

    // -- indicate that the backbuffer will now be used to present
    D3D12_RESOURCE_BARRIER barrier2 = {};
    barrier2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier2.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier2.Transition.pResource = render_ctx->render_targets[render_ctx->frame_index];
    barrier2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier2.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier2.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    render_ctx->direct_cmd_list->ResourceBarrier(1 , &barrier2);

    // -- finish populating command list
    render_ctx->direct_cmd_list->Close();

    ID3D12CommandList * cmd_lists [] = {render_ctx->direct_cmd_list};
    render_ctx->cmd_queue->ExecuteCommandLists(ARRAY_COUNT(cmd_lists), cmd_lists);

    render_ctx->swapchain->Present(1 /*sync interval*/, 0 /*present flag*/);

    return ret;
}
static void
create_triangle_vertices (float aspect_ratio, TextuVertex out_vertices []) {
    TextuVertex vtx1 = {};
    vtx1.position.x = 0.0f;
    vtx1.position.y = 0.25f * aspect_ratio;
    vtx1.position.z = 0.0f;
    vtx1.uv.x       = 0.5f;
    vtx1.uv.y       = 0.0f;

    TextuVertex vtx2 = {};
    vtx2.position.x = 0.25f;
    vtx2.position.y = -0.25f * aspect_ratio;
    vtx2.position.z = 0.0f;
    vtx2.uv.x       = 1.0f;
    vtx2.uv.y       = 1.0f;

    TextuVertex vtx3 = {};
    vtx3.position.x = -0.25f;
    vtx3.position.y = -0.25f * aspect_ratio;
    vtx3.position.z = 0.0f;
    vtx3.uv.x       = 0.0f;
    vtx3.uv.y       = 1.0f;

    out_vertices[0] = vtx1;
    out_vertices[1] = vtx2;
    out_vertices[2] = vtx3;
}
static void
create_quad_vertices (float aspect_ratio, TextuVertex out_vertices []) {
    TextuVertex vtx1 = {};
    vtx1.position.x = -0.3f;
    vtx1.position.y = 0.3f * aspect_ratio;
    vtx1.position.z = 0.0f;
    vtx1.uv.x       = 0.0f;
    vtx1.uv.y       = 0.5f;

    TextuVertex vtx2 = {};
    vtx2.position.x = 0.3f;
    vtx2.position.y = 0.3f * aspect_ratio;
    vtx2.position.z = 0.0f;
    vtx2.uv.x       = 0.5f;
    vtx2.uv.y       = 0.5f;

    TextuVertex vtx3 = {};
    vtx3.position.x = -0.3f;
    vtx3.position.y = -0.3f * aspect_ratio;
    vtx3.position.z = 0.0f;
    vtx3.uv.x       = 0.0f;
    vtx3.uv.y       = 0.0f;

    TextuVertex vtx4 = {};
    vtx4.position.x = 0.3f;
    vtx4.position.y = -0.3f * aspect_ratio;
    vtx4.position.z = 0.0f;
    vtx4.uv.x       = 0.5f;
    vtx4.uv.y       = 0.0f;

    out_vertices[0] = vtx1;
    out_vertices[1] = vtx2;
    out_vertices[2] = vtx3;
    out_vertices[3] = vtx4;
}
static bool
generate_checkerboard_pattern (
    uint32_t texture_size, uint32_t bytes_per_pixel,
    uint32_t row_pitch, uint32_t cell_width,
    uint32_t cell_height, uint8_t * texture_ptr
) {
    bool ret = false;
    if (texture_ptr) {
        for (uint32_t i = 0; i < texture_size; i += bytes_per_pixel) {
            
            uint32_t x = i % row_pitch;         // row index
            uint32_t y = i / row_pitch;         // column index

            // -- cell indices (xx, yy)
            uint32_t xx = x / cell_width;
            uint32_t yy = y / cell_height;

            // -- color cell
            if (xx % 2 == yy % 2) {
                // white
                texture_ptr[i] = 0xaa;          // R
                texture_ptr[i + 1] = 0xbb;      // G
                texture_ptr[i + 2] = 0xff;      // B
                texture_ptr[i + 3] = 0xff;      // A

            } else {
                // black
                texture_ptr[i] = 0x04;          // R
                texture_ptr[i + 1] = 0x04;      // G
                texture_ptr[i + 2] = 0x04;      // B
                texture_ptr[i + 3] = 0xff;      // A
            }
        }
        ret = true;
    }
    return ret;
}
static LRESULT CALLBACK
main_win_cb (HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    LRESULT ret = {};
    switch (uMsg) {
        /* WM_PAIN is not handled for now ... 
        case WM_PAINT: {
            
        } break;
        */
        case WM_CLOSE: {
            global_running = false;
            DestroyWindow(hwnd);
            ret = 0;
        } break;
        default: {
            ret = DefWindowProcA(hwnd, uMsg, wParam, lParam);
        } break;
    }
    return ret;
}
static void
copy_texture_data_to_texture_resource (
    D3DRenderContext * render_ctx,                      // destination resource
    ID3D12Resource * texture_upload_heap,               // intermediate resource
    D3D12_SUBRESOURCE_DATA * texture_data               // source data (data to copy)
) {
    UINT first_subresource = 0;
    UINT num_subresources = 1;
    UINT64 intermediate_offset = 0;
    auto textu_desc = render_ctx->texture->GetDesc();
    auto uheap_desc = texture_upload_heap->GetDesc();

    UINT64 mem_to_alloc = static_cast<UINT64>(sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) + sizeof(UINT) + sizeof(UINT64)) * num_subresources;
    void * mem_ptr = HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(mem_to_alloc));
    auto * layouts = static_cast<D3D12_PLACED_SUBRESOURCE_FOOTPRINT *>(mem_ptr);
    UINT64 * p_row_sizes_in_bytes = reinterpret_cast<UINT64 *>(layouts + num_subresources);
    UINT * p_num_rows = reinterpret_cast<UINT *>(p_row_sizes_in_bytes + num_subresources);

    UINT64 required_size = 0;
    render_ctx->device->GetCopyableFootprints(
        &textu_desc, first_subresource, num_subresources, intermediate_offset, layouts, p_num_rows, p_row_sizes_in_bytes,
        &required_size
    );
    BYTE * p_data = nullptr;
    texture_upload_heap->Map(0, nullptr, reinterpret_cast<void **>(&p_data));
    for (UINT i = 0; i < num_subresources; ++i) {
        D3D12_MEMCPY_DEST dst_data = {};
        dst_data.pData = p_data + layouts[i].Offset;
        dst_data.RowPitch = layouts[i].Footprint.RowPitch;
        dst_data.SlicePitch = SIZE_T(p_num_rows[i]) * SIZE_T(layouts[i].Footprint.RowPitch);

        for (UINT z = 0; z < layouts[i].Footprint.Depth; ++z) {
            BYTE * dst_slice = (BYTE *)dst_data.pData + dst_data.SlicePitch * z;
            BYTE * src_slice = (BYTE *)texture_data->pData + texture_data->SlicePitch * LONG_PTR(z);
            for (UINT y = 0; y < p_num_rows[i]; ++y) {
                auto size_to_copy = (SIZE_T)(p_row_sizes_in_bytes[i]);
                ::memcpy(
                    dst_slice + dst_data.RowPitch * y,
                    src_slice + texture_data->RowPitch * LONG_PTR(y),
                    size_to_copy
                );
            }
        }

    }
    texture_upload_heap->Unmap(0, nullptr);

    // currently this function doesn't work with buffer resources
    SIMPLE_ASSERT(textu_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER);

    /*if (textu_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        render_ctx->direct_cmd_list->CopyBufferRegion(render_ctx->texture, 0, texture_upload_heap, layouts[0].Offset, layouts[0].Footprint.Width);
    } else {*/
        for (UINT i = 0; i < num_subresources; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = render_ctx->texture;
            dst.SubresourceIndex = first_subresource + i;
            dst.PlacedFootprint = {};
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = texture_upload_heap;
            src.SubresourceIndex = 0;
            src.PlacedFootprint = layouts[i];
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

            render_ctx->direct_cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
    /*}*/
    HeapFree(GetProcessHeap(), 0, mem_ptr);
}
INT WINAPI
WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, INT) {
    // ========================================================================================================
#pragma region Windows_Setup
    WNDCLASSA wc = {};
    wc.style = CS_HREDRAW|CS_VREDRAW|CS_OWNDC;
    wc.lpfnWndProc   = main_win_cb;
    wc.hInstance     = hInstance;
    wc.lpszClassName = "d3d12_win32";

    SIMPLE_ASSERT(RegisterClassA(&wc));

    HWND hwnd = CreateWindowExA(
        0,                                      // Optional window styles.
        wc.lpszClassName,                       // Window class
        "3D box app",           // Window title
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,       // Window style
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, // Size and position settings
        0 /* Parent window */,  0 /* Menu */, hInstance /* Instance handle */, 0 /* Additional application data */
    );
    SIMPLE_ASSERT(hwnd);
#pragma endregion Windows_Setup

    // ========================================================================================================
#pragma region Enable_Debug_Layer
    UINT dxgiFactoryFlags = 0;
    #if ENABLE_DEBUG_LAYER > 0
        ID3D12Debug * debug_interface_dx = nullptr;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_interface_dx)))) {
            debug_interface_dx->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    #endif
#pragma endregion Enable_Debug_Layer

    // ========================================================================================================
#pragma region Initialization
    D3DRenderContext render_ctx = {.width = 1280, .height = 720};
    render_ctx.aspect_ratio = (float)render_ctx.width / (float)render_ctx.height;
    render_ctx.viewport.TopLeftX = 0;
    render_ctx.viewport.TopLeftY = 0;
    render_ctx.viewport.Width = (float)render_ctx.width;
    render_ctx.viewport.Height = (float)render_ctx.height;
    render_ctx.scissor_rect.left = 0;
    render_ctx.scissor_rect.top = 0;
    render_ctx.scissor_rect.right = render_ctx.width;
    render_ctx.scissor_rect.bottom = render_ctx.height;
    // Query Adapter (PhysicalDevice)
    IDXGIFactory * dxgi_factory = nullptr;
    CHECK_AND_FAIL(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&dxgi_factory)));

    constexpr uint32_t MaxAdapters = 8;
    IDXGIAdapter * adapters[MaxAdapters] = {};
    IDXGIAdapter * pAdapter;
    for (UINT i = 0; dxgi_factory->EnumAdapters(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        adapters[i] = pAdapter;
        DXGI_ADAPTER_DESC adapter_desc = {};
        ::printf("GPU Info [%d] :\n", i);
        if(SUCCEEDED(pAdapter->GetDesc(&adapter_desc))) {
            ::printf("\tDescription: %ls\n", adapter_desc.Description);
            ::printf("\tDedicatedVideoMemory: %zu\n", adapter_desc.DedicatedVideoMemory);
        }
    } // WARP -> Windows Advanced Rasterization ...

    // Create Logical Device
    auto res = D3D12CreateDevice(adapters[0], D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&render_ctx.device));
    CHECK_AND_FAIL(res);

    // Release adaptors
    for (unsigned i = 0; i < MaxAdapters; ++i) {
        if (adapters[i] != nullptr) {
            adapters[i]->Release();
        }
    }

    // Create Command Queues
    D3D12_COMMAND_QUEUE_DESC cmd_q_desc = {};
    cmd_q_desc.Type = D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT;
    cmd_q_desc.Flags = D3D12_COMMAND_QUEUE_FLAGS::D3D12_COMMAND_QUEUE_FLAG_NONE;
    res = render_ctx.device->CreateCommandQueue(&cmd_q_desc, IID_PPV_ARGS(&render_ctx.cmd_queue));
    CHECK_AND_FAIL(res);

    DXGI_MODE_DESC backbuffer_desc = {};
    backbuffer_desc.Width = render_ctx.width;
    backbuffer_desc.Height = render_ctx.height;
    backbuffer_desc.Format = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM;

    DXGI_SAMPLE_DESC sampler_desc = {};
    sampler_desc.Count = 1;
    sampler_desc.Quality = 0;

    // Create Swapchain
    DXGI_SWAP_CHAIN_DESC swapchain_desc = {};
    swapchain_desc.BufferDesc = backbuffer_desc;
    swapchain_desc.SampleDesc = sampler_desc;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = FRAME_COUNT;
    swapchain_desc.OutputWindow = hwnd;
    swapchain_desc.Windowed = TRUE;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchain_desc.Flags = DXGI_SWAP_CHAIN_FLAG::DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    res = dxgi_factory->CreateSwapChain(render_ctx.cmd_queue, &swapchain_desc, &render_ctx.swapchain);
    CHECK_AND_FAIL(res);

    // -- to get current backbuffer index
    CHECK_AND_FAIL(render_ctx.swapchain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&render_ctx.swapchain3));
    render_ctx.frame_index = render_ctx.swapchain3->GetCurrentBackBufferIndex();

    // ========================================================================================================
#pragma region Descriptors
    // -- create descriptor heaps

    // Create Render Target View Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK_AND_FAIL(render_ctx.device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&render_ctx.rtv_heap)));
    
    // NOTE(omid): We create a single descriptor heap for both SRV and CBV 
    // -- A shader resource view (SRV) for the texture  (index 0 of srv_cbv_heap) 
    // -- A constant buffer view (CBV) for animation    (index 1 of srv_cbv_heap) 

    // Create srv_cbv_heap for SRV and CBV
    // Flags indicate that this descriptor heap can be bound to the pipeline
    // and that descriptors contained in it can be referenced by a root table
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.NumDescriptors = 2;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK_AND_FAIL(render_ctx.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&render_ctx.srv_cbv_heap)));

#pragma endregion Descriptors

        // -- create frame resources: a rtv and a cmd-allocator for each frame
    render_ctx.rtv_descriptor_size = render_ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle_start = render_ctx.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FRAME_COUNT; ++i) {
        CHECK_AND_FAIL(render_ctx.swapchain3->GetBuffer(i, IID_PPV_ARGS(&render_ctx.render_targets[i])));
        
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = {};
        cpu_handle.ptr = rtv_handle_start.ptr + ((UINT64)i * render_ctx.rtv_descriptor_size);
        // -- create a rtv for each frame
        render_ctx.device->CreateRenderTargetView(render_ctx.render_targets[i], nullptr, cpu_handle);
        // -- create a cmd-allocator for each frame
        res = render_ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&render_ctx.cmd_allocator[i]));

    }

    // Create bundle allocator
    res = render_ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&render_ctx.bundle_allocator));

    CHECK_AND_FAIL(res);

    // ========================================================================================================
#pragma region Root Signature
    // Create root signature
    D3D12_FEATURE_DATA_ROOT_SIGNATURE feature_data = {};
    feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(render_ctx.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &feature_data, 1))) {
        feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        ::printf("root signature version 1_1 is not supported, switched to 1_0!");
    }

    D3D12_DESCRIPTOR_RANGE1 ranges [2] = {};

    // -- define a range of srv descriptor(s)
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // -- define a range of cbv descriptor(s)
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // NOTE(omid): descriptor tables are ranges in a descriptor heap

    D3D12_ROOT_PARAMETER1 root_paramters [2] = {};

    // -- srv parameter space (s0)
    root_paramters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_paramters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_paramters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    root_paramters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // -- cbv parameter space (b0)
    root_paramters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_paramters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_paramters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    root_paramters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 0;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_FLAGS root_signature_flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT    |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS          |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS        |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
        //D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

    D3D12_ROOT_SIGNATURE_DESC1 root_desc1 = {};
    root_desc1.NumParameters = ARRAY_COUNT(root_paramters);
    root_desc1.pParameters = &root_paramters[0];
    root_desc1.NumStaticSamplers = 1;
    root_desc1.pStaticSamplers = &sampler;
    root_desc1.Flags = root_signature_flags;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_signature_desc = {};
    root_signature_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_signature_desc.Desc_1_1 = root_desc1;

    ID3DBlob * signature = nullptr;
    ID3DBlob * signature_error_blob = nullptr;

    CHECK_AND_FAIL(D3D12SerializeVersionedRootSignature(&root_signature_desc, &signature, &signature_error_blob));

    CHECK_AND_FAIL(render_ctx.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&render_ctx.root_signature)));
    
#pragma endregion Root Signature

    // Load and compile shaders
    
#if defined(_DEBUG)
    UINT compiler_flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    UINT compiler_flags = 0;
#endif

    wchar_t const * shaders_path = L"./shaders/cbuffer_shader.hlsl";
    ID3DBlob * vertex_shader = nullptr;
    ID3DBlob * vs_err = nullptr;
    ID3DBlob * pixel_shader = nullptr;
    ID3DBlob * ps_err = nullptr;
    res = D3DCompileFromFile(shaders_path, nullptr, nullptr, "VertexShader_Main", "vs_5_0", compiler_flags, 0, &vertex_shader, &vs_err);
    if (FAILED(res)) {
        if (vs_err) {
            OutputDebugStringA((char *)vs_err->GetBufferPointer());
            vs_err->Release();
        } else {
            ::printf("could not load/compile shader\n");
        }
    }
    res = D3DCompileFromFile(shaders_path, nullptr, nullptr, "PixelShader_Main", "ps_5_0", compiler_flags, 0, &pixel_shader, &ps_err);
    if (FAILED(res)) {
        if (ps_err) {
            OutputDebugStringA((char *)ps_err->GetBufferPointer());
            ps_err->Release();
        }
    }
    SIMPLE_ASSERT(vertex_shader);
    SIMPLE_ASSERT(pixel_shader);

    // Create vertex-input-layout Elements
    D3D12_INPUT_ELEMENT_DESC input_desc [2];
    input_desc[0] = {};
    input_desc[0].SemanticName = "POSITION";
    input_desc[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    input_desc[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    
    input_desc[1] = {};
    input_desc[1].SemanticName = "TEXCOORD";
    input_desc[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    input_desc[1].AlignedByteOffset = 12; // bc of the position?
    input_desc[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    // Create pipeline state object

    D3D12_BLEND_DESC def_blend_desc = {};
    def_blend_desc.AlphaToCoverageEnable                    = FALSE;
    def_blend_desc.IndependentBlendEnable                   = FALSE;
    def_blend_desc.RenderTarget[0].BlendEnable              = FALSE;
    def_blend_desc.RenderTarget[0].LogicOpEnable            = FALSE;
    def_blend_desc.RenderTarget[0].SrcBlend                 = D3D12_BLEND_ONE;
    def_blend_desc.RenderTarget[0].DestBlend                = D3D12_BLEND_ZERO;
    def_blend_desc.RenderTarget[0].BlendOp                  = D3D12_BLEND_OP_ADD;
    def_blend_desc.RenderTarget[0].SrcBlendAlpha            = D3D12_BLEND_ONE;
    def_blend_desc.RenderTarget[0].DestBlendAlpha           = D3D12_BLEND_ZERO;
    def_blend_desc.RenderTarget[0].BlendOpAlpha             = D3D12_BLEND_OP_ADD;
    def_blend_desc.RenderTarget[0].LogicOp                  = D3D12_LOGIC_OP_NOOP;
    def_blend_desc.RenderTarget[0].RenderTargetWriteMask    = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC def_rasterizer_desc = {};
    def_rasterizer_desc.FillMode = D3D12_FILL_MODE_SOLID;
    def_rasterizer_desc.CullMode = D3D12_CULL_MODE_BACK;
    def_rasterizer_desc.FrontCounterClockwise = false;
    def_rasterizer_desc.DepthBias = 0;
    def_rasterizer_desc.DepthBiasClamp = 0.0f;
    def_rasterizer_desc.SlopeScaledDepthBias = 0.0f;
    def_rasterizer_desc.DepthClipEnable = TRUE;
    def_rasterizer_desc.ForcedSampleCount = 0;
    def_rasterizer_desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = render_ctx.root_signature;
    pso_desc.VS.pShaderBytecode = vertex_shader->GetBufferPointer();
    pso_desc.VS.BytecodeLength = vertex_shader->GetBufferSize();
    pso_desc.PS.pShaderBytecode = pixel_shader->GetBufferPointer();
    pso_desc.PS.BytecodeLength = pixel_shader->GetBufferSize();
    pso_desc.BlendState = def_blend_desc;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState = def_rasterizer_desc;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.InputLayout.pInputElementDescs = input_desc;
    pso_desc.InputLayout.NumElements = ARRAY_COUNT(input_desc);
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleDesc.Quality = 0;

    CHECK_AND_FAIL(render_ctx.device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&render_ctx.pso)));

    // Create command list
    CHECK_AND_FAIL(render_ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, render_ctx.cmd_allocator[render_ctx.frame_index], render_ctx.pso, IID_PPV_ARGS(&render_ctx.direct_cmd_list)));

    // vertex data
    /*TextuVertex vertices [3] = {};
    create_triangle_vertices(render_ctx.aspect_ratio, vertices);*/
    TextuVertex vertices [4] = {};
    create_quad_vertices(render_ctx.aspect_ratio, vertices);
    size_t vb_size = sizeof(vertices);

    D3D12_HEAP_PROPERTIES vb_heap_props = {};
    vb_heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    vb_heap_props.CreationNodeMask = 1U;
    vb_heap_props.VisibleNodeMask = 1U;

    D3D12_RESOURCE_DESC vb_desc = {};
    vb_desc.Dimension = D3D12_RESOURCE_DIMENSION::D3D12_RESOURCE_DIMENSION_BUFFER;
    vb_desc.Alignment = 0;
    vb_desc.Width = vb_size;
    vb_desc.Height = 1;
    vb_desc.DepthOrArraySize = 1;
    vb_desc.MipLevels = 1;
    vb_desc.Format = DXGI_FORMAT_UNKNOWN;
    vb_desc.SampleDesc.Count = 1;
    vb_desc.SampleDesc.Quality = 0;
    vb_desc.Layout = D3D12_TEXTURE_LAYOUT::D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    vb_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CHECK_AND_FAIL(render_ctx.device->CreateCommittedResource(
        &vb_heap_props, D3D12_HEAP_FLAG_NONE, &vb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&render_ctx.vertex_buffer)
    ));

    // Copy vertex data to vertex buffer
    uint8_t * vertex_data = nullptr;
    D3D12_RANGE vb_mem_range = {};
    vb_mem_range.Begin = vb_mem_range.End = 0; // We do not intend to read from this resource on the CPU.
    render_ctx.vertex_buffer->Map(0, &vb_mem_range, reinterpret_cast<void **>(&vertex_data));
    memcpy(vertex_data, vertices, vb_size);
    render_ctx.vertex_buffer->Unmap(0, nullptr /*aka full-range*/);

    // Initialize the vertex buffer view (vbv)
    render_ctx.vb_view.BufferLocation = render_ctx.vertex_buffer->GetGPUVirtualAddress();
    render_ctx.vb_view.StrideInBytes = sizeof(*vertices);
    render_ctx.vb_view.SizeInBytes = (UINT)vb_size;

#pragma region Create Texture
    // Note: This pointer is a CPU object but this resource needs to stay in scope until
    // the command list that references it has finished executing on the GPU.
    // We will flush the GPU at the end of this method to ensure the resource is not
    // prematurely destroyed.
    ID3D12Resource * texture_upload_heap = nullptr;

    // -- creating texture

    D3D12_HEAP_PROPERTIES textu_heap_props = {};
    textu_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    textu_heap_props.CreationNodeMask = 1U;
    textu_heap_props.VisibleNodeMask = 1U;

    // -- describe and create a 2D texture
    D3D12_RESOURCE_DESC texture_desc = {};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = 256;
    texture_desc.Height = 256;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.SampleDesc.Quality = 0;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CHECK_AND_FAIL(render_ctx.device->CreateCommittedResource(
        &textu_heap_props, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&render_ctx.texture)
    ));

    UINT64 upload_buffer_size = 0;
    UINT first_subresource = 0;
    UINT num_subresources = 1;
    auto t_desc = render_ctx.texture->GetDesc();
    render_ctx.device->GetCopyableFootprints(
        &t_desc,
        first_subresource, num_subresources,
        0, nullptr, nullptr, nullptr,
        &upload_buffer_size
    );

    // -- create the gpu upload buffer
    D3D12_HEAP_PROPERTIES ubuffer_heap_props = {};
    ubuffer_heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    ubuffer_heap_props.CreationNodeMask = 1U;
    ubuffer_heap_props.VisibleNodeMask = 1U;

    D3D12_RESOURCE_DESC gpu_ubuffer_desc = {};
    gpu_ubuffer_desc.Dimension = D3D12_RESOURCE_DIMENSION::D3D12_RESOURCE_DIMENSION_BUFFER;
    gpu_ubuffer_desc.Alignment = 0;
    gpu_ubuffer_desc.Width = upload_buffer_size;
    gpu_ubuffer_desc.Height = 1;
    gpu_ubuffer_desc.DepthOrArraySize = 1;
    gpu_ubuffer_desc.MipLevels = 1;
    gpu_ubuffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    gpu_ubuffer_desc.SampleDesc.Count = 1;
    gpu_ubuffer_desc.SampleDesc.Quality = 0;
    gpu_ubuffer_desc.Layout = D3D12_TEXTURE_LAYOUT::D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    gpu_ubuffer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CHECK_AND_FAIL(render_ctx.device->CreateCommittedResource(
        &ubuffer_heap_props, D3D12_HEAP_FLAG_NONE, &gpu_ubuffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&texture_upload_heap))
    );

    // -- generate texture data
    uint32_t texture_width      = 256;
    uint32_t texture_height     = 256;
    uint32_t bytes_per_pixel    = 4;
    uint32_t row_pitch          = texture_width * bytes_per_pixel;
    uint32_t cell_width         = (texture_width >> 3) * bytes_per_pixel;           // actual "cell_width" muliplied by "bytes_per_pixel"
    uint32_t cell_height        = (texture_height >> 3);
    uint32_t texture_size       = texture_width * texture_height * bytes_per_pixel;
    // TODO(omid): perhaps create texture on stack?
    uint8_t * texture_ptr = reinterpret_cast<uint8_t *>(::malloc(texture_size));
    // -- create a simple yellow and black checkerboard pattern
    generate_checkerboard_pattern(texture_size, bytes_per_pixel, row_pitch, cell_width, cell_height, texture_ptr);

    // Copy texture data to the intermediate upload heap and
    // then schedule a copy from the upload heap to the 2D texture
    D3D12_SUBRESOURCE_DATA texture_data = {};
    texture_data.pData = texture_ptr;
    texture_data.RowPitch = row_pitch;
    texture_data.SlicePitch = texture_data.RowPitch * texture_height;
    copy_texture_data_to_texture_resource(&render_ctx, texture_upload_heap, &texture_data);

#pragma endregion Create Texture

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.pResource = render_ctx.texture;
    render_ctx.direct_cmd_list->ResourceBarrier(1, &barrier);

    // -- describe and create a SRV for the texture
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = texture_desc.Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    render_ctx.device->CreateShaderResourceView(render_ctx.texture, &srv_desc, render_ctx.srv_cbv_heap->GetCPUDescriptorHandleForHeapStart());

    // -- close the command list and execute it to begin inital gpu setup
    CHECK_AND_FAIL(render_ctx.direct_cmd_list->Close());
    ID3D12CommandList * cmd_lists [] = {render_ctx.direct_cmd_list};
    render_ctx.cmd_queue->ExecuteCommandLists(ARRAY_COUNT(cmd_lists), cmd_lists);

#pragma region Create Constant Buffer
    const UINT cb_size = sizeof(SceneConstantBuffer);    // CB size is required to be 256-byte aligned.

    D3D12_HEAP_PROPERTIES cbuffer_heap_props = {};
    cbuffer_heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    cbuffer_heap_props.CreationNodeMask = 1U;
    cbuffer_heap_props.VisibleNodeMask = 1U;

        D3D12_RESOURCE_DESC cb_desc = {};
    cb_desc.Dimension = D3D12_RESOURCE_DIMENSION::D3D12_RESOURCE_DIMENSION_BUFFER;
    cb_desc.Alignment = 0;
    cb_desc.Width = cb_size;
    cb_desc.Height = 1;
    cb_desc.DepthOrArraySize = 1;
    cb_desc.MipLevels = 1;
    cb_desc.Format = DXGI_FORMAT_UNKNOWN;
    cb_desc.SampleDesc.Count = 1;
    cb_desc.SampleDesc.Quality = 0;
    cb_desc.Layout = D3D12_TEXTURE_LAYOUT::D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cb_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CHECK_AND_FAIL(render_ctx.device->CreateCommittedResource(
        &cbuffer_heap_props,
        D3D12_HEAP_FLAG_NONE,
        &cb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&render_ctx.constant_buffer)));


    render_ctx.srv_cbv_descriptor_size = render_ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cbv_cpu_handle = {};
    cbv_cpu_handle.ptr = render_ctx.srv_cbv_heap->GetCPUDescriptorHandleForHeapStart().ptr + (UINT64)render_ctx.srv_cbv_descriptor_size;

    // Describe and create a constant buffer view.
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
    cbv_desc.BufferLocation = render_ctx.constant_buffer->GetGPUVirtualAddress();
    cbv_desc.SizeInBytes = cb_size;
    render_ctx.device->CreateConstantBufferView(&cbv_desc, cbv_cpu_handle);

    // Map and initialize the constant buffer. We don't unmap this until the
    // app closes. Keeping things mapped for the lifetime of the resource is okay.
    D3D12_RANGE cb_mem_range = {};
    cb_mem_range.Begin = cb_mem_range.End = 0; // We do not intend to read from this resource on the CPU.
    CHECK_AND_FAIL(render_ctx.constant_buffer->Map(0, &cb_mem_range, reinterpret_cast<void**>(&render_ctx.cbv_data_begin_ptr)));
    ::memcpy(render_ctx.cbv_data_begin_ptr, &render_ctx.constant_buffer_data, sizeof(render_ctx.constant_buffer_data));

#pragma endregion Create Constant Buffer


#pragma region Bundle
    // -- create and record the bundle
    CHECK_AND_FAIL(render_ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, render_ctx.bundle_allocator, render_ctx.pso, IID_PPV_ARGS(&render_ctx.bundle)));
    render_ctx.bundle->SetGraphicsRootSignature(render_ctx.root_signature);
    render_ctx.bundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    render_ctx.bundle->IASetVertexBuffers(0, 1, &render_ctx.vb_view);
    render_ctx.bundle->DrawInstanced(4, 1, 0, 0);
    CHECK_AND_FAIL(render_ctx.bundle->Close());
#pragma endregion Bundle

    //----------------
    // Create fence
    // create synchronization objects and wait until assets have been uploaded to the GPU.
    CHECK_AND_FAIL(render_ctx.device->CreateFence(render_ctx.fence_value[render_ctx.frame_index], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&render_ctx.fence)));

    ++render_ctx.fence_value[render_ctx.frame_index];

    // Create an event handle to use for frame synchronization.
    render_ctx.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if(nullptr == render_ctx.fence_event) {
        // map the error code to an HRESULT value.
        CHECK_AND_FAIL(HRESULT_FROM_WIN32(GetLastError()));
    }
        
    // Wait for the command list to execute; we are reusing the same command 
    // list in our main loop but for now, we just want to wait for setup to 
    // complete before continuing.
    CHECK_AND_FAIL(wait_for_gpu(&render_ctx));

#pragma endregion Initialization

    // ========================================================================================================
#pragma region Main_Loop
    global_running = true;
    while(global_running) {
        MSG msg = {};
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        // OnUpdate()
        update_constant_buffer(&render_ctx);

        // OnRender() aka rendering
        CHECK_AND_FAIL(render_stuff(&render_ctx));

        CHECK_AND_FAIL(move_to_next_frame(&render_ctx));
    }
#pragma endregion Main_Loop

    // ========================================================================================================
#pragma region Cleanup_And_Debug
    CHECK_AND_FAIL(wait_for_gpu(&render_ctx));

    CloseHandle(render_ctx.fence_event);

    render_ctx.fence->Release();

    render_ctx.bundle->Release();

    texture_upload_heap->Release();
    
    ::free(texture_ptr);

    render_ctx.constant_buffer->Release();
    render_ctx.texture->Release();
    render_ctx.vertex_buffer->Release();

    render_ctx.direct_cmd_list->Release();
    render_ctx.pso->Release();

    pixel_shader->Release();
    vertex_shader->Release();

    render_ctx.root_signature->Release();
    if (signature_error_blob)
        signature_error_blob->Release();
    signature->Release();

    render_ctx.bundle_allocator->Release();

    for (unsigned i = 0; i < FRAME_COUNT; ++i) {
        render_ctx.render_targets[i]->Release();
        render_ctx.cmd_allocator[i]->Release();
    }

    render_ctx.srv_cbv_heap->Release();
    render_ctx.rtv_heap->Release();

    render_ctx.swapchain3->Release();
    render_ctx.swapchain->Release();
    render_ctx.cmd_queue->Release();
    render_ctx.device->Release();
    dxgi_factory->Release();

    #if (ENABLE_DEBUG_LAYER > 0)
        debug_interface_dx->Release();
    #endif

    // -- advanced debugging and reporting live objects [from https://walbourn.github.io/dxgi-debug-device/]

    typedef HRESULT (WINAPI * LPDXGIGETDEBUGINTERFACE)(REFIID, void ** );

    //HMODULE dxgidebug_dll = LoadLibraryEx( L"dxgidebug_dll.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 );
    HMODULE dxgidebug_dll = LoadLibrary(L"DXGIDebug.dll");
    if (dxgidebug_dll) {
        auto dxgiGetDebugInterface = reinterpret_cast<LPDXGIGETDEBUGINTERFACE>(
            reinterpret_cast<void*>(GetProcAddress(dxgidebug_dll, "DXGIGetDebugInterface")));

        IDXGIDebug1 * dxgi_debugger = nullptr;
        DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgi_debugger));
        dxgi_debugger->ReportLiveObjects(
            DXGI_DEBUG_ALL,
            DXGI_DEBUG_RLO_DETAIL
            /* DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL) */
        );
        dxgi_debugger->Release();
        FreeLibrary(dxgidebug_dll);
    }
#pragma endregion Cleanup_And_Debug

    return 0;
}
