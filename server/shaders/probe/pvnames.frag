#version 450
layout(location = 0) in vec4 v;
layout(location = 0) out vec4 o;
void main() {
    o = vec4(v.rgb, 1.0);
}
