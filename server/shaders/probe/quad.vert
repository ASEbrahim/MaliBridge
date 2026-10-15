#version 450
// Two triangles over the rectangle pc.rect (x0, y0, x1, y1 in NDC) at depth pc.z in colour pc.col
// (viewport_flip, dyn_state). x0 > x1 winds them the other way.
layout(push_constant) uniform P { vec4 rect; vec4 col; float z; } pc;
layout(location = 0) out vec4 col;
void main() {
    const int corner[6] = int[6](0, 1, 2, 2, 1, 3);
    int k = corner[gl_VertexIndex % 6];
    gl_Position = vec4((k & 1) != 0 ? pc.rect.z : pc.rect.x, (k & 2) != 0 ? pc.rect.w : pc.rect.y, pc.z, 1.0);
    col = pc.col;
}
