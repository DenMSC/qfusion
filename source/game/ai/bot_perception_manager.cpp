#include "ai_shutdown_hooks_holder.h"
#include "ai_caching_game_allocator.h"
#include "bot_perception_manager.h"
#include "bot.h"

static inline bool IsGenericProjectileVisible( const edict_t *self, const edict_t *ent ) {
	trace_t trace;
	edict_t *self_ = const_cast<edict_t *>( self );
	edict_t *ent_ = const_cast<edict_t *>( ent );
	G_Trace( &trace, self_->s.origin, nullptr, nullptr, ent_->s.origin, self_, MASK_OPAQUE );
	return trace.fraction == 1.0f || trace.ent == ENTNUM( ent );
}

// Try testing both origins and a mid point. Its very coarse but should produce satisfiable results in-game.
static inline bool IsLaserBeamVisible( const edict_t *self, const edict_t *ent ) {
	trace_t trace;
	edict_t *self_ = const_cast<edict_t *>( self );
	edict_t *ent_ = const_cast<edict_t *>( ent );
	G_Trace( &trace, self_->s.origin, nullptr, nullptr, ent_->s.origin, self_, MASK_OPAQUE );
	if( trace.fraction == 1.0f || trace.ent == ENTNUM( ent ) ) {
		return true;
	}

	G_Trace( &trace, self_->s.origin, nullptr, nullptr, ent_->s.origin2, self_, MASK_OPAQUE );
	if( trace.fraction == 1.0f || trace.ent == ENTNUM( ent ) ) {
		return true;
	}

	return false;
}

static inline bool IsGenericEntityInPvs( const edict_t *self, const edict_t *ent ) {
	return trap_inPVS( self->s.origin, ent->s.origin );
}

static inline bool IsLaserBeamInPvs( const edict_t *self, const edict_t *ent ) {
	// TODO: We traverse a BSP tree to get area num of self->s.origin on each trap_inPVS call.
	// Moreover we do it in a loop over all PVS-tested entities, it just looks even more obvious here.
	// The game module imports should provide an optimized version
	// of inPVS() call that allows precomputing the first result once before the loop.
	// TODO: Import and use BoxLeafNums() call and find all leafs where the beam is first?

	if( trap_inPVS( self->s.origin, ent->s.origin ) ) {
		return true;
	}
	if( trap_inPVS( self->s.origin, ent->s.origin2 ) ) {
		return true;
	}
	return false;
}

void EntitiesDetector::Run() {
	Clear();

	int entNums[MAX_EDICTS];
	int numEntsInRadius = GClip_FindInRadius( const_cast<float *>( self->s.origin ), MAX_RADIUS, entNums, MAX_EDICTS );

	// Note that we always skip own rockets, plasma, etc.
	// Otherwise all own bot shot events yield a danger.
	// There are some cases when an own rocket can hurt but they are either extremely rare or handled by bot fire code.
	// Own grenades are the only exception. We check grenade think time to skip grenades just fired by bot.
	// If a grenade is about to explode and is close to bot, its likely it has bounced of the world and can hurt.

	const edict_t *gameEdicts = game.edicts;
	for( int i = 0; i < numEntsInRadius; ++i ) {
		const edict_t *ent = gameEdicts + entNums[i];
		switch( ent->s.type ) {
			case ET_ROCKET:
				TryAddEntity( ent, DETECT_ROCKET_SQ_RADIUS, rawRockets );
				break;
			case ET_PLASMA:
				TryAddEntity( ent, DETECT_PLASMA_SQ_RADIUS, rawPlasmas );
				break;
			case ET_BLASTER:
				TryAddEntity( ent, DETECT_GB_BLAST_SQ_RADIUS, rawBlasts );
				break;
			case ET_GRENADE:
				TryAddGrenade( ent, rawGrenades );
				break;
			case ET_LASERBEAM:
				TryAddEntity( ent, DETECT_LG_BEAM_SQ_RADIUS, rawLasers );
			default:
				break;
		}
	}
}

