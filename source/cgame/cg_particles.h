#pragma once

#include "qcommon/types.h"
#include "client/renderer/renderer.h"

enum EasingFunction {
	EasingFunction_Linear,
	EasingFunction_Quadratic,
	EasingFunction_Cubic,
	EasingFunction_QuadraticEaseIn,
	EasingFunction_QuadraticEaseOut,
};

struct ParticleSystem {
	VertexBuffer vb;
	Span< GPUParticle > gpu_particles;
	size_t num_particles;
	Mesh mesh;

	EasingFunction color_easing;
	EasingFunction size_easing;

	BlendFunc blend_func;
	Texture texture;
	Vec3 acceleration;

	Texture color_curve;
	Texture size_curve;
};

enum RandomDistribution3DType : u8 {
	RandomDistribution3DType_Sphere,
	RandomDistribution3DType_Disk,
	RandomDistribution3DType_Line,
};

struct SphereDistribution {
	float radius;
};

struct ConeDistribution {
	Vec3 normal;
	float radius;
	float theta;
};

struct DiskDistribution {
	Vec3 normal;
	float radius;
};

struct LineDistribution {
	Vec3 end;
};

enum RandomDistributionType : u8 {
	RandomDistributionType_Uniform,
	RandomDistributionType_Normal,
};

struct RandomDistribution {
	u8 type;
	union {
		float uniform;
		float sigma;
	};
};

struct RandomDistribution3D {
	u8 type;
	union {
		SphereDistribution sphere;
		DiskDistribution disk;
		LineDistribution line;
	};
};

struct ParticleEmitter {
	Vec3 position;
	RandomDistribution3D position_distribution;

	Vec3 velocity;
	float end_velocity;
	ConeDistribution velocity_cone;

	Vec4 start_color;
	Vec3 end_color;
	RandomDistribution red_distribution;
	RandomDistribution green_distribution;
	RandomDistribution blue_distribution;
	RandomDistribution alpha_distribution;

	float start_size, end_size;
	RandomDistribution size_distribution;

	float lifetime;
	RandomDistribution lifetime_distribution;
	
	float emission_rate;
	float n;
};

void InitParticles();
void ShutdownParticles();

ParticleSystem NewParticleSystem( Allocator * a, size_t n, Texture texture );
void DeleteParticleSystem( Allocator * a, ParticleSystem ps );

void UpdateEasingCurves( ParticleSystem * ps );

void EmitParticles( ParticleSystem * ps, const ParticleEmitter & emitter );

void DrawParticles();

void InitParticleEditor();
void ShutdownParticleEditor();

void ResetParticleEditor();
void DrawParticleEditor();
