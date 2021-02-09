
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

#include "./headers/dynarray.h"
#include "utils.h"

// Currently we overload the meaning of FrameCount to mean both the maximum
// number of frames that will be queued to the GPU at a time, as well as the number
// of back buffers in the DXGI swap chain. For the majority of applications, this
// is convenient and works well. However, there will be certain cases where an
// application may want to queue up more frames than there are back buffers
// available.
// It should be noted that excessive buffering of frames dependent on user input
// may result in noticeable latency in your app.
#define FRAME_COUNT     2   // TODO(omid): check the inconsistant frame_count = 3 error after final gpu waiting, on "texture->release"
// TODO(omid): should FRAME_COUNT be SWAPCHAIN_BUFFER_COUNT? 
// TODO(omid): NUM_FRAME_RESOURCE = FRAME_COUNT ?
#define NUM_FRAME_RESOURCES     FRAME_COUNT
// 
#define MAX_RENDERITEM_COUNT    50
// TODO(omid): store render_items_count at run-time
#define OBJ_COUNT   22

enum SUBMESH_INDEX {
    _BOX_ID,
    _GRID_ID,
    _SPHERE_ID,
    _CYLINDER_ID
};

struct SceneContext {
    // camera settings (spherical coordinate)
    float theta;
    float phi;
    float radius;

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

bool global_running;
SceneContext global_scene_ctx;

struct D3DRenderContext {
    // Pipeline stuff
    D3D12_VIEWPORT                  viewport;
    D3D12_RECT                      scissor_rect;
    IDXGISwapChain3 *               swapchain3;
    IDXGISwapChain *                swapchain;
    ID3D12Device *                  device;
    ID3D12CommandQueue *            cmd_queue;
    ID3D12RootSignature *           root_signature;
    ID3D12PipelineState *           pso;
    ID3D12GraphicsCommandList *     direct_cmd_list;
    UINT                            rtv_descriptor_size;
    UINT                            cbv_srv_uav_descriptor_size;

    ID3D12DescriptorHeap *          rtv_heap;
    ID3D12DescriptorHeap *          cbv_heap;

    // App resources
    //ID3D12Resource *                vertex_buffer;
    //ID3D12Resource *                index_buffer;
    //D3D12_VERTEX_BUFFER_VIEW        vb_view;
    //D3D12_INDEX_BUFFER_VIEW         ib_view;
    PassConstantBuffer              main_pass_constants;

    // render items
    //UINT                            obj_count;
    RenderItem                      render_items[MAX_RENDERITEM_COUNT];
    UINT                            pass_cbv_offset;

    // TODO(omid): heap-alloc the mesh_geom and render_items
    MeshGeometry                    geom;

    // Synchronization stuff
    UINT                            frame_index;
    HANDLE                          fence_event;
    ID3D12Fence *                   fence;
    FrameResource                   frame_resources[NUM_FRAME_RESOURCES];

