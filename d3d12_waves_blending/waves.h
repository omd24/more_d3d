#pragma once
#include "headers/common.h"
#include <DirectXMath.h>

struct Waves {
    int nrow;
    int ncol;
    int nvtx;   // # of vertex
    int ntri;   // # of triangle
    float width;
    float depth;
    float height;

    float k1, k2, k3;    // simulation constants

    float time_step, spatial_step;

    DirectX::XMFLOAT3 * prev_sol;
    DirectX::XMFLOAT3 * curr_sol;
    DirectX::XMFLOAT3 * normal;
    DirectX::XMFLOAT3 * tangent_x;

};
size_t
Waves_CalculateRequiredSize (int m, int n);
Waves *
Waves_Init (BYTE * memory, int m, int n, float dx, float dt, float speed, float damping);
DirectX::XMFLOAT3 &
Waves_GetPosition (Waves * wave, int i);
DirectX::XMFLOAT3 &
Waves_GetNormal (Waves * wave, int i);
DirectX::XMFLOAT3 &
Waves_GetTangentX (Waves * wave, int i);
void
Waves_Update (Waves * wave, float dt, DirectX::XMFLOAT3 temp []);
void
Waves_Disturb (Waves * wave, int i, int j, float magnitude);
