#include "bot.h"
#include "ai_shutdown_hooks_holder.h"
#include "ai_manager.h"
#include "ai_objective_based_team_brain.h"
#include "ai_tactical_spots_detector.h"

ai_weapon_aim_type BuiltinWeaponAimType(int builtinWeapon)
{
    switch (builtinWeapon)
    {
        case WEAP_GUNBLADE:
            return AI_WEAPON_AIM_TYPE_PREDICTION_EXPLOSIVE;
        case WEAP_GRENADELAUNCHER:
            return AI_WEAPON_AIM_TYPE_DROP;
        case WEAP_ROCKETLAUNCHER:
            return AI_WEAPON_AIM_TYPE_PREDICTION_EXPLOSIVE;
        case WEAP_PLASMAGUN:
            return AI_WEAPON_AIM_TYPE_PREDICTION;
        default:
            return AI_WEAPON_AIM_TYPE_INSTANT_HIT;
    }
}

int BuiltinWeaponTier(int builtinWeapon)
{
    switch (builtinWeapon)
    {
        case WEAP_INSTAGUN:
            return 4;
        case WEAP_ELECTROBOLT:
        case WEAP_LASERGUN:
        case WEAP_PLASMAGUN:
        case WEAP_ROCKETLAUNCHER:
            return 3;
        case WEAP_MACHINEGUN:
        case WEAP_RIOTGUN:
            return 2;
        case WEAP_GRENADELAUNCHER:
            return 1;
        default:
            return 0;
    }
}

static void EscapePercent(const char *string, char *buffer, int bufferLen)
{
    int j = 0;
    for (const char *s = string; *s && j < bufferLen - 1; ++s)
    {
        if (*s != '%')
            buffer[j++] = *s;
        else if (j < bufferLen - 2)
            buffer[j++] = '%', buffer[j++] = '%';
    }
    buffer[j] = '\0';
}

void AI_Debug(const char *nick, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    AI_Debugv(nick, format, va);
    va_end(va);
}

void AI_Debugv(const char *nick, const char *format, va_list va)
{
    char concatBuffer[1024];

    int prefixLen = sprintf(concatBuffer, "t=%09d %s: ", level.time, nick);

    Q_vsnprintfz(concatBuffer + prefixLen, 1024 - prefixLen, format, va);

    // concatBuffer may contain player names such as "%APPDATA%"
    char outputBuffer[2048];
    EscapePercent(concatBuffer, outputBuffer, 2048);
    G_Printf(outputBuffer);
}

void AI_FailWith(const char *tag, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    AI_FailWithv(tag, format, va);
    va_end(va);
}

void AI_FailWithv(const char *tag, const char *format, va_list va)
{
    AI_Debugv(tag, format, va);
    fflush(stdout);
    abort();
}

void AITools_DrawLine( const vec3_t origin, const vec3_t dest )
{
    edict_t	*event;

    event = G_SpawnEvent( EV_GREEN_LASER, 0, const_cast<float *>(origin) );
    event->r.svflags = SVF_TRANSMITORIGIN2;
    VectorCopy( dest, event->s.origin2 );
    G_SetBoundsForSpanEntity( event, 8 );
    GClip_LinkEntity( event );
}

void AITools_DrawColorLine( const vec3_t origin, const vec3_t dest, int color, int parm )
{
    edict_t	*event;

    event = G_SpawnEvent( EV_PNODE, parm, const_cast<float *>(origin) );
    event->s.colorRGBA = color;
    event->r.svflags = SVF_TRANSMITORIGIN2;
    VectorCopy( dest, event->s.origin2 );
    G_SetBoundsForSpanEntity( event, 8 );
    GClip_LinkEntity( event );
}

static StaticVector<int, 16> hubAreas;

//==========================================
// AI_InitLevel
// Inits Map local parameters
//==========================================
void AI_InitLevel( void )
{
    AiAasWorld::Init(level.mapname);
    AiAasRouteCache::Init(*AiAasWorld::Instance());

    NavEntitiesRegistry::Instance()->Init();
}

void AI_Shutdown( void )
{
    hubAreas.clear();

    AI_UnloadLevel();

    AiShutdownHooksHolder::Instance()->InvokeHooks();
}