inline void EntitiesDetector::TryAddEntity( const edict_t *ent, float squareDistanceThreshold,
											EntsAndDistancesVector &entsAndDistances ) {
	assert( ent->s.type != ET_GRENADE );

	if( ent->s.ownerNum == ENTNUM( self ) ) {
		return;
	}

	if( GS_TeamBasedGametype() && self->s.team == ent->s.team ) {
		if( !g_allow_teamdamage->integer ) {
			return;
		}
	}

	float squareDistance = DistanceSquared( self->s.origin, ent->s.origin );
	if( squareDistance > squareDistanceThreshold ) {
		return;
	}

	entsAndDistances.emplace_back( EntAndDistance( ENTNUM( ent ), sqrtf( squareDistance ) ) );
}

inline void EntitiesDetector::TryAddGrenade( const edict_t *ent, EntsAndDistancesVector &entsAndDistances ) {
	assert( ent->s.type == ET_GRENADE );

	if( ent->s.ownerNum == ENTNUM( self ) ) {
		if( !g_allow_selfdamage->integer ) {
			return;
		}
		const auto timeout = GS_GetWeaponDef( WEAP_GRENADELAUNCHER )->firedef.timeout;
		// Ignore own grenades in first 500 millis
		if( level.time - ent->nextThink > timeout - 500 ) {
			return;
		}
	} else {
		if( GS_TeamBasedGametype() && ent->s.team == self->s.team ) {
			if( !g_allow_teamdamage->integer ) {
				return;
			}
		}
	}

	float squareDistance = DistanceSquared( self->s.origin, ent->s.origin );
	if( squareDistance < 300 * 300 ) {
		return;
	}

	entsAndDistances.emplace_back( EntAndDistance( ENTNUM( ent ), sqrtf( squareDistance ) ) );
}

class PlasmaBeam
{
	friend class PlasmaBeamsBuilder;

	PlasmaBeam()
		: startProjectile( nullptr ),
		endProjectile( nullptr ),
		owner( nullptr ),
		projectilesCount( 0 ) {}

public:
	PlasmaBeam( const edict_t *firstProjectile )
		: startProjectile( firstProjectile ),
		endProjectile( firstProjectile ),
		owner( game.edicts + firstProjectile->s.ownerNum ),
		projectilesCount( 1 ) {}

	const edict_t *startProjectile;
	const edict_t *endProjectile;
	const edict_t *owner; // May be null if this beam consists of projectiles of many players

	inline Vec3 start() { return Vec3( startProjectile->s.origin ); }
	inline Vec3 end() { return Vec3( endProjectile->s.origin ); }

	int projectilesCount;

	inline void AddProjectile( const edict_t *nextProjectile ) {
		endProjectile = nextProjectile;
		// If the beam is combined from projectiles of many players, the beam owner is unknown
		if( owner != nextProjectile->r.owner ) {
			owner = nullptr;
		}
		projectilesCount++;
	}
};

struct EntAndLineParam {
	int entNum;
	float t;

	inline EntAndLineParam( int entNum_, float t_ ) : entNum( entNum_ ), t( t_ ) {}
	inline bool operator<( const EntAndLineParam &that ) const { return t < that.t; }
};

class SameDirBeamsList
{
	friend class PlasmaBeamsBuilder;
	// All projectiles in this list belong to this line defined as a (point, direction) pair
	Vec3 lineEqnPoint;

	EntAndLineParam *sortedProjectiles;
	unsigned projectilesCount;

	static constexpr float DIST_TO_RAY_THRESHOLD = 200.0f;
	static constexpr float DIR_DOT_THRESHOLD = 0.995f;
	static constexpr float PRJ_PROXIMITY_THRESHOLD = 300.0f;

public:
	bool isAlreadySkipped;
	Vec3 avgDirection;
	PlasmaBeam *plasmaBeams;
	unsigned plasmaBeamsCount;

	SameDirBeamsList( const edict_t *firstEntity, const edict_t *bot );

