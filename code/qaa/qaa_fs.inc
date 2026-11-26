/*
===========================================================================
openQAA: mounting Quake Arena Arcade's package in the engine filesystem.

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.

Included by code/qcommon/files.c, whose pack and handle structures it uses.

The game's data is baseq3/pak0.bpk inside the user's XBLA package (STFS).
It is read in place: nothing is extracted and nothing is written. The cvar
fs_qaaPackage names one of
  - the STFS package file itself,
  - a folder holding the package's files (default.xex, baseq3/pak0.bpk), or
  - a .bpk file.
BPK records are keyed by a 64-bit name hash (QAA_BpkNameHash). A record is
visible to the filesystem by name once its path is known: from the name list
in fs_qaaNameList, and, once the hash is implemented, from any path in that
list without an explicit hash. Every record is reachable by its raw hash
through qaa_bpkread.
===========================================================================
*/

static cvar_t	*fs_qaaPackage;
static cvar_t	*fs_qaaNameList;
static char		fs_qaaSource[MAX_OSPATH * 2];	// what was mounted, for qaa_info
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


typedef struct {
	uint32_t	index;
	char		*name;
} qaaNamedRecord_t;

/*
=================
FS_QAA_ReadNameList

Names the records of `bpk` from fs_qaaNameList. Returns the number named;
*records and *names are Z_Malloc'd (free with Z_Free) when nonzero.
=================
*/
static int FS_QAA_ReadNameList( const qaaBpk_t *bpk, qaaNamedRecord_t **records, char **names, int *namesLength ) {
	const char *listPath = fs_qaaNameList->string;
	byte *seen;
	char *text, *line, *next;
	char path[MAX_QPATH];
	FILE *f;
	long length;
	int count = 0, capacity, lineNumber = 0, bad = 0, unmatched = 0;
	int used = 0;
	uint64_t hash;

	*records = NULL;
	*names = NULL;
	*namesLength = 0;
	if ( !listPath[0] ) {
		return 0;
	}
	f = Sys_FOpen( listPath, "rb" );
	if ( !f ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: cannot open fs_qaaNameList '%s'\n", listPath );
		return 0;
	}
	fseek( f, 0, SEEK_END );
	length = ftell( f );
	fseek( f, 0, SEEK_SET );
	if ( length <= 0 || length > 8 * 1024 * 1024 ) {
		fclose( f );
		return 0;
	}
	text = (char *)Z_Malloc( (int)length + 1 );
	if ( (long)fread( text, 1, (size_t)length, f ) != length ) {
		fclose( f );
		Z_Free( text );
		return 0;
	}
	fclose( f );
	text[length] = '\0';

	capacity = (int)QAA_BpkCount( bpk );
	seen = (byte *)Z_Malloc( capacity + 1 );
	*records = (qaaNamedRecord_t *)Z_Malloc( ( capacity + 1 ) * (int)sizeof( qaaNamedRecord_t ) );
	*names = (char *)Z_Malloc( (int)length + 1 );	// names are no longer than the list

	for ( line = text; line && *line; line = next ) {
		int index, parsed;

		next = strchr( line, '\n' );
		if ( next ) {
			*next++ = '\0';
		}
		lineNumber++;
		parsed = QAA_BpkParseNameLine( line, &hash, path, sizeof( path ) );
		if ( parsed < 0 ) {
			bad++;
			continue;
		}
		if ( parsed == 0 ) {
			continue;
		}
		FS_ConvertFilename( path );
		index = hash ? QAA_BpkFind( bpk, hash ) : -1;
		if ( index < 0 ) {
			unmatched++;
			continue;
		}
		if ( seen[index] || FS_CheckDirTraversal( path ) ) {
			continue;
		}
		seen[index] = 1;
		( *records )[count].index = (uint32_t)index;
		( *records )[count].name = *names + used;
		strcpy( *names + used, path );
		used += (int)strlen( path ) + 1;
		count++;
	}
	Z_Free( seen );
	Z_Free( text );
	*namesLength = used;
	Com_Printf( "fs_qaaNameList: %d of %d records named (%d lines, %d unmatched, %d malformed)\n",
		count, capacity, lineNumber, unmatched, bad );
	if ( !count ) {
		Z_Free( *records );
		Z_Free( *names );
		*records = NULL;
		*names = NULL;
	}
	return count;
}


