/*
Copyright (C) 2013 Victor Luchits

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

// r_mesh.c: transformation and sorting

#include "r_local.h"

drawList_t r_worldlist;
drawList_t r_shadowlist;
drawList_t r_portalmasklist;
drawList_t r_portallist, r_skyportallist;

/*
* R_InitDrawList
*/
void R_InitDrawList( drawList_t *list )
{
	memset( list, 0, sizeof( *list ) );
}

/*
* R_InitDrawLists
*/
void R_InitDrawLists( void )
{
	R_InitDrawList( &r_worldlist );
	R_InitDrawList( &r_portalmasklist );
	R_InitDrawList( &r_portallist );
	R_InitDrawList( &r_skyportallist );
	R_InitDrawList( &r_shadowlist );
}

/*
* R_ClearDrawList
*/
void R_ClearDrawList( drawList_t *list )
{
	if( !list ) {
		return;
	}

	// clear counters
	list->numDrawSurfs = 0;

	list->numSliceElems = list->numSliceElemsReal = 0;
	list->numSliceVerts = list->numSliceVertsReal = 0;

	// clear VBO slices
	if( list->vboSlices ) {
		memset( list->vboSlices, 0, sizeof( *list->vboSlices ) * list->maxVboSlices );
	}
	if( list->indirectCmds ) {
		memset( list->indirectCmds, 0, sizeof( *list->indirectCmds ) * list->maxIndirectsCmds );
	}
}

/*
* R_ReserveDrawSurfaces
*/
static void R_ReserveDrawSurfaces( drawList_t *list, int minMeshes )
{
	int oldSize, newSize;
	sortedDrawSurf_t *newDs;
	sortedDrawSurf_t *ds = list->drawSurfs;
	int maxMeshes = list->maxDrawSurfs;

	oldSize = maxMeshes;
	newSize = max( minMeshes, oldSize * 2 );

	newDs = R_Malloc( newSize * sizeof( sortedDrawSurf_t ) );
	if( ds ) {
		memcpy( newDs, ds, oldSize * sizeof( sortedDrawSurf_t ) );
		R_Free( ds );
	}
	
	list->drawSurfs = newDs;
	list->maxDrawSurfs = newSize;
}

/*
* R_PackDistKey
*/
static unsigned int R_PackDistKey( int shaderSort, int dist, int order )
{
	return (shaderSort << 26) | ((max(0x400 - dist, 0) << 5) & 0x7FFF) << 11 | min( order, 0x7FF );
}

/*
* R_PackSortKey
*/
static unsigned int R_PackSortKey( unsigned int shaderNum, int fogNum, 
	int portalNum, unsigned int entNum )
{
	return (shaderNum & 0x7FF) << 21 | (entNum & 0x7FF) << 10 | 
		(((portalNum+1) & 0x1F) << 5) | ((unsigned int)(fogNum+1) & 0x1F);
}

/*
* R_UnpackSortKey
*/
static void R_UnpackSortKey( unsigned int sortKey, unsigned int *shaderNum, int *fogNum, 
	int *portalNum, unsigned int *entNum )
{
	*shaderNum = (sortKey >> 21) & 0x7FF;
	*entNum = (sortKey >> 10) & 0x7FF;
	*portalNum = (signed int)((sortKey >> 5) & 0x1F) - 1;
	*fogNum = (signed int)(sortKey & 0x1F) - 1;
}

