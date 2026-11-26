/*
===========================================================================
FnQAA: mounting Quake Arena Arcade's package in the engine filesystem.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.

Included by code/qcommon/files.c, whose pack and handle structures it uses.

The game's data is baseq3/pak0.bpk inside the user's XBLA package (STFS).
It is read in place: nothing is extracted and nothing is written. The cvar
fs_qaaPackage names one of
  - the STFS package file itself,
  - a folder holding the package's files (default.xex, baseq3/pak0.bpk), or
  - a .bpk file.
With fs_qaaPackage empty, FS_QAA_FindPackage looks for the package in the
install and home folders, where the XBLA zip or an Xbox 360 Content folder
puts it.
BPK records are keyed by a 64-bit hash of the path (qaa_bpk_name_hash,
code/qaa/qaa_bpk.c). As in retail (default.xex: lookup at 0x82120C98), a
file is found only by its exact name: the lookup functions in files.c call
FS_QAA_ResolveName, which hashes the requested path and, on a hit, links the
record into the pack's hash chain under that name. BPK packs keep numfiles
at 0, so directory listings never include their records, because the table
holds only hashes and retail cannot enumerate it either. Every record is also
reachable by raw hash through qaa_bpkread.
===========================================================================
*/

static cvar_t	*fs_qaaPackage;
static char		fs_qaaSource[MAX_OSPATH * 2];	// what was mounted, for qaa_info
static char		fs_qaaFound[MAX_OSPATH * 2];	// the package FS_QAA_FindPackage found, if any
static const char *fs_qaaKind = "none";
static uint32_t	fs_qaaTitleId;

/*
=================
FS_QAA_OpenBpkRecord

Decodes one BPK record into a memory-backed handle.
=================
*/
static int FS_QAA_OpenBpkRecord( fileHandle_t *file, pack_t *pak, uint32_t index, const char *name ) {
	const qaaBpkEntry_t *entry = pak->qaaBpk ? QAA_BpkEntry( pak->qaaBpk, index ) : NULL;
	fileHandleData_t *f;
	fileHandle_t handle;
	void *data;
	int result;

	if ( !entry || entry->size > 0x7FFFFFFFu ) {
		*file = FS_INVALID_HANDLE;
		return -1;
	}

	// claim the slot first: FS_HandleForFile drops on exhaustion
	handle = FS_HandleForFile();
	data = malloc( entry->size ? entry->size : 1 );
	if ( !data ) {
		Com_Printf( S_COLOR_RED "Out of memory decoding %s@%s (%u bytes)\n", pak->pakBasename, name, entry->size );
		*file = FS_INVALID_HANDLE;
		return -1;
	}
	result = QAA_BpkRead( pak->qaaBpk, index, data, entry->size );
	if ( result != QAA_BPK_OK ) {
		Com_Printf( S_COLOR_RED "Error decoding %s@%s: %s\n", pak->pakBasename, name, QAA_BpkError( result ) );
		free( data );
		*file = FS_INVALID_HANDLE;
		return -1;
	}

	*file = handle;
	f = &fsh[ handle ];
	FS_InitHandle( f );
	f->qaaMemory = qtrue;
	f->qaaPos = 0;
	f->zipFile = qfalse;
	f->handleFiles.file.v = data;
	f->handleFiles.unique = qtrue;
	f->zipFilePos = 0;
	f->zipFileLen = (int)entry->size;
	f->pakIndex = pak->index;
	fs_lastPakIndex = pak->index;
	f->pak = pak;
	Q_strncpyz( f->name, name, sizeof( f->name ) );

	if ( fs_debug->integer ) {
		Com_Printf( "FS_FOpenFileRead: %s (BPK record %016llx in '%s')\n",
			name, (unsigned long long)entry->hash, pak->pakFilename );
	}

	return (int)entry->size;
}