	inline SameDirBeamsList( SameDirBeamsList &&that )
		: lineEqnPoint( that.lineEqnPoint ),
		sortedProjectiles( that.sortedProjectiles ),
		projectilesCount( that.projectilesCount ),
		isAlreadySkipped( that.isAlreadySkipped ),
		avgDirection( that.avgDirection ),
		plasmaBeams( that.plasmaBeams ),
		plasmaBeamsCount( that.plasmaBeamsCount ) {
		that.sortedProjectiles = nullptr;
		that.plasmaBeams = nullptr;
	}

	~SameDirBeamsList();

	bool TryAddProjectile( const edict_t *projectile );

	void BuildBeams();

	inline float ComputeLineEqnParam( const edict_t *projectile ) {
		const float *origin = projectile->s.origin;

		if( fabsf( avgDirection.X() ) > 0.1f ) {
			return ( origin[0] - lineEqnPoint.X() ) / avgDirection.X();
		}
		if( fabsf( avgDirection.Y() ) > 0.1f ) {
			return ( origin[1] - lineEqnPoint.Y() ) / avgDirection.Y();
		}
		return ( origin[2] - lineEqnPoint.Z() ) / avgDirection.Z();
	}
};

class PlasmaBeamsBuilder
{
	StaticVector<SameDirBeamsList, 1024> sameDirLists;

	static constexpr float SQ_DANGER_RADIUS = 300.0f * 300.0f;

	const edict_t *bot;
	BotPerceptionManager *perceptionManager;

public:
	PlasmaBeamsBuilder( const edict_t *bot_, BotPerceptionManager *perceptionManager_ )
		: bot( bot_ ), perceptionManager( perceptionManager_ ) {}

	void AddProjectile( const edict_t *projectile );
	void FindMostDangerousBeams();
};

CachingGameBufferAllocator<EntAndLineParam, MAX_EDICTS> sortedProjectilesBufferAllocator( "prj" );
CachingGameBufferAllocator<PlasmaBeam, MAX_EDICTS> plasmaBeamsBufferAllocator( "beams" );

SameDirBeamsList::SameDirBeamsList( const edict_t *firstEntity, const edict_t *bot )
	: lineEqnPoint( firstEntity->s.origin ),
	sortedProjectiles( nullptr ),
	projectilesCount( 0 ),
	avgDirection( firstEntity->velocity ),
	plasmaBeams( nullptr ),
	plasmaBeamsCount( 0 ) {
	avgDirection.NormalizeFast();

	// If distance from an infinite line of beam to bot is greater than threshold, skip;
	// Let's compute distance from bot to the beam infinite line;
	Vec3 botOrigin( bot->s.origin );
	float squaredDistanceToBeamLine = ( botOrigin - lineEqnPoint ).Cross( avgDirection ).SquaredLength();
	if( squaredDistanceToBeamLine > DIST_TO_RAY_THRESHOLD * DIST_TO_RAY_THRESHOLD ) {
		isAlreadySkipped = true;
	} else {
		sortedProjectiles = sortedProjectilesBufferAllocator.Alloc();
		plasmaBeams = plasmaBeamsBufferAllocator.Alloc();

		isAlreadySkipped = false;

		sortedProjectiles[projectilesCount++] = EntAndLineParam( ENTNUM( firstEntity ), ComputeLineEqnParam( firstEntity ) );
	}
}

SameDirBeamsList::~SameDirBeamsList() {
	if( isAlreadySkipped ) {
		return;
	}
	// (Do not spam log by messages unless we have allocated memory chunks)
	if( sortedProjectiles ) {
		sortedProjectilesBufferAllocator.Free( sortedProjectiles );
	}
	if( plasmaBeams ) {
		plasmaBeamsBufferAllocator.Free( plasmaBeams );
	}
	sortedProjectiles = nullptr;
	plasmaBeams = nullptr;
}

