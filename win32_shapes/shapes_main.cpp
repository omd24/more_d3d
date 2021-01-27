#include <d3d12.h>

#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxgidebug.h>

#include <dxcapi.h>

#include "utils.h"

#if !defined(NDEBUG) && !defined(_DEBUG)
#error "Define at least one."
#elif defined(NDEBUG) && defined(_DEBUG)
#error "Define at most one."
#endif

#if defined(_DEBUG)
#define ENABLE_DEBUG_LAYER 1
#else
#define ENABLE_DEBUG_LAYER 0
#endif

// TODO(omid): find a better way to disable warnings!
#pragma warning (disable: 28182)    // pointer can be NULL.
#pragma warning (disable: 6011)     // dereferencing a potentially null pointer

// Currently we overload the meaning of FrameCount to mean both the maximum
// number of frames that will be queued to the GPU at a time, as well as the number
// of back buffers in the DXGI swap chain. For the majority of applications, this
// is convenient and works well. However, there will be certain cases where an
// application may want to queue up more frames than there are back buffers
// available.
// It should be noted that excessive buffering of frames dependent on user input
// may result in noticeable latency in your app.
#define FRAME_COUNT     3
// Need a FrameResource for each frame
#define NUM_FRAME_RESOURCES         FRAME_COUNT

bool global_running;
SceneContext global_scene_ctx;

struct D3DRenderContext {
    // Pipeline stuff
    D3D12_VIEWPORT                  viewport;
    D3D12_RECT                      scissor_rect;
    IDXGISwapChain3 *               swapchain3;
    IDXGISwapChain *                swapchain;
    ID3D12Device *                  device;
    //ID3D12Resource *                render_targets[FRAME_COUNT];
    //ID3D12CommandAllocator *        cmd_allocator[FRAME_COUNT];
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

    // App resources
    ID3D12Resource *                texture;
    ID3D12Resource *                vertex_buffer;
    ID3D12Resource *                index_buffer;
    D3D12_VERTEX_BUFFER_VIEW        vb_view;
    D3D12_INDEX_BUFFER_VIEW         ib_view;
    ID3D12Resource *                constant_buffer;
    ObjectConstantBuffer            constant_buffer_data;
    uint8_t *                       cbv_data_begin_ptr;

    // Synchronization stuff
    UINT                            frame_index;
    HANDLE                          fence_event;
    ID3D12Fence *                   fence;
    UINT64                          fence_value[FRAME_COUNT];

    FrameResource frame_resources[NUM_FRAME_RESOURCES];
    // TODO(omid): perhaps allocate FrameResources on heap? 
    // FrameResource * frame_resources[NUM_FRAME_RESOURCES];
    FrameResource * current_frame_resource;
    int current_frame_resource_index;

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

    using namespace DirectX;

    // Convert Spherical to Cartesian coordinates.
    float x = global_scene_ctx.radius * sinf(global_scene_ctx.phi) * cosf(global_scene_ctx.theta);
    float z = global_scene_ctx.radius * sinf(global_scene_ctx.phi) * sinf(global_scene_ctx.theta);
    float y = global_scene_ctx.radius * cosf(global_scene_ctx.phi);

    // Build the view matrix.
    XMVECTOR pos = XMVectorSet(x, y, z, 1.0f);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    global_scene_ctx.view = XMMatrixLookAtLH(pos, target, up);

    XMMATRIX world_view_proj = global_scene_ctx.world * global_scene_ctx.view * global_scene_ctx.proj;
    XMStoreFloat4x4(&render_ctx->constant_buffer_data.world_view_proj, XMMatrixTranspose(world_view_proj));

