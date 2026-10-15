#version 450
// vb_strides (MK-05): one point per instance at pixel column gl_InstanceIndex of the 8x1 target;
// three instance-rate R32_UINT attributes (bindings 0, 2 and 3; every buffer word holds its own
// index) as red, green, blue / 255, so the read-back says which word each instance fetched.
layout(location = 0) in uint a0;
layout(location = 1) in uint a1;
layout(location = 2) in uint a2;
layout(location = 0) out vec4 col;
void main() {
    gl_Position = vec4((float(gl_InstanceIndex) + 0.5) / 4.0 - 1.0, 0.0, 0.5, 1.0);
    gl_PointSize = 1.0;
    col = vec4(float(a0), float(a1), float(a2), 255.0) / 255.0;
}