void AI_UnloadLevel()
{
    AI_RemoveBots();
    AiAasRouteCache::Shutdown();
    AiAasWorld::Shutdown();
}

void AI_GametypeChanged(const char *gametype)
{
    AiManager::OnGametypeChanged(gametype);
}

void AI_JoinedTeam(edict_t *ent, int team)
{
    AiManager::Instance()->OnBotJoinedTeam(ent, team);
}

void AI_CommonFrame()
{
    AiAasWorld::Instance()->Frame();

    NavEntitiesRegistry::Instance()->Update();

    AiManager::Instance()->Update();
}

static void FindHubAreas()
{
    if (!hubAreas.empty())
        return;

    AiAasWorld *aasWorld = AiAasWorld::Instance();
    if (!aasWorld->IsLoaded())
        return;

    // Select not more than hubAreas.capacity() grounded areas that have highest connectivity to other areas.

    struct AreaAndReachCount
    {
        int area, reachCount;
        AreaAndReachCount(int area_, int reachCount_): area(area_), reachCount(reachCount_) {}
        // Ensure that area with lowest reachCount will be evicted in pop_heap(), so use >
        bool operator<(const AreaAndReachCount &that) const { return reachCount > that.reachCount; }
    };

    StaticVector<AreaAndReachCount, hubAreas.capacity() + 1> bestAreasHeap;
    for (int i = 1; i < aasWorld->NumAreas(); ++i)
    {
        const auto &areaSettings = aasWorld->AreaSettings()[i];
        if (!(areaSettings.areaflags & AREA_GROUNDED))
            continue;
        if (areaSettings.areaflags & AREA_DISABLED)
            continue;
        if (areaSettings.contents & (AREACONTENTS_DONOTENTER|AREACONTENTS_LAVA|AREACONTENTS_SLIME|AREACONTENTS_WATER))
            continue;

        // Reject degenerate areas, pass only relatively large areas
        const auto &area = aasWorld->Areas()[i];
        if (area.maxs[0] - area.mins[0] < 128.0f)
            continue;
        if (area.maxs[1] - area.mins[1] < 128.0f)
            continue;

        // Count as useful only several kinds of reachabilities
        int usefulReachCount = 0;
        int reachNum = areaSettings.firstreachablearea;
        int lastReachNum = areaSettings.firstreachablearea + areaSettings.numreachableareas - 1;
        while (reachNum <= lastReachNum)
        {
            const auto &reach = aasWorld->Reachabilities()[reachNum];
            if (reach.traveltype == TRAVEL_WALK || reach.traveltype == TRAVEL_WALKOFFLEDGE)
                usefulReachCount++;
            ++reachNum;
        }

        // Reject early to avoid more expensive call to push_heap()
        if (!usefulReachCount)
            continue;

        bestAreasHeap.push_back(AreaAndReachCount(i, usefulReachCount));
        std::push_heap(bestAreasHeap.begin(), bestAreasHeap.end());

        // bestAreasHeap size should be always less than its capacity:
        // 1) to ensure that there is a free room for next area;
        // 2) to ensure that hubAreas capacity will not be exceeded.
        if (bestAreasHeap.size() == bestAreasHeap.capacity())
        {
            std::pop_heap(bestAreasHeap.begin(), bestAreasHeap.end());
            bestAreasHeap.pop_back();
        }
    }
    static_assert(bestAreasHeap.capacity() == hubAreas.capacity() + 1, "");
    for (const auto &areaAndReachCount: bestAreasHeap)
        hubAreas.push_back(areaAndReachCount.area);
}

static inline void ExtendDimension(float *mins, float *maxs, int dimension)
{
    float side = maxs[dimension] - mins[dimension];
    if (side < 48.0f)
    {
        maxs[dimension] += 0.5f * (48.0f - side);
        mins[dimension] -= 0.5f * (48.0f - side);
    }
}