    memcpy(render_ctx->cbv_data_begin_ptr, &render_ctx->constant_buffer_data, sizeof(render_ctx->constant_buffer_data));
}
static D3D12_RESOURCE_BARRIER
create_barrier (ID3D12Resource * resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}
static HRESULT
render_stuff (D3DRenderContext * render_ctx) {

    HRESULT ret = E_FAIL;
    UINT frame_index = render_ctx->frame_index;

    // Populate command list

    // -- reset cmd_allocator and cmd_list

    // Command list allocators can only be reset when the associated 
    // command lists have finished execution on the GPU; apps should use 
    // fences to determine GPU execution progress.
    render_ctx->frame_resources[frame_index].cmd_list_alloc->Reset();

    // However, when ExecuteCommandList() is called on a particular command 
    // list, that command list can then be reset at any time and must be before 
    // re-recording.
    ret = render_ctx->direct_cmd_list->Reset(render_ctx->frame_resources[frame_index].cmd_list_alloc, render_ctx->pso);
    CHECK_AND_FAIL(ret);

    // -- set root_signature, viewport and scissor
    render_ctx->direct_cmd_list->SetGraphicsRootSignature(render_ctx->root_signature);
    render_ctx->direct_cmd_list->RSSetViewports(1, &render_ctx->viewport);
    render_ctx->direct_cmd_list->RSSetScissorRects(1, &render_ctx->scissor_rect);

    // -- set descriptor heaps and root descriptor table (index 0 for srv, and index 1 for cbv)
    ID3D12DescriptorHeap * heaps [] = {render_ctx->srv_cbv_heap};
    render_ctx->direct_cmd_list->SetDescriptorHeaps(ARRAY_COUNT(heaps), heaps);
    render_ctx->direct_cmd_list->SetGraphicsRootDescriptorTable(0, render_ctx->srv_cbv_heap->GetGPUDescriptorHandleForHeapStart());

    SIMPLE_ASSERT(render_ctx->srv_cbv_descriptor_size > 0, "invalid descriptor size");
    D3D12_GPU_DESCRIPTOR_HANDLE cbv_gpu_handle = {};
    cbv_gpu_handle.ptr = render_ctx->srv_cbv_heap->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)render_ctx->srv_cbv_descriptor_size;
    render_ctx->direct_cmd_list->SetGraphicsRootDescriptorTable(1, cbv_gpu_handle);

    // -- indicate that the backbuffer will be used as the render target
    D3D12_RESOURCE_BARRIER barrier1 = create_barrier(render_ctx->frame_resources[frame_index].render_target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    render_ctx->direct_cmd_list->ResourceBarrier(1, &barrier1);

    // -- get CPU descriptor handle that represents the start of the rtv heap
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = render_ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    // -- apply initial offset
    rtv_handle.ptr = SIZE_T(INT64(rtv_handle.ptr) + INT64(render_ctx->frame_index) * INT64(render_ctx->rtv_descriptor_size));
    render_ctx->direct_cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

    // -- record command(s)

    // NOTE(omid): We can't use any "clear" method with bundles, so we use the command-list itself to clear rtv
    float clear_colors [] = {0.2f, 0.3f, 0.5f, 1.0f};
    render_ctx->direct_cmd_list->ClearRenderTargetView(rtv_handle, clear_colors, 0, nullptr);

    // -- execute bundle commands
    render_ctx->direct_cmd_list->ExecuteBundle(render_ctx->bundle);

    // -- indicate that the backbuffer will now be used to present
    D3D12_RESOURCE_BARRIER barrier2 = create_barrier(render_ctx->frame_resources[frame_index].render_target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    render_ctx->direct_cmd_list->ResourceBarrier(1, &barrier2);

    // -- finish populating command list
    render_ctx->direct_cmd_list->Close();

    ID3D12CommandList * cmd_lists [] = {render_ctx->direct_cmd_list};
    render_ctx->cmd_queue->ExecuteCommandLists(ARRAY_COUNT(cmd_lists), cmd_lists);

    render_ctx->swapchain->Present(1 /*sync interval*/, 0 /*present flag*/);

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
    SIMPLE_ASSERT(textu_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER, "invalid texture");

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
static LRESULT CALLBACK
main_win_cb (HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    LRESULT ret = {};
    switch (uMsg) {
        /* WM_PAIN is not handled for now ...
        case WM_PAINT: {

        } break;
        */
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        global_scene_ctx.mouse.x = GET_X_LPARAM(lParam);
        global_scene_ctx.mouse.y = GET_Y_LPARAM(lParam);
        SetCapture(hwnd);
    } break;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
        ReleaseCapture();
    } break;
    case WM_MOUSEMOVE: {
        handle_mouse_move(&global_scene_ctx, wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
    } break;
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
INT WINAPI
WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ INT) {
    // ========================================================================================================
#pragma region Windows_Setup
    WNDCLASSA wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = main_win_cb;
    wc.hInstance = hInstance;
    wc.lpszClassName = "d3d12_win32";

    SIMPLE_ASSERT(RegisterClassA(&wc), "could not register window class");

    HWND hwnd = CreateWindowExA(
        0,                                      // Optional window styles.
        wc.lpszClassName,                       // Window class
        "3D shapes app",                        // Window title
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,       // Window style
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, // Size and position settings
        0 /* Parent window */, 0 /* Menu */, hInstance /* Instance handle */, 0 /* Additional application data */
    );
    SIMPLE_ASSERT(hwnd, "could not create window");
#pragma endregion Windows_Setup

    // ========================================================================================================
#pragma region Enable_Debug_Layer
    UINT dxgiFactoryFlags = 0;
#if ENABLE_DEBUG_LAYER > 0
    ID3D12Debug * debug_interface_dx = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_interface_dx)))) {
        debug_interface_dx->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
#pragma endregion Enable_Debug_Layer

    // ========================================================================================================
#pragma region Initialization
    global_scene_ctx = {.width = 1280, .height = 720};
    global_scene_ctx.theta = 1.5f * DirectX::XM_PI;
    global_scene_ctx.phi = DirectX::XM_PIDIV4;
    global_scene_ctx.radius = 5.0f;
    global_scene_ctx.aspect_ratio = (float)global_scene_ctx.width / (float)global_scene_ctx.height;
    global_scene_ctx.world = IdentityMat();
    global_scene_ctx.view = IdentityMat();
    global_scene_ctx.proj = DirectX::XMMatrixPerspectiveFovLH(0.25f * DirectX::XM_PI, global_scene_ctx.aspect_ratio, 1.0f, 1000.0f);;

    D3DRenderContext render_ctx = {};

    render_ctx.viewport.TopLeftX = 0;
    render_ctx.viewport.TopLeftY = 0;
    render_ctx.viewport.Width = (float)global_scene_ctx.width;
    render_ctx.viewport.Height = (float)global_scene_ctx.height;
    render_ctx.viewport.MinDepth = 0.0f;
    render_ctx.viewport.MaxDepth = 1.0f;
    render_ctx.scissor_rect.left = 0;
    render_ctx.scissor_rect.top = 0;
    render_ctx.scissor_rect.right = global_scene_ctx.width;
    render_ctx.scissor_rect.bottom = global_scene_ctx.height;

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
        if (SUCCEEDED(pAdapter->GetDesc(&adapter_desc))) {
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
    backbuffer_desc.Width = global_scene_ctx.width;
    backbuffer_desc.Height = global_scene_ctx.height;
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
    // -- A shader resource view (SRV) for the texture      (index 0 of srv_cbv_heap) 
    // -- A constant buffer view (CBV) for word_view_proj   (index 1 of srv_cbv_heap) 

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
        CHECK_AND_FAIL(render_ctx.swapchain3->GetBuffer(i, IID_PPV_ARGS(&render_ctx.frame_resources[i].render_target)));

        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = {};
        cpu_handle.ptr = rtv_handle_start.ptr + ((UINT64)i * render_ctx.rtv_descriptor_size);
        // -- create a rtv for each frame
        render_ctx.device->CreateRenderTargetView(render_ctx.frame_resources[i].render_target, nullptr, cpu_handle);
        // -- create a cmd-allocator for each frame
        res = render_ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&render_ctx.frame_resources[i].cmd_list_alloc));

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

    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {};

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

    D3D12_ROOT_PARAMETER1 root_paramters[2] = {};

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
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
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

    ID3DBlob * signature_blob = nullptr;
    ID3DBlob * signature_error_blob = nullptr;

    CHECK_AND_FAIL(D3D12SerializeVersionedRootSignature(&root_signature_desc, &signature_blob, &signature_error_blob));

    CHECK_AND_FAIL(render_ctx.device->CreateRootSignature(0, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(), IID_PPV_ARGS(&render_ctx.root_signature)));

