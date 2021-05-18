
// Defaults for number of lights.
#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 3
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 0
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 0
#endif

#include "light_utils.hlsl"

struct MaterialData {
    uint diffuse_map_index;
    uint mat_pad0;
    uint mat_pad1;
    uint mat_pad2;

    float4 diffuse_albedo;
    float3 fresnel_r0;
    float roughness;
    float4x4 mat_transform;
};


// An array of textures, which is only supported in shader model 5.1+.  Unlike Texture2DArray, the textures
// in this array can be different sizes and formats, making it more flexible than texture arrays.
Texture2D global_diffuse_maps[4] : register(t0);

// Put in space1, so the texture array does not overlap with these resources.  
// The texture array will occupy registers t0, t1, t2, and t3 in space0. 
StructuredBuffer<MaterialData> global_mat_data : register(t0, space1);

SamplerState gsam_point_wrap : register(s0);
SamplerState gsam_point_clamp : register(s1);
SamplerState gsam_linear_wrap : register(s2);
SamplerState gsam_linear_clamp : register(s3);
SamplerState gsam_anisotropic_wrap : register(s4);
SamplerState gsam_anisotropic_clamp : register(s5);

// Constant data that varies per frame.
cbuffer CbPerObject : register(b0) {
    float4x4 global_world;
    float4x4 global_tex_transform;
    uint global_mat_index;
    uint obj_pad0;
    uint obj_pad1;
    uint obj_pad2;
};

// Constant data that varies per material.
cbuffer CbPerPass : register(b1) {
    float4x4 global_view;
    float4x4 global_inv_view;
    float4x4 global_proj;
    float4x4 global_inv_proj;
    float4x4 global_view_proj;
    float4x4 global_inv_view_proj;
    float3 global_eye_pos_w;
    float pass_pad0;
    float2 global_render_target_size;
    float2 global_inv_render_target_size;
    float global_nearz;
    float global_farz;
    float global_total_time;
    float global_delta_time;
    float4 global_ambient_light;

    // Indices [0, NUM_DIR_LIGHTS) are directional lights;
    // indices [NUM_DIR_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;
    // indices [NUM_DIR_LIGHTS+NUM_POINT_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHT+NUM_SPOT_LIGHTS)
    // are spot lights for a maximum of MaxLights per object.
    Light global_lights[MAX_LIGHTS];
};

struct VertexIn
{
    float3 pos_local : POSITION;
    float3 normal_local : NORMAL;
    float2 texc : TEXCOORD;
};

struct VertexOut
{
    float4 pos_homo : SV_POSITION;
    float3 pos_world : POSITION;
    float3 normal_world : NORMAL;
    float2 texc : TEXCOORD;
};

VertexOut VS (VertexIn vin)
{
    VertexOut vout = (VertexOut)0.0f;

    // Fetch the material data.
    MaterialData mat_data = global_mat_data[global_mat_index];

    // Transform to world space.
    float4 pos_world = mul(float4(vin.pos_local, 1.0f), global_world);
    vout.pos_world = pos_world.xyz;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.normal_world = mul(vin.normal_local, (float3x3)global_world);

    // Transform to homogeneous clip space.
    vout.pos_homo = mul(pos_world, global_view_proj);

    // Output vertex attributes for interpolation across triangle.
    float4 texc = mul(float4(vin.texc, 0.0f, 1.0f), global_tex_transform);
    vout.texc = mul(texc, mat_data.mat_transform).xy;
    return vout;
}

float4 PS (VertexOut pin) : SV_Target
{
    // Fetch the material data.
    MaterialData mat_data = global_mat_data[global_mat_index];
    float4 diffuse_albedo = mat_data.diffuse_albedo;
    float3 fresnel_r0 = mat_data.fresnel_r0;
    float roughness = mat_data.roughness;
    uint diffuse_tex_index = mat_data.diffuse_map_index;

    // Dynamically look up the texture in the array.
    diffuse_albedo *= global_diffuse_maps[diffuse_tex_index].Sample(gsam_linear_wrap, pin.texc);

    // Interpolating normal can unnormalize it, so renormalize it.
    pin.normal_world = normalize(pin.normal_world);

    // Vector from point being lit to eye. 
    float3 to_eye_w = normalize(global_eye_pos_w - pin.pos_world);

    // Light terms.
    float4 ambient = global_ambient_light * diffuse_albedo;

    const float shininess = 1.0f - roughness;
    Material mat = {diffuse_albedo, fresnel_r0, shininess};
    float3 shadow_factor = 1.0f;
    float4 direct_light = compute_lighting(global_lights, mat, pin.pos_world,
        pin.normal_world, to_eye_w, shadow_factor);

    float4 lit_color = ambient + direct_light;

    // Common convention to take alpha from diffuse albedo.
    lit_color.a = diffuse_albedo.a;

    return lit_color;
}


