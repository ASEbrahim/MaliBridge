#version 450
// Stand-in for Unreal Engine 4's colour-grading LUT pass (CombineLUTs) when the driver cannot
// compile the real one (Mali-G925, Stray): a neutral LUT - no grading, no white balance, a
// generic filmic curve instead of UE4's ACES - so the picture keeps its content and contrast.
// The LUT's input is UE4's log encoding of the scene colour; its output is the sRGB-encoded
// colour divided by 1.05, which the tonemapping pass multiplies back.
layout(location = 0) noperspective in vec2 uv;
layout(location = 0) out vec4 o;
const float N = 32.0; // UE4's LUT size
vec3 srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
void main() {
    vec3 n = vec3((uv - 0.5 / N) * (N / (N - 1.0)), float(gl_Layer) / (N - 1.0));
    vec3 lin = max(exp2((n - 0.434017599) * 14.0) * 0.18 - 0.00266771927, 0.0);
    vec3 t = clamp(lin * (2.51 * lin + 0.03) / (lin * (2.43 * lin + 0.59) + 0.14), 0.0, 1.0); // ACES fit (Narkowicz)
    o = vec4(srgb(t) / 1.05, 0.0);
}