/*
* R_AddSurfToDrawList
* 
* Calculate sortkey and store info used for batching and sorting.
* All 3D-geometry passes this function.
*/
bool R_AddSurfToDrawList( drawList_t *list, const entity_t *e, const mfog_t *fog, const shader_t *shader, 
	float dist, unsigned int order, const portalSurface_t *portalSurf, void *drawSurf )
{
	sortedDrawSurf_t *sds;
	int shaderSort;
	bool depthWrite;
	int renderFx;

	if( !list || !shader ) {
		return false;
	}
	if( Shader_ReadDepth( shader ) && ( rn.renderFlags & RF_SHADOWMAPVIEW ) ) {
		return false;
	}

	shaderSort = shader->sort;
	depthWrite = (shader->flags & SHADER_DEPTHWRITE) ? true : false;
	renderFx = e->renderfx;

	if( shader->cin ) {
		R_UploadCinematicShader( shader );
	}

	// reallocate if numDrawSurfs
	if( list->numDrawSurfs >= list->maxDrawSurfs ) {
		int minMeshes = MIN_RENDER_MESHES;
		if( rsh.worldBrushModel ) {
			minMeshes += rsh.worldBrushModel->numDrawSurfaces;
		}
		R_ReserveDrawSurfaces( list, minMeshes );
	}

	if( renderFx & RF_WEAPONMODEL ) {
		if( renderFx & RF_NOCOLORWRITE ) {
			// depth-pass for alpha-blended weapon:
			// write to depth but do not write to color
			if( !depthWrite ) {
				return false;
			}
			// reorder the mesh to be drawn after everything else
			// but before the blend-pass for the weapon
			shaderSort = SHADER_SORT_WEAPON;
		}
		else if( renderFx & RF_ALPHAHACK ) {
			// blend-pass for the weapon:
			// meshes that do not write to depth, are rendered as additives,
			// meshes that were previously added as SHADER_SORT_WEAPON (see above)
			// are now added to the very end of the list
			shaderSort = depthWrite ? SHADER_SORT_WEAPON2 : SHADER_SORT_ADDITIVE;
		}
	}
	else if( renderFx & RF_ALPHAHACK ) {
		// force shader sort to additive
		shaderSort = SHADER_SORT_ADDITIVE;
	}

	sds = &list->drawSurfs[list->numDrawSurfs++];
	sds->distKey = R_PackDistKey( shaderSort, (int)dist, order );
	sds->sortKey = R_PackSortKey( shader->id, fog ? fog - rsh.worldBrushModel->fogs : -1,
		portalSurf ? portalSurf - rn.portalSurfaces : -1, R_ENT2NUM(e) );
	sds->drawSurf = ( drawSurfaceType_t * )drawSurf;

	return true;
}

/*
* R_DrawSurfCompare
*
* Comparison callback function for R_SortDrawList
*/
static int R_DrawSurfCompare( const sortedDrawSurf_t *sbs1, const sortedDrawSurf_t *sbs2 )
{
	if( sbs1->distKey > sbs2->distKey )
		return 1;
	if( sbs2->distKey > sbs1->distKey )
		return -1;

	if( sbs1->sortKey > sbs2->sortKey )
		return 1;
	if( sbs2->sortKey > sbs1->sortKey )
		return -1;

	return 0;
}

/*
* R_SortDrawList
*
* Regular quicksort. Note that for all kinds of transparent meshes
* you probably want to set distance or draw order to prevent flickering
* due to quicksort's unstable nature.
*/
void R_SortDrawList( drawList_t *list )
{
	if( r_draworder->integer ) {
		return;
	}
	qsort( list->drawSurfs, list->numDrawSurfs, sizeof( sortedDrawSurf_t ), 
		(int (*)(const void *, const void *))R_DrawSurfCompare );
}

/*
* R_ReserveVBOSlices
*
* Ensures there's enough space to store the minSlices amount of slices
* plus some more.
*/
static void R_ReserveVBOSlices( drawList_t *list, unsigned int minSlices )
{
	unsigned int oldSize, newSize;
	vboSlice_t *slices, *newSlices;

	oldSize = list->maxVboSlices;
	newSize = max( minSlices, oldSize * 2 );

	slices = list->vboSlices;
	newSlices = R_Malloc( newSize * sizeof( vboSlice_t ) );
	if( slices ) {
		memcpy( newSlices, slices, oldSize * sizeof( vboSlice_t ) );
		R_Free( slices );
	}

	list->vboSlices = newSlices;
	list->maxVboSlices = newSize;
}

