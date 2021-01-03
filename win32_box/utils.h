#pragma once

#include <windows.h>
#include <directxmath.h>
#include <windowsx.h>
#include <stdio.h>

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
#define SIMPLE_ASSERT(exp) if(!(exp))   {*(int *)0 = 0;}
#define CLAMP_VALUE(val, lb, ub)        val < lb ? lb : (val > ub ? ub : val); 

struct TextuVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT2 uv;
};
struct SceneContext {
    // camera settings
    float theta;
    float phi;
    float radius;

    // mouse position
    POINT mouse;

    // projection matrix
    DirectX::XMMATRIX world;
    DirectX::XMMATRIX view;
    DirectX::XMMATRIX proj;

    // display-related data
    UINT width;
    UINT height;
    float aspect_ratio;
};
static DirectX::XMFLOAT4X4
Identity4x4() {
    static DirectX::XMFLOAT4X4 I(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);

    return I;
}
static DirectX::XMMATRIX
IdentityMat() {
    static DirectX::XMMATRIX I(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);

    return I;
}
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
