#include "include/common.glsl"
#include "include/uniforms.glsl"

layout( std140 ) uniform u_Outline {
	float u_OutlineHeight;
};

qf_varying vec4 v_Color;

#if VERTEX_SHADER

#include "include/skinning.glsl"

in vec4 a_Position;
in vec3 a_Normal;

void main() {
	vec4 Position = a_Position;
	vec3 Normal = a_Normal;

#ifdef SKINNED
	Skin( Position, Normal );
#endif

	Position += vec4( Normal * u_OutlineHeight, 0.0 );
	gl_Position = u_P * u_V * u_M * Position;

	v_Color = sRGBToLinear( u_ModelColor );
}

#else

out vec4 f_Albedo;

void main() {
	f_Albedo = LinearTosRGB( v_Color );
}

#endif