bool SameDirBeamsList::TryAddProjectile( const edict_t *projectile ) {
	Vec3 direction( projectile->velocity );

	direction.NormalizeFast();

	if( direction.Dot( avgDirection ) < DIR_DOT_THRESHOLD ) {
		return false;
	}

	// Do not process a projectile, but "consume" it anyway...
	if( isAlreadySkipped ) {
		return true;
	}

	// Update average direction
	avgDirection += direction;
	avgDirection.NormalizeFast();

	sortedProjectiles[projectilesCount++] = EntAndLineParam( ENTNUM( projectile ), ComputeLineEqnParam( projectile ) );
	std::push_heap( sortedProjectiles, sortedProjectiles + projectilesCount );

	return true;
}

void SameDirBeamsList::BuildBeams() {
	if( isAlreadySkipped ) {
		return;
	}

	if( projectilesCount == 0 ) {
		AI_FailWith( "SameDirBeamsList::BuildBeams()", "Projectiles count: %d\n", projectilesCount );
	}

	const edict_t *const gameEdicts = game.edicts;

	// Get the projectile that has a maximal `t`
	std::pop_heap( sortedProjectiles, sortedProjectiles + projectilesCount );
	const edict_t *prevProjectile = gameEdicts + sortedProjectiles[--projectilesCount].entNum;

	plasmaBeams[plasmaBeamsCount++] = PlasmaBeam( prevProjectile );

	while( projectilesCount > 0 ) {
		// Get the projectile that has a maximal `t` atm
		std::pop_heap( sortedProjectiles, sortedProjectiles + projectilesCount );
		const edict_t *currProjectile = gameEdicts + sortedProjectiles[--projectilesCount].entNum;

		float prevToCurrLen = ( Vec3( prevProjectile->s.origin ) - currProjectile->s.origin ).SquaredLength();
		if( prevToCurrLen < PRJ_PROXIMITY_THRESHOLD * PRJ_PROXIMITY_THRESHOLD ) {
			// Add the projectile to the last beam
			plasmaBeams[plasmaBeamsCount - 1].AddProjectile( currProjectile );
		} else {
			// Construct new plasma beam at the end of beams array
			plasmaBeams[plasmaBeamsCount++] = PlasmaBeam( currProjectile );
		}
	}
}

void PlasmaBeamsBuilder::AddProjectile( const edict_t *projectile ) {
	for( unsigned i = 0; i < sameDirLists.size(); ++i ) {
		if( sameDirLists[i].TryAddProjectile( projectile ) ) {
			return;
		}
	}
	new ( sameDirLists.unsafe_grow_back() )SameDirBeamsList( projectile, bot );
}