/*
=================
FS_QAA_CreateBpkPack

Builds a pack_t over an open archive; the pack owns `bpk`. Its records get
names only as they are looked up (FS_QAA_ResolveName).
=================
*/
static pack_t *FS_QAA_CreateBpkPack( qaaBpk_t *bpk, const char *displayName ) {
	const uint32_t count = QAA_BpkCount( bpk );
	const int hashSize = FS_PakHashSize( count > 0 ? (int)count : 1 );
	const size_t fileNameLen = strlen( displayName ) + 1;
	pack_t *pack;
	size_t size;

	size = sizeof( *pack ) + hashSize * sizeof( pack->hashTable[0] ) + count * sizeof( pack->qaaFiles[0] )
		+ PAD( fileNameLen, sizeof( int ) ) + PAD( sizeof( "pak0" ), sizeof( int ) );

	pack = (pack_t *)Z_TagMalloc( (int)size, TAG_PACK );
	Com_Memset( pack, 0, size );
	pack->format = PACK_FORMAT_BPK;
	pack->qaaBpk = bpk;
	pack->numfiles = 0;		// not enumerable: see the top of this file
	pack->hashSize = hashSize;
	pack->hashTable = (fileInPack_t **)( pack + 1 );
	pack->qaaFiles = (fileInPack_t *)( pack->hashTable + pack->hashSize );
	pack->pakFilename = (char *)( pack->qaaFiles + count );
	pack->pakBasename = pack->pakFilename + PAD( fileNameLen, sizeof( int ) );
	Com_Memcpy( pack->pakFilename, displayName, fileNameLen );
	strcpy( pack->pakBasename, "pak0" );

	pack->checksum = (int)LittleLong( (int)QAA_BpkTableCrc( bpk ) );
	pack->pure_checksum = pack->checksum;
	return pack;
}


/*
=================
FS_QAA_FreeNames

Releases the names FS_QAA_ResolveName gave a pack's records.
=================
*/
static void FS_QAA_FreeNames( pack_t *pak ) {
	uint32_t i;

	if ( !pak->qaaBpk || !pak->qaaFiles ) {
		return;
	}
	for ( i = 0; i < QAA_BpkCount( pak->qaaBpk ); i++ ) {
		if ( pak->qaaFiles[i].name ) {
			Z_Free( pak->qaaFiles[i].name );
			pak->qaaFiles[i].name = NULL;
		}
	}
}


/*
=================
FS_QAA_ResolveName

Called before a lookup of `filename` in the search path. For each mounted
BPK pack whose table holds the name's hash, links that record into the
pack's hash chain under this name, so the lookup that follows finds it like
any pack entry. A record keeps the first spelling it was found by; the hash
and FS_FilenameCompare both ignore case and separators.
=================
*/
static void FS_QAA_ResolveName( const char *filename ) {
	const searchpath_t *search;
	uint64_t nameHash = 0;
	qboolean hashed = qfalse;

	if ( !fs_qaaMounted || !filename || !filename[0] ) {
		return;
	}
	for ( search = fs_searchpaths; search; search = search->next ) {
		pack_t *pak = search->pack;
		fileInPack_t *file;
		long hash;
		int index;

		if ( !pak || !pak->qaaBpk ) {
			continue;
		}
		if ( !hashed ) {
			nameHash = qaa_bpk_name_hash( filename );
			hashed = qtrue;
		}
		index = QAA_BpkFind( pak->qaaBpk, nameHash );
		if ( index < 0 || pak->qaaFiles[index].name ) {
			continue;
		}
		file = &pak->qaaFiles[index];
		file->name = CopyString( filename );
		file->pos = (unsigned long)index;
		file->size = QAA_BpkEntry( pak->qaaBpk, (uint32_t)index )->size;
		hash = FS_HashFileName( file->name, pak->hashSize );
		file->next = pak->hashTable[hash];
		pak->hashTable[hash] = file;
	}
}


