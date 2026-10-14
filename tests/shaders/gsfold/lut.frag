#version 450
// A stand-in for the shape of UE4's CombineLUTs fragment shader (what the bridge recognizes):
// reads the slice (Layer) and a uv, decodes UE4's log encoding, writes location 0. Its own output
// is some grading (a red tint); the bridge's neutral stand-in, when it replaces this, gives none.
layout(location = 0) noperspective in vec2 uv;
layout(location = 0) out vec4 o;
void main() {
    vec3 n = vec3((uv - 0.5 / 32.0) * (32.0 / 31.0), float(gl_Layer) / 31.0);
    vec3 lin = exp2((n - 0.434017599) * 14.0) * 0.18;
    o = vec4(clamp(lin, 0.0, 1.0) * vec3(1.0, 0.2, 0.2), 0.0);
}