static int FindGoalAASArea(edict_t *ent)
{
    AiAasWorld *aasWorld = AiAasWorld::Instance();
    if (!aasWorld->IsLoaded())
        return 0;

    Vec3 mins(ent->r.mins), maxs(ent->r.maxs);
    // Extend AABB XY dimensions
    ExtendDimension(mins.Data(), maxs.Data(), 0);
    ExtendDimension(mins.Data(), maxs.Data(), 1);
    // Z needs special extension rules
    float presentHeight = maxs.Z() - mins.Z();
    float playerHeight = playerbox_stand_maxs[2] - playerbox_stand_mins[2];
    if (playerHeight > presentHeight)
        maxs.Z() += playerHeight - presentHeight;


    // Find all areas in bounds
    int areas[16];
    // Convert bounds to absolute ones
    mins += ent->s.origin;
    maxs += ent->s.origin;
    const int numAreas = aasWorld->BBoxAreas(mins, maxs, areas, 16);

    // Find hub areas (or use cached)
    FindHubAreas();

    int bestArea = 0;
    int bestAreaReachCount = 0;
    AiAasRouteCache *routeCache = AiAasRouteCache::Shared();
    for (int i = 0; i < numAreas; ++i)
    {
        const int areaNum = areas[i];
        int areaReachCount = 0;
        for (const int hubAreaNum: hubAreas)
        {
            const aas_area_t &hubArea = aasWorld->Areas()[hubAreaNum];
            Vec3 hubAreaPoint(hubArea.center);
            hubAreaPoint.Z() = hubArea.mins[2] + std::min(24.0f, hubArea.maxs[2] - hubArea.mins[2]);
            // Do not waste pathfinder cycles testing for preferred flags that may fail.
            constexpr int travelFlags = Bot::ALLOWED_TRAVEL_FLAGS;
            if (routeCache->ReachabilityToGoalArea(hubAreaNum, hubAreaPoint.Data(), areaNum, travelFlags))
            {
                areaReachCount++;
                // Thats't enough, do not waste CPU cycles
                if (areaReachCount == 4)
                    return areaNum;
            }
        }
        if (areaReachCount > bestAreaReachCount)
        {
            bestArea = areaNum;
            bestAreaReachCount = areaReachCount;
        }
    }
    if (bestArea)
        return bestArea;

    // Fall back to a default method and hope it succeeds
    return aasWorld->FindAreaNum(ent);
}