/*
=================
FS_QAA_OpenPackage

Opens baseq3/pak0.bpk from fs_qaaPackage without extracting it.
=================
*/
static qaaBpk_t *FS_QAA_OpenPackage( const char *source ) {
	char error[256];
	char path[MAX_OSPATH * 2];
	byte magic[4];
	qaaIo_t io;
	qaaBpk_t *bpk;
	fileOffset_t statSize;
	fileTime_t statTime;

	if ( QAA_IoOpenFile( source, &io ) != 0 ) {
		// a folder holding the package's files
		Com_sprintf( path, sizeof( path ), "%s%c" BASEGAME "%cpak0.bpk", source, PATH_SEP, PATH_SEP );
		if ( QAA_IoOpenFile( path, &io ) != 0 ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: fs_qaaPackage '%s' is neither a file nor a folder holding "
				BASEGAME "/pak0.bpk\n", source );
			return NULL;
		}
		Com_sprintf( path, sizeof( path ), "%s%cdefault.xex", source, PATH_SEP );
		if ( !Sys_GetFileStats( path, &statSize, &statTime, &statTime ) ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: '%s' has no default.xex; mounting its pak0.bpk anyway\n", source );
		}
		fs_qaaKind = "folder";
		Com_sprintf( fs_qaaSource, sizeof( fs_qaaSource ), "%s%c" BASEGAME "%cpak0.bpk", source, PATH_SEP, PATH_SEP );
	} else if ( QAA_IoRead( &io, 0, magic, sizeof( magic ) ) == 0
		&& ( !memcmp( magic, "LIVE", 4 ) || !memcmp( magic, "CON ", 4 ) || !memcmp( magic, "PIRS", 4 ) ) ) {
		qaaStfs_t *stfs = QAA_StfsOpen( &io, error, sizeof( error ) );
		int index;

		if ( !stfs ) {
			QAA_IoClose( &io );
			Com_Printf( S_COLOR_YELLOW "WARNING: fs_qaaPackage '%s': %s\n", source, error );
			return NULL;
		}
		fs_qaaTitleId = QAA_StfsTitleId( stfs );
		if ( fs_qaaTitleId != 0x584108C8u ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: package title ID is %08X, not Quake Arena Arcade's 584108C8\n",
				fs_qaaTitleId );
		}
		index = QAA_StfsFind( stfs, BASEGAME "/pak0.bpk" );
		if ( index < 0 || QAA_StfsOpenFile( stfs, index, &io ) != 0 ) {
			QAA_StfsClose( stfs );
			Com_Printf( S_COLOR_YELLOW "WARNING: package '%s' has no readable " BASEGAME "/pak0.bpk\n", source );
			return NULL;
		}
		QAA_StfsClose( stfs );	// the file reader keeps the package open
		fs_qaaKind = "STFS package";
		Com_sprintf( fs_qaaSource, sizeof( fs_qaaSource ), "%s/" BASEGAME "/pak0.bpk", source );
	} else {
		fs_qaaKind = "BPK file";
		Q_strncpyz( fs_qaaSource, source, sizeof( fs_qaaSource ) );
	}

	bpk = QAA_BpkOpen( &io, error, sizeof( error ) );
	if ( !bpk ) {
		QAA_IoClose( &io );
		Com_Printf( S_COLOR_YELLOW "WARNING: '%s': %s\n", fs_qaaSource, error );
		fs_qaaKind = "none";
		fs_qaaSource[0] = '\0';
		return NULL;
	}
	return bpk;
}


/*
=================
FS_QAA_IsPackage

QAA: PC deviation (no Xbox storage to read from). True if path is Quake
Arena Arcade's package as the user copied it: the STFS file (title
584108C8, content type 000D0000, an Arcade title, so saves and title
updates of the same title are skipped) or a folder holding its files
(baseq3/pak0.bpk), as Xbox 360 tools and emulators extract it.
=================
*/
static qboolean FS_QAA_IsPackage( const char *path ) {
	char bpkPath[MAX_OSPATH * 2];
	byte magic[4];
	qaaIo_t io;
	qboolean found = qfalse;

	if ( QAA_IoOpenFile( path, &io ) != 0 ) {
		Com_sprintf( bpkPath, sizeof( bpkPath ), "%s%c" BASEGAME "%cpak0.bpk", path, PATH_SEP, PATH_SEP );
		if ( QAA_IoOpenFile( bpkPath, &io ) != 0 ) {
			return qfalse;
		}
		QAA_IoClose( &io );
		return qtrue;
	}
	if ( QAA_IoRead( &io, 0, magic, sizeof( magic ) ) == 0
		&& ( !memcmp( magic, "LIVE", 4 ) || !memcmp( magic, "CON ", 4 ) || !memcmp( magic, "PIRS", 4 ) ) ) {
		qaaStfs_t *stfs = QAA_StfsOpen( &io, NULL, 0 );
		if ( stfs ) {
			found = QAA_StfsTitleId( stfs ) == 0x584108C8u && QAA_StfsContentType( stfs ) == 0x000D0000u
				? qtrue : qfalse;
			QAA_StfsClose( stfs );	// owns io
			return found;
		}
	}
	QAA_IoClose( &io );
	return qfalse;
}


