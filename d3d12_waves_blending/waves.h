#pragma once

#include <DirectXMath.h>
#include <ppl.h>
#include "headers/common.h"

using namespace DirectX;
using namespace concurrency;

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

    XMFLOAT3 * prev_sol;
    XMFLOAT3 * curr_sol;
    XMFLOAT3 * normal;
    XMFLOAT3 * tangent_x;

};
inline size_t
Waves_CalculateRequiredSize (int m, int n) {
    int n_vtx = m * n;
    SIMPLE_ASSERT(n_vtx > 0, "Invalid waves dimensions");
    return sizeof(Waves) + 4 * (sizeof(XMFLOAT3) * n_vtx);
}
inline Waves *
Waves_Init (BYTE * memory, int m, int n, float dx, float dt, float speed, float damping) {

    Waves * ret = nullptr;
    ret = reinterpret_cast<Waves *>(memory);
    ret->nrow = m;
    ret->ncol = n;
    ret->nvtx = m * n;
    ret->ntri = (m - 1) * (n - 1) * 2;

    // Setup pointers (arrays)
    ret->prev_sol   = reinterpret_cast<XMFLOAT3 *>(memory + sizeof(Waves));
    ret->curr_sol   = reinterpret_cast<XMFLOAT3 *>(memory + sizeof(Waves) + ret->nvtx * sizeof(XMFLOAT3));
    ret->normal     = reinterpret_cast<XMFLOAT3 *>(memory + sizeof(Waves) + 2 * (ret->nvtx * sizeof(XMFLOAT3)));
    ret->tangent_x  = reinterpret_cast<XMFLOAT3 *>(memory + sizeof(Waves) + 3 * (ret->nvtx * sizeof(XMFLOAT3)));

    ret->time_step = dt;
    ret->spatial_step = dx;

    float d = damping * dt + 2.0f;
    float e = (speed * speed) * (dt * dt) / (dx * dx);
    ret->k1 = (damping * dt - 2.0f) / d;
    ret->k2 = (4.0f - 8.0f * e) / d;
    ret->k3 = (2.0f * e) / d;

    // Generate grid vertices in system memory.

    float half_width = (n - 1) * dx * 0.5f;
    float half_depth = (m - 1) * dx * 0.5f;
    for (int i = 0; i < m; ++i) {
        float z = half_depth - i * dx;
        for (int j = 0; j < n; ++j) {
            float x = -half_width + j * dx;

            ret->prev_sol[i * n + j] = XMFLOAT3(x, 0.0f, z);
            ret->curr_sol[i * n + j] = XMFLOAT3(x, 0.0f, z);
            ret->normal[i * n + j] = XMFLOAT3(0.0f, 1.0f, 0.0f);
            ret->tangent_x[i * n + j] = XMFLOAT3(1.0f, 0.0f, 0.0f);
        }
    }

    ret->width = ret->ncol * ret->spatial_step;
    ret->depth = ret->nrow * ret->spatial_step;

    return ret;
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
        for (int i = 0; i < wave->nvtx; i++) {
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