void PlasmaBeamsBuilder::FindMostDangerousBeams() {
	trace_t trace;
	Vec3 botOrigin( bot->s.origin );

	for( unsigned i = 0; i < sameDirLists.size(); ++i ) {
		sameDirLists[i].BuildBeams();
	}

	const auto *weaponDef = GS_GetWeaponDef( WEAP_PLASMAGUN );
	const float plasmaDamage = 0.5f * ( weaponDef->firedef.damage + weaponDef->firedef_weak.damage );
	const float splashRadius = 1.2f * 0.5f * ( weaponDef->firedef.splash_radius + weaponDef->firedef_weak.splash_radius );
	float minDamageScore = 0.0f;

	for( const SameDirBeamsList &beamsList: sameDirLists ) {
		if( beamsList.isAlreadySkipped ) {
			continue;
		}

		for( unsigned i = 0; i < beamsList.plasmaBeamsCount; ++i ) {
			PlasmaBeam *beam = beamsList.plasmaBeams + i;

			Vec3 botToBeamStart = beam->start() - botOrigin;
			Vec3 botToBeamEnd = beam->end() - botOrigin;

			if( botToBeamStart.SquaredLength() > SQ_DANGER_RADIUS && botToBeamEnd.SquaredLength() > SQ_DANGER_RADIUS ) {
				continue;
			}

			Vec3 beamStartToEnd = beam->end() - beam->start();

			float dotBotToStartWithDir = botToBeamStart.Dot( beamStartToEnd );
			float dotBotToEndWithDir = botToBeamEnd.Dot( beamStartToEnd );

			// If the beam has entirely passed the bot and is flying away, skip it
			if( dotBotToStartWithDir > 0 && dotBotToEndWithDir > 0 ) {
				continue;
			}

			Vec3 tracedBeamStart = beam->start();
			Vec3 tracedBeamEnd = beam->end();

			// It works for single-projectile beams too
			Vec3 beamDir( beam->startProjectile->velocity );
			beamDir.NormalizeFast();
			tracedBeamEnd += 256.0f * beamDir;

			G_Trace( &trace, tracedBeamStart.Data(), nullptr, nullptr, tracedBeamEnd.Data(), nullptr, MASK_AISOLID );
			if( trace.fraction == 1.0f ) {
				continue;
			}

			// Direct hit
			if( bot == game.edicts + trace.ent ) {
				float damageScore = beam->projectilesCount * plasmaDamage;
				if( damageScore > minDamageScore ) {
					if( perceptionManager->TryAddDanger( damageScore, trace.endpos, beamsList.avgDirection.Data(), beam->owner ) ) {
						minDamageScore = damageScore;
					}
				}
				continue;
			}

			// Splash hit
			float hitVecLen = botOrigin.FastDistanceTo( trace.endpos );
			if( hitVecLen < splashRadius ) {
				// We treat up to 3 projectiles as a single explosion cluster (other projectiles are still flying)
				float damageScore = std::max( 3, beam->projectilesCount ) * ( 1.0f - hitVecLen / splashRadius );
				if( damageScore > minDamageScore ) {
					if( perceptionManager->TryAddDanger( damageScore, trace.endpos, beamsList.avgDirection.Data(), beam->owner ) ) {
						minDamageScore = damageScore;
					}
				}
			}
		}
	}
}

bool BotPerceptionManager::TryAddDanger( float damageScore, const vec3_t hitPoint, const vec3_t direction,
										 const edict_t *owner, bool splash ) {
	if( primaryDanger ) {
		if( primaryDanger->damage >= damageScore ) {
			return false;
		}
	}

	if( Danger *danger = plasmaBeamDangersPool.New() ) {
		danger->damage = damageScore;
		danger->hitPoint.Set( hitPoint );
		danger->direction.Set( direction );
		danger->attacker = owner;
		danger->splash = splash;
		if( primaryDanger ) {
			primaryDanger->DeleteSelf();
		}
		primaryDanger = danger;
		return true;
	}

	return false;
}


void BotPerceptionManager::ClearDangers() {
	if( primaryDanger ) {
		primaryDanger->DeleteSelf();
	}

	primaryDanger = nullptr;
}

// TODO: Do not detect dangers that may not be seen by bot, but make bot aware if it can hear the danger
void BotPerceptionManager::Frame() {
	RegisterVisibleEnemies();

	if( primaryDanger && primaryDanger->IsValid() ) {
		return;
	}

	ClearDangers();

	EntitiesDetector entitiesDetector( self );
	entitiesDetector.Run();

	if( !entitiesDetector.visibleRockets.empty() ) {
		const auto &def = GS_GetWeaponDef( WEAP_ROCKETLAUNCHER );
		FindProjectileDangers( entitiesDetector.visibleRockets, 1.35f * def->firedef.splash_radius, def->firedef.damage );
	}

	if( !entitiesDetector.visibleBlasts.empty() ) {
		const auto &def = GS_GetWeaponDef( WEAP_GUNBLADE );
		FindProjectileDangers( entitiesDetector.visibleBlasts, 1.20f * def->firedef.splash_radius, def->firedef.damage );
	}

	if( !entitiesDetector.visibleGrenades.empty() ) {
		const auto &def = GS_GetWeaponDef( WEAP_GRENADELAUNCHER );
		FindProjectileDangers( entitiesDetector.visibleGrenades, 1.75f * def->firedef.splash_radius, def->firedef.damage );
	}

	if( !entitiesDetector.visiblePlasmas.empty() ) {
		FindPlasmaDangers( entitiesDetector.visiblePlasmas );
	}

	if( !entitiesDetector.visibleLasers.empty() ) {
		FindLaserDangers( entitiesDetector.visibleLasers );
	}

	// Set the primary danger timeout after all
	if( primaryDanger ) {
		primaryDanger->timeoutAt = level.time + Danger::TIMEOUT;
	}
}