/*
=================
FS_QAA_FindIn

Checks every entry of dir (files and folders) with FS_QAA_IsPackage.
=================
*/
static qboolean FS_QAA_FindIn( const char *dir, char *out, int outSize ) {
	static const char *const kinds[2] = { "", "/" };	// files, then folders
	char path[MAX_OSPATH * 2];
	qboolean found = qfalse;
	int kind;

	for ( kind = 0; kind < 2 && !found; kind++ ) {
		int i, count;
		char **list = Sys_ListFiles( dir, kinds[kind], NULL, &count, 0 );
		for ( i = 0; i < count && !found; i++ ) {
			if ( Q_streq( list[i], "." ) || Q_streq( list[i], ".." ) ) {
				continue;
			}
			Com_sprintf( path, sizeof( path ), "%s%c%s", dir, PATH_SEP, list[i] );
			if ( FS_QAA_IsPackage( path ) ) {
				Q_strncpyz( out, path, outSize );
				found = qtrue;
			}
		}
		Sys_FreeFileList( list );
	}
	return found;
}


/*
=================
FS_QAA_FindPackage

QAA: PC deviation. With fs_qaaPackage empty, looks for the package where a
user would put it, in the install folder (fs_basepath), then the home
folder:
  - <root>/584108C8/000D0000/<package>: the XBLA zip extracted as it is
    (the zip holds the package under the console's own folder names),
  - <root>/Content/<profile>/584108C8/000D0000/<package>: an Xbox 360
    Content folder copied from the console or a storage device,
  - <root>/<package>: the package file, or a folder of its files, on its own.
=================
*/
static qboolean FS_QAA_FindPackage( char *out, int outSize ) {
	static const char *const contentNames[2] = { "Content", "content" };
	const char *roots[2];
	char dir[MAX_OSPATH * 2];
	int r, c, i, count;

	roots[0] = fs_basepath ? fs_basepath->string : "";
	roots[1] = fs_homepath ? fs_homepath->string : "";
	for ( r = 0; r < 2; r++ ) {
		if ( !roots[r][0] || ( r == 1 && !Q_stricmp( roots[0], roots[1] ) ) ) {
			continue;
		}
		Com_sprintf( dir, sizeof( dir ), "%s%c584108C8%c000D0000", roots[r], PATH_SEP, PATH_SEP );
		if ( FS_QAA_FindIn( dir, out, outSize ) ) {
			return qtrue;
		}
		for ( c = 0; c < 2; c++ ) {
			char base[MAX_OSPATH * 2];
			char **profiles;

			Com_sprintf( base, sizeof( base ), "%s%c%s", roots[r], PATH_SEP, contentNames[c] );
			profiles = Sys_ListFiles( base, "/", NULL, &count, 0 );
			for ( i = 0; i < count; i++ ) {
				if ( Q_streq( profiles[i], "." ) || Q_streq( profiles[i], ".." ) ) {
					continue;
				}
				Com_sprintf( dir, sizeof( dir ), "%s%c%s%c584108C8%c000D0000", base, PATH_SEP, profiles[i],
					PATH_SEP, PATH_SEP );
				if ( FS_QAA_FindIn( dir, out, outSize ) ) {
					Sys_FreeFileList( profiles );
					return qtrue;
				}
			}
			Sys_FreeFileList( profiles );
		}
		if ( FS_QAA_FindIn( roots[r], out, outSize ) ) {
			return qtrue;
		}
	}
	return qfalse;
}


