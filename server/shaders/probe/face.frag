#version 450
// Red and blue as given, green = gl_FrontFacing (viewport_flip: a negative height flips the facing).
layout(location = 0) in vec4 col;
layout(location = 0) out vec4 o;
void main() { o = vec4(col.r, gl_FrontFacing ? 1.0 : 0.0, col.b, 1.0); }
