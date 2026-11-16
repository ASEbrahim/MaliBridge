#version 450
// view3d_layers' feedback-loop sub-cases, the control: the same read of a plain 2D-array image's layer
layout(set = 0, binding = 0) uniform sampler2DArray tex;
layout(location = 0) flat in uint layer;
layout(location = 0) out vec4 o;
void main()
{
    o = texelFetch(tex, ivec3(ivec2(gl_FragCoord.xy), int(layer)), 0).bgra;
}