void BotPerceptionManager::FindPlasmaDangers( const EntNumsVector &entNums ) {
	PlasmaBeamsBuilder plasmaBeamsBuilder( self, this );
	const edict_t *gameEdicts = game.edicts;

	for( unsigned i = 0; i < entNums.size(); ++i ) {
		plasmaBeamsBuilder.AddProjectile( gameEdicts + entNums[i] );
	}
	plasmaBeamsBuilder.FindMostDangerousBeams();
}

void BotPerceptionManager::FindLaserDangers( const EntNumsVector &entNums ) {
	trace_t trace;
	edict_t *const gameEdicts = game.edicts;
	float maxDamageScore = 0.0f;

	for( unsigned i = 0; i < entNums.size(); ++i ) {
		edict_t *beam = gameEdicts + entNums[i];
		G_Trace( &trace, beam->s.origin, vec3_origin, vec3_origin, beam->s.origin2, beam, MASK_AISOLID );
		if( trace.fraction == 1.0f ) {
			continue;
		}

		if( self != game.edicts + trace.ent ) {
			continue;
		}

		edict_t *owner = game.edicts + beam->s.ownerNum;

		Vec3 direction( beam->s.origin2 );
		direction -= beam->s.origin;
		float squareLen = direction.SquaredLength();
		if( squareLen > 1 ) {
			direction *= 1.0f / sqrtf( squareLen );
		} else {
			// Very rare but really seen case - beam has zero length
			vec3_t forward, right, up;
			AngleVectors( owner->s.angles, forward, right, up );
			direction += forward;
			direction += right;
			direction += up;
			direction.NormalizeFast();
		}

		// Modify potential damage from a beam by its owner accuracy
		float damageScore = 50.0f;
		if( owner->team != self->team && owner->r.client ) {
			const auto &ownerStats = owner->r.client->level.stats;
			if( ownerStats.accuracy_shots[AMMO_LASERS] > 10 ) {
				float extraDamage = 75.0f;
				extraDamage *= ownerStats.accuracy_hits[AMMO_LASERS];
				extraDamage /= ownerStats.accuracy_shots[AMMO_LASERS];
				damageScore += extraDamage;
			}
		}

		if( damageScore > maxDamageScore ) {
			if( TryAddDanger( damageScore, trace.endpos, direction.Data(), owner, false ) ) {
				maxDamageScore = damageScore;
			}
		}
	}
}

void BotPerceptionManager::FindProjectileDangers( const EntNumsVector &entNums, float dangerRadius, float damageScale ) {
	trace_t trace;
	float minPrjFraction = 1.0f;
	float minDamageScore = 0.0f;
	Vec3 botOrigin( self->s.origin );
	edict_t *const gameEdicts = game.edicts;

	for( unsigned i = 0; i < entNums.size(); ++i ) {
		edict_t *target = gameEdicts + entNums[i];
		Vec3 end = Vec3( target->s.origin ) + 2.0f * Vec3( target->velocity );
		G_Trace( &trace, target->s.origin, target->r.mins, target->r.maxs, end.Data(), target, MASK_AISOLID );
		if( trace.fraction >= minPrjFraction ) {
			continue;
		}

		minPrjFraction = trace.fraction;
		float hitVecLen = botOrigin.FastDistanceTo( trace.endpos );
		if( hitVecLen >= dangerRadius ) {
			continue;
		}

		float damageScore = 1.0f - hitVecLen / dangerRadius;
		if( damageScore <= minDamageScore ) {
			continue;
		}

		// Velocity may be zero for some projectiles (e.g. grenades)
		Vec3 direction( target->velocity );
		float squaredLen = direction.SquaredLength();
		if( squaredLen > 0.1f ) {
			direction *= 1.0f / sqrtf( squaredLen );
		} else {
			direction = Vec3( &axis_identity[AXIS_UP] );
		}
		if( TryAddDanger( damageScore, trace.endpos, direction.Data(), gameEdicts + target->s.ownerNum, true ) ) {
			minDamageScore = damageScore;
		}
	}
}

