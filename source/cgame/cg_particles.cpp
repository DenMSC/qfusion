#include "qcommon/assets.h"
#include "qcommon/fs.h"
#include "qcommon/serialization.h"
#include "cgame/cg_local.h"

#include "imgui/imgui.h"

void InitParticles() {
	constexpr Vec3 gravity = Vec3( 0, 0, -GRAVITY );

	cgs.ions = NewParticleSystem( sys_allocator, 8192, FindTexture( "$particle" ) );
	cgs.smoke = NewParticleSystem( sys_allocator, 1024, FindTexture( "gfx/misc/cartoon_smokepuff3" ) );

	cgs.sparks = NewParticleSystem( sys_allocator, 8192, FindTexture( "$particle" ) );
	cgs.sparks.acceleration = gravity;
	cgs.sparks.blend_func = BlendFunc_Blend;
}

void ShutdownParticles() {
	DeleteParticleSystem( sys_allocator, cgs.ions );
	DeleteParticleSystem( sys_allocator, cgs.sparks );
	DeleteParticleSystem( sys_allocator, cgs.smoke );
}

ParticleSystem NewParticleSystem( Allocator * a, size_t n, Texture texture ) {
	ParticleSystem ps = { };
	ps.particles = ALLOC_SPAN( a, Particle, n );
	ps.blend_func = BlendFunc_Add;

	ps.texture = texture;

	ps.vb = NewParticleVertexBuffer( n );
	ps.vb_memory = ALLOC_MANY( a, GPUParticle, n );

	{
		constexpr Vec2 verts[] = {
			Vec2( -0.5f, -0.5f ),
			Vec2( 0.5f, -0.5f ),
			Vec2( -0.5f, 0.5f ),
			Vec2( 0.5f, 0.5f ),
		};

		Vec2 half_pixel = 0.5f / Vec2( texture.width, texture.height );
		Vec2 uvs[] = {
			half_pixel,
			Vec2( 1.0f - half_pixel.x, half_pixel.y ),
			Vec2( half_pixel.x, 1.0f - half_pixel.y ),
			1.0f - half_pixel,
		};

		constexpr u16 indices[] = { 0, 1, 2, 3 };

		MeshConfig mesh_config;
		mesh_config.positions = NewVertexBuffer( verts, sizeof( verts ) );
		mesh_config.positions_format = VertexFormat_Floatx2;
		mesh_config.tex_coords = NewVertexBuffer( uvs, sizeof( uvs ) );
		mesh_config.indices = NewIndexBuffer( indices, sizeof( indices ) );
		mesh_config.num_vertices = ARRAY_COUNT( indices );
		mesh_config.primitive_type = PrimitiveType_TriangleStrip;

		ps.mesh = NewMesh( mesh_config );
	}

	return ps;
}

void DeleteParticleSystem( Allocator * a, ParticleSystem ps ) {
	FREE( a, ps.particles.ptr );
	FREE( a, ps.vb_memory );
	DeleteVertexBuffer( ps.vb );
	DeleteMesh( ps.mesh );
}

static float EvaluateEasingDerivative( EasingFunction func, float t ) {
	switch( func ) {
		case EasingFunction_Linear: return 1.0f;
		case EasingFunction_Quadratic: return t < 0.5f ? 4.0f * t : -4.0f * t + 4.0f;
		case EasingFunction_QuadraticEaseIn: return 2.0f * t;
		case EasingFunction_QuadraticEaseOut: return -2.0f * t + 2.0f;
	}

	return 0.0f;
}

static void UpdateParticle( const ParticleSystem * ps, Particle * particle, Vec3 acceleration, float dt ) {
	particle->t += dt;
	float t = particle->t / particle->lifetime;
}

void UpdateParticleSystem( ParticleSystem * ps, float dt ) {
	{
		ZoneScopedN( "Update particles" );
		for( size_t i = 0; i < ps->num_particles; i++ ) {
			UpdateParticle( ps, &ps->particles[ i ], ps->acceleration, dt );
		}
	}

	ZoneScopedN( "Delete expired particles" );

	// delete expired particles
	for( size_t i = 0; i < ps->num_particles; i++ ) {
		Particle & particle = ps->particles[ i ];
		if( particle.t > particle.lifetime ) {
			ps->num_particles--;
			Swap2( &particle, &ps->particles[ ps->num_particles ] );
			i--;
		}
	}
}