/*
=================
FS_QAA_MountPackage

Adds the package's pak0.bpk to the search path as a baseq3 pack.
=================
*/
static void FS_QAA_MountPackage( void ) {
	searchpath_t *search;
	qaaBpk_t *bpk;
	pack_t *pak;
	const char *source;

	fs_qaaPackage = Cvar_Get( "fs_qaaPackage", "", CVAR_INIT | CVAR_PROTECTED );
	Cvar_SetDescription( fs_qaaPackage, "Quake Arena Arcade's XBLA package (the STFS file), a folder holding "
		"its files (default.xex, baseq3/pak0.bpk), or a .bpk file. Read in place, never written. "
		"Empty: found in the install or home folder." );

	fs_qaaKind = "none";
	fs_qaaSource[0] = '\0';
	fs_qaaTitleId = 0;
	fs_qaaMounted = 0;
	fs_qaaFound[0] = '\0';
	source = fs_qaaPackage->string;
	if ( !source[0] ) {
		if ( !FS_QAA_FindPackage( fs_qaaFound, sizeof( fs_qaaFound ) ) ) {
			return;
		}
		Com_Printf( "Found the Quake Arena Arcade package: %s\n", fs_qaaFound );
		source = fs_qaaFound;
	}
	bpk = FS_QAA_OpenPackage( source );
	if ( !bpk ) {
		return;
	}
	pak = FS_QAA_CreateBpkPack( bpk, fs_qaaSource );
	pak->pakGamename = BASEGAME;
	pak->index = fs_packCount;
	pak->referenced = 0;
	pak->exclude = qfalse;
	fs_packCount++;
	fs_qaaMounted++;

	search = (searchpath_t *)Z_TagMalloc( sizeof( *search ), TAG_SEARCH_PACK );
	Com_Memset( search, 0, sizeof( *search ) );
	search->pack = pak;
	search->next = fs_searchpaths;
	fs_searchpaths = search;

	Com_Printf( "Quake Arena Arcade %s: %s, %u records\n", fs_qaaKind, fs_qaaSource, QAA_BpkCount( bpk ) );
}


/*
=================
FS_QAA_CheckPackage

Replaces FnQL's retail Quake Live / Quake III install check (FS_CheckIdPaks,
removed): FnQAA requires only that a QAA package is mounted.
=================
*/
static void FS_QAA_CheckPackage( void ) {
	const searchpath_t *search;

	for ( search = fs_searchpaths; search; search = search->next ) {
		if ( search->pack && search->pack->format == PACK_FORMAT_BPK ) {
			return;
		}
	}
	if ( fs_qaaPackage && fs_qaaPackage->string[0] ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: no Quake Arena Arcade package is mounted: fs_qaaPackage '%s' "
			"is not one.\n", fs_qaaPackage->string );
		return;
	}
	Com_Printf( S_COLOR_YELLOW "WARNING: no Quake Arena Arcade package found. Extract the XBLA zip into %s "
		"(it makes 584108C8%c000D0000%c...), or copy the package file or your Xbox 360 Content folder there, "
		"or set fs_qaaPackage to it.\n", fs_basepath->string, PATH_SEP, PATH_SEP );
}


/*
=================
FS_QAA_FindRecord
=================
*/
static pack_t *FS_QAA_FindRecord( uint64_t hash, int *index ) {
	const searchpath_t *search;

	for ( search = fs_searchpaths; search; search = search->next ) {
		if ( search->pack && search->pack->qaaBpk ) {
			*index = QAA_BpkFind( search->pack->qaaBpk, hash );
			if ( *index >= 0 ) {
				return search->pack;
			}
		}
	}
	*index = -1;
	return NULL;
}