static bool IsEnemyVisible( const edict_t *self, const edict_t *enemyEnt ) {
	trace_t trace;
	edict_t *const gameEdicts = game.edicts;
	edict_t *ignore = gameEdicts + ENTNUM( self );

	Vec3 traceStart( self->s.origin );
	traceStart.Z() += self->viewheight;
	Vec3 traceEnd( enemyEnt->s.origin );

	G_Trace( &trace, traceStart.Data(), nullptr, nullptr, traceEnd.Data(), ignore, MASK_OPAQUE );
	if( trace.fraction == 1.0f || gameEdicts + trace.ent == enemyEnt ) {
		return true;
	}

	vec3_t dims;
	if( enemyEnt->r.client ) {
		// We're sure clients in-game have quite large and well-formed hitboxes, so no dimensions test is required.
		// However we have a much more important test to do.
		// If this point usually corresponding to an enemy chest/weapon is not
		// considered visible for a bot but is really visible, the bot behavior looks weird.
		// That's why this special test is added.

		// If the view height makes a considerable spatial distinction
		if( abs( enemyEnt->viewheight ) > 8 ) {
			traceEnd.Z() += enemyEnt->viewheight;
			G_Trace( &trace, traceStart.Data(), nullptr, nullptr, traceEnd.Data(), ignore, MASK_OPAQUE );
			if( trace.fraction == 1.0f || gameEdicts + trace.ent == enemyEnt ) {
				return true;
			}
		}

		// We have deferred dimensions computations to a point after trace call.
		for( int i = 0; i < 3; ++i ) {
			dims[i] = enemyEnt->r.maxs[i] - enemyEnt->r.mins[i];
		}
	}
	else {
		for( int i = 0; i < 3; ++i ) {
			dims[i] = enemyEnt->r.maxs[i] - enemyEnt->r.mins[i];
		}
		// Prevent further testing in degenerate case (there might be non-player enemies).
		if( !dims[0] || !dims[1] || !dims[2] ) {
			return false;
		}
		if( std::max( dims[0], std::max( dims[1], dims[2] ) ) < 8 ) {
			return false;
		}
	}

	// Try testing 4 corners of enemy projection onto bot's "view".
	// It is much less expensive that testing all 8 corners of the hitbox.

	Vec3 enemyToBotDir( self->s.origin );
	enemyToBotDir -= enemyEnt->s.origin;
	enemyToBotDir.NormalizeFast();

	vec3_t right, up;
	MakeNormalVectors( enemyToBotDir.Data(), right, up );

	// Add some inner margin to the hitbox (a real model is less than it and the computations are coarse).
	const float sideOffset = ( 0.8f * std::min( dims[0], dims[1] ) ) / 2;
	float zOffset[2] = { enemyEnt->r.maxs[2] - 0.1f * dims[2], enemyEnt->r.mins[2] + 0.1f * dims[2] };
	// Switch the side from left to right
	for( int i = -1; i <= 1; i += 2 ) {
		// Switch Z offset
		for( int j = 0; j < 2; j++ ) {
			// traceEnd = Vec3( enemyEnt->s.origin ) + i * sideOffset * right;
			traceEnd.Set( right );
			traceEnd *= i * sideOffset;
			traceEnd += enemyEnt->s.origin;
			traceEnd.Z() += zOffset[j];
			G_Trace( &trace, traceStart.Data(), nullptr, nullptr, traceEnd.Data(), ignore, MASK_OPAQUE );
			if( trace.fraction == 1.0f || gameEdicts + trace.ent == enemyEnt ) {
				return true;
			}
		}
	}

	return false;
}