/*
* R_AddVBOSlice
*/
void R_AddVBOSlice( unsigned int index, unsigned int numVerts, unsigned int numElems, 
	unsigned int firstVert, unsigned int firstElem )
{
	drawList_t *list = rn.meshlist;
	vboSlice_t *slice;

	if( index >= list->maxVboSlices ) {
		unsigned int minSlices = index + 1;
		if( rsh.worldBrushModel ) {
			minSlices = max( rsh.worldBrushModel->numDrawSurfaces, minSlices );
		}
		R_ReserveVBOSlices( list, minSlices );
	}

	slice = &list->vboSlices[index];
	if( !slice->numVerts ) {
		// initialize the slice
		slice->firstVert = firstVert;
		slice->firstElem = firstElem;
		slice->numVerts = numVerts;
		slice->numElems = numElems;
		slice->numVertsReal = numVerts;
		slice->numElemsReal = numElems;
	}
	else {
		list->numSliceVertsReal -= slice->numVerts;
		list->numSliceElemsReal -= slice->numElems;

		if( firstVert < slice->firstVert ) {
			// prepend
			slice->numVerts = slice->numVerts + slice->firstVert - firstVert;
			slice->numElems = slice->numElems + slice->firstElem - firstElem;

			slice->firstVert = firstVert;
			slice->firstElem = firstElem;
		} else {
			// append
			slice->numVerts = max( slice->numVerts, numVerts + firstVert - slice->firstVert );
			slice->numElems = max( slice->numElems, numElems + firstElem - slice->firstElem );
		}

		slice->numVertsReal += numVerts;
		slice->numElemsReal += numElems;
	}

	list->numSliceVerts += numVerts;
	list->numSliceVertsReal += slice->numVerts;
	list->numSliceElems += numElems;
	list->numSliceElemsReal += slice->numElems;
}

/*
* R_GetVBOSlice
*/
vboSlice_t *R_GetVBOSlice( unsigned int index )
{
	drawList_t *list = rn.meshlist;

	if( index >= list->maxVboSlices ) {
		return NULL;
	}
	return &list->vboSlices[index];
}

/*
* R_ReserveIndirectCmds
*/
static void R_ReserveIndirectCmds( drawList_t *list, unsigned int minCmds )
{
	unsigned int oldSize, newSize;
	drawElementsIndirectCommand_t *cmds, *newCmds;

	oldSize = list->maxIndirectsCmds;
	newSize = max( minCmds, oldSize * 2 );

	cmds = list->indirectCmds;
	newCmds = R_Malloc( newSize * sizeof( drawElementsIndirectCommand_t ) );
	if( cmds ) {
		memcpy( newCmds, cmds, oldSize * sizeof( drawElementsIndirectCommand_t ) );
		R_Free( cmds );
	}

	list->indirectCmds = newCmds;
	list->maxIndirectsCmds = newSize;
}

/*
* R_AddIndirectCmd
*/
bool R_AddIndirectCmd( unsigned int index, unsigned int firstElem, unsigned count )
{
	drawList_t *list = rn.meshlist;
	drawElementsIndirectCommand_t *cmd;

	if( !glConfig.ext.multi_draw_indirect ) {
		return false;
	}

	if( index >= list->maxIndirectsCmds ) {
		unsigned int minCmds = index + 1;
		if( rsh.worldBrushModel ) {
			minCmds = max( rsh.worldBrushModel->numsurfaces, minCmds );
		}
		R_ReserveIndirectCmds( list, minCmds );
	}

	cmd = &list->indirectCmds[index];

	assert( cmd->count == 0 );

	cmd->firstElement = firstElem;
	cmd->count = count;
	cmd->instanceCount = 1;
	cmd->baseVertex = 0;
	cmd->baseInstance = 0;
	return true;
}

/*
* R_GetIndirectCmd
*/
drawElementsIndirectCommand_t *R_GetIndirectCmd( unsigned int index )
{
	drawList_t *list = rn.meshlist;

	if( index >= list->maxIndirectsCmds ) {
		return NULL;
	}
	return &list->indirectCmds[index];
}

static const beginDrawSurf_cb r_beginDrawSurfCb[ST_MAX_TYPES] =
{
	/* ST_NONE */
	NULL,
	/* ST_BSP */
	(beginDrawSurf_cb)&R_DrawBSPSurf,
	/* ST_SKY */
	(beginDrawSurf_cb)&R_DrawSkySurf,
	/* ST_ALIAS */
	(beginDrawSurf_cb)&R_DrawAliasSurf,
	/* ST_SKELETAL */
	(beginDrawSurf_cb)&R_DrawSkeletalSurf,
	/* ST_SPRITE */
	(beginDrawSurf_cb)&R_BeginSpriteSurf,
	/* ST_POLY */
	(beginDrawSurf_cb)&R_BeginPolySurf,
	/* ST_CORONA */
	(beginDrawSurf_cb)&R_BeginCoronaSurf,
	/* ST_NULLMODEL */
	(beginDrawSurf_cb)&R_DrawNullSurf,
};

