#pragma once

#include "structure.h"

typedef struct sqlite3 sqlite3;

// Opt-in switch for the fast scan. When true (the default), the initial library
// scan in GetFolderDataFromPath uses ScanSongsFast; flip it to false to fall
// back to the original SearchSongsFromPath for A/B comparison.
extern bool g_useFastScan;

// Faster, parallel reimplementation of SearchSongsFromPath.
//
// Produces the same song/folder data as the original scan path: the same CRC32
// folder/parent values and the same "recurse into a subdirectory only when its
// folder row was newly inserted" rule. Internal ordering and progress logging
// can differ because directories are listed with EnumerateDirectory
// (NtQueryDirectoryFile), charts are parsed with ParseChartsParallel, and song
// rows are written with reusable prepared statements.
//
// `root` and `path` carry the same meaning as in SearchSongsFromPath and are
// always equal at every call site. Returns the number of charts and custom
// folders inserted, matching SearchSongsFromPath's count.
int ScanSongsFast(CSTR root, sqlite3* sql, CSTR path);