void DrawParticleSystem( ParticleSystem * ps ) {
	if( ps->num_particles == 0 )
		return;

	ZoneScoped;

	for( size_t i = 0; i < ps->num_particles; i++ ) {
		const Particle & particle = ps->particles[ i ];
		ps->vb_memory[ i ].position = particle.position;
		ps->vb_memory[ i ].scale = particle.size;
		ps->vb_memory[ i ].end_scale = particle.end_size;
		ps->vb_memory[ i ].color = particle.color;
		ps->vb_memory[ i ].end_color = particle.end_color;
		ps->vb_memory[ i ].velocity = particle.velocity;
		ps->vb_memory[ i ].time = particle.t;
		ps->vb_memory[ i ].lifetime = particle.lifetime;
	}
	
	// generate color easing curve
	u8 color_curve[ 256 ];
	float colorEasingCurrent = 0.0f;
	for( int i = 1; i < 256; i++ ) {
		colorEasingCurrent += EvaluateEasingDerivative( ps->color_easing, 1 / 256.0f );
		color_curve[ i ] = Clamp( 0.0f, colorEasingCurrent, 256.0f );
	}

	TextureConfig colorEasingCurveConfig;
	colorEasingCurveConfig.width = 256;
	colorEasingCurveConfig.height = 1;
	colorEasingCurveConfig.data = color_curve;
	colorEasingCurveConfig.format = TextureFormat_R_U8;
	colorEasingCurveConfig.wrap = TextureWrap_Clamp;

	Texture colorCurve = NewTexture( colorEasingCurveConfig );
	
	// generate size easing curve
	u8 size_curve[ 256 ];
	float sizeEasingCurrent = 0.0f;
	for( int i = 1; i < 256; i++ ) {
		sizeEasingCurrent += EvaluateEasingDerivative( ps->size_easing, 1 / 256.0f );
		size_curve[ i ] = Clamp( 0.0f, sizeEasingCurrent, 256.0f );
	}

	TextureConfig sizeEasingCurveConfig;
	sizeEasingCurveConfig.width = 256;
	sizeEasingCurveConfig.height = 1;
	sizeEasingCurveConfig.data = size_curve;
	sizeEasingCurveConfig.format = TextureFormat_R_U8;
	sizeEasingCurveConfig.wrap = TextureWrap_Clamp;

	Texture sizeCurve = NewTexture( sizeEasingCurveConfig );

	WriteVertexBuffer( ps->vb, ps->vb_memory, ps->num_particles * sizeof( GPUParticle ) );

	DrawInstancedParticles( ps->mesh, ps->vb, ps->texture, colorCurve, sizeCurve, ps->blend_func, ps->acceleration, ps->num_particles );
}

void DrawParticles() {
	float dt = cg.frameTime / 1000.0f;
	UpdateParticleSystem( &cgs.ions, dt );
	UpdateParticleSystem( &cgs.sparks, dt );
	UpdateParticleSystem( &cgs.smoke, dt );
	DrawParticleSystem( &cgs.ions );
	DrawParticleSystem( &cgs.sparks );
	DrawParticleSystem( &cgs.smoke );
}

static void EmitParticle( ParticleSystem * ps, float lifetime, Vec3 position, Vec3 velocity, float dvelocity, Vec4 color, Vec4 end_color, float size, float end_size ) {
	if( ps->num_particles == ps->particles.n )
		return;

	Particle & particle = ps->particles[ ps->num_particles ];

	particle.t = 0.0f;
	particle.lifetime = lifetime;

	particle.position = position;
	particle.velocity = velocity;

	particle.dvelocity = dvelocity;

	particle.color = RGBA8( color );
	particle.end_color = RGBA8( end_color );

	particle.size = size;
	particle.end_size = end_size;

	ps->num_particles++;
}

static float SampleRandomDistribution( RNG * rng, RandomDistribution dist ) {
	if( dist.type == RandomDistributionType_Uniform ) {
		return random_float11( rng ) * dist.uniform;
	}

	return SampleNormalDistribution( rng ) * dist.sigma;
}

