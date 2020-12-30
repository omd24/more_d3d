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
