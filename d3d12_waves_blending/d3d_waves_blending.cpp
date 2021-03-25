
#include "headers/common.h"


#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxgidebug.h>

#include <dxcapi.h>

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
#pragma warning (disable: 26495)    // not initializing struct members

#include "headers/utils.h"
#include "headers/game_timer.h"
#include "headers/dds_loader.h"

#include "waves.h"

// TODO(omid): Swapchain backbuffer count and queuing frames count can be the same (refer to earlier samples)
#define NUM_BACKBUFFERS         2
#define NUM_QUEUING_FRAMES      3

enum RENDER_LAYER : int {
    OPAQUE_LAYER = 0,
    TRANSPARENT_LAYER = 1,
    ALPHATESTED_LAYER = 2,

    _COUNT_RENDER_LAYER
};
enum ALL_RENDERITEMS {
    RITEM_WATER = 0,
    RITEM_GRID = 1,
    RITEM_BOX = 2,

    _COUNT_RENDERITEM
};
enum GEOM_INDEX {
    GEOM_BOX = 0,
    GEOM_WATER = 1,
    GEOM_GRID = 2,

    _COUNT_GEOM
};
enum MAT_INDEX {
    MAT_WOOD_CRATE = 0,
    MAT_GRASS = 1,
    MAT_WATER = 2,

    _COUNT_MATERIAL
};
enum TEX_INDEX {
    TEX_CRATE01 = 0,
    TEX_WATER = 1,
    TEX_GRASS = 2,

    _COUNT_TEX
};
enum SAMPLER_INDEX {
    SAMPLER_POINT_WRAP = 0,
    SAMPLER_POINT_CLAMP = 1,
    SAMPLER_LINEAR_WRAP = 2,
    SAMPLER_LINEAR_CLAMP = 3,
    SAMPLER_ANISOTROPIC_WRAP = 4,
    SAMPLER_ANISOTROPIC_CLAMP = 5,

    _COUNT_SAMPLER
};
struct SceneContext {
    // camera settings (spherical coordinate)
    float theta;
    float phi;
    float radius;

    // light (sun) settings
    float sun_theta;
    float sun_phi;

    // mouse position
    POINT mouse;

    // world view projection matrices
    XMFLOAT3   eye_pos;
    XMFLOAT4X4 view;
    XMFLOAT4X4 proj;

    // display-related data
    UINT width;
    UINT height;
    float aspect_ratio;
};

GameTimer global_timer;
bool global_running;
SceneContext global_scene_ctx;

struct RenderItemArray {
    RenderItem  ritems[_COUNT_RENDERITEM];
    uint32_t    size;
};
struct D3DRenderContext {
    // Pipeline stuff
    D3D12_VIEWPORT                  viewport;
    D3D12_RECT                      scissor_rect;
    IDXGISwapChain3 *               swapchain3;
    IDXGISwapChain *                swapchain;
    ID3D12Device *                  device;
    ID3D12RootSignature *           root_signature;
    ID3D12PipelineState *           psos[_COUNT_RENDER_LAYER];

    // Command objects
    ID3D12CommandQueue *            cmd_queue;
    ID3D12CommandAllocator *        direct_cmd_list_alloc;
    ID3D12GraphicsCommandList *     direct_cmd_list;

    UINT                            rtv_descriptor_size;
    UINT                            cbv_srv_uav_descriptor_size;

    ID3D12DescriptorHeap *          rtv_heap;
    ID3D12DescriptorHeap *          dsv_heap;
    ID3D12DescriptorHeap *          srv_heap;

    PassConstants                   main_pass_constants;
    UINT                            pass_cbv_offset;

    // List of all the render items.
    RenderItemArray                 all_ritems;
    // Render items divided by PSO.
    RenderItemArray                 opaque_ritems;
    RenderItemArray                 transparent_ritems;
    RenderItemArray                 alphatested_ritems;

    MeshGeometry                    geom[_COUNT_GEOM];

    // Synchronization stuff
    UINT                            frame_index;
    HANDLE                          fence_event;
    ID3D12Fence *                   fence;
    FrameResource                   frame_resources[NUM_QUEUING_FRAMES];

    // Each swapchain backbuffer needs a render target
    ID3D12Resource *                render_targets[NUM_BACKBUFFERS];
    UINT                            backbuffer_index;

    ID3D12Resource *                depth_stencil_buffer;

    Material                        materials[_COUNT_MATERIAL];
    Texture                         textures[_COUNT_TEX];
};

