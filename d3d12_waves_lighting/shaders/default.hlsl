#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 1
#endif
#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 0
#endif
#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 0
#endif

#include "light_utils.hlsl"

cbuffer PerObjectConstantBuffer : register(b0) {
    float4x4 global_world;
}
cbuffer MaterialConstantBuffer : register(b1) {
    float4      global_diffuse_albedo;
    float3      global_fresnel_r0;
    float       global_roughness;
    float4x4    global_mat_transform;
};
cbuffer PerPassConstantBuffer : register(b2) {
    float4x4    global_view;
    float4x4    global_inv_view;
    float4x4    global_proj;
    float4x4    global_inv_proj;
    float4x4    global_view_proj;
    float4x4    global_inv_view_proj;
    float3      global_eye_pos_w;
    float       cb_per_obj_padding;
    float2      global_render_target_size;
    float2      global_inv_render_target_size;
    float       global_near_z;
    float       global_far_z;
    float       global_total_time;
    float       global_delta_time;
    float4      global_ambient_light;
    
    // Indices [0, NUM_DIR_LIGHTS) are directional lights;
    // indices [NUM_DIR_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;
    // indices [NUM_DIR_LIGHTS+NUM_POINT_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHT+NUM_SPOT_LIGHTS)
    // are spot lights for a maximum of MAX_LIGHTS per object.
    Light       global_lights [MAX_LIGHTS];
}
struct VertexShaderInput {
    float3 pos_local  : POSITION;
    float3 normal_local : NORMAL;
};
struct VertexShaderOutput {
    float4 pos_homogenous_clip_space : SV_Position;
    float3 pos_world : Position;
    float3 normal_world : NORMAL;
};
VertexShaderOutput
VertexShader_Main (VertexShaderInput vin) {
    VertexShaderOutput res = (VertexShaderOutput) 0.0f;
    
    // transform to world space
    float4 pos_world = mul(float4(vin.pos_local, 1.0f), global_world);
    res.pos_world = pos_world.xyz;
    
    // assuming nonuniform scale (otherwise have to use inverse-transpose of world-matrix)
    res.normal_world = mul(vin.normal_local, (float3x3) global_world);
    
    // transform to homogenous clip space
    res.pos_homogenous_clip_space = mul(pos_world, global_view_proj);

    return res;
}
float4
PixelShader_Main (VertexShaderOutput pin) : SV_Target {
    // interpolations of normal can unnormalize it so renormalize!
    pin.normal_world = normalize(pin.normal_world);
    
    // vector from point being lit "to eye"
    float3 to_eye = normalize(global_eye_pos_w - pin.pos_world);
    
    // indirect lighting
    float4 ambient = global_ambient_light * global_diffuse_albedo;
    
    const float shininess = 1.0f - global_roughness;
    Material mat = { global_diffuse_albedo, global_fresnel_r0, shininess };
    float3 shadow_factor = 1.0f;
    float4 direct_light = compute_lighting(
        global_lights, mat, pin.pos_world, pin.normal_world, to_eye, shadow_factor
    );
    float4 lit_color = ambient + direct_light;
    
    // common convention to take alpha from diffuse material
    lit_color.a = global_diffuse_albedo.a;

    return lit_color;
}