static const batchDrawSurf_cb r_batchDrawSurfCb[ST_MAX_TYPES] =
{
	/* ST_NONE */
	NULL,
	/* ST_BSP */
	NULL,
	/* ST_SKY */
	NULL,
	/* ST_ALIAS */
	NULL,
	/* ST_SKELETAL */
	NULL,
	/* ST_SPRITE */
	(batchDrawSurf_cb)&R_BatchSpriteSurf,
	/* ST_POLY */
	(batchDrawSurf_cb)&R_BatchPolySurf,
	/* ST_CORONA */
	(batchDrawSurf_cb)&R_BatchCoronaSurf,
	/* ST_NULLMODEL */
	NULL,
};

/*
* R_DrawSurfaces
*/
static void _R_DrawSurfaces( drawList_t *list )
{
	unsigned int i;
	unsigned int sortKey;
	unsigned int shaderNum = 0, prevShaderNum = MAX_SHADERS;
	unsigned int entNum = 0, prevEntNum = MAX_REF_ENTITIES;
	int portalNum = -1, prevPortalNum = -100500;
	int fogNum = -1, prevFogNum = -100500;
	sortedDrawSurf_t *sds;
	int drawSurfType;
	bool batchDrawSurf = false, prevBatchDrawSurf = false;
	const shader_t *shader;
	const entity_t *entity;
	const mfog_t *fog;
	const portalSurface_t *portalSurface;
	float depthmin = 0.0f, depthmax = 0.0f;
	bool depthHack = false, cullHack = false;
	bool infiniteProj = false, prevInfiniteProj = false;
	bool depthWrite = false;
	bool depthCopied = false;
	int entityFX = 0, prevEntityFX = -1;
	mat4_t projectionMatrix;
	int riFBO = 0;

	if( !list->numDrawSurfs ) {
		return;
	}

	riFBO = RB_BoundFrameBufferObject();

	for( i = 0; i < list->numDrawSurfs; i++ ) {
		sds = list->drawSurfs + i;
		sortKey = sds->sortKey;
		drawSurfType = *(int *)sds->drawSurf;

		assert( drawSurfType > ST_NONE && drawSurfType < ST_MAX_TYPES );

		// decode draw surface properties
		R_UnpackSortKey( sortKey, &shaderNum, &fogNum, &portalNum, &entNum );

		shader = R_ShaderById( shaderNum );
		entity = R_NUM2ENT(entNum);
		fog = fogNum >= 0 ? rsh.worldBrushModel->fogs + fogNum : NULL;
		portalSurface = portalNum >= 0 ? rn.portalSurfaces + portalNum : NULL;
		entityFX = entity->renderfx;

		// see if we need to reset mesh properties in the backend
		if( !prevBatchDrawSurf || shaderNum != prevShaderNum || fogNum != prevFogNum || 
			portalNum != prevPortalNum ||
			( entNum != prevEntNum && !(shader->flags & SHADER_ENTITY_MERGABLE) ) || 
			entityFX != prevEntityFX ) {

			if( prevBatchDrawSurf ) {
				RB_EndBatch();
			}

			// hack the depth range to prevent view model from poking into walls
			if( entity->flags & RF_WEAPONMODEL ) {
				if( !depthHack ) {
					depthHack = true;
					RB_GetDepthRange( &depthmin, &depthmax );
					RB_DepthRange( depthmin, depthmin + 0.3 * ( depthmax - depthmin ) );
				}
			} else {
				if( depthHack ) {
					depthHack = false;
					RB_DepthRange( depthmin, depthmax );
				}
			}

			if( entNum != prevEntNum ) {
				// backface culling for left-handed weapons
				if( entity->flags & RF_CULLHACK ) {
					cullHack = true;
					RB_FlipFrontFace();
				} else if( cullHack ) {
					cullHack = false;
					RB_FlipFrontFace();
				}

				if( shader->flags & SHADER_AUTOSPRITE ) 
					R_TranslateForEntity( entity );
				else
					R_TransformForEntity( entity );
			}

			depthWrite = shader->flags & SHADER_DEPTHWRITE ? true : false;
			if( !depthWrite && !depthCopied && Shader_ReadDepth( shader ) ) {
				depthCopied = true;
				if( rn.fbDepthAttachment && rsh.screenTextureCopy ) {
					RB_BlitFrameBufferObject( rsh.screenTextureCopy->fbo, 
						GL_DEPTH_BUFFER_BIT, FBO_COPY_NORMAL );
				}
			}

			// sky and things that don't use depth test use infinite projection matrix
			// to not pollute the farclip
			infiniteProj = entity->renderfx & RF_NODEPTHTEST ? true : (shader->flags & SHADER_SKY ? true : false);
			if( infiniteProj != prevInfiniteProj ) {
				if( infiniteProj ) {
					Matrix4_Copy( rn.projectionMatrix, projectionMatrix );
					Matrix4_PerspectiveProjectionToInfinity( Z_NEAR, projectionMatrix, glConfig.depthEpsilon );
					RB_LoadProjectionMatrix( projectionMatrix );
				}
				else {
					RB_LoadProjectionMatrix( rn.projectionMatrix );
				}
			}

			RB_BindShader( entity, shader, fog );

			RB_SetShadowBits( (rsc.entShadowBits[entNum] & rn.shadowBits) & rsc.renderedShadowBits );

			RB_SetPortalSurface( portalSurface );

			batchDrawSurf = r_beginDrawSurfCb[drawSurfType]( entity, shader, fog, portalSurface, sds->drawSurf );

			prevShaderNum = shaderNum;
			prevEntNum = entNum;
			prevFogNum = fogNum;
			prevBatchDrawSurf = batchDrawSurf;
			prevPortalNum = portalNum;
			prevInfiniteProj = infiniteProj;
			prevEntityFX = entityFX;

			if( batchDrawSurf ) {
				RB_BeginBatch();
			}
		}

		if( batchDrawSurf ) {
			assert( r_batchDrawSurfCb[drawSurfType] != NULL );
			r_batchDrawSurfCb[drawSurfType]( entity, shader, fog, portalSurface, sds->drawSurf );
		}
	}

	if( batchDrawSurf ) {
		RB_EndBatch();
	}
	if( depthHack ) {
		RB_DepthRange( depthmin, depthmax );
	}
	if( cullHack ) {
		RB_FlipFrontFace();
	}

	RB_BindFrameBufferObject( riFBO );
}

