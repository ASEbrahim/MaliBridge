#version 450
// A pass-through that only sets the layer (UE4's WriteToSliceGS shape).
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec2 uv_in[];
layout(location = 2) flat in int layer_in[];
layout(location = 0) out vec2 uv;
void main() {
    gl_Position = gl_in[0].gl_Position; uv = uv_in[0]; gl_Layer = layer_in[0]; EmitVertex();
    gl_Position = gl_in[1].gl_Position; uv = uv_in[1]; gl_Layer = layer_in[1]; EmitVertex();
    gl_Position = gl_in[2].gl_Position; uv = uv_in[2]; gl_Layer = layer_in[2]; EmitVertex();
}
