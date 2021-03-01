#pragma once

#include <DirectXMath.h>
#include <ppl.h>
#include "headers/common.h"

using namespace DirectX;
using namespace concurrency;

#define WAVE_VTX_CNT   16384

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

    XMFLOAT3 prev_sol[WAVE_VTX_CNT];
    XMFLOAT3 curr_sol[WAVE_VTX_CNT];
    XMFLOAT3 normal[WAVE_VTX_CNT];
    XMFLOAT3 tangent_x[WAVE_VTX_CNT];

};
inline void
Waves_Init (Waves * wave, int m, int n, float dx, float dt, float speed, float damping) {
    wave->nrow = m;
    wave->ncol = n;

    wave->nvtx = m * n;
    SIMPLE_ASSERT(WAVE_VTX_CNT == wave->nvtx, "Incorrect vertex count");

    wave->ntri = (m - 1) * (n - 1) * 2;

    wave->time_step = dt;
    wave->spatial_step = dx;

    float d = damping * dt + 2.0f;
    float e = (speed * speed) * (dt * dt) / (dx * dx);
    wave->k1 = (damping * dt - 2.0f) / d;
    wave->k2 = (4.0f - 8.0f * e) / d;
    wave->k3 = (2.0f * e) / d;

    // Generate grid vertices in system memory.

    float half_width = (n - 1) * dx * 0.5f;
    float half_depth = (m - 1) * dx * 0.5f;
    for (int i = 0; i < m; ++i) {
        float z = half_depth - i * dx;
        for (int j = 0; j < n; ++j) {
            float x = -half_width + j * dx;

            wave->prev_sol[i * n + j] = XMFLOAT3(x, 0.0f, z);
            wave->curr_sol[i * n + j] = XMFLOAT3(x, 0.0f, z);
            wave->normal[i * n + j] = XMFLOAT3(0.0f, 1.0f, 0.0f);
            wave->tangent_x[i * n + j] = XMFLOAT3(1.0f, 0.0f, 0.0f);
        }
    }

    wave->width = wave->ncol * wave->spatial_step;
    wave->depth = wave->nrow * wave->spatial_step;
}
DirectX::XMFLOAT3 &
Waves_GetPosition (Waves * wave, int i) {
    return wave->curr_sol[i];
}
DirectX::XMFLOAT3 &
Waves_GetNormal (Waves * wave, int i) {
    return wave->normal[i];
}
DirectX::XMFLOAT3 &
Waves_GetTangentX (Waves * wave, int i) {
    return wave->tangent_x[i];
}
inline void
Waves_Update (Waves * wave, float dt, XMFLOAT3 temp []) {
    static float t = 0;

        // Accumulate time.
    t += dt;

    /*
    void 
    on_colsiotn (int xx, int yy, collider c) {
        
        if (c.name == "wall") {
            gameobject.velocity = 0;
        }        
    
    }

    */

    // Only update the simulation at the specified time step.
    if (t >= wave->time_step) {
        // Only update interior points; we use zero boundary conditions.
        concurrency::parallel_for(1, wave->nrow - 1, [wave](int i)
        //for(int i = 1; i < wave->nrow-1; ++i)
                                  {
                                      for (int j = 1; j < wave->ncol - 1; ++j) {
                                          // After this update we will be discarding the old previous
                                          // buffer, so overwrite that buffer with the new update.
                                          // Note how we can do this inplace (read/write to same element) 
                                          // because we won't need prev_ij again and the assignment happens last.

                                          // Note j indexes x and i indexes z: h(x_j, z_i, t_k)
                                          // Moreover, our +z axis goes "down"; this is just to 
                                          // keep consistent with our row indices going down.

                                          wave->prev_sol[i * wave->ncol + j].y =
                                              wave->k1 * wave->prev_sol[i * wave->ncol + j].y +
                                              wave->k2 * wave->curr_sol[i * wave->ncol + j].y +
                                              wave->k3 * (wave->curr_sol[(i + 1) * wave->ncol + j].y +
                                                          wave->curr_sol[(i - 1) * wave->ncol + j].y +
                                                          wave->curr_sol[i * wave->ncol + j + 1].y +
                                                          wave->curr_sol[i * wave->ncol + j - 1].y);
                                      }
                                  });

        // We just overwrote the previous buffer with the new data, so
        // this data needs to become the current solution and the old
        // current solution becomes the new previous solution.

        // Swap prev with curr solution
        for (unsigned i = 0; i < WAVE_VTX_CNT; i++) {
        //write any swapping technique
            temp[i] = wave->prev_sol[i];
            wave->prev_sol[i] = wave->curr_sol[i];
            wave->curr_sol[i] = temp[i];
        }

        t = 0.0f; // reset time

        //
        // Compute normals using finite difference scheme.
        //
        concurrency::parallel_for(1, wave->nrow - 1, [wave](int i)
        //for(int i = 1; i < wave->nrow - 1; ++i)
                                  {
                                      for (int j = 1; j < wave->ncol - 1; ++j) {
                                          float l = wave->curr_sol[i * wave->ncol + j - 1].y;
                                          float r = wave->curr_sol[i * wave->ncol + j + 1].y;
                                          float t = wave->curr_sol[(i - 1) * wave->ncol + j].y;
                                          float b = wave->curr_sol[(i + 1) * wave->ncol + j].y;
                                          wave->normal[i * wave->ncol + j].x = -r + l;
                                          wave->normal[i * wave->ncol + j].y = 2.0f * wave->spatial_step;
                                          wave->normal[i * wave->ncol + j].z = b - t;

                                          XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&wave->normal[i * wave->ncol + j]));
                                          XMStoreFloat3(&wave->normal[i * wave->ncol + j], n);

                                          wave->tangent_x[i * wave->ncol + j] = XMFLOAT3(2.0f * wave->spatial_step, r - l, 0.0f);
                                          XMVECTOR T = XMVector3Normalize(XMLoadFloat3(&wave->tangent_x[i * wave->ncol + j]));
                                          XMStoreFloat3(&wave->tangent_x[i * wave->ncol + j], T);
                                      }
                                  });
    }
}
inline void
Waves_Disturb (Waves * wave, int i, int j, float magnitude) {
    // Don't disturb boundaries.
    SIMPLE_ASSERT(i > 1 && i < wave->nrow - 2, );
    assert(j > 1 && j < wave->ncol - 2);

    float half_mag = 0.5f * magnitude;

    // Disturb the ijth vertex height and its neighbors.
    wave->curr_sol[i * wave->ncol + j].y += magnitude;
    wave->curr_sol[i * wave->ncol + j + 1].y += half_mag;
    wave->curr_sol[i * wave->ncol + j - 1].y += half_mag;
    wave->curr_sol[(i + 1) * wave->ncol + j].y += half_mag;
    wave->curr_sol[(i - 1) * wave->ncol + j].y += half_mag;
}
