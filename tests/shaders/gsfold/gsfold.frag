#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(0.25, 0.5, 0.75, 1.0) + vec4(float(gl_Layer) * 0.0, 0.0, 0.0, 0.0); }
