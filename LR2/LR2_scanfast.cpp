#include "LR2_scanfast.h"

#include "En_dirscan.h"
#include "Engine.h"
#include "LR2_songmanage.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <span>
#include <vector>

bool g_useFastScan = true;

#ifdef _WIN32

namespace {

// Keep the chunk large enough to feed all worker threads, but small enough that
// a single slow chart cannot hide progress for too long.
constexpr size_t kParseChunk = 64;

using ScanClock = std::chrono::steady_clock;

struct ChartFile {
	CSTR path;
	int lastWriteUnixtime;
};

struct ScanCollectStats {
	size_t visitedFolders = 0;
	size_t queuedCharts = 0;
	size_t insertedCustomFolders = 0;
};

int
logCount(const size_t value)
{
	return value > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(value);
}

int
logDurationMs(const long long value)
{
	return value > INT_MAX ? INT_MAX : static_cast<int>(value);
}

long long
elapsedMs(const ScanClock::time_point started)
{
	const auto elapsed = ScanClock::now() - started;
	return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

bool
bindText(sqlite3_stmt* stmt, const int index, const CSTR& value)
{
	const char* text = value.body ? value.body : "";
	return sqlite3_bind_text(stmt, index, text, -1, SQLITE_STATIC) == SQLITE_OK;
}

bool
bindInt(sqlite3_stmt* stmt, const int index, const int value)
{
	return sqlite3_bind_int(stmt, index, value) == SQLITE_OK;
}

void
assignColumn(CSTR& target, sqlite3_stmt* stmt, const int column)
{
	const auto* text =
	  reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
	target = text ? text : "";
}

class PreparedStatement {
  public:
	PreparedStatement(sqlite3* sql, const char* query)
	{
		const int rc = sqlite3_prepare_v2(sql, query, -1, &stmt, nullptr);
		if (rc != SQLITE_OK) {
			ErrorLogFmtAdd(
			  "Fast scan SQL prepare failed: %s\n", sqlite3_errmsg(sql));
			stmt = nullptr;
		}
	}

	~PreparedStatement()
	{
		if (stmt != nullptr) {
			sqlite3_finalize(stmt);
		}
	}

	PreparedStatement(const PreparedStatement&) = delete;
	PreparedStatement& operator=(const PreparedStatement&) = delete;

	bool isValid() const { return stmt != nullptr; }
	sqlite3_stmt* get() const { return stmt; }

	void reset() const
	{
		if (stmt != nullptr) {
			sqlite3_reset(stmt);
			sqlite3_clear_bindings(stmt);
		}
	}

  private:
	sqlite3_stmt* stmt = nullptr;
};

class TagLookup {
  public:
	explicit TagLookup(sqlite3* sql)
	  : sql(sql)
	  , stmt(sql,
	         "SELECT hash,title,subtitle,genre,artist,subartist,tag,level,"
	         "difficulty,mode,exlevel FROM tag WHERE hash = ?")
	{
	}

	bool apply(BMSMETA& meta) const
	{
		if (!stmt.isValid()) {
			return false;
		}

		sqlite3_stmt* query = stmt.get();
		stmt.reset();
		if (!bindText(query, 1, meta.hash)) {
			ErrorLogFmtAdd("Fast scan tag bind failed: %s\n", sqlite3_errmsg(sql));
			stmt.reset();
			return false;
		}

		const int rc = sqlite3_step(query);
		if (rc == SQLITE_ROW) {
			assignColumn(meta.title, query, 1);
			assignColumn(meta.subtitle, query, 2);
			assignColumn(meta.genre, query, 3);
			assignColumn(meta.artist, query, 4);
			assignColumn(meta.subartist, query, 5);
			assignColumn(meta.tag, query, 6);
			meta.selLevel = sqlite3_column_int(query, 7);
			meta.difficulty = sqlite3_column_int(query, 8);
			meta.keymode = sqlite3_column_int(query, 9);
			meta.exlevel = sqlite3_column_int(query, 10);
			stmt.reset();
			return true;
		}

		if (rc != SQLITE_DONE) {
			ErrorLogFmtAdd("Fast scan tag lookup failed: %s\n", sqlite3_errmsg(sql));
		}
		stmt.reset();
		return false;
	}

  private:
	sqlite3* sql;
	PreparedStatement stmt;
};

class SongInserter {
  public:
	explicit SongInserter(sqlite3* sql)
	  : sql(sql)
	  , stmt(sql,
	         "INSERT INTO song "
	         "(hash,title,subtitle,genre,artist,subartist,level,date,path,"
	         "folder,stagefile,banner,backbmp,parent,maxbpm,minbpm,random,"
	         "longnote,judge,mode,bga,difficulty,favorite,type,txt,karinotes,"
	         "adddate,exlevel) "
	         "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)")
	{
	}

	bool insert(const BMSMETA& meta,
	            const CSTR& chartPath,
	            const int chartTime,
	            const int addDate) const
	{
		if (!stmt.isValid()) {
			return false;
		}

		sqlite3_stmt* insertStmt = stmt.get();
		stmt.reset();

		const CSTR folder = AssignCRC32(meta.folderpath);
		const CSTR parent = AssignCRC32(meta.parentfolderpath);

		bool ok = true;
		ok = bindText(insertStmt, 1, meta.hash) && ok;
		ok = bindText(insertStmt, 2, meta.title) && ok;
		ok = bindText(insertStmt, 3, meta.subtitle) && ok;
		ok = bindText(insertStmt, 4, meta.genre) && ok;
		ok = bindText(insertStmt, 5, meta.artist) && ok;
		ok = bindText(insertStmt, 6, meta.subartist) && ok;
		ok = bindInt(insertStmt, 7, meta.selLevel) && ok;
		ok = bindInt(insertStmt, 8, chartTime) && ok;
		ok = bindText(insertStmt, 9, chartPath) && ok;
		ok = bindText(insertStmt, 10, folder) && ok;
		ok = bindText(insertStmt, 11, meta.stagefilepath) && ok;
		ok = bindText(insertStmt, 12, meta.bannerpath) && ok;
		ok = bindText(insertStmt, 13, meta.backBMPpath) && ok;
		ok = bindText(insertStmt, 14, parent) && ok;
		ok = bindInt(insertStmt, 15, meta.maxbpm) && ok;
		ok = bindInt(insertStmt, 16, meta.minbpm) && ok;
		ok = bindInt(insertStmt, 17, meta.random) && ok;
		ok = bindInt(insertStmt, 18, meta.longnote) && ok;
		ok = bindInt(insertStmt, 19, meta.judge) && ok;
		ok = bindInt(insertStmt, 20, meta.keymode) && ok;
		ok = bindInt(insertStmt, 21, meta.bga) && ok;
		ok = bindInt(insertStmt, 22, meta.difficulty) && ok;
		ok = bindInt(insertStmt, 23, 0) && ok;
		ok = bindInt(insertStmt, 24, 0) && ok;
		ok = bindInt(insertStmt, 25, meta.hasTxt) && ok;
		ok = bindInt(insertStmt, 26, meta.notecount) && ok;
		ok = bindInt(insertStmt, 27, addDate) && ok;
		ok = bindInt(insertStmt, 28, meta.exlevel) && ok;

		if (!ok) {
			ErrorLogFmtAdd("Fast scan song bind failed: %s\n", sqlite3_errmsg(sql));
			stmt.reset();
			return false;
		}

		const int rc = sqlite3_step(insertStmt);
		if (rc != SQLITE_DONE) {
			ErrorLogFmtAdd("Fast scan song insert failed: %s\n", sqlite3_errmsg(sql));
			stmt.reset();
			return false;
		}

		stmt.reset();
		return true;
	}

  private:
	sqlite3* sql;
	PreparedStatement stmt;
};

// One loading-screen update. Mirrors the draw block ParseBMSMETA(flag=1) used to
// run, but driven from the main thread between parse batches instead of once per
// chart.
void
finishScanProgressFrame()
{
	if (hBackImage > 0) {
		DrawGraph(0, 0, hBackImage, 0);
	}
	ScreenFlip();
	ClsDrawScreen();
	clsDx();
	ProcessMessage();
}

void
drawScanProgress(const char* message)
{
	printfDx("%s\n", message);
	finishScanProgressFrame();
}

void
drawScanChartProgress(const char* message,
                      const ChartFile& chart)
{
	printfDx("%s\n", message);
	printfDx("%s\n", chart.path.body);
	finishScanProgressFrame();
}

void
drawBatchChartProgress(const char* message,
                       const std::vector<ChartFile>& charts,
                       const size_t start,
                       const size_t end)
{
	for (size_t i = start; i < end; i++) {
		drawScanChartProgress(message, charts[i]);
	}
}

void
fillBatchPaths(std::vector<CSTR>& batchPaths,
               const std::vector<ChartFile>& charts,
               const size_t start,
               const size_t end)
{
	batchPaths.clear();
	for (size_t i = start; i < end; i++) {
		batchPaths.push_back(charts[i].path);
	}
}

// Phase A: walk the whole subtree, inserting folder/custom-folder rows with the
// same CRC32 and recursion rules as SearchSongsFromPath, while collecting every
// chart path and mtime for the parallel parse phase.
int
collectFolderTree(CSTR root,
                  sqlite3* sql,
                  CSTR path,
                  std::vector<ChartFile>& charts,
                  ScanCollectStats& stats)
{
	if (root.right(1).isDiff("\\")) {
		root.add("\\");
	}

	stats.visitedFolders++;
	if (stats.visitedFolders == 1 || stats.visitedFolders % 256 == 0) {
		ErrorLogFmtAdd("Fast scan collect: folders=%d charts=%d path=%s\n",
		               logCount(stats.visitedFolders),
		               logCount(stats.queuedCharts),
		               root.body);
	}
	ErrorLogTabAdd();

	const int now = static_cast<int>(GetNowUnixtime());
	const std::vector<DirEntry> entries = EnumerateDirectory(root);

	int count = 0;
	char str[2048];

	// Files: gather charts for the parallel pass, insert loose .lr2folder files.
	for (const auto& entry : entries) {
		if (entry.isDirectory) {
			continue;
		}
		if (IsBmsFile(entry.name)) {
			CSTR chartPath(root);
			chartPath.add(entry.name.body);
			charts.push_back(ChartFile{ chartPath, entry.lastWriteUnixtime });
			stats.queuedCharts++;
		}
		else if (IsLR2Folder(entry.name)) {
			CSTR folderPath(root);
			folderPath.add(entry.name.body);
			BMSMETA meta;
			ParseBMSMETA(&meta, folderPath, 0);
			sqlite3_snprintf(2048, str, "INSERT INTO folder (path , title , parent , category , info_a , info_b , command , max , date , type , banner , adddate) VALUES(\'%q\',\'%q\',\'%q\',\'%q\',\'%q\',\'%q\',\'%q\',%d , %d , 2 , \'%q\' , %d) ",
				folderPath.body, meta.title.body, AssignCRC32(path).body, meta.genre.body, meta.artist.body, meta.subartist.body, meta.tag.body, meta.selLevel, entry.lastWriteUnixtime, meta.bannerpath.body, now);
			if (SQL_Run(str, sql) != 0) {
				ErrorLogFmtAdd("Fast scan custom folder insert failed: %s\n", sqlite3_errmsg(sql));
			}
			stats.insertedCustomFolders++;
			count++;
		}
	}

	// Subdirectories: insert the folder row, recurse only when it was new.
	for (const auto& entry : entries) {
		if (!entry.isDirectory) {
			continue;
		}

		CSTR searchPath(root);
		searchPath.add(entry.name.body).add("\\");

		CSTR folderinfo(searchPath);
		folderinfo.add("folderinfo.txt");

		if (IsFileExist(folderinfo)) {
			BMSMETA meta;
			ParseBMSMETA(&meta, folderinfo, 0);
			if (meta.judge != 2) {
				meta.judge = 1;
			}
			sqlite3_snprintf(2048, str, "INSERT INTO folder (path , title , parent , category , info_a , info_b , command , max , date , type , banner , adddate) VALUES(\'%q\',\'%q\',\'%q\',\'%q\',\'%q\',\'%q\',\'%q\',%d , %d , %d , \'%q\' , %d) ",
				searchPath.body, meta.title.body, AssignCRC32(path).body, meta.genre.body, meta.artist.body, meta.subartist.body, meta.tag.body, meta.selLevel, entry.lastWriteUnixtime, meta.judge, meta.bannerpath.body, now);
			if (SQL_Run(str, sql) == 0) {
				count += collectFolderTree(searchPath, sql, searchPath, charts, stats);
			}
		}
		else {
			sqlite3_snprintf(2048, str, "INSERT INTO folder (path , title , parent , date , type , adddate) VALUES(\'%q\',\'%q\',\'%q\',%d , 1 , %d )",
				searchPath.body, entry.name.body, AssignCRC32(path).body, entry.lastWriteUnixtime, now);
			if (SQL_Run(str, sql) == 0) {
				count += collectFolderTree(searchPath, sql, searchPath, charts, stats);
			}
		}
	}

	ErrorLogTabSub();
	return count;
}

} // namespace

int
ScanSongsFast(CSTR root, sqlite3* sql, CSTR path)
{
	// Phase A: collect every chart in the subtree (folders inserted inline).
	const auto collectStarted = ScanClock::now();
	std::vector<ChartFile> charts;
	ScanCollectStats stats;
	int count = collectFolderTree(root, sql, path, charts, stats);

	const int now = static_cast<int>(GetNowUnixtime());
	const size_t total = charts.size();
	ErrorLogFmtAdd(
	  "Fast scan collect finished: folders=%d charts=%d custom=%d ms=%d\n",
	  logCount(stats.visitedFolders),
	  logCount(total),
	  logCount(stats.insertedCustomFolders),
	  logDurationMs(elapsedMs(collectStarted)));
	if (total == 0) {
		return count;
	}

	// Phase B/C: parse charts in parallel by batch, then write results with
	// reusable SQLite statements on this single writer thread.
	TagLookup tags(sql);
	SongInserter songs(sql);
	std::vector<CSTR> batchPaths;
	batchPaths.reserve(kParseChunk);
	char message[256];

	for (size_t start = 0; start < total; start += kParseChunk) {
		const size_t end = std::min(start + kParseChunk, total);
		fillBatchPaths(batchPaths, charts, start, end);
		const std::span<const CSTR> batch{ batchPaths.data(), batchPaths.size() };

		snprintf(message,
		         sizeof(message),
		         "Fast scan parsing... %d / %d",
		         logCount(end),
		         logCount(total));
		drawBatchChartProgress(message, charts, start, end);
		ErrorLogFmtAdd("Fast scan batch %d-%d / %d: parse start\n",
		               logCount(start + 1),
		               logCount(end),
		               logCount(total));

		const auto parseStarted = ScanClock::now();
		std::vector<BMSMETA> metas = ParseChartsParallel(batch);
		const long long parseMs = elapsedMs(parseStarted);

		snprintf(message,
		         sizeof(message),
		         "Fast scan inserting... %d / %d",
		         logCount(end),
		         logCount(total));
		drawScanProgress(message);
		const auto insertStarted = ScanClock::now();
		for (size_t i = 0; i < metas.size(); i++) {
			BMSMETA& meta = metas[i];
			tags.apply(meta);
			songs.insert(meta, charts[start + i].path, charts[start + i].lastWriteUnixtime, now);
			count++;
		}
		ErrorLogFmtAdd(
		  "Fast scan batch %d-%d / %d: parse=%dms insert=%dms\n",
		  logCount(start + 1),
		  logCount(end),
		  logCount(total),
		  logDurationMs(parseMs),
		  logDurationMs(elapsedMs(insertStarted)));
	}

	return count;
}

#else

int
ScanSongsFast(CSTR /*root*/, sqlite3* /*sql*/, CSTR /*path*/)
{
	// FIXME(linux): stub, matching SearchSongsFromPath's non-Windows behavior.
	return 0;
}

#endif // _WIN32