/*
=================
FS_QAA_CreateBpkPack

Builds a pack_t over an open archive; the pack owns `bpk`.
=================
*/
static pack_t *FS_QAA_CreateBpkPack( qaaBpk_t *bpk, const char *displayName ) {
	qaaNamedRecord_t *records;
	char *names, *namePtr;
	fileInPack_t *curFile;
	pack_t *pack;
	int namedCount, namesLength, hashSize, i;
	size_t size, fileNameLen;

	namedCount = FS_QAA_ReadNameList( bpk, &records, &names, &namesLength );
	hashSize = FS_PakHashSize( namedCount > 0 ? namedCount : 1 );
	fileNameLen = strlen( displayName ) + 1;
	size = sizeof( *pack ) + hashSize * sizeof( pack->hashTable[0] ) + namedCount * sizeof( pack->buildBuffer[0] )
		+ PAD( namesLength, sizeof( int ) ) + PAD( fileNameLen, sizeof( int ) ) + PAD( sizeof( "pak0" ), sizeof( int ) );

	pack = (pack_t *)Z_TagMalloc( (int)size, TAG_PACK );
	Com_Memset( pack, 0, size );
	pack->format = PACK_FORMAT_BPK;
	pack->qaaBpk = bpk;
	pack->numfiles = namedCount;
	pack->hashSize = hashSize;
	pack->hashTable = (fileInPack_t **)( pack + 1 );
	pack->buildBuffer = (fileInPack_t *)( pack->hashTable + pack->hashSize );
	namePtr = (char *)( pack->buildBuffer + namedCount );
	pack->pakFilename = namePtr + PAD( namesLength, sizeof( int ) );
	pack->pakBasename = pack->pakFilename + PAD( fileNameLen, sizeof( int ) );
	Com_Memcpy( pack->pakFilename, displayName, fileNameLen );
	strcpy( pack->pakBasename, "pak0" );

	curFile = pack->buildBuffer;
	for ( i = 0; i < namedCount; i++ ) {
		const qaaBpkEntry_t *entry = QAA_BpkEntry( bpk, records[i].index );
		const long hash = FS_HashFileName( records[i].name, pack->hashSize );

		curFile->name = namePtr;
		strcpy( namePtr, records[i].name );
		namePtr += strlen( records[i].name ) + 1;
		curFile->pos = records[i].index;
		curFile->size = entry->size;
		curFile->next = pack->hashTable[ hash ];
		pack->hashTable[ hash ] = curFile;
		curFile++;
	}
	if ( records ) {
		Z_Free( records );
		Z_Free( names );
	}

	pack->checksum = (int)LittleLong( (int)QAA_BpkTableCrc( bpk ) );
	pack->pure_checksum = pack->checksum;
	return pack;
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
FS_QAA_MountPackage

Adds the package's pak0.bpk to the search path as a baseq3 pack.
=================
*/
static void FS_QAA_MountPackage( void ) {
	searchpath_t *search;
	qaaBpk_t *bpk;
	pack_t *pak;

	fs_qaaPackage = Cvar_Get( "fs_qaaPackage", "", CVAR_INIT | CVAR_PROTECTED );
	Cvar_SetDescription( fs_qaaPackage, "Quake Arena Arcade's XBLA package (the STFS file), a folder holding "
		"its files (default.xex, baseq3/pak0.bpk), or a .bpk file. Read in place, never written." );
	fs_qaaNameList = Cvar_Get( "fs_qaaNameList", "", CVAR_INIT | CVAR_PROTECTED );
	Cvar_SetDescription( fs_qaaNameList, "Text file naming BPK records, one per line: \"<16 hex digit hash> <path>\" "
		"or a path alone (hashed by the engine once the BPK name hash is known)." );

	fs_qaaKind = "none";
	fs_qaaSource[0] = '\0';
	fs_qaaTitleId = 0;
	if ( !fs_qaaPackage->string[0] ) {
		return;
	}
	bpk = FS_QAA_OpenPackage( fs_qaaPackage->string );
	if ( !bpk ) {
		return;
	}
	pak = FS_QAA_CreateBpkPack( bpk, fs_qaaSource );
	pak->pakGamename = BASEGAME;
	pak->index = fs_packCount;
	pak->referenced = 0;
	pak->exclude = qfalse;
	pak->workshopItemId = 0;
	fs_packFiles += pak->numfiles;
	fs_packCount++;

	search = (searchpath_t *)Z_TagMalloc( sizeof( *search ), TAG_SEARCH_PACK );
	Com_Memset( search, 0, sizeof( *search ) );
	search->pack = pak;
	search->next = fs_searchpaths;
	fs_searchpaths = search;

	Com_Printf( "Quake Arena Arcade %s: %s, %u records, %d named\n", fs_qaaKind, fs_qaaSource,
		QAA_BpkCount( bpk ), pak->numfiles );
}


/*
=================
FS_QAA_CheckPackage

Replaces FnQL's Quake Live / Quake III base pak check (FS_CheckIdPaks).
=================
*/
static void FS_QAA_CheckPackage( void ) {
	const searchpath_t *search;

	(void)FS_CheckIdPaks;	// FnQL's check, kept in files.c but not called
	for ( search = fs_searchpaths; search; search = search->next ) {
		if ( search->pack && search->pack->format == PACK_FORMAT_BPK ) {
			if ( !QAA_BpkNameHashKnown() && search->pack->numfiles == 0 ) {
				Com_Printf( S_COLOR_YELLOW "WARNING: the BPK name hash is not implemented yet and no "
					"fs_qaaNameList names its records, so no file in %s is visible by name.\n",
					search->pack->pakFilename );
			}
			return;
		}
	}
	Com_Printf( S_COLOR_YELLOW "WARNING: no Quake Arena Arcade package is mounted; set fs_qaaPackage "
		"to your XBLA package or to a folder holding its files.\n" );
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
	if ( fs_qaaTitleId ) {
		Com_Printf( "title ID: %08X\n", fs_qaaTitleId );
	}
	Com_Printf( "BPK name hash: %s\n", QAA_BpkNameHashKnown() ? "implemented" : "placeholder (not yet known)" );
	for ( search = fs_searchpaths; search; search = search->next ) {
		if ( search->pack && search->pack->qaaBpk ) {
			Com_Printf( "%s: version %u, %u records, %d named, table crc32 %08x\n", search->pack->pakFilename,
				QAA_BpkVersion( search->pack->qaaBpk ), QAA_BpkCount( search->pack->qaaBpk ),
				search->pack->numfiles, QAA_BpkTableCrc( search->pack->qaaBpk ) );
		}
	}
}