void BotPerceptionManager::RegisterVisibleEnemies() {
	if( GS_MatchState() == MATCH_STATE_COUNTDOWN || GS_ShootingDisabled() ) {
		return;
	}

	// Compute look dir before loop
	vec3_t lookDir;
	AngleVectors( self->s.angles, lookDir, nullptr, nullptr );

	const float dotFactor = self->ai->botRef->FovDotFactor();
	auto *botBrain = &self->ai->botRef->botBrain;

	// Note: non-client entities also may be candidate targets.
	StaticVector<EntAndDistance, MAX_EDICTS> candidateTargets;

	edict_t *const gameEdicts = game.edicts;
	for( int i = 1; i < game.numentities; ++i ) {
		edict_t *ent = gameEdicts + i;
		if( botBrain->MayNotBeFeasibleEnemy( ent ) ) {
			continue;
		}

		// Reject targets quickly by fov
		Vec3 toTarget( ent->s.origin );
		toTarget -= self->s.origin;
		float squareDistance = toTarget.SquaredLength();
		if( squareDistance < 1 ) {
			continue;
		}
		if( squareDistance > ent->aiVisibilityDistance * ent->aiVisibilityDistance ) {
			continue;
		}

		float invDistance = Q_RSqrt( squareDistance );
		toTarget *= invDistance;
		if( toTarget.Dot( lookDir ) < dotFactor ) {
			continue;
		}

		// It seams to be more instruction cache-friendly to just add an entity to a plain array
		// and sort it once after the loop instead of pushing an entity in a heap on each iteration
		candidateTargets.emplace_back( EntAndDistance( ENTNUM( ent ), 1.0f / invDistance ) );
	}

	StaticVector<uint16_t, MAX_CLIENTS> visibleTargets;
	static_assert( AiBaseEnemyPool::MAX_TRACKED_ENEMIES <= MAX_CLIENTS, "targetsInPVS capacity may be exceeded" );

	entitiesDetector.FilterRawEntitiesWithDistances( candidateTargets, visibleTargets, botBrain->MaxTrackedEnemies(),
													 IsGenericEntityInPvs, IsEnemyVisible );

	for( auto entNum: visibleTargets )
		botBrain->OnEnemyViewed( gameEdicts + entNum );

	botBrain->AfterAllEnemiesViewed();

	self->ai->botRef->CheckAlertSpots( visibleTargets );
}

template<unsigned N, unsigned M, typename PvsFunc, typename VisFunc>
void EntitiesDetector::FilterRawEntitiesWithDistances( StaticVector<EntAndDistance, N> &rawEnts,
													   StaticVector<uint16_t, M> &filteredEnts,
													   unsigned visEntsLimit,
													   PvsFunc pvsFunc, VisFunc visFunc ) {
	filteredEnts.clear();

	// Do not call inPVS() and G_Visible() inside a single loop for all raw ents.
	// Sort all entities by distance to the bot.
	// Then select not more than visEntsLimit nearest entities in PVS, then call visFunc().
	// It may cause data loss (far entities that may have higher logical priority),
	// but in a common good case (when there are few visible entities) it preserves data,
	// and in the worst case mentioned above it does not act weird from player POV and prevents server hang up.

	std::sort( rawEnts.begin(), rawEnts.end() );

	const edict_t *gameEdicts = game.edicts;

	StaticVector<uint16_t, M> entsInPvs;
	unsigned limit = std::min( visEntsLimit, std::min( rawEnts.size(), entsInPvs.capacity() ) );
	for( unsigned i = 0; i < limit; ++i ) {
		uint16_t entNum = (uint16_t)rawEnts[i].entNum;
		if( pvsFunc( self, gameEdicts + entNum ) ) {
			entsInPvs.push_back( entNum );
		}
	}

	for( auto entNum: entsInPvs ) {
		const edict_t *ent = gameEdicts + entNum;
		if( visFunc( self, ent ) ) {
			filteredEnts.push_back( entNum );
		}
	}
};