static void EmitParticle( ParticleSystem * ps, const ParticleEmitter & emitter, float t ) {
	float lifetime = Max2( 0.0f, emitter.lifetime + SampleRandomDistribution( &cls.rng, emitter.lifetime_distribution ) );

	Vec3 position = emitter.position;

	switch( emitter.position_distribution.type ) {
		case RandomDistribution3DType_Sphere: {
			position += UniformSampleInsideSphere( &cls.rng ) * emitter.position_distribution.sphere.radius;
		} break;

		case RandomDistribution3DType_Disk: {
			Vec2 p = UniformSampleDisk( &cls.rng );
			position += emitter.position_distribution.disk.radius * Vec3( p, 0.0f );
			// TODO: emitter.position_disk.normal;
		} break;

		case RandomDistribution3DType_Line: {
			position = Lerp( position, t, emitter.position_distribution.line.end );
		} break;
	}

	// TODO: separate velocity and direction
	Vec3 velocity = emitter.velocity + UniformSampleInsideSphere( &cls.rng ) * emitter.velocity_cone.radius;
	float dvelocity = ( emitter.end_velocity - emitter.velocity_cone.radius ) / lifetime;

	Vec4 color = emitter.start_color;
	color.x += SampleRandomDistribution( &cls.rng, emitter.red_distribution );
	color.y += SampleRandomDistribution( &cls.rng, emitter.green_distribution );
	color.z += SampleRandomDistribution( &cls.rng, emitter.blue_distribution );
	color.w += SampleRandomDistribution( &cls.rng, emitter.alpha_distribution );
	color = Clamp01( color );

	Vec4 end_color = Vec4( emitter.end_color, -color.w );

	float size = Max2( 0.0f, emitter.start_size + SampleRandomDistribution( &cls.rng, emitter.size_distribution ) );
	float end_size = emitter.end_size;

	EmitParticle( ps, lifetime, position, velocity, dvelocity, color, end_color, size, end_size );
}

static void EmitParticles( ParticleSystem * ps, const ParticleEmitter & emitter, float dt ) {
	ZoneScoped;

	float p = emitter.emission_rate > 0 ? emitter.emission_rate * dt : emitter.n;
	u32 n = u32( p );
	float remaining_p = p - n;

	for( u32 i = 0; i < n; i++ ) {
		float t = float( i ) / ( p + 1.0f );
		EmitParticle( ps, emitter, t );
	}

	if( random_p( &cls.rng, remaining_p ) ) {
		EmitParticle( ps, emitter, p / ( p + 1.0f ) );
	}
}

void EmitParticles( ParticleSystem * ps, const ParticleEmitter & emitter ) {
	EmitParticles( ps, emitter, cg.frameTime / 1000.0f );
}

enum ParticleEmitterVersion : u32 {
	ParticleEmitterVersion_First,
};

static void Serialize( SerializationBuffer * buf, SphereDistribution & sphere ) { *buf & sphere.radius; }
static void Serialize( SerializationBuffer * buf, ConeDistribution & cone ) { *buf & cone.normal & cone.radius & cone.theta; }
static void Serialize( SerializationBuffer * buf, DiskDistribution & disk ) { *buf & disk.normal & disk.radius; }
static void Serialize( SerializationBuffer * buf, LineDistribution & line ) { *buf & line.end; }

static void Serialize( SerializationBuffer * buf, RandomDistribution & dist ) {
	*buf & dist.type;
	if( dist.type == RandomDistributionType_Uniform )
		*buf & dist.uniform;
	else
		*buf & dist.sigma;
}

static void Serialize( SerializationBuffer * buf, RandomDistribution3D & dist ) {
	*buf & dist.type;
	if( dist.type == RandomDistribution3DType_Sphere )
		*buf & dist.sphere;
	else if( dist.type == RandomDistribution3DType_Disk )
		*buf & dist.disk;
	else
		*buf & dist.line;
}

static void Serialize( SerializationBuffer * buf, ParticleEmitter & emitter ) {
	u32 version = ParticleEmitterVersion_First;
	*buf & version;

	*buf & emitter.position & emitter.position_distribution;
	*buf & emitter.velocity & emitter.velocity_cone;

	*buf & emitter.start_color & emitter.end_color & emitter.red_distribution & emitter.green_distribution & emitter.blue_distribution & emitter.alpha_distribution;

	*buf & emitter.start_size & emitter.end_size & emitter.size_distribution;

	*buf & emitter.lifetime & emitter.lifetime_distribution;

	*buf & emitter.emission_rate & emitter.n;
}

/*
 * particle editor
 */

static ParticleSystem editor_ps = { };
static ParticleEmitter editor_emitter;
static char editor_texture_name[ 256 ];
static bool editor_one_shot;
static bool editor_blend;

void InitParticleEditor() {
	strcpy( editor_texture_name, "$particle" );
	editor_one_shot = false;
	editor_blend = false;

	editor_ps = NewParticleSystem( sys_allocator, 8192, FindTexture( StringHash( ( const char * ) editor_texture_name ) ) );
	editor_ps.blend_func = editor_blend ? BlendFunc_Blend : BlendFunc_Add;
	editor_emitter = { };

	editor_emitter.velocity_cone.radius = 400.0f;
	editor_emitter.end_velocity = 400.0f;
	editor_emitter.start_color = vec4_white;
	editor_emitter.end_color = vec4_white.xyz();
	editor_emitter.start_size = 16.0f;
	editor_emitter.end_size = 16.0f;
	editor_emitter.lifetime = 1.0f;
	editor_emitter.emission_rate = 1000;
}

