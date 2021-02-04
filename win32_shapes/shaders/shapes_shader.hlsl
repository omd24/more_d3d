cbuffer PerObjectConstantBuffer : register(b0) {
    float4x4 global_world;
}
cbuffer PerPassConstantBuffer : register(b1) {
    float4x4 global_view;
    float4x4 global_inv_view;
    float4x4 global_proj;
    float4x4 global_inv_proj;
    float4x4 global_view_proj;
    float4x4 global_inv_view_proj;
    float4x4 global_eye_pos_w;
    //float4x4 cb_per_obj_padding;
    float4x4 global_render_target_size;
    float4x4 global_inv_render_target_size;
    float4x4 global_near_z;
    float4x4 global_far_z;
    float4x4 global_total_time;
    float4x4 global_delta_time;
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
    res.color = vin.color;
    return res;
}
float4
PixelShader_Main (VertexShaderOutput pin) : SV_Target {
    return pin.color;
}