    // TODO(omid): Do we need the followings now? 
    FrameResource * current_frame_resource;
    int current_frame_resource_index;

};
// NOTE(omid): Don't worry this is expermental!
#define _BOX_VTX_CNT   24
#define _BOX_IDX_CNT   36

#define _GRID_VTX_CNT   2400
#define _GRID_IDX_CNT   13806

#define _SPHERE_VTX_CNT   401
#define _SPHERE_IDX_CNT   2280

#define _CYLINDER_VTX_CNT   485
#define _CYLINDER_IDX_CNT   2520

#define _TOTAL_VTX_CNT  (_BOX_VTX_CNT + _GRID_VTX_CNT + _SPHERE_VTX_CNT + _CYLINDER_VTX_CNT)
#define _TOTAL_IDX_CNT  (_BOX_IDX_CNT + _GRID_IDX_CNT + _SPHERE_IDX_CNT + _CYLINDER_IDX_CNT)

static void
create_shape_geometry (BYTE * memory, D3DRenderContext * render_ctx, Vertex vertices [], uint16_t indices []) {

    // box
    UINT bsz = sizeof(GeomVertex) * _BOX_VTX_CNT;
    UINT bsz_id = bsz + sizeof(uint16_t) * _BOX_IDX_CNT;
    // grid
    UINT gsz = bsz_id + sizeof(GeomVertex) * _GRID_VTX_CNT;
    UINT gsz_id = gsz + sizeof(uint16_t) * _GRID_IDX_CNT;
    // sphere
    UINT ssz = gsz_id + sizeof(GeomVertex) * _SPHERE_VTX_CNT;
    UINT ssz_id = ssz + sizeof(uint16_t) * _SPHERE_IDX_CNT;
    // cylinder
    UINT csz = ssz_id + sizeof(GeomVertex) * _CYLINDER_VTX_CNT;
    //UINT csz_id = csz + sizeof(uint16_t) * _CYLINDER_IDX_CNT; // not used

    GeomVertex *    box_vertices = reinterpret_cast<GeomVertex *>(memory);
    uint16_t *      box_indices = reinterpret_cast<uint16_t *>(memory + bsz);
    GeomVertex *    grid_vertices = reinterpret_cast<GeomVertex *>(memory + bsz_id);
    uint16_t *      grid_indices = reinterpret_cast<uint16_t *>(memory + gsz);
    GeomVertex *    sphere_vertices = reinterpret_cast<GeomVertex *>(memory + gsz_id);
    uint16_t *      sphere_indices = reinterpret_cast<uint16_t *>(memory + ssz);
    GeomVertex *    cylinder_vertices = reinterpret_cast<GeomVertex *>(memory + ssz_id);
    uint16_t *      cylinder_indices = reinterpret_cast<uint16_t *>(memory + csz);

    create_box(1.5f, 0.5f, 1.5f, box_vertices, box_indices);
    create_grid(20.0f, 30.0f, 60, 40, grid_vertices, grid_indices);
    create_sphere(0.5f, sphere_vertices, sphere_indices);
    create_cylinder(0.5f, 0.3f, 3.0f, cylinder_vertices, cylinder_indices);

    // We are concatenating all the geometry into one big vertex/index buffer.  So
    // define the regions in the buffer each submesh covers.

    // Cache the vertex offsets to each object in the concatenated vertex buffer.
    UINT box_vertex_offset = 0;
    UINT grid_vertex_offset = _BOX_VTX_CNT;
    UINT sphere_vertex_offset = grid_vertex_offset + _GRID_VTX_CNT;
    UINT cylinder_vertex_offset = sphere_vertex_offset + _SPHERE_VTX_CNT;

    // Cache the starting index for each object in the concatenated index buffer.
    UINT box_index_offset = 0;
    UINT grid_index_offset = _BOX_IDX_CNT;
    UINT sphere_index_offset = grid_index_offset + _GRID_IDX_CNT;
    UINT cylinder_index_offsett = sphere_index_offset + _SPHERE_IDX_CNT;

    // Define the SubmeshGeometry that cover different 
    // regions of the vertex/index buffers.

    SubmeshGeometry box_submesh = {};
    box_submesh.index_count = _BOX_IDX_CNT;
    box_submesh.start_index_location = box_index_offset;
    box_submesh.base_vertex_location = box_vertex_offset;

    SubmeshGeometry grid_submesh = {};
    grid_submesh.index_count = _GRID_IDX_CNT;
    grid_submesh.start_index_location = grid_index_offset;
    grid_submesh.base_vertex_location = grid_vertex_offset;

    SubmeshGeometry sphere_submesh = {};
    sphere_submesh.index_count = _SPHERE_IDX_CNT;
    sphere_submesh.start_index_location = sphere_index_offset;
    sphere_submesh.base_vertex_location = sphere_vertex_offset;

    SubmeshGeometry cylinder_submesh = {};
    cylinder_submesh.index_count = _CYLINDER_IDX_CNT;
    cylinder_submesh.start_index_location = cylinder_index_offsett;
    cylinder_submesh.base_vertex_location = cylinder_vertex_offset;

    // Extract the vertex elements we are interested in and pack the
    // vertices of all the meshes into one vertex buffer.

    UINT k = 0;
    for (size_t i = 0; i < _BOX_VTX_CNT; ++i, ++k) {
        vertices[k].Pos = box_vertices[i].Position;
        vertices[k].Color = XMFLOAT4(DirectX::Colors::Khaki);
    }

    for (size_t i = 0; i < _GRID_VTX_CNT; ++i, ++k) {
        vertices[k].Pos = grid_vertices[i].Position;
        vertices[k].Color = XMFLOAT4(DirectX::Colors::ForestGreen);
    }

    for (size_t i = 0; i < _SPHERE_VTX_CNT; ++i, ++k) {
        vertices[k].Pos = sphere_vertices[i].Position;
        vertices[k].Color = XMFLOAT4(DirectX::Colors::Crimson);
    }

    for (size_t i = 0; i < _CYLINDER_VTX_CNT; ++i, ++k) {
        vertices[k].Pos = cylinder_vertices[i].Position;
        vertices[k].Color = XMFLOAT4(DirectX::Colors::SteelBlue);
    }

    // -- pack indices
    k = 0;
    for (size_t i = 0; i < _BOX_IDX_CNT; ++i, ++k) {
        indices[k] = box_indices[i];
    }

    for (size_t i = 0; i < _GRID_IDX_CNT; ++i, ++k) {
        indices[k] = grid_indices[i];
    }

    for (size_t i = 0; i < _SPHERE_IDX_CNT; ++i, ++k) {
        indices[k] = sphere_indices[i];
    }

    for (size_t i = 0; i < _CYLINDER_IDX_CNT; ++i, ++k) {
        indices[k] = cylinder_indices[i];
    }

    UINT vb_byte_size = _TOTAL_VTX_CNT * sizeof(Vertex);
    UINT ib_byte_size = _TOTAL_IDX_CNT * sizeof(uint16_t);

    // -- Fill out render_ctx geom (output)

    D3DCreateBlob(vb_byte_size, &render_ctx->geom.vb_cpu);
    CopyMemory(render_ctx->geom.vb_cpu->GetBufferPointer(), vertices, vb_byte_size);

    D3DCreateBlob(ib_byte_size, &render_ctx->geom.ib_cpu);
    CopyMemory(render_ctx->geom.ib_cpu->GetBufferPointer(), indices, ib_byte_size);

    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, vertices, vb_byte_size, &render_ctx->geom.vb_uploader, &render_ctx->geom.vb_gpu);
    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, indices, ib_byte_size, &render_ctx->geom.ib_uploader, &render_ctx->geom.ib_gpu);

    render_ctx->geom.vb_byte_stide = sizeof(Vertex);
    render_ctx->geom.vb_byte_size = vb_byte_size;
    render_ctx->geom.ib_byte_size = ib_byte_size;

    render_ctx->geom.submesh_names[_BOX_ID] = "box";
    render_ctx->geom.submesh_geoms[_BOX_ID] = box_submesh;
    render_ctx->geom.submesh_names[_GRID_ID] = "grid";
    render_ctx->geom.submesh_geoms[_GRID_ID] = grid_submesh;
    render_ctx->geom.submesh_names[_SPHERE_ID] = "shpere";
    render_ctx->geom.submesh_geoms[_SPHERE_ID] = sphere_submesh;
    render_ctx->geom.submesh_names[_CYLINDER_ID] = "cylinder";
    render_ctx->geom.submesh_geoms[_CYLINDER_ID] = cylinder_submesh;
}
static void
create_render_items (RenderItem render_items [], MeshGeometry * geom) {
    // NOTE(omid): RenderItems elements 
    /*
        0: box
        1: grid
        2-22: cylinders and spheres
    */

    UINT _curr = 0;

    XMStoreFloat4x4(&render_items[_curr].world, XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(0.0f, 0.5f, 0.0f));
    render_items[_curr].obj_cbuffer_index = _curr;
    render_items[_curr].geometry = geom;
    render_items[_curr].primitive_type = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    render_items[_curr].index_count = geom->submesh_geoms[_BOX_ID].index_count;
    render_items[_curr].start_index_loc = geom->submesh_geoms[_BOX_ID].start_index_location;
    render_items[_curr].base_vertex_loc = geom->submesh_geoms[_BOX_ID].base_vertex_location;
    render_items[_curr].n_frames_dirty = FRAME_COUNT;
    ++_curr;

    render_items[_curr].world = Identity4x4();
    render_items[_curr].obj_cbuffer_index = _curr;
    render_items[_curr].geometry = geom;
    render_items[_curr].primitive_type = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    render_items[_curr].index_count = geom->submesh_geoms[_GRID_ID].index_count;
    render_items[_curr].start_index_loc = geom->submesh_geoms[_GRID_ID].start_index_location;
    render_items[_curr].base_vertex_loc = geom->submesh_geoms[_GRID_ID].base_vertex_location;
    render_items[_curr].n_frames_dirty = FRAME_COUNT;
    ++_curr;

    for (int i = 0; i < 5; ++i) {
        XMMATRIX left_cylinder_world = XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f);
        XMMATRIX right_cylinder_world = XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i * 5.0f);

        XMMATRIX left_sphere_world = XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f);
        XMMATRIX right_sphere_world = XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i * 5.0f);

        XMStoreFloat4x4(&render_items[_curr].world, right_cylinder_world);
        render_items[_curr].obj_cbuffer_index = _curr;
        render_items[_curr].geometry = geom;
        render_items[_curr].primitive_type = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        render_items[_curr].index_count = geom->submesh_geoms[_CYLINDER_ID].index_count;
        render_items[_curr].start_index_loc = geom->submesh_geoms[_CYLINDER_ID].start_index_location;
        render_items[_curr].base_vertex_loc = geom->submesh_geoms[_CYLINDER_ID].base_vertex_location;
        render_items[_curr].n_frames_dirty = FRAME_COUNT;
        ++_curr;

        XMStoreFloat4x4(&render_items[_curr].world, left_cylinder_world);
        render_items[_curr].obj_cbuffer_index = _curr;
        render_items[_curr].geometry = geom;
        render_items[_curr].primitive_type = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        render_items[_curr].index_count = geom->submesh_geoms[_CYLINDER_ID].index_count;
        render_items[_curr].start_index_loc = geom->submesh_geoms[_CYLINDER_ID].start_index_location;
        render_items[_curr].base_vertex_loc = geom->submesh_geoms[_CYLINDER_ID].base_vertex_location;
        render_items[_curr].n_frames_dirty = FRAME_COUNT;
        ++_curr;

        XMStoreFloat4x4(&render_items[_curr].world, left_sphere_world);
        render_items[_curr].obj_cbuffer_index = _curr;
        render_items[_curr].geometry = geom;
        render_items[_curr].primitive_type = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        render_items[_curr].index_count = geom->submesh_geoms[_SPHERE_ID].index_count;
        render_items[_curr].start_index_loc = geom->submesh_geoms[_SPHERE_ID].start_index_location;
        render_items[_curr].base_vertex_loc = geom->submesh_geoms[_SPHERE_ID].base_vertex_location;
        render_items[_curr].n_frames_dirty = FRAME_COUNT;
        ++_curr;

        XMStoreFloat4x4(&render_items[_curr].world, right_sphere_world);
        render_items[_curr].obj_cbuffer_index = _curr;
        render_items[_curr].geometry = geom;
        render_items[_curr].primitive_type = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        render_items[_curr].index_count = geom->submesh_geoms[_SPHERE_ID].index_count;
        render_items[_curr].start_index_loc = geom->submesh_geoms[_SPHERE_ID].start_index_location;
        render_items[_curr].base_vertex_loc = geom->submesh_geoms[_SPHERE_ID].base_vertex_location;
        render_items[_curr].n_frames_dirty = FRAME_COUNT;
        ++_curr;
    }
}
// -- indexed drawing
static void
draw_render_items (
    ID3D12GraphicsCommandList * cmd_list,
    ID3D12DescriptorHeap * cbv_heap,
    UINT64 descriptor_increment_size,
    RenderItem render_items [],
    UINT current_frame_index
) {
    current_frame_index = (current_frame_index + 1) % FRAME_COUNT;
    for (size_t i = 0; i < OBJ_COUNT; ++i) {
        D3D12_VERTEX_BUFFER_VIEW vbv = Mesh_GetVertexBufferView(render_items[i].geometry);
        D3D12_INDEX_BUFFER_VIEW ibv = Mesh_GetIndexBufferView(render_items[i].geometry);
        cmd_list->IASetVertexBuffers(0, 1, &vbv);
        cmd_list->IASetIndexBuffer(&ibv);
        cmd_list->IASetPrimitiveTopology(render_items[i].primitive_type);

        // Offset to the CBV in the descriptor heap for this object and for this frame resource.
        UINT cbv_index = current_frame_index * OBJ_COUNT + render_items[i].obj_cbuffer_index;

        D3D12_GPU_DESCRIPTOR_HANDLE cbv_handle = {};
        cbv_handle.ptr = cbv_heap->GetGPUDescriptorHandleForHeapStart().ptr + cbv_index * descriptor_increment_size;

        cmd_list->SetGraphicsRootDescriptorTable(0, cbv_handle);
        cmd_list->DrawIndexedInstanced(render_items[i].index_count, 1, render_items[i].start_index_loc, render_items[i].base_vertex_loc, 0);
    }
}
static void
create_descriptor_heaps (D3DRenderContext * render_ctx) {

    // Need a CBV descriptor for each object for each frame resource,
    // +1 for the per-pass CBV for each frame resource.
    UINT n_descriptors = (OBJ_COUNT + 1) * FRAME_COUNT;

    // Save an offset to the start of the pass CBVs.  These are the last 3 descriptors.
    render_ctx->pass_cbv_offset = OBJ_COUNT * FRAME_COUNT;

    D3D12_DESCRIPTOR_HEAP_DESC cbv_heap_desc = {};
    cbv_heap_desc.NumDescriptors = n_descriptors;
    cbv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    cbv_heap_desc.NodeMask = 0;
    render_ctx->device->CreateDescriptorHeap(&cbv_heap_desc, IID_PPV_ARGS(&render_ctx->cbv_heap));

    // Create Render Target View Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK_AND_FAIL(render_ctx->device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&render_ctx->rtv_heap)));
}
static void
create_root_signature (ID3D12Device * device, ID3D12RootSignature ** root_signature) {
    D3D12_DESCRIPTOR_RANGE cbv_table0 = {};
    cbv_table0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    cbv_table0.NumDescriptors = 1;
    cbv_table0.BaseShaderRegister = 0;
    cbv_table0.RegisterSpace = 0;
    cbv_table0.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE cbv_table1 = {};
    cbv_table1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    cbv_table1.NumDescriptors = 1;
    cbv_table1.BaseShaderRegister = 1;
    cbv_table1.RegisterSpace = 0;
    cbv_table1.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // Root parameter can be a table, root descriptor or root constants.
    D3D12_ROOT_PARAMETER slot_root_params[2] = {};
    // Create root CBVs.
    slot_root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    slot_root_params[0].DescriptorTable.pDescriptorRanges = &cbv_table0;
    slot_root_params[0].DescriptorTable.NumDescriptorRanges = 1;
    slot_root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    slot_root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    slot_root_params[1].DescriptorTable.pDescriptorRanges = &cbv_table1;
    slot_root_params[1].DescriptorTable.NumDescriptorRanges = 1;
    slot_root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // A root signature is an array of root parameters.
    D3D12_ROOT_SIGNATURE_DESC root_sig_desc = {};
    root_sig_desc.NumParameters = 2;
    root_sig_desc.pParameters = slot_root_params;
    root_sig_desc.NumStaticSamplers = 0;
    root_sig_desc.pStaticSamplers = nullptr;
    root_sig_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
    ID3DBlob * serialized_root_sig = nullptr;
    ID3DBlob * error_blob = nullptr;
    D3D12SerializeRootSignature(&root_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized_root_sig, &error_blob);

    if (error_blob) {
        ::OutputDebugStringA((char*)error_blob->GetBufferPointer());
    }

    device->CreateRootSignature(0, serialized_root_sig->GetBufferPointer(), serialized_root_sig->GetBufferSize(), IID_PPV_ARGS(root_signature));
}
static void
create_pso (D3DRenderContext * render_ctx, IDxcBlob * vertex_shader_code, IDxcBlob * pixel_shader_code) {
    // -- Create vertex-input-layout Elements
    
    D3D12_INPUT_ELEMENT_DESC input_desc[2];
    input_desc[0] = {};
    input_desc[0].SemanticName = "POSITION";
    input_desc[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    input_desc[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    input_desc[1] = {};
    input_desc[1].SemanticName = "COLOR";
    input_desc[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    input_desc[1].AlignedByteOffset = 12; // bc of the position byte-size
    input_desc[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    // -- Create pipeline state object

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
    def_rasterizer_desc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    def_rasterizer_desc.CullMode = D3D12_CULL_MODE_BACK;
    def_rasterizer_desc.FrontCounterClockwise = false;
    def_rasterizer_desc.DepthBias = 0;
    def_rasterizer_desc.DepthBiasClamp = 0.0f;
    def_rasterizer_desc.SlopeScaledDepthBias = 0.0f;
    def_rasterizer_desc.DepthClipEnable = TRUE;
    def_rasterizer_desc.ForcedSampleCount = 0;
    def_rasterizer_desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = render_ctx->root_signature;
    pso_desc.VS.pShaderBytecode = vertex_shader_code->GetBufferPointer();
    pso_desc.VS.BytecodeLength = vertex_shader_code->GetBufferSize();
    pso_desc.PS.pShaderBytecode = pixel_shader_code->GetBufferPointer();
    pso_desc.PS.BytecodeLength = pixel_shader_code->GetBufferSize();
    pso_desc.BlendState = def_blend_desc;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState = def_rasterizer_desc;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso_desc.InputLayout.pInputElementDescs = input_desc;
    pso_desc.InputLayout.NumElements = ARRAY_COUNT(input_desc);
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleDesc.Quality = 0;

    CHECK_AND_FAIL(render_ctx->device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&render_ctx->pso)));
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
        scene_ctx->phi = CLAMP_VALUE(scene_ctx->phi, 0.1f, DirectX::XM_PI - 0.1f);
    } else if ((wParam & MK_RBUTTON) != 0) {
        // make each pixel correspond to a 0.05 unit in scene
        float dx = 0.05f * (float)(x - scene_ctx->mouse.x);
        float dy = 0.05f * (float)(y - scene_ctx->mouse.y);

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
    for (unsigned i = 0; i < OBJ_COUNT; i++) {
        UINT obj_index = render_ctx->render_items[i].obj_cbuffer_index;
        UINT cbuffer_size = sizeof(ObjectConstantBuffer);
        // Only update the cbuffer data if the constants have changed.  
        // This needs to be tracked per frame resource.
        if (render_ctx->render_items[i].n_frames_dirty > 0) {
            XMMATRIX world = XMLoadFloat4x4(&render_ctx->render_items[i].world);
            ObjectConstantBuffer obj_cbuffer = {};
            XMStoreFloat4x4(&obj_cbuffer.world, XMMatrixTranspose(world));
            uint8_t * obj_ptr = render_ctx->frame_resources[frame_index].obj_cb_data_ptr + ((UINT64)obj_index * cbuffer_size);
            memcpy(obj_ptr, &obj_cbuffer, cbuffer_size);

            // Next FrameResource need to be updated too.
            render_ctx->render_items[i].n_frames_dirty--;
        }
    }
}
static void
update_pass_cbuffers (D3DRenderContext * render_ctx) {

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

    uint8_t * pass_ptr = render_ctx->frame_resources[render_ctx->frame_index].pass_cb_data_ptr;
    memcpy(pass_ptr, &render_ctx->main_pass_constants, sizeof(PassConstantBuffer));
}
static HRESULT
move_to_next_frame (D3DRenderContext * render_ctx, UINT * out_frame_index) {

    HRESULT ret = E_FAIL;
    UINT frame_index = *out_frame_index;

    // -- 1. schedule a signal command in the queue
    UINT64 const current_fence_value = render_ctx->frame_resources[frame_index].fence;
    ret = render_ctx->cmd_queue->Signal(render_ctx->fence, current_fence_value);
    CHECK_AND_FAIL(ret);

    // -- 2. update frame index
    *out_frame_index = render_ctx->swapchain3->GetCurrentBackBufferIndex();
    //render_ctx->frame_index = (render_ctx->frame_index + 1) % NUM_FRAME_RESOURCES;

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
    UINT frame_index = render_ctx->frame_index;

    // -- 1. schedule a signal command in the queue
    ret = render_ctx->cmd_queue->Signal(render_ctx->fence, render_ctx->frame_resources[frame_index].fence);
    CHECK_AND_FAIL(ret);

    // -- 2. wait until the fence has been processed
    ret = render_ctx->fence->SetEventOnCompletion(render_ctx->frame_resources[frame_index].fence, render_ctx->fence_event);
    CHECK_AND_FAIL(ret);
    WaitForSingleObjectEx(render_ctx->fence_event, INFINITE /*return only when the object is signaled*/, false);

    // -- 3. increment fence value for the current frame
    ++render_ctx->frame_resources[frame_index].fence;

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

    // -- set viewport and scissor
    render_ctx->direct_cmd_list->RSSetViewports(1, &render_ctx->viewport);
    render_ctx->direct_cmd_list->RSSetScissorRects(1, &render_ctx->scissor_rect);

    // -- indicate that the backbuffer will be used as the render target
    D3D12_RESOURCE_BARRIER barrier1 = create_barrier(render_ctx->frame_resources[frame_index].render_target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    render_ctx->direct_cmd_list->ResourceBarrier(1, &barrier1);

    // -- get CPU descriptor handle that represents the start of the rtv heap
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = render_ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    // -- apply initial offset
    rtv_handle.ptr = SIZE_T(INT64(rtv_handle.ptr) + INT64(render_ctx->frame_index) * INT64(render_ctx->rtv_descriptor_size));
    float clear_colors [] = {0.2f, 0.3f, 0.5f, 1.0f};
    render_ctx->direct_cmd_list->ClearRenderTargetView(rtv_handle, clear_colors, 0, nullptr);
    render_ctx->direct_cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

    ID3D12DescriptorHeap * heaps [] = {render_ctx->cbv_heap};
    render_ctx->direct_cmd_list->SetDescriptorHeaps(ARRAY_COUNT(heaps), heaps);

    render_ctx->direct_cmd_list->SetGraphicsRootSignature(render_ctx->root_signature);

    int pass_cbv_index = render_ctx->pass_cbv_offset + frame_index;
    D3D12_GPU_DESCRIPTOR_HANDLE pass_cbv_handle = {};
    pass_cbv_handle.ptr = render_ctx->cbv_heap->GetGPUDescriptorHandleForHeapStart().ptr + pass_cbv_index * (UINT64)render_ctx->cbv_srv_uav_descriptor_size;
    render_ctx->direct_cmd_list->SetGraphicsRootDescriptorTable(1, pass_cbv_handle);

    draw_render_items(render_ctx->direct_cmd_list, render_ctx->cbv_heap, render_ctx->cbv_srv_uav_descriptor_size, render_ctx->render_items, frame_index);

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
    global_scene_ctx.theta = 1.5f * XM_PI;
    global_scene_ctx.phi = 0.2f * XM_PI;
    global_scene_ctx.radius = 15.0f;
    global_scene_ctx.aspect_ratio = (float)global_scene_ctx.width / (float)global_scene_ctx.height;
    global_scene_ctx.eye_pos = {0.0f, 0.0f, 0.0f};
    global_scene_ctx.view = Identity4x4();
    XMMATRIX p = DirectX::XMMatrixPerspectiveFovLH(0.25f * XM_PI, global_scene_ctx.aspect_ratio, 1.0f, 1000.0f);
    XMStoreFloat4x4(&global_scene_ctx.proj, p);

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
    // store CBV_SRV_UAV descriptor increment size for later
    render_ctx.cbv_srv_uav_descriptor_size = render_ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

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
#pragma region Descriptor Heaps Creation
    create_descriptor_heaps(&render_ctx);
#pragma endregion Descriptor Heaps Creation

        // -- create frame resources: rtv, cmd-allocator and cbuffers for each frame
    render_ctx.rtv_descriptor_size = render_ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle_start = render_ctx.rtv_heap->GetCPUDescriptorHandleForHeapStart();

    UINT obj_cb_size = sizeof(ObjectConstantBuffer);
    UINT pass_cb_size = sizeof(PassConstantBuffer);

    for (UINT i = 0; i < FRAME_COUNT; ++i) {
        CHECK_AND_FAIL(render_ctx.swapchain3->GetBuffer(i, IID_PPV_ARGS(&render_ctx.frame_resources[i].render_target)));

        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = {};
        cpu_handle.ptr = rtv_handle_start.ptr + ((UINT64)i * render_ctx.rtv_descriptor_size);
        // -- create a rtv for each frame
        render_ctx.device->CreateRenderTargetView(render_ctx.frame_resources[i].render_target, nullptr, cpu_handle);
        // -- create a cmd-allocator for each frame
        res = render_ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&render_ctx.frame_resources[i].cmd_list_alloc));

        // -- create cbuffers as upload_buffer
        create_upload_buffer(render_ctx.device, (UINT64)obj_cb_size * OBJ_COUNT, &render_ctx.frame_resources[i].obj_cb_data_ptr, &render_ctx.frame_resources[i].obj_cb);
        // Initialize cb data
        ::memcpy(render_ctx.frame_resources[i].obj_cb_data_ptr, &render_ctx.frame_resources[i].obj_cb_data, sizeof(render_ctx.frame_resources[i].obj_cb_data));

        create_upload_buffer(render_ctx.device, pass_cb_size, &render_ctx.frame_resources[i].pass_cb_data_ptr, &render_ctx.frame_resources[i].pass_cb);
        // Initialize cb data
        ::memcpy(render_ctx.frame_resources[i].pass_cb_data_ptr, &render_ctx.frame_resources[i].pass_cb_data, sizeof(render_ctx.frame_resources[i].pass_cb_data));
    }

    for (UINT i = 0; i < FRAME_COUNT; ++i) {
        // 1. per obj cbuffer
        {
            // create constant buffer view.
            for (UINT j = 0; j < OBJ_COUNT; ++j) {
                D3D12_GPU_VIRTUAL_ADDRESS cb_address = render_ctx.frame_resources[i].obj_cb->GetGPUVirtualAddress();

                // Offset to the ith object constant buffer in the buffer.
                cb_address += j * (UINT64)obj_cb_size;

                // Offset to the object cbv in the descriptor heap.
                int heap_index = i * OBJ_COUNT + j;

                D3D12_CPU_DESCRIPTOR_HANDLE cbv_handle = {};
                cbv_handle.ptr = render_ctx.cbv_heap->GetCPUDescriptorHandleForHeapStart().ptr + heap_index * (UINT64)render_ctx.cbv_srv_uav_descriptor_size;

                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
                cbv_desc.BufferLocation = cb_address;
                cbv_desc.SizeInBytes = obj_cb_size;

                render_ctx.device->CreateConstantBufferView(&cbv_desc, cbv_handle);
            }
        }
        // 2. per pass cbuffer
        {
            D3D12_GPU_VIRTUAL_ADDRESS cb_address = render_ctx.frame_resources[i].pass_cb->GetGPUVirtualAddress();

            // Offset to the pass cbv in the descriptor heap.
            int heap_index = render_ctx.pass_cbv_offset + i;
            D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
            handle.ptr = render_ctx.cbv_heap->GetCPUDescriptorHandleForHeapStart().ptr + heap_index * (UINT64)render_ctx.cbv_srv_uav_descriptor_size;

            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
            cbv_desc.BufferLocation = cb_address;
            cbv_desc.SizeInBytes = pass_cb_size;

            render_ctx.device->CreateConstantBufferView(&cbv_desc, handle);
        }
    }

    CHECK_AND_FAIL(res);

    // ========================================================================================================
#pragma region Root Signature
    create_root_signature(render_ctx.device, &render_ctx.root_signature);
#pragma endregion Root Signature

    // Load and compile shaders

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
    create_pso(&render_ctx, vertex_shader_code, pixel_shader_code);

    // Create command list
    ID3D12CommandAllocator * current_alloc = render_ctx.frame_resources[render_ctx.frame_index].cmd_list_alloc;
    if (current_alloc) {
        render_ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, current_alloc, render_ctx.pso, IID_PPV_ARGS(&render_ctx.direct_cmd_list));
    }
#pragma endregion PSO Creation

#pragma region Shapes and RenderItems Creation

    Vertex *    vts = (Vertex *)::malloc(sizeof(Vertex) * _TOTAL_VTX_CNT);
    uint16_t *  ids = (uint16_t *)::malloc(sizeof(uint16_t) * _TOTAL_IDX_CNT);
    BYTE *      memory = (BYTE *)::malloc(sizeof(GeomVertex) * _TOTAL_VTX_CNT + sizeof(uint16_t) * _TOTAL_IDX_CNT);

    create_shape_geometry(memory, &render_ctx, vts, ids);
    create_render_items(render_ctx.render_items, &render_ctx.geom);

#pragma endregion Shapes and RenderItems Creation

    // -- close the command list and execute it to begin inital gpu setup
    CHECK_AND_FAIL(render_ctx.direct_cmd_list->Close());
    ID3D12CommandList * cmd_lists [] = {render_ctx.direct_cmd_list};
    render_ctx.cmd_queue->ExecuteCommandLists(ARRAY_COUNT(cmd_lists), cmd_lists);

    //----------------
    // Create fence
    // create synchronization objects and wait until assets have been uploaded to the GPU.

    UINT frame_index = render_ctx.frame_index;
    CHECK_AND_FAIL(render_ctx.device->CreateFence(render_ctx.frame_resources[frame_index].fence, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&render_ctx.fence)));

    ++render_ctx.frame_resources[frame_index].fence;

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
        update_camera(&global_scene_ctx);
        update_pass_cbuffers(&render_ctx);
        update_obj_cbuffers(&render_ctx);

        // OnRender() aka rendering
        CHECK_AND_FAIL(draw_main(&render_ctx));

        CHECK_AND_FAIL(move_to_next_frame(&render_ctx, &render_ctx.frame_index));
    }