/*
=================
FS_QAA_BpkRead_f

qaa_bpkread <hash> [hash ...]: reads BPK records by raw 64-bit name hash
through the filesystem's handle path (open, FS_Read, close) and prints each
record's size, CRC-32 and first bytes. Writes nothing.
=================
*/
static void FS_QAA_BpkRead_f( void ) {
	int arg;

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "Usage: qaa_bpkread <16 hex digit hash> [...]\n" );
		return;
	}
	for ( arg = 1; arg < Cmd_Argc(); arg++ ) {
		const char *text = Cmd_Argv( arg );
		char name[32];
		byte buffer[65536];
		byte head[4] = { 0, 0, 0, 0 };
		uint64_t hash = 0;
		uint32_t crc = 0;
		fileHandle_t f;
		pack_t *pak;
		int index, length, total = 0, i, n, valid = 1;

		for ( i = 0; text[i]; i++ ) {
			const char c = text[i];
			const int v = ( c >= '0' && c <= '9' ) ? c - '0' : ( c >= 'a' && c <= 'f' ) ? c - 'a' + 10
				: ( c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : -1;
			if ( v < 0 || i >= 16 ) {
				valid = 0;
				break;
			}
			hash = ( hash << 4 ) | (uint64_t)v;
		}
		if ( !valid || i == 0 ) {
			Com_Printf( "qaa_bpkread: '%s' is not a hex hash\n", text );
			continue;
		}
		pak = FS_QAA_FindRecord( hash, &index );
		if ( !pak ) {
			Com_Printf( "qaa_bpkread %016llx: no such record in a mounted BPK\n", (unsigned long long)hash );
			continue;
		}
		Com_sprintf( name, sizeof( name ), "#%016llx", (unsigned long long)hash );
		length = FS_QAA_OpenBpkRecord( &f, pak, (uint32_t)index, name );
		if ( length < 0 || f == FS_INVALID_HANDLE ) {
			Com_Printf( "qaa_bpkread %016llx: failed to decode\n", (unsigned long long)hash );
			continue;
		}
		while ( ( n = FS_Read( buffer, sizeof( buffer ), f ) ) > 0 ) {
			if ( total == 0 ) {
				Com_Memcpy( head, buffer, n < 4 ? n : 4 );
			}
			crc = QAA_Crc32( crc, buffer, (size_t)n );
			total += n;
		}
		FS_FCloseFile( f );
		for ( i = 0; i < 4; i++ ) {
			name[i] = ( head[i] >= 32 && head[i] < 127 ) ? (char)head[i] : '.';
		}
		name[4] = '\0';
		Com_Printf( "qaa_bpkread %016llx: %d bytes (record %d, stored %u), crc32 %08x, begins '%s'%s\n",
			(unsigned long long)hash, total, index, QAA_BpkEntry( pak->qaaBpk, (uint32_t)index )->storedSize,
			crc, name, total == length ? "" : " SHORT READ" );
	}
}


/*
=================
FS_QAA_Info_f
=================
*/
static void FS_QAA_Info_f( void ) {
	const searchpath_t *search;

	Com_Printf( "fs_qaaPackage: '%s' (%s)\n", fs_qaaPackage ? fs_qaaPackage->string : "", fs_qaaKind );
	if ( fs_qaaFound[0] ) {
		Com_Printf( "found automatically: %s\n", fs_qaaFound );
	}
	if ( fs_qaaTitleId ) {
		Com_Printf( "title ID: %08X\n", fs_qaaTitleId );
	}
	for ( search = fs_searchpaths; search; search = search->next ) {
		if ( search->pack && search->pack->qaaBpk ) {
			uint32_t i, named = 0;
			for ( i = 0; i < QAA_BpkCount( search->pack->qaaBpk ); i++ ) {
				named += search->pack->qaaFiles[i].name ? 1 : 0;
			}
			Com_Printf( "%s: version %u, %u records (%u looked up by name so far), table crc32 %08x\n",
				search->pack->pakFilename, QAA_BpkVersion( search->pack->qaaBpk ),
				QAA_BpkCount( search->pack->qaaBpk ), named, QAA_BpkTableCrc( search->pack->qaaBpk ) );
		}
	}
}
