#version 450
// view3d_layers' feedback-loop sub-cases: reads the 3D image itself at this texel of slice base + layer, while the pass renders it
// (ATTACHMENT_FEEDBACK_LOOP_OPTIMAL), and writes it red and blue swapped
layout(push_constant) uniform P { int base; } pc;
layout(set = 0, binding = 0) uniform sampler3D tex;
layout(location = 0) flat in uint layer;
layout(location = 0) out vec4 o;
void main()
{
    o = texelFetch(tex, ivec3(ivec2(gl_FragCoord.xy), pc.base + int(layer)), 0).bgra;
}