/*
* R_DrawSurfaces
*/
void R_DrawSurfaces( drawList_t *list )
{
	bool triOutlines;
	
	triOutlines = RB_EnableTriangleOutlines( false );
	if( !triOutlines ) {
		// do not recurse into normal mode when rendering triangle outlines
		_R_DrawSurfaces( list );
	}
	RB_EnableTriangleOutlines( triOutlines );
}

/*
* R_DrawOutlinedSurfaces
*/
void R_DrawOutlinedSurfaces( drawList_t *list )
{
	bool triOutlines;
	
	if( rn.renderFlags & RF_SHADOWMAPVIEW )
		return;

	// properly store and restore the state, as the 
	// R_DrawOutlinedSurfaces calls can be nested
	triOutlines = RB_EnableTriangleOutlines( true );
	_R_DrawSurfaces( list );
	RB_EnableTriangleOutlines( triOutlines );
}

/*
* R_CopyOffsetElements
*/
void R_CopyOffsetElements( const elem_t *inelems, int numElems, int vertsOffset, elem_t *outelems )
{
	int i;

	for( i = 0; i < numElems; i++, inelems++, outelems++ ) {
		*outelems = vertsOffset + *inelems;
	}
}

/*
* R_CopyOffsetTriangles
*/
void R_CopyOffsetTriangles( const elem_t *inelems, int numElems, int vertsOffset, elem_t *outelems )
{
	int i;
	int numTris = numElems / 3;

	for( i = 0; i < numTris; i++, inelems += 3, outelems += 3 ) {
		outelems[0] = vertsOffset + inelems[0];
		outelems[1] = vertsOffset + inelems[1];
		outelems[2] = vertsOffset + inelems[2];
	}
}

