#version 450
// One point per instance at pixel column gl_InstanceIndex; red/green from two instance-rate attributes.
layout(location = 0) in float a0;
layout(location = 1) in float a1;
layout(location = 0) out vec4 col;
void main() {
    gl_Position = vec4((float(gl_InstanceIndex) + 0.5) / 4.0 - 1.0, 0.0, 0.5, 1.0);
    gl_PointSize = 1.0;
    col = vec4(a0, a1, 0.0, 1.0);
}