void AI_AddNavEntity(edict_t *ent, ai_nav_entity_flags flags)
{
    if (!flags)
    {
        G_Printf(S_COLOR_RED "AI_AddNavEntity(): flags are empty");
        return;
    }
    int onlyMutExFlags = flags & (AI_NAV_REACH_AT_TOUCH | AI_NAV_REACH_AT_RADIUS | AI_NAV_REACH_ON_EVENT);
    // Valid mutual exclusive flags give a power of two
    if (onlyMutExFlags & (onlyMutExFlags - 1))
    {
        G_Printf(S_COLOR_RED, "AI_AddNavEntity(): illegal flags %x for nav entity %s", flags, ent->classname);
        return;
    }

    if ((flags & AI_NAV_NOTIFY_SCRIPT) && !(flags & (AI_NAV_REACH_AT_TOUCH|AI_NAV_REACH_AT_RADIUS)))
    {
        G_Printf(S_COLOR_RED,
                 "AI_AddNavEntity(): NOTIFY_SCRIPT flag may be combined only with REACH_AT_TOUCH or REACH_AT_RADIUS");
        return;
    }

    NavEntityFlags navEntityFlags = NavEntityFlags::NONE;
    if (flags & AI_NAV_REACH_AT_TOUCH)
        navEntityFlags = navEntityFlags | NavEntityFlags::REACH_AT_TOUCH;
    if (flags & AI_NAV_REACH_AT_RADIUS)
        navEntityFlags = navEntityFlags | NavEntityFlags::REACH_AT_RADIUS;
    if (flags & AI_NAV_REACH_ON_EVENT)
        navEntityFlags = navEntityFlags | NavEntityFlags::REACH_ON_EVENT;
    if (flags & AI_NAV_REACH_IN_GROUP)
        navEntityFlags = navEntityFlags | NavEntityFlags::REACH_IN_GROUP;
    if (flags & AI_NAV_DROPPED)
        navEntityFlags = navEntityFlags | NavEntityFlags::DROPPED_ENTITY;
    if (flags & AI_NAV_MOVABLE)
        navEntityFlags = navEntityFlags | NavEntityFlags::MOVABLE;
    if (flags & AI_NAV_NOTIFY_SCRIPT)
        navEntityFlags = navEntityFlags | NavEntityFlags::NOTIFY_SCRIPT;

    int areaNum = FindGoalAASArea(ent);
    // Allow addition of temporary unreachable goals based on movable entities
    if (areaNum || (flags & AI_NAV_MOVABLE))
    {
        NavEntitiesRegistry::Instance()->AddNavEntity(ent, areaNum, navEntityFlags);
        return;
    }
    constexpr const char *format = S_COLOR_RED "AI_AddNavEntity(): Can't find an area num for %s @ %.3f %.3f %.3f\n";
    G_Printf(format, ent->classname, ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

void AI_RemoveNavEntity(edict_t *ent)
{
    NavEntity *navEntity = NavEntitiesRegistry::Instance()->NavEntityForEntity(ent);
    // (An nav. item absence is not an error, this function is called for each entity in game)
    if (!navEntity)
        return;

    AiManager::Instance()->ClearGoals(navEntity, nullptr);
    NavEntitiesRegistry::Instance()->RemoveNavEntity(navEntity);
}

void AI_NavEntityReached(edict_t *ent)
{
    AiManager::Instance()->NavEntityReached(ent);
}

//==========================================
// G_FreeAI
// removes the AI handle from memory
//==========================================
void G_FreeAI( edict_t *ent )
{
    if (!ent->ai)
        return;

    AiManager::Instance()->UnlinkAi(ent->ai);

    // Perform virtual destructor call
    ent->ai->aiRef->~Ai();

    G_Free(ent->ai);
    ent->ai = nullptr;
}

void G_SpawnAI( edict_t *ent, float skillLevel )
{
    if (ent->ai)
        AI_FailWith("G_SpawnAI()", "Entity AI has been already initialized\n");

    if (!(ent->r.svflags & SVF_FAKECLIENT))
        AI_FailWith("G_SpawnAI()", "Only fake clients are supported\n");

    size_t memSize = sizeof(ai_handle_t) + sizeof(Bot);
    size_t alignmentBytes = 0;
    if (sizeof(ai_handle_t) % 16)
        alignmentBytes = 16 - sizeof(ai_handle_t) % 16;
    memSize += alignmentBytes;

    char *mem = (char *)G_Malloc(memSize);
    ent->ai = (ai_handle_t *)mem;
    ent->ai->type = AI_ISBOT;

    char *botMem = mem + sizeof(ai_handle_t) + alignmentBytes;
    ent->ai->botRef = new(botMem) Bot(ent, skillLevel);
    ent->ai->aiRef = ent->ai->botRef;

    AiManager::Instance()->LinkAi(ent->ai);
}

//==========================================
// AI_GetType
//==========================================
ai_type AI_GetType( const ai_handle_t *ai )
{
    return ai ? ai->type : AI_INACTIVE;
}

void AI_TouchedEntity(edict_t *self, edict_t *ent)
{
    self->ai->aiRef->TouchedEntity(ent);
}

void AI_DamagedEntity(edict_t *self, edict_t *ent, int damage)
{
    if (self->ai->botRef)
        self->ai->botRef->OnEnemyDamaged(ent, damage);
}

void AI_Pain(edict_t *self, edict_t *attacker, int kick, int damage)
{
    if (self->ai->botRef)
        self->ai->botRef->Pain(attacker, kick, damage);
}

void AI_Think(edict_t *self)
{
    if( !self->ai || self->ai->type == AI_INACTIVE )
        return;

    self->ai->aiRef->Update();
}

void AI_SpawnBot( const char *team )
{
    AiManager::Instance()->SpawnBot(team);
}

void AI_Respawn( edict_t *ent )
{
    AiManager::Instance()->RespawnBot(ent);
}

void AI_RemoveBot( const char *name )
{
    AiManager::Instance()->RemoveBot(name);
}

void AI_RemoveBots()
{
    AiManager::Instance()->RemoveBots();
}

void AI_Cheat_NoTarget( edict_t *ent )
{
    if (!sv_cheats->integer)
        return;

    ent->flags ^= FL_NOTARGET;
    if (ent->flags & FL_NOTARGET)
        G_PrintMsg(ent, "Bot Notarget ON\n");
    else
        G_PrintMsg(ent, "Bot Notarget OFF\n");
}