void ShutdownParticleEditor() {
	DeleteParticleSystem( sys_allocator, editor_ps );
}

void ResetParticleEditor() {
	DeleteParticleSystem( sys_allocator, editor_ps );
	editor_ps = NewParticleSystem( sys_allocator, 8192, FindTexture( StringHash( ( const char * ) editor_texture_name ) ) );
	editor_ps.blend_func = editor_blend ? BlendFunc_Blend : BlendFunc_Add;
}

static void RandomDistributionEditor( const char * id, RandomDistribution * dist, float range ) {
	constexpr const char * names[] = { "Uniform", "Normal" };

	TempAllocator temp = cls.frame_arena.temp();

	if( ImGui::BeginCombo( temp( "Distribution##{}", id ), names[ dist->type ] ) ) {
		for( int i = 0; i < 2; i++ ) {
			if( ImGui::Selectable( names[ i ], i == dist->type ) )
				dist->type = RandomDistributionType( i );
			if( i == dist->type )
				ImGui::SetItemDefaultFocus();
		}

		ImGui::EndCombo();
	}

	switch( dist->type ) {
		case RandomDistributionType_Uniform:
			ImGui::SliderFloat( temp( "Range##{}", id ), &dist->uniform, 0, range, "%.2f" );
			break;

		case RandomDistributionType_Normal:
			ImGui::SliderFloat( temp( "Stddev##{}", id ), &dist->sigma, 0, 8, "%.2f" );
			break;
	}
}

