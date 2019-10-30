#include "include/uniforms.glsl"
#include "include/common.glsl"

qf_varying vec2 v_TexCoord;
qf_varying vec4 v_Color;
qf_varying float v_Time;

#if VERTEX_SHADER

uniform sampler2D u_SizeCurve;
uniform sampler2D u_ColorCurve;
uniform vec3 u_ParticleAcceleration;

in vec2 a_Position;
in vec2 a_TexCoord;

in vec3 a_ParticlePosition;

in float a_ParticleScale;
in float a_ParticleEndScale;

in vec4 a_ParticleColor;
in vec4 a_ParticleEndColor;

in vec3 a_ParticleVelocity;
in float a_ParticleTime;
in float a_ParticleLifeTime;

void main() {
	v_TexCoord = a_TexCoord;
	v_Time = a_ParticleTime / a_ParticleLifeTime;

	vec3 camera_right = vec3( u_V[ 0 ].x, u_V[ 1 ].x, u_V[ 2 ].x );
	vec3 camera_up = vec3( u_V[ 0 ].y, u_V[ 1 ].y, u_V[ 2 ].y );

	float size_easing = qf_texture( u_SizeCurve, vec2( v_Time, 0.0 ) ).r;
	float size = mix( a_ParticleScale, a_ParticleEndScale, size_easing );

	float color_easing = qf_texture( u_ColorCurve, vec2( v_Time, 0.0 ) ).r;
	v_Color = mix( a_ParticleColor, a_ParticleEndColor, size_easing );

	vec3 right = a_Position.x * size * camera_right;
	vec3 up = a_Position.y * size * camera_up;
	vec3 position = a_ParticlePosition;
	position += a_ParticleVelocity * a_ParticleTime;
	position += ( 1.0 / 2.0 ) * u_ParticleAcceleration * a_ParticleTime * a_ParticleTime;
	position += right + up;

	gl_Position = u_P * u_V * vec4( position, 1.0 );
}

#else

uniform sampler2D u_BaseTexture;
uniform sampler2D u_ColorRamp; // TODO: make sampler1D

out vec4 f_Albedo;

void main() {
	// TODO: soft particles
	f_Albedo = LinearTosRGB( qf_texture( u_BaseTexture, v_TexCoord ) * v_Color );
}

#endif
