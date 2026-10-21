#version 450

layout(set = 0, binding = 0) uniform sampler2D SourceDepth;
layout(push_constant) uniform ResolveConstants
{
	vec4 SourceRect;
} Constants;

void main()
{
	gl_FragDepth = texelFetch(SourceDepth, ivec2(Constants.SourceRect.xy + gl_FragCoord.xy * Constants.SourceRect.zw), 0).r;
}
