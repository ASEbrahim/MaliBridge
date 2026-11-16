#version 450
// PV-names: a vertex input and the output that passes it on share one OpName ("TEXCOORD", as
// DXVK names a passed-through attribute and its varying). GLSL cannot declare two variables of
// one name, so the two differ in the last letter here and build_probe.sh makes them alike in
// the SPIR-V. One triangle over the target from gl_VertexIndex; the colour is the attribute.
layout(location = 0) in vec4 TEXCOORD;
layout(location = 0) out vec4 TEXCOORE;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    TEXCOORE = TEXCOORD;
}