void DrawParticleEditor() {
	TempAllocator temp = cls.frame_arena.temp();

	bool emit = false;

	ImGui::PushFont( cls.console_font );
	ImGui::BeginChild( "Particle editor", ImVec2( 300, 0 ) );
	{
		ImGuiWindowFlags popup_flags = ( ImGuiWindowFlags_NoDecoration & ~ImGuiWindowFlags_NoTitleBar ) | ImGuiWindowFlags_NoMove;

		if( ImGui::Button( "Load..." ) ) {
			ImGui::OpenPopup( "Load" );
		}

		ImGui::SameLine();

		if( ImGui::Button( "Save..." ) ) {
			ImGui::OpenPopup( "Save" );
		}

		if( ImGui::BeginPopupModal( "Load", NULL, popup_flags ) ) {
			static char name[ 64 ];
			ImGui::PushItemWidth( 300 );
			if( ImGui::IsWindowAppearing() ) {
				ImGui::SetKeyboardFocusHere();
				strcpy( name, "" );
			}
			bool ok = ImGui::InputText( "##loadpath", name, sizeof( name ), ImGuiInputTextFlags_EnterReturnsTrue );
			ImGui::PopItemWidth();
			ok = ImGui::Button( "Load" ) || ok;

			if( ok ) {
				Span< const char > data = AssetBinary( temp( "particles/{}.emitter", name ) ).cast< const char >();
				if( data.ptr != NULL ) {
					bool ok = Deserialize( editor_emitter, data.ptr, data.n );
					assert( ok );
				}

				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();

			if( ImGui::Button( "Cancel" ) )
				ImGui::CloseCurrentPopup();

			ImGui::EndPopup();
		}

		if( ImGui::BeginPopupModal( "Save", NULL, popup_flags ) ) {
			static char name[ 64 ];
			ImGui::PushItemWidth( 300 );
			if( ImGui::IsWindowAppearing() ) {
				ImGui::SetKeyboardFocusHere();
				strcpy( name, "" );
			}
			bool ok = ImGui::InputText( "##savepath", name, sizeof( name ), ImGuiInputTextFlags_EnterReturnsTrue );
			ImGui::PopItemWidth();
			ok = ImGui::Button( "Save" ) || ok;

			if( ok ) {
				char buf[ 1024 ];
				SerializationBuffer sb( SerializationMode_Serializing, buf, sizeof( buf ) );
				sb & editor_emitter;
				assert( !sb.error );
				// TODO: writefile can fail
				WriteFile( temp( "base/particles/{}.emitter", name ), buf, sb.cursor - buf );
				HotloadAssets( &temp );

				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();

			if( ImGui::Button( "Cancel" ) )
				ImGui::CloseCurrentPopup();

			ImGui::EndPopup();
		}

		ImGui::Separator();

		if( ImGui::InputText( "Texture", editor_texture_name, sizeof( editor_texture_name ) ) ) {
			ResetParticleEditor();
		}

		ImGui::Checkbox( "Blend", &editor_blend );
		editor_ps.blend_func = editor_blend ? BlendFunc_Blend : BlendFunc_Add;

		ImGui::Separator();

		constexpr const char * position_distribution_names[] = { "Sphere", "Disk", "Line" };
		if( ImGui::BeginCombo( "Position distribution", position_distribution_names[ editor_emitter.position_distribution.type ] ) ) {
			for( int i = 0; i < 3; i++ ) {
				if( ImGui::Selectable( position_distribution_names[ i ], i == editor_emitter.position_distribution.type ) )
					editor_emitter.position_distribution.type = RandomDistribution3DType( i );
				if( i == editor_emitter.position_distribution.type )
					ImGui::SetItemDefaultFocus();
			}

			ImGui::EndCombo();
		}

		editor_emitter.position = Vec3( 0 );

		switch( editor_emitter.position_distribution.type ) {
			case RandomDistribution3DType_Sphere:
				ImGui::SliderFloat( "Radius", &editor_emitter.position_distribution.sphere.radius, 0, 100, "%.2f" );
				break;

			case RandomDistribution3DType_Disk:
				ImGui::SliderFloat( "Radius", &editor_emitter.position_distribution.disk.radius, 0, 100, "%.2f" );
				break;

			case RandomDistribution3DType_Line:
				editor_emitter.position = Vec3( 0, -300, 0 );
				editor_emitter.position_distribution.line.end = Vec3( 0, 300, 0 );
				break;
		}

		ImGui::Separator();

		ImGui::SliderFloat( "Start velocity", &editor_emitter.velocity_cone.radius, 0, 1000, "%.2f" );
		ImGui::SliderFloat( "End velocity", &editor_emitter.end_velocity, 0, 1000, "%.2f" );

		ImGui::Separator();

		ImGui::ColorEdit4( "Start color", editor_emitter.start_color.ptr() );
		ImGui::ColorEdit3( "End color", editor_emitter.end_color.ptr() );

		if( ImGui::TreeNodeEx( "Start color randomness", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_NoAutoOpenOnLog ) ) {
			ImGui::PushStyleColor( ImGuiCol_Text, IM_COL32( 255, 0, 0, 255 ) );
			RandomDistributionEditor( "r", &editor_emitter.red_distribution, 1.0f );
			ImGui::PopStyleColor();

			ImGui::PushStyleColor( ImGuiCol_Text, IM_COL32( 0, 255, 0, 255 ) );
			RandomDistributionEditor( "g", &editor_emitter.green_distribution, 1.0f );
			ImGui::PopStyleColor();

			ImGui::PushStyleColor( ImGuiCol_Text, IM_COL32( 0, 0, 255, 255 ) );
			RandomDistributionEditor( "b", &editor_emitter.blue_distribution, 1.0f );
			ImGui::PopStyleColor();

			RandomDistributionEditor( "a", &editor_emitter.alpha_distribution, 1.0f );

			ImGui::TreePop();
		}

		ImGui::Separator();

		ImGui::SliderFloat( "Start size", &editor_emitter.start_size, 0, 256, "%.2f" );
		ImGui::SliderFloat( "End size", &editor_emitter.end_size, 0, 256, "%.2f" );
		RandomDistributionEditor( "size", &editor_emitter.size_distribution, editor_emitter.start_size );

		ImGui::Separator();

		ImGui::SliderFloat( "Lifetime", &editor_emitter.lifetime, 0, 10, "%.2f" );
		RandomDistributionEditor( "lifetime", &editor_emitter.lifetime_distribution, editor_emitter.lifetime );

		ImGui::Separator();

		ImGui::Checkbox( "One shot mode", &editor_one_shot );

		if( editor_one_shot ) {
			ImGui::SliderFloat( "Particle count", &editor_emitter.n, 0, 500, "%.2f" );
			editor_emitter.emission_rate = 0;
			emit = ImGui::Button( "Go" );
		}
		else {
			ImGui::SliderFloat( "Emission rate", &editor_emitter.emission_rate, 0, 500, "%.2f" );
		}
	}
	ImGui::EndChild();
	ImGui::PopFont();

	RendererSetView( Vec3( -400, 0, 400 ), EulerDegrees3( 45, 0, 0 ), 90 );

	float dt = cls.frametime / 1000.0f;

	if( !editor_one_shot || emit || editor_ps.num_particles == 0 ) {
		EmitParticles( &editor_ps, editor_emitter, dt );
	}

	UpdateParticleSystem( &editor_ps, dt );
	DrawParticleSystem( &editor_ps );
}
