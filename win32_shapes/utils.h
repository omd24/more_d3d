/* ===========================================================
   #File: utils.cpp #
   #Date: 25 Jan 2021 #
   #Revision: 1.0 #
   #Creator: Omid Miresmaeili #
   #Description: renderer utility tools #
   #Notice: (C) Copyright 2021 by Omid. All Rights Reserved. #
   =========================================================== */

#pragma once

#include "headers/common.h"
#include "headers/dynarray.h"

#define SUCCEEDED(hr)   (((HRESULT)(hr)) >= 0)
//#define SUCCEEDED_OPERATION(hr)   (((HRESULT)(hr)) == S_OK)
#define FAILED(hr)      (((HRESULT)(hr)) < 0)
//#define FAILED_OPERATION(hr)      (((HRESULT)(hr)) != S_OK)
#define CHECK_AND_FAIL(hr)                          \
    if (FAILED(hr)) {                               \
        ::printf("[ERROR] " #hr "() failed at line %d. \n", __LINE__);   \
        ::abort();                                  \
    }                                               \

#define ARRAY_COUNT(arr)                sizeof(arr)/sizeof(arr[0])
#define CLAMP_VALUE(val, lb, ub)        val < lb ? lb : (val > ub ? ub : val); 


using namespace DirectX;

static XMFLOAT4X4
Identity4x4() {
    static XMFLOAT4X4 I(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);

    return I;
}
static XMMATRIX
IdentityMat() {
    static XMMATRIX I(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);

    return I;
}

struct ObjectConstantBuffer {
    XMFLOAT4X4 world_view_proj;
    float padding[48];             // Padding so the constant buffer is 256-byte aligned
};
static_assert(256 == sizeof(ObjectConstantBuffer), "Constant buffer size must be 256b aligned");
struct PassConstantBuffer {
    XMFLOAT4X4 view;
    XMFLOAT4X4 inverse_view;
    XMFLOAT4X4 proj;
    XMFLOAT4X4 inverse_proj;
    XMFLOAT4X4 view_proj;
    XMFLOAT4X4 inverse_view_proj;
    XMFLOAT3 eye_posw;
    float cbuffer_per_obj_pad1;
    XMFLOAT2 render_target_size;
    XMFLOAT2 inverse_render_target_size;
    float nearz;
    float farz;
    float total_time;
    float delta_time;
    float padding[20];             // Padding so the constant buffer is 256-byte aligned
};
static_assert(512 == sizeof(PassConstantBuffer), "Constant buffer size must be 256b aligned");

static void
create_upload_buffer (ID3D12Device * device, UINT64 total_size, BYTE ** mapped_data, ID3D12Resource ** out_upload_buffer) {

    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.CreationNodeMask = 1U;
    heap_props.VisibleNodeMask = 1U;

    D3D12_RESOURCE_DESC rsc_desc = {};
    rsc_desc.Dimension = D3D12_RESOURCE_DIMENSION::D3D12_RESOURCE_DIMENSION_BUFFER;
    rsc_desc.Alignment = 0;
    rsc_desc.Width = total_size;
    rsc_desc.Height = 1;
    rsc_desc.DepthOrArraySize = 1;
    rsc_desc.MipLevels = 1;
    rsc_desc.Format = DXGI_FORMAT_UNKNOWN;
    rsc_desc.SampleDesc.Count = 1;
    rsc_desc.SampleDesc.Quality = 0;
    rsc_desc.Layout = D3D12_TEXTURE_LAYOUT::D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rsc_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CHECK_AND_FAIL(device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &rsc_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(out_upload_buffer)));

    D3D12_RANGE mem_range = {};
    mem_range.Begin = 0;
    mem_range.End = 0;
    CHECK_AND_FAIL((*out_upload_buffer)->Map(0, &mem_range, reinterpret_cast<void**>(mapped_data)));

    // We do not need to unmap until we are done with the resource.  However, we must not write to
    // the resource while it is in use by the GPU (so we must use synchronization techniques).
}