static void
load_texture (
    ID3D12Device * device,
    ID3D12GraphicsCommandList * cmd_list,
    wchar_t const * tex_path,
    Texture * out_texture
) {

    uint8_t * ddsData;
    D3D12_SUBRESOURCE_DATA * subresources;
    UINT n_subresources = 0;

    LoadDDSTextureFromFile(device, tex_path, &out_texture->resource, &ddsData, &subresources, &n_subresources);

    UINT64 upload_buffer_size = get_required_intermediate_size(out_texture->resource, 0,
                                                               n_subresources);

   // Create the GPU upload buffer.
    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = upload_buffer_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&out_texture->upload_heap)
    );

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = out_texture->resource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Use Heap-allocating UpdateSubresources implementation for variable number of subresources (which is the case for textures).
    update_subresources_heap(
        cmd_list, out_texture->resource, out_texture->upload_heap,
        0, 0, n_subresources, subresources
    );
    cmd_list->ResourceBarrier(1, &barrier);

    ::free(subresources);
    ::free(ddsData);
}
static void
create_materials (Material out_materials []) {
    strcpy_s(out_materials[MAT_GRASS].name, "grass");
    out_materials[MAT_GRASS].mat_cbuffer_index = 0;
    out_materials[MAT_GRASS].diffuse_srvheap_index = 0;
    out_materials[MAT_GRASS].diffuse_albedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    out_materials[MAT_GRASS].fresnel_r0 = XMFLOAT3(0.01f, 0.01f, 0.01f);
    out_materials[MAT_GRASS].roughness = 0.125f;
    out_materials[MAT_GRASS].mat_transform = Identity4x4();

    // -- not a good water material (we don't have transparency and env reflection rn)
    strcpy_s(out_materials[MAT_WATER].name, "water");
    out_materials[MAT_WATER].mat_cbuffer_index = 1;
    out_materials[MAT_WATER].diffuse_srvheap_index = 1;
    out_materials[MAT_WATER].diffuse_albedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    out_materials[MAT_WATER].fresnel_r0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
    out_materials[MAT_WATER].roughness = 0.0f;
    out_materials[MAT_WATER].mat_transform = Identity4x4();

    strcpy_s(out_materials[MAT_WOOD_CRATE].name, "wood_crate");
    out_materials[MAT_WOOD_CRATE].mat_cbuffer_index = 2;
    out_materials[MAT_WOOD_CRATE].diffuse_srvheap_index = 2;
    out_materials[MAT_WOOD_CRATE].diffuse_albedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    out_materials[MAT_WOOD_CRATE].fresnel_r0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
    out_materials[MAT_WOOD_CRATE].roughness = 0.2f;
    out_materials[MAT_WOOD_CRATE].mat_transform = Identity4x4();
}
static float
calc_hill_height (float x, float z) {
    return 0.3f * (z * sinf(0.1f * x) + x * cosf(0.1f * z));
}
static XMFLOAT3
calc_hill_normal (float x, float z) {
    // n = (-df/dx, 1, -df/dz)
    XMFLOAT3 n(
        -0.03f * z * cosf(0.1f * x) - 0.3f * cosf(0.1f * z),
        1.0f,
        -0.3f * sinf(0.1f * x) + 0.03f * x * sinf(0.1f * z)
    );

    XMVECTOR unit_normal = XMVector3Normalize(XMLoadFloat3(&n));
    XMStoreFloat3(&n, unit_normal);

    return n;
}
static void
create_shape_geometry (D3DRenderContext * render_ctx) {

    // required sizes
    int nvtx = 24;
    int nidx = 36;

    Vertex *    vertices = (Vertex *)::malloc(sizeof(Vertex) * nvtx);
    uint16_t *  indices = (uint16_t *)::malloc(sizeof(uint16_t) * nidx);
    BYTE *      scratch = (BYTE *)::malloc(sizeof(GeomVertex) * nvtx + sizeof(uint16_t) * nidx);

    // box
    UINT bsz = sizeof(GeomVertex) * nvtx;
    GeomVertex *    box_vertices = reinterpret_cast<GeomVertex *>(scratch);
    uint16_t *      box_indices = reinterpret_cast<uint16_t *>(scratch + bsz);

    create_box(8.0f, 8.0f, 8.0f, box_vertices, box_indices);

    SubmeshGeometry box_submesh = {};
    box_submesh.index_count = nidx;
    box_submesh.start_index_location = 0;
    box_submesh.base_vertex_location = 0;

    UINT k = 0;
    for (size_t i = 0; i < nvtx; ++i, ++k) {
        vertices[k].position = box_vertices[i].Position;
        vertices[k].normal = box_vertices[i].Normal;
        vertices[k].texc = box_vertices[i].TexC;
    }

    // -- pack indices
    k = 0;
    for (size_t i = 0; i < nidx; ++i, ++k) {
        indices[k] = box_indices[i];
    }

    UINT vb_byte_size = nvtx * sizeof(Vertex);
    UINT ib_byte_size = nidx * sizeof(uint16_t);

    // -- Fill out geom
    D3DCreateBlob(vb_byte_size, &render_ctx->geom[GEOM_BOX].vb_cpu);
    if (vertices)
        CopyMemory(render_ctx->geom[GEOM_BOX].vb_cpu->GetBufferPointer(), vertices, vb_byte_size);

    D3DCreateBlob(ib_byte_size, &render_ctx->geom[GEOM_BOX].ib_cpu);
    if (indices)
        CopyMemory(render_ctx->geom[GEOM_BOX].ib_cpu->GetBufferPointer(), indices, ib_byte_size);

    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, vertices, vb_byte_size, &render_ctx->geom[GEOM_BOX].vb_uploader, &render_ctx->geom[GEOM_BOX].vb_gpu);
    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, indices, ib_byte_size, &render_ctx->geom[GEOM_BOX].ib_uploader, &render_ctx->geom[GEOM_BOX].ib_gpu);

    render_ctx->geom[GEOM_BOX].vb_byte_stide = sizeof(Vertex);
    render_ctx->geom[GEOM_BOX].vb_byte_size = vb_byte_size;
    render_ctx->geom[GEOM_BOX].ib_byte_size = ib_byte_size;
    render_ctx->geom[GEOM_BOX].index_format = DXGI_FORMAT_R16_UINT;

    render_ctx->geom[GEOM_BOX].submesh_names[0] = "box";
    render_ctx->geom[GEOM_BOX].submesh_geoms[0] = box_submesh;

    // -- cleanup
    free(scratch);
    free(indices);
    free(vertices);
}
static void
create_land_geometry (D3DRenderContext * render_ctx) {

    // required sizes calculations
    int nrow = 50;
    int ncol = 50;
    int nvtx = nrow * ncol;
    int nidx = (nrow - 1) * (ncol - 1) * 6;     // every 6 vertices form a quad

    Vertex * vertices = (Vertex *)::malloc(sizeof(Vertex) * nvtx);
    uint16_t * indices = (uint16_t *)::malloc(sizeof(uint16_t) * nidx);
    GeomVertex * grid = (GeomVertex *)::malloc(sizeof(GeomVertex) * nvtx);

    create_grid(160.0f, 160.0f, 50, 50, grid, indices);

    // Extract the vertex elements we are interested and apply the height function to
    // each vertex.  In addition, color the vertices based on their height so we have
    // sandy looking beaches, grassy low hills, and snow mountain peaks.

    for (int i = 0; i < nvtx; ++i) {
        auto & p = grid[i].Position;
        vertices[i].position = p;
        vertices[i].position.y = calc_hill_height(p.x, p.z);
        vertices[i].normal = calc_hill_normal(p.x, p.z);
        vertices[i].texc = grid[i].TexC;
    }

    UINT vb_byte_size = nvtx * sizeof(Vertex);
    UINT ib_byte_size = nidx * sizeof(uint16_t);

    // -- Fill out render_ctx geom (output)

    CHECK_AND_FAIL(D3DCreateBlob(vb_byte_size, &render_ctx->geom[GEOM_GRID].vb_cpu));
    if (vertices)
        CopyMemory(render_ctx->geom[GEOM_GRID].vb_cpu->GetBufferPointer(), vertices, vb_byte_size);

    D3DCreateBlob(ib_byte_size, &render_ctx->geom[GEOM_GRID].ib_cpu);
    if (indices)
        CopyMemory(render_ctx->geom[GEOM_GRID].ib_cpu->GetBufferPointer(), indices, ib_byte_size);

    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, vertices, vb_byte_size, &render_ctx->geom[GEOM_GRID].vb_uploader, &render_ctx->geom[GEOM_GRID].vb_gpu);
    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, indices, ib_byte_size, &render_ctx->geom[GEOM_GRID].ib_uploader, &render_ctx->geom[GEOM_GRID].ib_gpu);

    render_ctx->geom[GEOM_GRID].vb_byte_stide = sizeof(Vertex);
    render_ctx->geom[GEOM_GRID].vb_byte_size = vb_byte_size;
    render_ctx->geom[GEOM_GRID].ib_byte_size = ib_byte_size;
    render_ctx->geom[GEOM_GRID].index_format = DXGI_FORMAT_R16_UINT;

    SubmeshGeometry submesh;
    submesh.index_count = nidx;
    submesh.start_index_location = 0;
    submesh.base_vertex_location = 0;

    render_ctx->geom[GEOM_GRID].submesh_names[0] = "grid";
    render_ctx->geom[GEOM_GRID].submesh_geoms[0] = submesh;

    ::free(grid);
    ::free(indices);
    ::free(vertices);
}
static void
create_water_geometry (UINT nrow, UINT ncol, UINT ntri, D3DRenderContext * render_ctx) {
    uint32_t _WAVE_VTX_CNT = ncol * nrow;
    Vertex * vertices = (Vertex *)::malloc(sizeof(Vertex) * _WAVE_VTX_CNT);
    uint32_t _idx_cnt = 3 * ntri;
    uint32_t * indices = (uint32_t *)::malloc(_idx_cnt * sizeof(uint32_t)); // 3 indices per face
    SIMPLE_ASSERT(_WAVE_VTX_CNT < 0x0000ffff, "Invalid vertex count");

    // Iterate over each quad.
    int m = nrow;
    int n = ncol;
    int k = 0;
    for (int i = 0; i < m - 1; ++i) {
        for (int j = 0; j < n - 1; ++j) {
            indices[k] = (i * n + j);
            indices[k + 1] = (i * n + j + 1);       // TODO(omid): address this warning 
            indices[k + 2] = ((i + 1) * n + j);

            indices[k + 3] = ((i + 1) * n + j);
            indices[k + 4] = (i * n + j + 1);
            indices[k + 5] = ((i + 1) * n + j + 1);

            k += 6; // next quad
        }
    }

    UINT vb_byte_size = _WAVE_VTX_CNT * sizeof(Vertex);
    UINT ib_byte_size = _idx_cnt * sizeof(uint32_t);

    // -- Fill out render_ctx geom (output)

    //D3DCreateBlob(vb_byte_size, &render_ctx->water_geom.vb_cpu);
    //CopyMemory(render_ctx->water_geom.vb_cpu->GetBufferPointer(), vertices, vb_byte_size);

    CHECK_AND_FAIL(D3DCreateBlob(ib_byte_size, &render_ctx->geom[GEOM_WATER].ib_cpu));
    if (indices)
        CopyMemory(render_ctx->geom[GEOM_WATER].ib_cpu->GetBufferPointer(), indices, ib_byte_size);

    //create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, vertices, vb_byte_size, &render_ctx->geom[GEOM_WATER].vb_uploader, &render_ctx->geom[GEOM_WATER].vb_gpu);
    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, indices, ib_byte_size, &render_ctx->geom[GEOM_WATER].ib_uploader, &render_ctx->geom[GEOM_WATER].ib_gpu);

    render_ctx->geom[GEOM_WATER].vb_byte_stide = sizeof(Vertex);
    render_ctx->geom[GEOM_WATER].vb_byte_size = vb_byte_size;
    render_ctx->geom[GEOM_WATER].ib_byte_size = ib_byte_size;
    render_ctx->geom[GEOM_WATER].index_format = DXGI_FORMAT_R32_UINT;

    SubmeshGeometry submesh;
    submesh.index_count = _idx_cnt;
    submesh.start_index_location = 0;
    submesh.base_vertex_location = 0;

    render_ctx->geom[GEOM_WATER].submesh_names[0] = "water";
    render_ctx->geom[GEOM_WATER].submesh_geoms[0] = submesh;

    ::free(indices);
    ::free(vertices);
}
static void
create_render_items (
    RenderItemArray * all_ritems,
    RenderItemArray * opaque_ritems,
    RenderItemArray * transparent_ritems,
    RenderItemArray * alphatested_ritems,
    MeshGeometry * box_geom, MeshGeometry * water_geom, MeshGeometry * grid_geom,
    Material materials []
) {
    all_ritems->ritems[RITEM_WATER].world = Identity4x4();
    all_ritems->ritems[RITEM_WATER].tex_transform = Identity4x4();
    XMStoreFloat4x4(&all_ritems->ritems[RITEM_WATER].tex_transform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
    all_ritems->ritems[RITEM_WATER].obj_cbuffer_index = 0;
    all_ritems->ritems[RITEM_WATER].mat = &materials[MAT_WATER];
    all_ritems->ritems[RITEM_WATER].geometry = water_geom;
    all_ritems->ritems[RITEM_WATER].primitive_type = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    all_ritems->ritems[RITEM_WATER].index_count = water_geom->submesh_geoms[0].index_count;
    all_ritems->ritems[RITEM_WATER].start_index_loc = water_geom->submesh_geoms[0].start_index_location;
    all_ritems->ritems[RITEM_WATER].base_vertex_loc = water_geom->submesh_geoms[0].base_vertex_location;
    all_ritems->ritems[RITEM_WATER].n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_WATER].mat->n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_WATER].initialized = true;
    all_ritems->size++;
    transparent_ritems->ritems[0] = all_ritems->ritems[RITEM_WATER];
    transparent_ritems->size++;

    all_ritems->ritems[RITEM_GRID].world = Identity4x4();
    all_ritems->ritems[RITEM_GRID].tex_transform = Identity4x4();
    XMStoreFloat4x4(&all_ritems->ritems[RITEM_GRID].tex_transform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
    all_ritems->ritems[RITEM_GRID].obj_cbuffer_index = 1;
    all_ritems->ritems[RITEM_GRID].mat = &materials[MAT_GRASS];
    all_ritems->ritems[RITEM_GRID].geometry = grid_geom;
    all_ritems->ritems[RITEM_GRID].primitive_type = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    all_ritems->ritems[RITEM_GRID].index_count = grid_geom->submesh_geoms[0].index_count;
    all_ritems->ritems[RITEM_GRID].start_index_loc = grid_geom->submesh_geoms[0].start_index_location;
    all_ritems->ritems[RITEM_GRID].base_vertex_loc = grid_geom->submesh_geoms[0].base_vertex_location;
    all_ritems->ritems[RITEM_GRID].n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_GRID].mat->n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_GRID].initialized = true;
    all_ritems->size++;
    opaque_ritems->ritems[0] = all_ritems->ritems[RITEM_GRID];
    opaque_ritems->size++;

    all_ritems->ritems[RITEM_BOX].world = Identity4x4();
    XMStoreFloat4x4(&all_ritems->ritems[RITEM_BOX].world, XMMatrixTranslation(3.0f, 2.0f, -9.0f));
    all_ritems->ritems[RITEM_BOX].tex_transform = Identity4x4();
    all_ritems->ritems[RITEM_BOX].obj_cbuffer_index = 2;
    all_ritems->ritems[RITEM_BOX].geometry = box_geom;
    all_ritems->ritems[RITEM_BOX].mat = &materials[MAT_WOOD_CRATE];
    all_ritems->ritems[RITEM_BOX].primitive_type = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    all_ritems->ritems[RITEM_BOX].index_count = box_geom->submesh_geoms[0].index_count;
    all_ritems->ritems[RITEM_BOX].start_index_loc = box_geom->submesh_geoms[0].start_index_location;
    all_ritems->ritems[RITEM_BOX].base_vertex_loc = box_geom->submesh_geoms[0].base_vertex_location;
    all_ritems->ritems[RITEM_BOX].n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_BOX].mat->n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_BOX].initialized = true;
    all_ritems->size++;
    opaque_ritems->ritems[1] = all_ritems->ritems[RITEM_BOX];
    opaque_ritems->size++;
}
// -- indexed drawing
static void
draw_render_items (
    ID3D12GraphicsCommandList * cmd_list,
    ID3D12Resource * object_cbuffer,
    ID3D12Resource * mat_cbuffer,
    UINT64 descriptor_increment_size,
    ID3D12DescriptorHeap * srv_heap,
    RenderItemArray * ritem_array, // TODO(omid): pass pointer to renderitems
    UINT current_frame_index
) {
    UINT objcb_byte_size = (UINT64)sizeof(ObjectConstants);
    UINT matcb_byte_size = (UINT64)sizeof(MaterialConstants);
    for (size_t i = 0; i < ritem_array->size; ++i) {
        if (ritem_array->ritems[i].initialized) {
            D3D12_VERTEX_BUFFER_VIEW vbv = Mesh_GetVertexBufferView(ritem_array->ritems[i].geometry);
            D3D12_INDEX_BUFFER_VIEW ibv = Mesh_GetIndexBufferView(ritem_array->ritems[i].geometry);
            cmd_list->IASetVertexBuffers(0, 1, &vbv);
            cmd_list->IASetIndexBuffer(&ibv);
            cmd_list->IASetPrimitiveTopology(ritem_array->ritems[i].primitive_type);

            D3D12_GPU_DESCRIPTOR_HANDLE tex = srv_heap->GetGPUDescriptorHandleForHeapStart();
            tex.ptr += descriptor_increment_size * ritem_array->ritems[i].mat->diffuse_srvheap_index;

            D3D12_GPU_VIRTUAL_ADDRESS objcb_address = object_cbuffer->GetGPUVirtualAddress();
            objcb_address += (UINT64)ritem_array->ritems[i].obj_cbuffer_index * objcb_byte_size;

            D3D12_GPU_VIRTUAL_ADDRESS matcb_address = mat_cbuffer->GetGPUVirtualAddress();
            matcb_address += (UINT64)ritem_array->ritems[i].mat->mat_cbuffer_index * matcb_byte_size;

            cmd_list->SetGraphicsRootDescriptorTable(0, tex);
            cmd_list->SetGraphicsRootConstantBufferView(1, objcb_address);
            cmd_list->SetGraphicsRootConstantBufferView(3, matcb_address);
            cmd_list->DrawIndexedInstanced(ritem_array->ritems[i].index_count, 1, ritem_array->ritems[i].start_index_loc, ritem_array->ritems[i].base_vertex_loc, 0);
        }
    }
}
static void
create_descriptor_heaps (D3DRenderContext * render_ctx) {

    // Create Shader Resource View descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
    srv_heap_desc.NumDescriptors = _COUNT_TEX;
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    render_ctx->device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&render_ctx->srv_heap));

    // Fill out the heap with actual descriptors
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_cpu_handle = render_ctx->srv_heap->GetCPUDescriptorHandleForHeapStart();

    // grass texture
    ID3D12Resource * grass_tex = render_ctx->textures[TEX_GRASS].resource;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = grass_tex->GetDesc().Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = grass_tex->GetDesc().MipLevels;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    render_ctx->device->CreateShaderResourceView(grass_tex, &srv_desc, descriptor_cpu_handle);

    // water texture
    ID3D12Resource * water_tex = render_ctx->textures[TEX_WATER].resource;
    memset(&srv_desc, 0, sizeof(srv_desc));         // reset desc
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = water_tex->GetDesc().Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = water_tex->GetDesc().MipLevels;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    descriptor_cpu_handle.ptr += render_ctx->cbv_srv_uav_descriptor_size;   // next descriptr
    render_ctx->device->CreateShaderResourceView(water_tex, &srv_desc, descriptor_cpu_handle);

    // crate texture
    ID3D12Resource * box_tex = render_ctx->textures[TEX_CRATE01].resource;
    memset(&srv_desc, 0, sizeof(srv_desc)); // reset desc
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = box_tex->GetDesc().Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = box_tex->GetDesc().MipLevels;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    descriptor_cpu_handle.ptr += render_ctx->cbv_srv_uav_descriptor_size;   // next descriptor
    render_ctx->device->CreateShaderResourceView(box_tex, &srv_desc, descriptor_cpu_handle);


    // Create Render Target View Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.NumDescriptors = NUM_BACKBUFFERS;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    render_ctx->device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&render_ctx->rtv_heap));

    // Create Depth Stencil View Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc;
    dsv_heap_desc.NumDescriptors = 1;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsv_heap_desc.NodeMask = 0;
    render_ctx->device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&render_ctx->dsv_heap));
}
static void
get_static_samplers (D3D12_STATIC_SAMPLER_DESC out_samplers []) {
    // 0: PointWrap
    out_samplers[SAMPLER_POINT_WRAP] = {};
    out_samplers[SAMPLER_POINT_WRAP].ShaderRegister = 0;
    out_samplers[SAMPLER_POINT_WRAP].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    out_samplers[SAMPLER_POINT_WRAP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_POINT_WRAP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_POINT_WRAP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_POINT_WRAP].MipLODBias = 0;
    out_samplers[SAMPLER_POINT_WRAP].MaxAnisotropy = 16;
    out_samplers[SAMPLER_POINT_WRAP].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    out_samplers[SAMPLER_POINT_WRAP].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    out_samplers[SAMPLER_POINT_WRAP].MinLOD = 0.f;
    out_samplers[SAMPLER_POINT_WRAP].MaxLOD = D3D12_FLOAT32_MAX;
    out_samplers[SAMPLER_POINT_WRAP].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out_samplers[SAMPLER_POINT_WRAP].RegisterSpace = 0;

    // 1: PointClamp
    out_samplers[SAMPLER_POINT_CLAMP] = {};
    out_samplers[SAMPLER_POINT_CLAMP].ShaderRegister = 1;
    out_samplers[SAMPLER_POINT_CLAMP].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    out_samplers[SAMPLER_POINT_CLAMP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_POINT_CLAMP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_POINT_CLAMP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_POINT_CLAMP].MipLODBias = 0;
    out_samplers[SAMPLER_POINT_CLAMP].MaxAnisotropy = 16;
    out_samplers[SAMPLER_POINT_CLAMP].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    out_samplers[SAMPLER_POINT_CLAMP].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    out_samplers[SAMPLER_POINT_CLAMP].MinLOD = 0.f;
    out_samplers[SAMPLER_POINT_CLAMP].MaxLOD = D3D12_FLOAT32_MAX;
    out_samplers[SAMPLER_POINT_CLAMP].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out_samplers[SAMPLER_POINT_CLAMP].RegisterSpace = 0;

    // 2: LinearWrap
    out_samplers[SAMPLER_LINEAR_WRAP] = {};
    out_samplers[SAMPLER_LINEAR_WRAP].ShaderRegister = 2;
    out_samplers[SAMPLER_LINEAR_WRAP].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    out_samplers[SAMPLER_LINEAR_WRAP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_LINEAR_WRAP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_LINEAR_WRAP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_LINEAR_WRAP].MipLODBias = 0;
    out_samplers[SAMPLER_LINEAR_WRAP].MaxAnisotropy = 16;
    out_samplers[SAMPLER_LINEAR_WRAP].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    out_samplers[SAMPLER_LINEAR_WRAP].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    out_samplers[SAMPLER_LINEAR_WRAP].MinLOD = 0.f;
    out_samplers[SAMPLER_LINEAR_WRAP].MaxLOD = D3D12_FLOAT32_MAX;
    out_samplers[SAMPLER_LINEAR_WRAP].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out_samplers[SAMPLER_LINEAR_WRAP].RegisterSpace = 0;

    // 3: LinearClamp
    out_samplers[SAMPLER_LINEAR_CLAMP] = {};
    out_samplers[SAMPLER_LINEAR_CLAMP].ShaderRegister = 3;
    out_samplers[SAMPLER_LINEAR_CLAMP].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    out_samplers[SAMPLER_LINEAR_CLAMP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_LINEAR_CLAMP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_LINEAR_CLAMP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_LINEAR_CLAMP].MipLODBias = 0;
    out_samplers[SAMPLER_LINEAR_CLAMP].MaxAnisotropy = 16;
    out_samplers[SAMPLER_LINEAR_CLAMP].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    out_samplers[SAMPLER_LINEAR_CLAMP].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    out_samplers[SAMPLER_LINEAR_CLAMP].MinLOD = 0.f;
    out_samplers[SAMPLER_LINEAR_CLAMP].MaxLOD = D3D12_FLOAT32_MAX;
    out_samplers[SAMPLER_LINEAR_CLAMP].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out_samplers[SAMPLER_LINEAR_CLAMP].RegisterSpace = 0;

    // 4: AnisotropicWrap
    out_samplers[SAMPLER_ANISOTROPIC_WRAP] = {};
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].ShaderRegister = 4;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].Filter = D3D12_FILTER_ANISOTROPIC;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].MipLODBias = 0.0f;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].MaxAnisotropy = 8;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].MinLOD = 0.f;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].MaxLOD = D3D12_FLOAT32_MAX;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].RegisterSpace = 0;

    // 5: AnisotropicClamp
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP] = {};
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].ShaderRegister = 5;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].Filter = D3D12_FILTER_ANISOTROPIC;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].MipLODBias = 0.0f;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].MaxAnisotropy = 8;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].MinLOD = 0.f;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].MaxLOD = D3D12_FLOAT32_MAX;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].RegisterSpace = 0;
}
static void
create_root_signature (ID3D12Device * device, ID3D12RootSignature ** root_signature) {
    D3D12_DESCRIPTOR_RANGE tex_table = {};
    tex_table.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tex_table.NumDescriptors = 1;
    tex_table.BaseShaderRegister = 0;
    tex_table.RegisterSpace = 0;
    tex_table.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER slot_root_params[4] = {};
    // NOTE(omid): Perfomance tip! Order from most frequent to least frequent.
    slot_root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    slot_root_params[0].DescriptorTable.NumDescriptorRanges = 1;
    slot_root_params[0].DescriptorTable.pDescriptorRanges = &tex_table;
    slot_root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    slot_root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    slot_root_params[1].Descriptor.ShaderRegister = 0;
    slot_root_params[1].Descriptor.RegisterSpace = 0;
    slot_root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    slot_root_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    slot_root_params[2].Descriptor.ShaderRegister = 1;
    slot_root_params[2].Descriptor.RegisterSpace = 0;
    slot_root_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    slot_root_params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    slot_root_params[3].Descriptor.ShaderRegister = 2;
    slot_root_params[3].Descriptor.RegisterSpace = 0;
    slot_root_params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[_COUNT_SAMPLER] = {};
    get_static_samplers(samplers);

    // A root signature is an array of root parameters.
    D3D12_ROOT_SIGNATURE_DESC root_sig_desc = {};
    root_sig_desc.NumParameters = 4;
    root_sig_desc.pParameters = slot_root_params;
    root_sig_desc.NumStaticSamplers = _COUNT_SAMPLER;
    root_sig_desc.pStaticSamplers = samplers;
    root_sig_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob * serialized_root_sig = nullptr;
    ID3DBlob * error_blob = nullptr;
    D3D12SerializeRootSignature(&root_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized_root_sig, &error_blob);

    if (error_blob) {
        ::OutputDebugStringA((char*)error_blob->GetBufferPointer());
    }

    device->CreateRootSignature(0, serialized_root_sig->GetBufferPointer(), serialized_root_sig->GetBufferSize(), IID_PPV_ARGS(root_signature));
}
static void
create_pso (D3DRenderContext * render_ctx, IDxcBlob * vertex_shader_code, IDxcBlob * pixel_shader_code_opaque, IDxcBlob * pixel_shader_code_alphatested) {
    // -- Create vertex-input-layout Elements

    D3D12_INPUT_ELEMENT_DESC input_desc[3];
    input_desc[0] = {};
    input_desc[0].SemanticName = "POSITION";
    input_desc[0].SemanticIndex = 0;
    input_desc[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    input_desc[0].InputSlot = 0;
    input_desc[0].AlignedByteOffset = 0;
    input_desc[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    input_desc[1] = {};
    input_desc[1].SemanticName = "NORMAL";
    input_desc[1].SemanticIndex = 0;
    input_desc[1].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    input_desc[1].InputSlot= 0;
    input_desc[1].AlignedByteOffset = 12; // bc of the position byte-size
    input_desc[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    input_desc[2] = {};
    input_desc[2].SemanticName = "TEXCOORD";
    input_desc[2].SemanticIndex = 0;
    input_desc[2].Format = DXGI_FORMAT_R32G32_FLOAT;
    input_desc[2].InputSlot = 0;
    input_desc[2].AlignedByteOffset = 24; // bc of the position and normal
    input_desc[2].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    //
    // -- Create PSO for Opaque objs
    //
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

    /* Depth Stencil Description */
    D3D12_DEPTH_STENCIL_DESC ds_desc = {};
    ds_desc.DepthEnable = TRUE;
    ds_desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds_desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ds_desc.StencilEnable = FALSE;
    ds_desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    ds_desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    D3D12_DEPTH_STENCILOP_DESC def_stencil_op = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    ds_desc.FrontFace = def_stencil_op;
    ds_desc.BackFace = def_stencil_op;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaque_pso_desc = {};
    opaque_pso_desc.pRootSignature = render_ctx->root_signature;
    opaque_pso_desc.VS.pShaderBytecode = vertex_shader_code->GetBufferPointer();
    opaque_pso_desc.VS.BytecodeLength = vertex_shader_code->GetBufferSize();
    opaque_pso_desc.PS.pShaderBytecode = pixel_shader_code_opaque->GetBufferPointer();
    opaque_pso_desc.PS.BytecodeLength = pixel_shader_code_opaque->GetBufferSize();
    opaque_pso_desc.BlendState = def_blend_desc;
    opaque_pso_desc.SampleMask = UINT_MAX;
    opaque_pso_desc.RasterizerState = def_rasterizer_desc;
    opaque_pso_desc.DepthStencilState = ds_desc;
    opaque_pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    opaque_pso_desc.InputLayout.pInputElementDescs = input_desc;
    opaque_pso_desc.InputLayout.NumElements = ARRAY_COUNT(input_desc);
    opaque_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    opaque_pso_desc.NumRenderTargets = 1;
    opaque_pso_desc.RTVFormats[0] = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM;
    opaque_pso_desc.SampleDesc.Count = 1;
    opaque_pso_desc.SampleDesc.Quality = 0;

    render_ctx->device->CreateGraphicsPipelineState(&opaque_pso_desc, IID_PPV_ARGS(&render_ctx->psos[OPAQUE_LAYER]));
    //
    // -- Create PSO for Transparent objs
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC transparent_pso_desc = opaque_pso_desc;

    D3D12_RENDER_TARGET_BLEND_DESC transparency_blend_desc = {};
    transparency_blend_desc.BlendEnable = true;
    transparency_blend_desc.LogicOpEnable = false;
    transparency_blend_desc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    transparency_blend_desc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    transparency_blend_desc.BlendOp = D3D12_BLEND_OP_ADD;
    transparency_blend_desc.SrcBlendAlpha = D3D12_BLEND_ONE;
    transparency_blend_desc.DestBlendAlpha = D3D12_BLEND_ZERO;
    transparency_blend_desc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    transparency_blend_desc.LogicOp = D3D12_LOGIC_OP_NOOP;
    transparency_blend_desc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    transparent_pso_desc.BlendState.RenderTarget[0] = transparency_blend_desc;
    render_ctx->device->CreateGraphicsPipelineState(&transparent_pso_desc, IID_PPV_ARGS(&render_ctx->psos[TRANSPARENT_LAYER]));
    //
    // -- Create PSO for AlphaTested objs
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC alpha_pso_desc = opaque_pso_desc;
    alpha_pso_desc.PS.pShaderBytecode = pixel_shader_code_alphatested->GetBufferPointer();
    alpha_pso_desc.PS.BytecodeLength = pixel_shader_code_alphatested->GetBufferSize();
    alpha_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    render_ctx->device->CreateGraphicsPipelineState(&alpha_pso_desc, IID_PPV_ARGS(&render_ctx->psos[ALPHATESTED_LAYER]));
}
static void
handle_keyboard_input (SceneContext * scene_ctx, GameTimer * gt) {
    float dt = gt->delta_time;

    if (GetAsyncKeyState(VK_LEFT) & 0x8000)
        scene_ctx->sun_theta -= 1.0f * dt;

    if (GetAsyncKeyState(VK_RIGHT) & 0x8000)
        scene_ctx->sun_theta += 1.0f * dt;

    if (GetAsyncKeyState(VK_UP) & 0x8000)
        scene_ctx->sun_phi -= 1.0f * dt;

    if (GetAsyncKeyState(VK_DOWN) & 0x8000)
        scene_ctx->sun_phi += 1.0f * dt;

    scene_ctx->sun_phi = CLAMP_VALUE(scene_ctx->sun_phi, 0.1f, XM_PIDIV2);
}
static void
handle_mouse_move (SceneContext * scene_ctx, WPARAM wParam, int x, int y) {
    if ((wParam & MK_LBUTTON) != 0) {
        // make each pixel correspond to a quarter of a degree
        float dx = DirectX::XMConvertToRadians(0.25f * (float)(x - scene_ctx->mouse.x));
        float dy = DirectX::XMConvertToRadians(0.25f * (float)(y - scene_ctx->mouse.y));

        // update angles (to orbit camera around)
        scene_ctx->theta += dx;
        scene_ctx->phi += dy;

        // clamp phi
        scene_ctx->phi = CLAMP_VALUE(scene_ctx->phi, 0.1f, XM_PI - 0.1f);
    } else if ((wParam & MK_RBUTTON) != 0) {
        // make each pixel correspond to a 0.2 unit in scene
        float dx = 0.2f * (float)(x - scene_ctx->mouse.x);
        float dy = 0.2f * (float)(y - scene_ctx->mouse.y);

        // update camera radius
        scene_ctx->radius += dx - dy;

        // clamp radius
        scene_ctx->radius = CLAMP_VALUE(scene_ctx->radius, 5.0f, 150.0f);
    }
    scene_ctx->mouse.x = x;
    scene_ctx->mouse.y = y;
}
static void
update_camera (SceneContext * sc) {
    // Convert Spherical to Cartesian coordinates.
    sc->eye_pos.x = sc->radius * sinf(sc->phi) * cosf(sc->theta);
    sc->eye_pos.z = sc->radius * sinf(sc->phi) * sinf(sc->theta);
    sc->eye_pos.y = sc->radius * cosf(sc->phi);

    // Build the view matrix.
    XMVECTOR pos = XMVectorSet(sc->eye_pos.x, sc->eye_pos.y, sc->eye_pos.z, 1.0f);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
    XMStoreFloat4x4(&sc->view, view);
}
static void
update_obj_cbuffers (D3DRenderContext * render_ctx) {
    UINT frame_index = render_ctx->frame_index;
    UINT cbuffer_size = sizeof(ObjectConstants);
    // Only update the cbuffer data if the constants have changed.  
    // This needs to be tracked per frame resource.
    for (unsigned i = 0; i < render_ctx->all_ritems.size; i++) {
        if (
            render_ctx->all_ritems.ritems[i].n_frames_dirty > 0 &&
            render_ctx->all_ritems.ritems[i].initialized
            ) {
            UINT obj_index = render_ctx->all_ritems.ritems[i].obj_cbuffer_index;
            XMMATRIX world = XMLoadFloat4x4(&render_ctx->all_ritems.ritems[i].world);
            XMMATRIX tex_transform = XMLoadFloat4x4(&render_ctx->all_ritems.ritems[i].tex_transform);

            ObjectConstants obj_cbuffer = {};
            XMStoreFloat4x4(&obj_cbuffer.world, XMMatrixTranspose(world));
            XMStoreFloat4x4(&obj_cbuffer.tex_transform, XMMatrixTranspose(tex_transform));

            uint8_t * obj_ptr = render_ctx->frame_resources[frame_index].obj_cb_data_ptr + ((UINT64)obj_index * cbuffer_size);
            memcpy(obj_ptr, &obj_cbuffer, cbuffer_size);

            // Next FrameResource need to be updated too.
            render_ctx->all_ritems.ritems[i].n_frames_dirty--;
        }
    }
}
static void
update_mat_cbuffers (D3DRenderContext * render_ctx) {
    UINT frame_index = render_ctx->frame_index;
    UINT cbuffer_size = sizeof(MaterialConstants);
    for (int i = 0; i < _COUNT_MATERIAL; ++i) {
        // Only update the cbuffer data if the constants have changed.  If the cbuffer
        // data changes, it needs to be updated for each FrameResource.
        Material * mat = &render_ctx->materials[i];
        if (mat->n_frames_dirty > 0) {
            XMMATRIX mat_transform = XMLoadFloat4x4(&mat->mat_transform);

            MaterialConstants mat_constants;
            mat_constants.diffuse_albedo = render_ctx->materials[i].diffuse_albedo;
            mat_constants.fresnel_r0 = render_ctx->materials[i].fresnel_r0;
            mat_constants.roughness = render_ctx->materials[i].roughness;
            XMStoreFloat4x4(&mat_constants.mat_transform, XMMatrixTranspose(mat_transform));

            uint8_t * mat_ptr = render_ctx->frame_resources[frame_index].mat_cb_data_ptr + ((UINT64)mat->mat_cbuffer_index * cbuffer_size);
            memcpy(mat_ptr, &mat_constants, cbuffer_size);

            // Next FrameResource need to be updated too.
            mat->n_frames_dirty--;
        }
    }
}
static void
update_pass_cbuffers (D3DRenderContext * render_ctx, GameTimer * timer) {

    XMMATRIX view = XMLoadFloat4x4(&global_scene_ctx.view);
    XMMATRIX proj = XMLoadFloat4x4(&global_scene_ctx.proj);

    XMMATRIX view_proj = XMMatrixMultiply(view, proj);
    XMVECTOR det_view = XMMatrixDeterminant(view);
    XMMATRIX inv_view = XMMatrixInverse(&det_view, view);
    XMVECTOR det_proj = XMMatrixDeterminant(proj);
    XMMATRIX inv_proj = XMMatrixInverse(&det_proj, proj);
    XMVECTOR det_view_proj = XMMatrixDeterminant(view_proj);
    XMMATRIX inv_view_proj = XMMatrixInverse(&det_view_proj, view_proj);

    XMStoreFloat4x4(&render_ctx->main_pass_constants.view, XMMatrixTranspose(view));
    XMStoreFloat4x4(&render_ctx->main_pass_constants.inverse_view, XMMatrixTranspose(inv_view));
    XMStoreFloat4x4(&render_ctx->main_pass_constants.proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&render_ctx->main_pass_constants.inverse_proj, XMMatrixTranspose(inv_proj));
    XMStoreFloat4x4(&render_ctx->main_pass_constants.view_proj, XMMatrixTranspose(view_proj));
    XMStoreFloat4x4(&render_ctx->main_pass_constants.inverse_view_proj, XMMatrixTranspose(inv_view_proj));
    render_ctx->main_pass_constants.eye_posw = global_scene_ctx.eye_pos;

    render_ctx->main_pass_constants.render_target_size = XMFLOAT2((float)global_scene_ctx.width, (float)global_scene_ctx.height);
    render_ctx->main_pass_constants.inverse_render_target_size = XMFLOAT2(1.0f / global_scene_ctx.width, 1.0f / global_scene_ctx.height);
    render_ctx->main_pass_constants.nearz = 1.0f;
    render_ctx->main_pass_constants.farz = 1000.0f;
    render_ctx->main_pass_constants.delta_time = timer->delta_time;
    render_ctx->main_pass_constants.total_time = Timer_GetTotalTime(timer);
    render_ctx->main_pass_constants.ambient_light = {.25f, .25f, .35f, 1.0f};

    render_ctx->main_pass_constants.lights[0].direction = {0.57735f, -0.57735f, 0.57735f};
    render_ctx->main_pass_constants.lights[0].strength = {0.6f, 0.6f, 0.6f};
    render_ctx->main_pass_constants.lights[1].direction = {-0.57735f, -0.57735f, 0.57735f};
    render_ctx->main_pass_constants.lights[1].strength = {0.3f, 0.3f, 0.3f};
    render_ctx->main_pass_constants.lights[2].direction = {0.0f, -0.707f, -0.707f};
    render_ctx->main_pass_constants.lights[2].strength = {0.15f, 0.15f, 0.15f};

    uint8_t * pass_ptr = render_ctx->frame_resources[render_ctx->frame_index].pass_cb_data_ptr;
    memcpy(pass_ptr, &render_ctx->main_pass_constants, sizeof(PassConstants));
}
static void
animate_material (Material * mat, GameTimer * timer) {
    // Scroll the water material texture coordinates.
    float& tu = mat->mat_transform(3, 0);
    float& tv = mat->mat_transform(3, 1);

    tu += 0.1f * timer->delta_time;
    tv += 0.02f * timer->delta_time;

    if (tu >= 1.0f)
        tu -= 1.0f;

    if (tv >= 1.0f)
        tv -= 1.0f;

    mat->mat_transform(3, 0) = tu;
    mat->mat_transform(3, 1) = tv;

    // Material has changed, so need to update cbuffer.
    mat->n_frames_dirty = NUM_QUEUING_FRAMES;
}
static int
rand_int (int a, int b) {
    return a + rand() % ((b - a) + 1);
}
// Returns random float in [0, 1).
static float
rand_float () {
    return (float)(rand()) / (float)RAND_MAX;
}
// Returns random float in [a, b).
static float
rand_float (float a, float b) {
    return a + rand_float() * (b - a);
}
static void
update_waves_vb (Waves * waves, D3DRenderContext * render_ctx, GameTimer * timer) {
    float total_time = Timer_GetTotalTime(timer);
    float delta_time = timer->delta_time;

    // Every quarter second, generate a random wave.
    static float t_base = 0.0f;
    if ((total_time - t_base) >= 0.25f) {
        t_base += 0.25f;

        int i = rand_int(4, waves->nrow - 5);
        int j = rand_int(4, waves->ncol - 5);

        float r = rand_float(0.2f, 0.5f);

        Waves_Disturb(waves, i, j, r);
    }

    // Update the wave simulation.
    XMFLOAT3 * temp = (XMFLOAT3 *)::malloc(sizeof(XMFLOAT3) * waves->nvtx);
    Waves_Update(waves, delta_time, temp);
    ::free(temp);

    // Update the wave vertex buffer with the new solution.
    UINT frame_index = render_ctx->frame_index;
    uint8_t * wave_ptr = render_ctx->frame_resources[frame_index].waves_vb_data_ptr;

    //D3D12_RANGE mem_range = {};
    //mem_range.Begin = 0;
    //mem_range.End = 0;
    //CHECK_AND_FAIL(
    //    render_ctx->frame_resources[frame_index].waves_vb->Map(0, &mem_range, reinterpret_cast<void**>(&wave_ptr))
    //);

    UINT v_size = (UINT64)sizeof(Vertex);
    for (int i = 0; i < waves->nvtx; ++i) {
        Vertex v;

        v.position = Waves_GetPosition(waves, i);
        v.normal = waves->normal[i];

            // Derive tex-coords from position by 
        // mapping [-w/2,w/2] --> [0,1]
        v.texc.x = 0.5f + v.position.x / waves->width;
        v.texc.y = 0.5f - v.position.z / waves->depth;

        ::memcpy(wave_ptr + (UINT64)i * v_size, &v, v_size);
    }
    // NOTE(omid): We did the upload_buffer mapping to data pointer (when creating the upload_buffer)

    // Set the dynamic VB of the wave renderitem to the current frame VB.
    render_ctx->all_ritems.ritems[RITEM_WATER].geometry->vb_gpu = render_ctx->frame_resources[frame_index].waves_vb;
}
static HRESULT
move_to_next_frame (D3DRenderContext * render_ctx, UINT * out_frame_index, UINT * out_backbuffer_index) {

    HRESULT ret = E_FAIL;
    UINT frame_index = *out_frame_index;

    // -- 1. schedule a signal command in the queue
    UINT64 const current_fence_value = render_ctx->frame_resources[frame_index].fence;
    ret = render_ctx->cmd_queue->Signal(render_ctx->fence, current_fence_value);
    CHECK_AND_FAIL(ret);

    // -- 2. update frame index
    *out_backbuffer_index = render_ctx->swapchain3->GetCurrentBackBufferIndex();
    *out_frame_index = (render_ctx->frame_index + 1) % NUM_QUEUING_FRAMES;

    // -- 3. if the next frame is not ready to be rendered yet, wait until it is ready
    if (render_ctx->fence->GetCompletedValue() < render_ctx->frame_resources[frame_index].fence) {
        ret = render_ctx->fence->SetEventOnCompletion(render_ctx->frame_resources[frame_index].fence, render_ctx->fence_event);
        CHECK_AND_FAIL(ret);
        WaitForSingleObjectEx(render_ctx->fence_event, INFINITE /*return only when the object is signaled*/, false);
    }

    // -- 3. set the fence value for the next frame
    render_ctx->frame_resources[frame_index].fence = current_fence_value + 1;

    return ret;
}
static HRESULT
wait_for_gpu (D3DRenderContext * render_ctx) {
    HRESULT ret = E_FAIL;

    /*UINT frame_index = render_ctx->frame_index;*/

    // Do it for all queuing frames to do a thorough cleanup

    for (unsigned i = 0; i < NUM_QUEUING_FRAMES; i++) {

        // -- 1. schedule a signal command in the queue
        ret = render_ctx->cmd_queue->Signal(render_ctx->fence, render_ctx->frame_resources[i].fence);
        CHECK_AND_FAIL(ret);

        // -- 2. wait until the fence has been processed
        ret = render_ctx->fence->SetEventOnCompletion(render_ctx->frame_resources[i].fence, render_ctx->fence_event);
        CHECK_AND_FAIL(ret);
        WaitForSingleObjectEx(render_ctx->fence_event, INFINITE /*return only when the object is signaled*/, false);

        // -- 3. increment fence value for the current frame
        ++render_ctx->frame_resources[i].fence;
    }
    return ret;
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
draw_main (D3DRenderContext * render_ctx) {
    HRESULT ret = E_FAIL;
    UINT frame_index = render_ctx->frame_index;
    UINT backbuffer_index = render_ctx->backbuffer_index;

    // Populate command list

    // -- reset cmd_allocator and cmd_list

    // Command list allocators can only be reset when the associated 
    // command lists have finished execution on the GPU; apps should use 
    // fences to determine GPU execution progress.
    render_ctx->frame_resources[frame_index].cmd_list_alloc->Reset();

    // However, when ExecuteCommandList() is called on a particular command 
    // list, that command list can then be reset at any time and must be before 
    // re-recording.
    ret = render_ctx->direct_cmd_list->Reset(render_ctx->frame_resources[frame_index].cmd_list_alloc, render_ctx->psos[OPAQUE_LAYER]);
    CHECK_AND_FAIL(ret);

    // -- set viewport and scissor
    render_ctx->direct_cmd_list->RSSetViewports(1, &render_ctx->viewport);
    render_ctx->direct_cmd_list->RSSetScissorRects(1, &render_ctx->scissor_rect);

    // -- indicate that the backbuffer will be used as the render target
    D3D12_RESOURCE_BARRIER barrier1 = create_barrier(render_ctx->render_targets[backbuffer_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    render_ctx->direct_cmd_list->ResourceBarrier(1, &barrier1);

    // -- get CPU descriptor handle that represents the start of the rtv heap
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = render_ctx->dsv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = render_ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv_handle.ptr = SIZE_T(INT64(rtv_handle.ptr) + INT64(render_ctx->backbuffer_index) * INT64(render_ctx->rtv_descriptor_size));    // -- apply initial offset
    float clear_colors [] = {0.2f, 0.3f, 0.5f, 1.0f};
    render_ctx->direct_cmd_list->ClearRenderTargetView(rtv_handle, clear_colors, 0, nullptr);
    render_ctx->direct_cmd_list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    render_ctx->direct_cmd_list->OMSetRenderTargets(1, &rtv_handle, true, &dsv_handle);

    ID3D12DescriptorHeap* descriptor_heaps [] = {render_ctx->srv_heap};
    render_ctx->direct_cmd_list->SetDescriptorHeaps(_countof(descriptor_heaps), descriptor_heaps);

    render_ctx->direct_cmd_list->SetGraphicsRootSignature(render_ctx->root_signature);

    // Bind per-pass constant buffer.  We only need to do this once per-pass.
    ID3D12Resource * pass_cb = render_ctx->frame_resources[frame_index].pass_cb;
    render_ctx->direct_cmd_list->SetGraphicsRootConstantBufferView(2, pass_cb->GetGPUVirtualAddress());

    draw_render_items(
        render_ctx->direct_cmd_list,
        render_ctx->frame_resources[frame_index].obj_cb,
        render_ctx->frame_resources[frame_index].mat_cb,
        render_ctx->cbv_srv_uav_descriptor_size,
        render_ctx->srv_heap,
        &render_ctx->all_ritems, frame_index
    );

    // -- indicate that the backbuffer will now be used to present
    D3D12_RESOURCE_BARRIER barrier2 = create_barrier(render_ctx->render_targets[backbuffer_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    render_ctx->direct_cmd_list->ResourceBarrier(1, &barrier2);

    // -- finish populating command list
    render_ctx->direct_cmd_list->Close();

    ID3D12CommandList * cmd_lists [] = {render_ctx->direct_cmd_list};
    render_ctx->cmd_queue->ExecuteCommandLists(ARRAY_COUNT(cmd_lists), cmd_lists);

    render_ctx->swapchain->Present(1 /*sync interval*/, 0 /*present flag*/);

    return ret;
}
static void
RenderContext_Init (D3DRenderContext * render_ctx) {
    SIMPLE_ASSERT(render_ctx, "render-ctx not valid");

    memset(render_ctx, 0, sizeof(D3DRenderContext));

    render_ctx->viewport.TopLeftX = 0;
    render_ctx->viewport.TopLeftY = 0;
    render_ctx->viewport.Width = (float)global_scene_ctx.width;
    render_ctx->viewport.Height = (float)global_scene_ctx.height;
    render_ctx->viewport.MinDepth = 0.0f;
    render_ctx->viewport.MaxDepth = 1.0f;
    render_ctx->scissor_rect.left = 0;
    render_ctx->scissor_rect.top = 0;
    render_ctx->scissor_rect.right = global_scene_ctx.width;
    render_ctx->scissor_rect.bottom = global_scene_ctx.height;

    // -- initialize light data
    render_ctx->main_pass_constants.lights[0].strength = {.5f,.5f,.5f};
    render_ctx->main_pass_constants.lights[0].falloff_start = 1.0f;
    render_ctx->main_pass_constants.lights[0].direction = {0.0f, -1.0f, 0.0f};
    render_ctx->main_pass_constants.lights[0].falloff_end = 10.0f;
    render_ctx->main_pass_constants.lights[0].position = {0.0f, 0.0f, 0.0f};
    render_ctx->main_pass_constants.lights[0].spot_power = 64.0f;

    render_ctx->main_pass_constants.lights[1].strength = {.5f,.5f,.5f};
    render_ctx->main_pass_constants.lights[1].falloff_start = 1.0f;
    render_ctx->main_pass_constants.lights[1].direction = {0.0f, -1.0f, 0.0f};
    render_ctx->main_pass_constants.lights[1].falloff_end = 10.0f;
    render_ctx->main_pass_constants.lights[1].position = {0.0f, 0.0f, 0.0f};
    render_ctx->main_pass_constants.lights[1].spot_power = 64.0f;

    render_ctx->main_pass_constants.lights[2].strength = {.5f,.5f,.5f};
    render_ctx->main_pass_constants.lights[2].falloff_start = 1.0f;
    render_ctx->main_pass_constants.lights[2].direction = {0.0f, -1.0f, 0.0f};
    render_ctx->main_pass_constants.lights[2].falloff_end = 10.0f;
    render_ctx->main_pass_constants.lights[2].position = {0.0f, 0.0f, 0.0f};
    render_ctx->main_pass_constants.lights[2].spot_power = 64.0f;

}
static LRESULT CALLBACK
main_win_cb (HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    LRESULT ret = {};
    switch (uMsg) {
        /* WM_PAINT is not handled for now ...
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
    //case WM_CLOSE: {
    //    global_running = false;
    //    PostQuitMessage(0);
    //    //DestroyWindow(hwnd);
    //    ret = 0;
    //} break;
    case WM_DESTROY: {
        global_running = false;
        //PostQuitMessage(0);
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
        "3D Waves Blending app",               // Window title
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
    global_scene_ctx.theta = 1.5f * XM_PI;
    global_scene_ctx.phi = XM_PIDIV2 - 0.1f;
    global_scene_ctx.radius = 50.0f;
    global_scene_ctx.sun_theta = 1.25f * XM_PI;
    global_scene_ctx.sun_phi = XM_PIDIV4;
    global_scene_ctx.aspect_ratio = (float)global_scene_ctx.width / (float)global_scene_ctx.height;
    global_scene_ctx.eye_pos = {0.0f, 0.0f, 0.0f};
    global_scene_ctx.view = Identity4x4();
    XMMATRIX p = DirectX::XMMatrixPerspectiveFovLH(0.25f * XM_PI, global_scene_ctx.aspect_ratio, 1.0f, 1000.0f);
    XMStoreFloat4x4(&global_scene_ctx.proj, p);

    D3DRenderContext * render_ctx = (D3DRenderContext *)::malloc(sizeof(D3DRenderContext));
    RenderContext_Init(render_ctx);

    // Waves Initial Setup
    uint32_t const nrow = 128;
    uint32_t const ncols = 128;
    uint32_t const N_VTX = nrow * ncols;
    size_t wave_size = Waves_CalculateRequiredSize(nrow, ncols);
    BYTE * wave_memory = (BYTE *)::malloc(wave_size);
    Waves * waves = Waves_Init(wave_memory, nrow, ncols, 1.0f, 0.03f, 4.0f, 0.2f);

    // Query Adapter (PhysicalDevice)
    IDXGIFactory * dxgi_factory = nullptr;
    CHECK_AND_FAIL(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&dxgi_factory)));

    uint32_t const MaxAdapters = 8;
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
    auto res = D3D12CreateDevice(adapters[0], D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&render_ctx->device));
    CHECK_AND_FAIL(res);

    // Release adaptors
    for (unsigned i = 0; i < MaxAdapters; ++i) {
        if (adapters[i] != nullptr) {
            adapters[i]->Release();
        }
    }
    // store CBV_SRV_UAV descriptor increment size for later
    render_ctx->cbv_srv_uav_descriptor_size = render_ctx->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

#pragma region Create Command Objects
    // Create Command Queue
    D3D12_COMMAND_QUEUE_DESC cmd_q_desc = {};
    cmd_q_desc.Type = D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT;
    cmd_q_desc.Flags = D3D12_COMMAND_QUEUE_FLAGS::D3D12_COMMAND_QUEUE_FLAG_NONE;
    render_ctx->device->CreateCommandQueue(&cmd_q_desc, IID_PPV_ARGS(&render_ctx->cmd_queue));

    // Create Command Allocator
    render_ctx->device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&render_ctx->direct_cmd_list_alloc)
    );

    // Create Command List
    if (render_ctx->direct_cmd_list_alloc) {
        render_ctx->device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            render_ctx->direct_cmd_list_alloc,
            render_ctx->psos[OPAQUE_LAYER], IID_PPV_ARGS(&render_ctx->direct_cmd_list)
        );

        // Reset the command list to prep for initialization commands.
        // NOTE(omid): Command list needs to be closed before calling Reset.
        render_ctx->direct_cmd_list->Close();
        render_ctx->direct_cmd_list->Reset(render_ctx->direct_cmd_list_alloc, nullptr);
    }
#pragma endregion

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
    swapchain_desc.BufferCount = NUM_BACKBUFFERS;
    swapchain_desc.OutputWindow = hwnd;
    swapchain_desc.Windowed = TRUE;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchain_desc.Flags = DXGI_SWAP_CHAIN_FLAG::DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    if (render_ctx->cmd_queue)
        dxgi_factory->CreateSwapChain(render_ctx->cmd_queue, &swapchain_desc, &render_ctx->swapchain);

    // -- to get current backbuffer index
    CHECK_AND_FAIL(render_ctx->swapchain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&render_ctx->swapchain3));
    render_ctx->backbuffer_index = render_ctx->swapchain3->GetCurrentBackBufferIndex();

// ========================================================================================================
#pragma region Load Textures
    // crate
    strcpy_s(render_ctx->textures[TEX_CRATE01].name, "woodcrate01");
    wcscpy_s(render_ctx->textures[TEX_CRATE01].filename, L"../Textures/WoodCrate02.dds");
    load_texture(
        render_ctx->device, render_ctx->direct_cmd_list,
        render_ctx->textures[TEX_CRATE01].filename, &render_ctx->textures[TEX_CRATE01]
    );
    // water
    strcpy_s(render_ctx->textures[TEX_WATER].name, "watertex");
    wcscpy_s(render_ctx->textures[TEX_WATER].filename, L"../Textures/water1.dds");
    load_texture(
        render_ctx->device, render_ctx->direct_cmd_list,
        render_ctx->textures[TEX_WATER].filename, &render_ctx->textures[TEX_WATER]
    );
    // grass
    strcpy_s(render_ctx->textures[TEX_GRASS].name, "grasstex");
    wcscpy_s(render_ctx->textures[TEX_GRASS].filename, L"../Textures/grass.dds");
    load_texture(
        render_ctx->device, render_ctx->direct_cmd_list,
        render_ctx->textures[TEX_GRASS].filename, &render_ctx->textures[TEX_GRASS]
    );
#pragma endregion

    create_descriptor_heaps(render_ctx);

#pragma region Dsv_Creation
// Create the depth/stencil buffer and view.
    D3D12_RESOURCE_DESC ds_desc;
    ds_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    ds_desc.Alignment = 0;
    ds_desc.Width = global_scene_ctx.width;
    ds_desc.Height = global_scene_ctx.height;
    ds_desc.DepthOrArraySize = 1;
    ds_desc.MipLevels = 1;

    // NOTE(omid): SSAO requires an SRV to the depth buffer to read from 
    // the depth buffer.  Therefore, because we need to create two views to the same resource:
    //   1. SRV format: DXGI_FORMAT_R24_UNORM_X8_TYPELESS
    //   2. DSV Format: DXGI_FORMAT_D24_UNORM_S8_UINT
    // we need to create the depth buffer resource with a typeless format.  
    ds_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;

    ds_desc.SampleDesc.Count = 1;
    ds_desc.SampleDesc.Quality = 0;
    ds_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ds_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES ds_heap_props = {};
    ds_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    ds_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    ds_heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    ds_heap_props.CreationNodeMask = 1;
    ds_heap_props.VisibleNodeMask = 1;

    D3D12_CLEAR_VALUE opt_clear;
    opt_clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    opt_clear.DepthStencil.Depth = 1.0f;
    opt_clear.DepthStencil.Stencil = 0;
    render_ctx->device->CreateCommittedResource(
        &ds_heap_props,
        D3D12_HEAP_FLAG_NONE,
        &ds_desc,
        D3D12_RESOURCE_STATE_COMMON,
        &opt_clear,
        IID_PPV_ARGS(&render_ctx->depth_stencil_buffer)
    );

    // Create descriptor to mip level 0 of entire resource using the format of the resource.
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
    dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.Texture2D.MipSlice = 0;
    render_ctx->device->CreateDepthStencilView(
        render_ctx->depth_stencil_buffer,
        &dsv_desc,
        render_ctx->dsv_heap->GetCPUDescriptorHandleForHeapStart()
    );
#pragma endregion Dsv_Creation

#pragma region Rtv_Creation
    // -- create frame resources: rtv, cmd-allocator and cbuffers for each frame
    render_ctx->rtv_descriptor_size = render_ctx->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle_start = render_ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < NUM_BACKBUFFERS; ++i) {
        CHECK_AND_FAIL(render_ctx->swapchain3->GetBuffer(i, IID_PPV_ARGS(&render_ctx->render_targets[i])));

        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = {};
        cpu_handle.ptr = rtv_handle_start.ptr + ((UINT64)i * render_ctx->rtv_descriptor_size);
        // -- create a rtv for each frame
        render_ctx->device->CreateRenderTargetView(render_ctx->render_targets[i], nullptr, cpu_handle);
    }
#pragma endregion Rtv_Creation

#pragma region Create CBuffers and Dynamic Vertex Buffer (waves_vb)
    UINT obj_cb_size = sizeof(ObjectConstants);
    UINT mat_cb_size = sizeof(MaterialConstants);
    UINT pass_cb_size = sizeof(PassConstants);
    UINT vertex_size = sizeof(Vertex);
    for (UINT i = 0; i < NUM_QUEUING_FRAMES; ++i) {
        // -- create a cmd-allocator for each frame
        res = render_ctx->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&render_ctx->frame_resources[i].cmd_list_alloc));

        // -- create cbuffers as upload_buffer
        create_upload_buffer(render_ctx->device, (UINT64)obj_cb_size * _COUNT_RENDERITEM, &render_ctx->frame_resources[i].obj_cb_data_ptr, &render_ctx->frame_resources[i].obj_cb);
        // Initialize cb data
        ::memcpy(render_ctx->frame_resources[i].obj_cb_data_ptr, &render_ctx->frame_resources[i].obj_cb_data, sizeof(render_ctx->frame_resources[i].obj_cb_data));

        create_upload_buffer(render_ctx->device, (UINT64)mat_cb_size * _COUNT_MATERIAL, &render_ctx->frame_resources[i].mat_cb_data_ptr, &render_ctx->frame_resources[i].mat_cb);
        // Initialize cb data
        ::memcpy(render_ctx->frame_resources[i].mat_cb_data_ptr, &render_ctx->frame_resources[i].mat_cb_data, sizeof(render_ctx->frame_resources[i].mat_cb_data));

        create_upload_buffer(render_ctx->device, pass_cb_size * 1, &render_ctx->frame_resources[i].pass_cb_data_ptr, &render_ctx->frame_resources[i].pass_cb);
        // Initialize cb data
        ::memcpy(render_ctx->frame_resources[i].pass_cb_data_ptr, &render_ctx->frame_resources[i].pass_cb_data, sizeof(render_ctx->frame_resources[i].pass_cb_data));

        create_upload_buffer(render_ctx->device, (UINT64)vertex_size * N_VTX, &render_ctx->frame_resources[i].waves_vb_data_ptr, &render_ctx->frame_resources[i].waves_vb);
        // Initialize cb data
        ::memcpy(render_ctx->frame_resources[i].waves_vb_data_ptr, &render_ctx->frame_resources[i].waves_vb_data, sizeof(render_ctx->frame_resources[i].waves_vb_data));
    }
#pragma endregion

    // ========================================================================================================
#pragma region Root_Signature_Creation
    create_root_signature(render_ctx->device, &render_ctx->root_signature);
#pragma endregion Root_Signature_Creation

    // Load and compile shaders

#pragma region Compile_Shaders
// -- using DXC shader compiler [from https://asawicki.info/news_1719_two_shader_compilers_of_direct3d_12]

    IDxcLibrary * dxc_lib = nullptr;
    HRESULT hr = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&dxc_lib));
    // if (FAILED(hr)) Handle error
    IDxcCompiler * dxc_compiler = nullptr;
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc_compiler));
    // if (FAILED(hr)) Handle error

    wchar_t const * shaders_path = L"./shaders/default.hlsl";
    uint32_t code_page = CP_UTF8;
    IDxcBlobEncoding * shader_blob = nullptr;
    IDxcOperationResult * dxc_res = nullptr;
    IDxcBlob * vertex_shader_code = nullptr;
    IDxcBlob * pixel_shader_code = nullptr;
    hr = dxc_lib->CreateBlobFromFile(shaders_path, &code_page, &shader_blob);
    if (shader_blob) {
        IDxcIncludeHandler * include_handler = nullptr;
        dxc_lib->CreateIncludeHandler(&include_handler);
        hr = dxc_compiler->Compile(shader_blob, shaders_path, L"VertexShader_Main", L"vs_6_0", nullptr, 0, nullptr, 0, include_handler, &dxc_res);
        dxc_res->GetStatus(&hr);
        dxc_res->GetResult(&vertex_shader_code);
        hr = dxc_compiler->Compile(shader_blob, shaders_path, L"PixelShader_Main", L"ps_6_0", nullptr, 0, nullptr, 0, include_handler, &dxc_res);
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

#pragma endregion Compile_Shaders

#pragma region PSO_Creation
    create_pso(render_ctx, vertex_shader_code, pixel_shader_code, pixel_shader_code);
#pragma endregion PSO_Creation

#pragma region Shapes_And_Renderitem_Creation
    create_land_geometry(render_ctx);
    create_water_geometry(waves->nrow, waves->ncol, waves->ntri, render_ctx);

    create_shape_geometry(render_ctx);
    create_materials(render_ctx->materials);
    create_render_items(
        &render_ctx->all_ritems,
        &render_ctx->opaque_ritems,
        &render_ctx->transparent_ritems,
        &render_ctx->alphatested_ritems,
        &render_ctx->geom[GEOM_BOX],
        &render_ctx->geom[GEOM_WATER],
        &render_ctx->geom[GEOM_GRID],
        render_ctx->materials
    );

#pragma endregion Shapes_And_Renderitem_Creation

    // NOTE(omid): Before closing/executing command list specify the depth-stencil-buffer transition from its initial state to be used as a depth buffer.
    D3D12_RESOURCE_BARRIER ds_barrier = create_barrier(render_ctx->depth_stencil_buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    render_ctx->direct_cmd_list->ResourceBarrier(1, &ds_barrier);

    // -- close the command list and execute it to begin inital gpu setup
    CHECK_AND_FAIL(render_ctx->direct_cmd_list->Close());
    ID3D12CommandList * cmd_lists [] = {render_ctx->direct_cmd_list};
    render_ctx->cmd_queue->ExecuteCommandLists(ARRAY_COUNT(cmd_lists), cmd_lists);

    //----------------
    // Create fence
    // create synchronization objects and wait until assets have been uploaded to the GPU.

    UINT frame_index = render_ctx->frame_index;
    CHECK_AND_FAIL(render_ctx->device->CreateFence(render_ctx->frame_resources[frame_index].fence, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&render_ctx->fence)));

    ++render_ctx->frame_resources[frame_index].fence;

    // Create an event handle to use for frame synchronization.
    render_ctx->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (nullptr == render_ctx->fence_event) {
        // map the error code to an HRESULT value.
        CHECK_AND_FAIL(HRESULT_FROM_WIN32(GetLastError()));
    }

    // Wait for the command list to execute; we are reusing the same command 
    // list in our main loop but for now, we just want to wait for setup to 
    // complete before continuing.
    CHECK_AND_FAIL(wait_for_gpu(render_ctx));

#pragma endregion Initialization

    // ========================================================================================================
#pragma region Main_Loop
    global_running = true;
    Timer_Init(&global_timer);
    Timer_Reset(&global_timer);
    while (global_running) {
        MSG msg = {};
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        Timer_Tick(&global_timer);

        animate_material(&render_ctx->materials[MAT_WATER], &global_timer);

        handle_keyboard_input(&global_scene_ctx, &global_timer);
        update_camera(&global_scene_ctx);
        update_pass_cbuffers(render_ctx, &global_timer);
        update_mat_cbuffers(render_ctx);
        update_obj_cbuffers(render_ctx);

        update_waves_vb(waves, render_ctx, &global_timer);

        CHECK_AND_FAIL(draw_main(render_ctx));

        CHECK_AND_FAIL(move_to_next_frame(render_ctx, &render_ctx->frame_index, &render_ctx->backbuffer_index));
    }
#pragma endregion Main_Loop

    // ========================================================================================================
#pragma region Cleanup_And_Debug
    CHECK_AND_FAIL(wait_for_gpu(render_ctx));

    CloseHandle(render_ctx->fence_event);

    render_ctx->fence->Release();

    // release queuing frame resources
    for (size_t i = 0; i < NUM_QUEUING_FRAMES; i++) {
        render_ctx->frame_resources[i].obj_cb->Unmap(0, nullptr);
        render_ctx->frame_resources[i].mat_cb->Unmap(0, nullptr);
        render_ctx->frame_resources[i].pass_cb->Unmap(0, nullptr);
        render_ctx->frame_resources[i].waves_vb->Unmap(0, nullptr);
        render_ctx->frame_resources[i].obj_cb->Release();
        render_ctx->frame_resources[i].mat_cb->Release();
        render_ctx->frame_resources[i].pass_cb->Release();
        render_ctx->frame_resources[i].waves_vb->Release();

        render_ctx->frame_resources[i].cmd_list_alloc->Release();
    }
    for (unsigned i = 0; i < _COUNT_GEOM; i++) {
        render_ctx->geom[i].ib_uploader->Release();
        if (i != GEOM_WATER)    // water uses a dynamic vb
            render_ctx->geom[i].vb_uploader->Release();

        render_ctx->geom[i].ib_gpu->Release();
        render_ctx->geom[i].vb_gpu->Release();
    }

    for (int i = 0; i < _COUNT_RENDER_LAYER; ++i) {
        render_ctx->psos[i]->Release();
    }

    pixel_shader_code->Release();
    vertex_shader_code->Release();

    render_ctx->root_signature->Release();

    // release swapchain backbuffers resources
    for (unsigned i = 0; i < NUM_BACKBUFFERS; ++i) {
        render_ctx->render_targets[i]->Release();
    }

    render_ctx->dsv_heap->Release();
    render_ctx->rtv_heap->Release();
    render_ctx->srv_heap->Release();

    render_ctx->depth_stencil_buffer->Release();

    for (unsigned i = 0; i < _COUNT_TEX; i++) {
        render_ctx->textures[i].upload_heap->Release();
        render_ctx->textures[i].resource->Release();
    }

    render_ctx->swapchain3->Release();
    render_ctx->swapchain->Release();
    render_ctx->direct_cmd_list->Release();
    render_ctx->direct_cmd_list_alloc->Release();
    render_ctx->cmd_queue->Release();
    render_ctx->device->Release();
    dxgi_factory->Release();

    ::free(wave_memory);

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

        // -- consume var to avoid warning
        dxgiGetDebugInterface = dxgiGetDebugInterface;
    }
#pragma endregion Cleanup_And_Debug

    return 0;
}
