/*
Copyright (C) 2012 Victor Luchits

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

#include "ftlib_local.h"

struct mempool_s *ftlibPool;

/*
* FTLIB_Init
*/
bool FTLIB_Init( bool verbose ) {
	ftlibPool = FTLIB_AllocPool( "Generic pool" );

	FTLIB_InitSubsystems( verbose );

	trap_Cmd_AddCommand( "fontlist", &FTLIB_PrintFontList );

	return true;
}

/*
* FTLIB_Shutdown
*/
void FTLIB_Shutdown( bool verbose ) {
	FTLIB_ShutdownSubsystems( verbose );

	FTLIB_FreePool( &ftlibPool );

	trap_Cmd_RemoveCommand( "fontlist" );
}

/*
* FTLIB_CopyString
*/
char *FTLIB_CopyString( const char *in ) {
	char *out;

	out = ( char* )FTLIB_Alloc( ftlibPool, sizeof( char ) * ( strlen( in ) + 1 ) );
	Q_strncpyz( out, in, sizeof( char ) * ( strlen( in ) + 1 ) );

	return out;
}