#pragma endregion Root Signature

    // Load and compile shaders

#if defined(_DEBUG)
    UINT compiler_flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    UINT compiler_flags = 0;
#endif

#pragma region Compile Shaders
// -- using DXC shader compiler [from https://asawicki.info/news_1719_two_shader_compilers_of_direct3d_12]

    IDxcLibrary * dxc_lib = nullptr;
    HRESULT hr = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&dxc_lib));
    // if (FAILED(hr)) Handle error
    IDxcCompiler * dxc_compiler = nullptr;
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc_compiler));
    // if (FAILED(hr)) Handle error

    wchar_t const * shaders_path = L"./shaders/shapes_shader.hlsl";
    uint32_t code_page = CP_UTF8;
    IDxcBlobEncoding * shader_blob = nullptr;
    IDxcOperationResult * dxc_res = nullptr;
    IDxcBlob * vertex_shader_code = nullptr;
    IDxcBlob * pixel_shader_code = nullptr;
    hr = dxc_lib->CreateBlobFromFile(shaders_path, &code_page, &shader_blob);
    if (shader_blob) {
        hr = dxc_compiler->Compile(shader_blob, shaders_path, L"VertexShader_Main", L"vs_6_0", nullptr, 0, nullptr, 0, nullptr, &dxc_res);
        dxc_res->GetStatus(&hr);
        dxc_res->GetResult(&vertex_shader_code);
        hr = dxc_compiler->Compile(shader_blob, shaders_path, L"PixelShader_Main", L"ps_6_0", nullptr, 0, nullptr, 0, nullptr, &dxc_res);
        dxc_res->GetStatus(&hr);
        dxc_res->GetResult(&pixel_shader_code);

        if (FAILED(hr)) {
            if (dxc_res) {
                IDxcBlobEncoding * errorsBlob = nullptr;
                hr = dxc_res->GetErrorBuffer(&errorsBlob);
                if (SUCCEEDED(hr) && errorsBlob) {
                    OutputDebugStringA((const char*)errorsBlob->GetBufferPointer());
                    return(0);
                }
            }
            // Handle compilation error...
        }
    }
    SIMPLE_ASSERT(vertex_shader_code, "invalid shader");
    SIMPLE_ASSERT(pixel_shader_code, "invalid shader");