#pragma endregion Main_Loop

    // ========================================================================================================
#pragma region Cleanup_And_Debug
    CHECK_AND_FAIL(wait_for_gpu(&render_ctx));

    CloseHandle(render_ctx.fence_event);

    render_ctx.fence->Release();

    // release queuing frame resources
    for (size_t i = 0; i < NUM_FRAME_RESOURCES; i++) {
        render_ctx.frame_resources[i].obj_cb->Unmap(0, nullptr);
        render_ctx.frame_resources[i].pass_cb->Unmap(0, nullptr);
        render_ctx.frame_resources[i].obj_cb->Release();
        render_ctx.frame_resources[i].pass_cb->Release();
    }

    render_ctx.geom.ib_uploader->Release();
    render_ctx.geom.vb_uploader->Release();

    render_ctx.geom.ib_gpu->Release();
    render_ctx.geom.vb_gpu->Release();

    render_ctx.direct_cmd_list->Release();
    render_ctx.pso->Release();

    pixel_shader_code->Release();
    vertex_shader_code->Release();

    render_ctx.root_signature->Release();

    // release swapchain backbuffers resources
    for (unsigned i = 0; i < FRAME_COUNT; ++i) {
        render_ctx.frame_resources[i].render_target->Release();
        render_ctx.frame_resources[i].cmd_list_alloc->Release();
    }

    render_ctx.cbv_heap->Release();
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

        // -- consume var to avoid warning
        dxgiGetDebugInterface = dxgiGetDebugInterface;
    }
#pragma endregion Cleanup_And_Debug

    return 0;
}

