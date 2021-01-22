//cbuffer SceneConstantBuffer : register(b0) {
//    float4 offset;
//    float4 padding [15];
//}
cbuffer PerObjectConstantBuffer : register(b0) {
    float4x4 global_world_view_proj;
}
struct PixelShaderInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
};

Texture2D global_texture : register(t0);
SamplerState global_sampler : register(s0);

PixelShaderInput
VertexShader_Main (float3 p : POSITION, float2 uv : TEXCOORD) {
    PixelShaderInput result;
    //result.position = p + offset;       // apply offset from cbuffer
    result.position = mul(float4(p, 1.0f), global_world_view_proj);
    result.uv = uv;
    return result;
}

float4
PixelShader_Main (PixelShaderInput input) : SV_Target {
    return global_texture.Sample(global_sampler, input.uv);
}

