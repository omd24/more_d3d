cbuffer PerObjectConstantBuffer : register(b0) {
    float4x4 global_world;
    float4   global_color;
}
cbuffer PerPassConstantBuffer : register(b1) {
    float4x4 global_view;
    float4x4 global_inv_view;
    float4x4 global_proj;
    float4x4 global_inv_proj;
    float4x4 global_view_proj;
    float4x4 global_inv_view_proj;
    float3   global_eye_pos_w;
    float    cb_per_obj_padding;
    float2   global_render_target_size;
    float2   global_inv_render_target_size;
    float    global_near_z;
    float    global_far_z;
    float    global_total_time;
    float    global_delta_time;
}
struct VertexShaderInput {
    float3 pos_local  : POSITION;
    float4 color : COLOR;
};
struct VertexShaderOutput {
    float4 pos_homogenous_clip_space : SV_Position;
    float4 color : COLOR;
};
VertexShaderOutput
VertexShader_Main (VertexShaderInput vin) {
    VertexShaderOutput res;
    float4 pos_world = mul(float4(vin.pos_local, 1.0f), global_world);
    res.pos_homogenous_clip_space = mul(pos_world, global_view_proj);
    res.color = vin.color + global_color;
    return res;
}
float4
PixelShader_Main (VertexShaderOutput pin) : SV_Target {
    return pin.color;
}