#pragma endregion Compile Shaders

#pragma region PSO Creation
    // Create vertex-input-layout Elements
    D3D12_INPUT_ELEMENT_DESC input_desc[2];
    input_desc[0] = {};
    input_desc[0].SemanticName = "POSITION";
    input_desc[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    input_desc[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    input_desc[1] = {};
    input_desc[1].SemanticName = "TEXCOORD";
    input_desc[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    input_desc[1].AlignedByteOffset = 12; // bc of the position byte-size
    input_desc[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    // Create pipeline state object

    D3D12_BLEND_DESC def_blend_desc = {};
    def_blend_desc.AlphaToCoverageEnable = FALSE;
    def_blend_desc.IndependentBlendEnable = FALSE;
    def_blend_desc.RenderTarget[0].BlendEnable = FALSE;
    def_blend_desc.RenderTarget[0].LogicOpEnable = FALSE;
    def_blend_desc.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    def_blend_desc.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    def_blend_desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    def_blend_desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    def_blend_desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    def_blend_desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    def_blend_desc.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    def_blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

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
    pso_desc.VS.pShaderBytecode = vertex_shader_code->GetBufferPointer();
    pso_desc.VS.BytecodeLength = vertex_shader_code->GetBufferSize();
    pso_desc.PS.pShaderBytecode = pixel_shader_code->GetBufferPointer();
    pso_desc.PS.BytecodeLength = pixel_shader_code->GetBufferSize();
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
    ID3D12CommandAllocator * current_alloc = render_ctx.frame_resources[render_ctx.frame_index].cmd_list_alloc;
    if (current_alloc) {
        render_ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, current_alloc, render_ctx.pso, IID_PPV_ARGS(&render_ctx.direct_cmd_list));
    }
#pragma endregion PSO Creation

// TODO(omid): VB and IB should also use a default_heap (not upload heap), similar to the texture. 
#pragma region Create Vertex Buffer and Index Buffer
    // vertex data and indices
    TextuVertex vertices[8] = {};
    uint16_t indices[36] = {};
    create_box_vertices(vertices, indices);
    size_t vb_size = sizeof(vertices);
    size_t ib_size = sizeof(indices);

    // Create VB as an upload buffer
    uint8_t * vertex_data = nullptr;
    create_upload_buffer(render_ctx.device, vb_size, &vertex_data, &render_ctx.vertex_buffer);
    // Copy vertex data to vertex buffer
    memcpy(vertex_data, vertices, vb_size);

    // Initialize the vertex buffer view (vbv)
    render_ctx.vb_view.BufferLocation = render_ctx.vertex_buffer->GetGPUVirtualAddress();
    render_ctx.vb_view.StrideInBytes = sizeof(*vertices);
    render_ctx.vb_view.SizeInBytes = (UINT)vb_size;

    // --repeat for index buffer...

    // Create IB as an upload buffer
    uint8_t * indices_data = nullptr;
    create_upload_buffer(render_ctx.device, ib_size, &indices_data, &render_ctx.index_buffer);
    // Copy index data to index buffer
    memcpy(indices_data, indices, ib_size);

    // Initialize the ib buffer view (ibv)
    render_ctx.ib_view.BufferLocation = render_ctx.index_buffer->GetGPUVirtualAddress();
    render_ctx.ib_view.Format = DXGI_FORMAT_R16_UINT;
    render_ctx.ib_view.SizeInBytes = (UINT)ib_size;

#pragma endregion Create Vertex Buffer and Index Buffer
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
    uint32_t texture_width = 256;
    uint32_t texture_height = 256;
    uint32_t bytes_per_pixel = 4;
    uint32_t row_pitch = texture_width * bytes_per_pixel;
    uint32_t cell_width = (texture_width >> 3) * bytes_per_pixel;           // actual "cell_width" muliplied by "bytes_per_pixel"
    uint32_t cell_height = (texture_height >> 3);
    uint32_t texture_size = texture_width * texture_height * bytes_per_pixel;
    
    uint8_t * texture_ptr = nullptr;
    DYN_ARRAY_INIT(uint8_t, texture_ptr);
    DYN_ARRAY_EXPAND(uint8_t, texture_ptr, texture_size);
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
    // Create constant buffer as upload buffer
    const UINT cb_size = sizeof(ObjectConstantBuffer);    // CB size is required to be 256-byte aligned.
    create_upload_buffer(render_ctx.device, cb_size, &render_ctx.cbv_data_begin_ptr, &render_ctx.constant_buffer);

    // Describe and create a constant buffer view.
    render_ctx.srv_cbv_descriptor_size = render_ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cbv_cpu_handle = {};
    cbv_cpu_handle.ptr = render_ctx.srv_cbv_heap->GetCPUDescriptorHandleForHeapStart().ptr + (UINT64)render_ctx.srv_cbv_descriptor_size;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
    cbv_desc.BufferLocation = render_ctx.constant_buffer->GetGPUVirtualAddress();
    cbv_desc.SizeInBytes = cb_size;
    render_ctx.device->CreateConstantBufferView(&cbv_desc, cbv_cpu_handle);

    // Initialize cb data
    ::memcpy(render_ctx.cbv_data_begin_ptr, &render_ctx.constant_buffer_data, sizeof(render_ctx.constant_buffer_data));

#pragma endregion Create Constant Buffer


#pragma region Bundle
    // -- create and record the bundle
    CHECK_AND_FAIL(render_ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, render_ctx.bundle_allocator, render_ctx.pso, IID_PPV_ARGS(&render_ctx.bundle)));
    render_ctx.bundle->SetGraphicsRootSignature(render_ctx.root_signature);
    render_ctx.bundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    render_ctx.bundle->IASetVertexBuffers(0, 1, &render_ctx.vb_view);
    render_ctx.bundle->IASetIndexBuffer(&render_ctx.ib_view);
    render_ctx.bundle->DrawIndexedInstanced(ARRAY_COUNT(indices), 1, 0, 0, 0);
    CHECK_AND_FAIL(render_ctx.bundle->Close());
#pragma endregion Bundle

    //----------------
    // Create fence
    // create synchronization objects and wait until assets have been uploaded to the GPU.
    CHECK_AND_FAIL(render_ctx.device->CreateFence(render_ctx.fence_value[render_ctx.frame_index], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&render_ctx.fence)));

    ++render_ctx.fence_value[render_ctx.frame_index];

    // Create an event handle to use for frame synchronization.
    render_ctx.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (nullptr == render_ctx.fence_event) {
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
    while (global_running) {
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

    ::printf("length = %zu\n", DYN_ARRAY_LENGTH(texture_ptr));
    ::printf("capacity = %zu\n", DYN_ARRAY_CAPACITY(texture_ptr));
    DYN_ARRAY_DEINIT(texture_ptr);

    render_ctx.constant_buffer->Unmap(0, nullptr);
    render_ctx.constant_buffer->Release();

    render_ctx.texture->Release();
    
    render_ctx.index_buffer->Unmap(0, nullptr);
    render_ctx.index_buffer->Release();

    render_ctx.vertex_buffer->Unmap(0, nullptr);
    render_ctx.vertex_buffer->Release();

    render_ctx.direct_cmd_list->Release();
    render_ctx.pso->Release();

    pixel_shader_code->Release();
    vertex_shader_code->Release();

    render_ctx.root_signature->Release();
    if (signature_error_blob)
        signature_error_blob->Release();
    signature_blob->Release();

    render_ctx.bundle_allocator->Release();

    // release FrameResources
    for (unsigned i = 0; i < FRAME_COUNT; ++i) {
        render_ctx.frame_resources[i].render_target->Release();
        render_ctx.frame_resources[i].cmd_list_alloc->Release();
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

    typedef HRESULT (WINAPI * LPDXGIGETDEBUGINTERFACE)(REFIID, void **);

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