/*
* R_BuildQuadElements
*/
void R_BuildQuadElements( int vertsOffset, int numVerts, elem_t *elems )
{
	int i;

	for( i = 0; i < numVerts; i += 4, vertsOffset += 4, elems += 6 ) {
		elems[0] = vertsOffset;
		elems[1] = vertsOffset + 2 - 1;
		elems[2] = vertsOffset + 2;

		elems[3] = vertsOffset;
		elems[4] = vertsOffset + 3 - 1;
		elems[5] = vertsOffset + 3;
	}
}

/*
* R_BuildTrifanElements
*/
void R_BuildTrifanElements( int vertsOffset, int numVerts, elem_t *elems )
{
	int i;

	for( i = 2; i < numVerts; i++, elems += 3 ) {
		elems[0] = vertsOffset;
		elems[1] = vertsOffset + i - 1;
		elems[2] = vertsOffset + i;
	}
}

/*
* R_BuildTangentVectors
*/
void R_BuildTangentVectors( int numVertexes, vec4_t *xyzArray, vec4_t *normalsArray, 
	vec2_t *stArray, int numTris, elem_t *elems, vec4_t *sVectorsArray )
{
	int i, j;
	float d, *v[3], *tc[3];
	vec_t *s, *t, *n;
	vec3_t stvec[3], cross;
	vec3_t stackTVectorsArray[128];
	vec3_t *tVectorsArray;

	if( numVertexes > sizeof( stackTVectorsArray )/sizeof( stackTVectorsArray[0] ) )
		tVectorsArray = R_Malloc( sizeof( vec3_t )*numVertexes );
	else
		tVectorsArray = stackTVectorsArray;

	// assuming arrays have already been allocated
	// this also does some nice precaching
	memset( sVectorsArray, 0, numVertexes * sizeof( *sVectorsArray ) );
	memset( tVectorsArray, 0, numVertexes * sizeof( *tVectorsArray ) );

	for( i = 0; i < numTris; i++, elems += 3 )
	{
		for( j = 0; j < 3; j++ )
		{
			v[j] = ( float * )( xyzArray + elems[j] );
			tc[j] = ( float * )( stArray + elems[j] );
		}

		// calculate two mostly perpendicular edge directions
		VectorSubtract( v[1], v[0], stvec[0] );
		VectorSubtract( v[2], v[0], stvec[1] );

		// we have two edge directions, we can calculate the normal then
		CrossProduct( stvec[1], stvec[0], cross );

		for( j = 0; j < 3; j++ )
		{
			stvec[0][j] = ( ( tc[1][1] - tc[0][1] ) * ( v[2][j] - v[0][j] ) - ( tc[2][1] - tc[0][1] ) * ( v[1][j] - v[0][j] ) );
			stvec[1][j] = ( ( tc[1][0] - tc[0][0] ) * ( v[2][j] - v[0][j] ) - ( tc[2][0] - tc[0][0] ) * ( v[1][j] - v[0][j] ) );
		}

		// inverse tangent vectors if their cross product goes in the opposite
		// direction to triangle normal
		CrossProduct( stvec[1], stvec[0], stvec[2] );
		if( DotProduct( stvec[2], cross ) < 0 )
		{
			VectorInverse( stvec[0] );
			VectorInverse( stvec[1] );
		}

		for( j = 0; j < 3; j++ )
		{
			VectorAdd( sVectorsArray[elems[j]], stvec[0], sVectorsArray[elems[j]] );
			VectorAdd( tVectorsArray[elems[j]], stvec[1], tVectorsArray[elems[j]] );
		}
	}

	// normalize
	for( i = 0, s = *sVectorsArray, t = *tVectorsArray, n = *normalsArray; i < numVertexes; i++, s += 4, t += 3, n += 4 )
	{
		// keep s\t vectors perpendicular
		d = -DotProduct( s, n );
		VectorMA( s, d, n, s );
		VectorNormalize( s );

		d = -DotProduct( t, n );
		VectorMA( t, d, n, t );

		// store polarity of t-vector in the 4-th coordinate of s-vector
		CrossProduct( n, s, cross );
		if( DotProduct( cross, t ) < 0 )
			s[3] = -1;
		else
			s[3] = 1;
	}

	if( tVectorsArray != stackTVectorsArray )
		R_Free( tVectorsArray );
}