// FrameResource stores the resources needed for the CPU to build the command lists for a frame.
struct FrameResource {
    // We cannot reset the allocator until the GPU is done processing the commands.
    // So each frame needs their own allocator.
    ID3D12CommandAllocator * cmd_list_alloc;

    // We cannot update a cbuffer until the GPU is done processing the commands
    // that reference it.  So each frame needs their own cbuffers.
    PassConstantBuffer * pass_cb;
    ObjectConstantBuffer * object_cb;

    // Fence value to mark commands up to this fence point.  This lets us
    // check if these frame resources are still in use by the GPU.
    UINT64 fence = 0;
};
struct RenderItem {
    // World matrix of the shape that describes the object's local space
    // relative to the world space, which defines the position, orientation,
    // and scale of the object in the world.
    XMFLOAT4X4 world;

    // Dirty flag indicating the object data has changed and we need to update the constant buffer.
    // Because we have an object cbuffer for each FrameResource, we have to apply the
    // update to each FrameResource.  Thus, when we modify obect data we should set 
    // NumFramesDirty = gNumFrameResources so that each frame resource gets the update.
    int count_frames_dirty;

    // Index into GPU constant buffer corresponding to the ObjectCB for this render item.
    UINT obj_cbuffer_index;

    // TODO(omid): add mesh/geometry data 

    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType;

    // DrawIndexedInstanced parameters.
    UINT index_count;
    UINT start_index_loc;
    int base_vertex_loc;
};

struct TextuVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT2 uv;
};
struct SceneContext {
    // camera settings (spherical coordinate)
    float theta;
    float phi;
    float radius;

    // mouse position
    POINT mouse;

    // world view projection matrices
    DirectX::XMMATRIX world;
    DirectX::XMMATRIX view;
    DirectX::XMMATRIX proj;

    // display-related data
    UINT width;
    UINT height;
    float aspect_ratio;
};

