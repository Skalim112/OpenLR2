#pragma once

// Reusable, self-contained building blocks for fast song-library scanning.
// Inspired by Bobini1/RhythmGame's SongDbScanner, but kept independent of the
// database schema: nothing here touches SQLite, DxLib or the log. Callers feed
// in paths and consume plain results on the main thread.

#include "strclass.h"
#include "structure.h"

#include <span>
#include <vector>

// One entry returned by EnumerateDirectory.
// `name` is encoded in the system ANSI codepage (CP_ACP), exactly like the
// names returned by FindFirstFileA, so it stays byte-compatible with existing
// database rows, fopen() and SQLite '%q' binding.
struct DirEntry {
	CSTR name;
	bool isDirectory;
	int lastWriteUnixtime; // seconds since the Unix epoch
};

// Fast, allocation-light directory listing.
//
// Windows: uses NtQueryDirectoryFile with a 64 KiB buffer, returning many
// entries per syscall instead of one per call like FindNextFileA. The "." and
// ".." pseudo-entries are skipped.
//
// Returns an empty vector if the directory cannot be opened.
std::vector<DirEntry> EnumerateDirectory(CSTR directory);

// Parses many BMS charts in parallel and returns the metadata aligned 1:1 with
// `paths` (result[i] corresponds to paths[i]).
//
// Each worker calls ParseBMSMETA(..., 0) on its own BMSMETA slot, so no DxLib
// drawing, SQLite access or logging happens off the main thread. The caller is
// responsible for inserting the returned metadata into the database.
std::vector<BMSMETA> ParseChartsParallel(std::span<const CSTR> paths);