static void
handle_mouse_move(SceneContext * scene_ctx, WPARAM wParam, int x, int y) {
    if ((wParam & MK_LBUTTON) != 0) {
        // make each pixel correspond to a quarter of a degree
        float dx = DirectX::XMConvertToRadians(0.25f * (float)(x - scene_ctx->mouse.x));
        float dy = DirectX::XMConvertToRadians(0.25f * (float)(y - scene_ctx->mouse.y));

        // update angles (to orbit camera around)
        scene_ctx->theta += dx;
        scene_ctx->phi += dy;

        // clamp phi
        scene_ctx->phi = CLAMP_VALUE(scene_ctx->phi, 0.1f, DirectX::XM_PI - 0.1f);
    } else if ((wParam & MK_RBUTTON) != 0) {
        // make each pixel correspond to a 0.005 unit in scene
        float dx = 0.005f * (float)(x - scene_ctx->mouse.x);
        float dy = 0.005f * (float)(y - scene_ctx->mouse.y);

        // update camera radius
        scene_ctx->radius += dx - dy;

        // clamp radius
        scene_ctx->radius = CLAMP_VALUE(scene_ctx->radius, 3.0f, 15.0f);
    }
    scene_ctx->mouse.x = x;
    scene_ctx->mouse.y = y;
}
static void
create_box_vertices (TextuVertex out_vertices [], uint16_t out_indices []) {
    // TODO(omid): Check the issue with uv values
    // this is related to texture cell width?
    float uv_min = 0.0f;
    float uv_max = 0.6f;

    TextuVertex vtx1 = {};
    vtx1.position.x = -0.5f;
    vtx1.position.y = -0.5f;
    vtx1.position.z = -0.5f;
    vtx1.uv.x = uv_max;
    vtx1.uv.y = uv_max;

    TextuVertex vtx2 = {};
    vtx2.position.x = -0.5f;
    vtx2.position.y = +0.5f;
    vtx2.position.z = -0.5f;
    vtx2.uv.x = uv_min;
    vtx2.uv.y = uv_max;

    TextuVertex vtx3 = {};
    vtx3.position.x = +0.5f;
    vtx3.position.y = +0.5f;
    vtx3.position.z = -0.5f;
    vtx3.uv.x = uv_min;
    vtx3.uv.y = uv_min;

    TextuVertex vtx4 = {};
    vtx4.position.x = +0.5f;
    vtx4.position.y = -0.5f;
    vtx4.position.z = -0.5f;
    vtx4.uv.x = uv_max;
    vtx4.uv.y = uv_min;

    TextuVertex vtx5 = {};
    vtx5.position.x = -0.5f;
    vtx5.position.y = -0.5f;
    vtx5.position.z = +0.5f;
    vtx5.uv.x = uv_max;
    vtx5.uv.y = uv_max;

    TextuVertex vtx6 = {};
    vtx6.position.x = -0.5f;
    vtx6.position.y = +0.5f;
    vtx6.position.z = +0.5f;
    vtx6.uv.x = uv_min;
    vtx6.uv.y = uv_max;

    TextuVertex vtx7 = {};
    vtx7.position.x = +0.5f;
    vtx7.position.y = +0.5f;
    vtx7.position.z = +0.5f;
    vtx7.uv.x = uv_min;
    vtx7.uv.y = uv_min;

    TextuVertex vtx8 = {};
    vtx8.position.x = +0.5f;
    vtx8.position.y = -0.5f;
    vtx8.position.z = +0.5f;
    vtx8.uv.x = uv_max;
    vtx8.uv.y = uv_min;

    out_vertices[0] = vtx1;
    out_vertices[1] = vtx2;
    out_vertices[2] = vtx3;
    out_vertices[3] = vtx4;
    out_vertices[4] = vtx5;
    out_vertices[5] = vtx6;
    out_vertices[6] = vtx7;
    out_vertices[7] = vtx8;

    // front face
    out_indices[0] = 0;
    out_indices[1] = 1;
    out_indices[2] = 2;
    out_indices[3] = 0;
    out_indices[4] = 2;
    out_indices[5] = 3;

    // back face
    out_indices[6] = 4;
    out_indices[7] = 6;
    out_indices[8] = 5;
    out_indices[9] = 4;
    out_indices[10] = 7;
    out_indices[11] = 6;

    // left face
    out_indices[12] = 4;
    out_indices[13] = 5;
    out_indices[14] = 1;
    out_indices[15] = 4;
    out_indices[16] = 1;
    out_indices[17] = 0;

    // right face
    out_indices[18] = 3;
    out_indices[19] = 2;
    out_indices[20] = 6;
    out_indices[21] = 3;
    out_indices[22] = 6;
    out_indices[23] = 7;

    // top face
    out_indices[24] = 1;
    out_indices[25] = 5;
    out_indices[26] = 6;
    out_indices[27] = 1;
    out_indices[28] = 6;
    out_indices[29] = 2;

    // bottom face
    out_indices[30] = 4;
    out_indices[31] = 0;
    out_indices[32] = 3;
    out_indices[33] = 4;
    out_indices[34] = 3;
    out_indices[35] = 7;

    //{
    //    // front face
    //    0, 1, 2,
    //    0, 2, 3,

    //    // back face
    //    4, 6, 5,
    //    4, 7, 6,

    //    // left face
    //    4, 5, 1,
    //    4, 1, 0,

    //    // right face
    //    3, 2, 6,
    //    3, 6, 7,

    //    // top face
    //    1, 5, 6,
    //    1, 6, 2,

    //    // bottom face
    //    4, 0, 3,
    //    4, 3, 7
    //}

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
                texture_ptr[i] = 0xdd;          // R
                texture_ptr[i + 1] = 0xee;      // G
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
