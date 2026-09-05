#if defined _wros_import_included
	#endinput
#endif
#define _wros_import_included

/*
 * wros-import.inc - pull a record off offstyles.net into this server's own timer.
 *
 * wros already fetches every record for the current map, already knows which
 * offstyle style maps to which local shavit style (wros.cfg), and already
 * downloads and caches replay files. This file adds the one thing it doesn't do:
 * writing a record into shavit's playertimes and putting its replay in front of
 * the replay bot. It hangs off the record info submenu rather than a command of
 * its own, so importing is "open !oswr, pick a time, import it".
 *
 * Two rules shape everything below.
 *
 * 1. An import never destroys replay data. The displaced replay is moved into
 *    backup/ before the imported one lands, an existing backup is never
 *    overwritten, and keeping an import archives the backup rather than
 *    deleting it.
 *
 * 2. An import is ephemeral until someone says otherwise. It is rolled back on
 *    map end, and a journal file on disk means a crash mid-map still gets rolled
 *    back on the next boot. "Keep" in the menu is what makes it permanent.
 */

#define IMPORT_TRACK        0
#define IMPORT_LOCK_TIMEOUT 120     // seconds before a held import lock is treated as stuck
#define IMPORT_DIR          "data/wros_import"
// Journal, staging file and replay backups are all keyed on the game port. Two
// srcds instances can bind-mount the same addons/sourcemod/data - cssserver and
// okwl do - and without the key one server's map change would roll back the
// other's live import, and their backups would overwrite each other. The port is
// the only thing that distinguishes them from inside the plugin.
char gS_ImportInstance[8];

void Import_SetInstanceKey()
{
	ConVar cvPort = FindConVar("hostport");
	int    iPort  = (cvPort != null) ? cvPort.IntValue : 0;

	FormatEx(gS_ImportInstance, sizeof(gS_ImportInstance), "%d", iPort);
}

void Import_GetJournalPath(char[] buffer, int maxlen)
{
	BuildPath(Path_SM, buffer, maxlen, IMPORT_DIR ... "/pending-%s.txt", gS_ImportInstance);
}

void Import_GetStagingPath(char[] buffer, int maxlen)
{
	BuildPath(Path_SM, buffer, maxlen, IMPORT_DIR ... "/staging-%s.replay", gS_ImportInstance);
}

// Everything needed to put the leaderboard and the replay folder back exactly as
// they were before the import.
enum struct ImportUndo
{
	bool  bAvailable;
	bool  bPersist;          // survives the map change; set by "Keep this import"
	bool  bWasUpdate;        // false = we inserted, so undo deletes
	char  sMap[PLATFORM_MAX_PATH];
	char  sName[MAX_NAME_LENGTH];
	int   iAccountId;
	int   iLocalStyle;
	float fImportedTime;

	// The playertimes row as it was, only meaningful when bWasUpdate.
	float fPrevTime;
	int   iPrevJumps;
	int   iPrevDate;
	int   iPrevStrafes;
	float fPrevSync;
	float fPrevPoints;
	float fPrevPerfs;

	bool  bReplayInstalled;  // we wrote a replay into the style folder
	bool  bHadBackup;        // and something was there first, kept under backup/
}

ImportUndo gImportUndo;

// One import at a time, server wide: the tail of the pipeline moves replay files
// around and rewrites the leaderboard cache, and two of those interleaving would
// race over both. Held across HTTP and SQL callbacks, so it expires rather than
// stranding if one of them never arrives.
bool            gB_ImportInFlight   = false;
int             gI_ImportLockedAt   = 0;
int             gI_ImportRequester  = 0;    // client serial, 0 for console
WROS_RecordInfo gImportPending;
int             gI_ImportLocalStyle = -1;
float           gF_ImportPreWR      = 0.0;
bool            gB_ImportOverwrite  = false;
char            gS_ImportReplayPath[PLATFORM_MAX_PATH];

Database gH_ImportDatabase = null;
char     gS_ImportPrefix[32];

Convar gCV_ImportEnabled;
Convar gCV_ImportBlockedStyles;
Convar gCV_ImportPersistDefault;

// --- Lifecycle ---------------------------------------------------------------

void Import_OnPluginStart()
{
	gCV_ImportEnabled = new Convar("os_import_enable", "1",
		"Allow admins to import offstyles.net records into this server's timer from the !oswr record menu.", 0, true, 0.0, true, 1.0);

	gCV_ImportBlockedStyles = new Convar("os_import_blocked_styles", "",
		"Local shavit style indices that must never be imported into, comma separated. Empty allows every mapped style."
		..."\nWorth setting if this server also uploads to another global database (sourcejump, for instance), where an imported time sitting in playertimes looks like a run that was set here.", 0);

	gCV_ImportPersistDefault = new Convar("os_import_persist_default", "0",
		"Should an import survive the map change without being confirmed?"
		..."\n0 = imports are ephemeral and roll back on map end unless kept (recommended)"
		..."\n1 = imports are permanent as soon as they land", 0, true, 0.0, true, 1.0);

	RegConsoleCmd("sm_wros_undo", Command_ImportUndo, "Reverses the last offstyles.net import.");
	RegConsoleCmd("sm_wros_keep", Command_ImportKeep, "Makes the last offstyles.net import permanent.");

	// The names the old offstyledb fork used. Upstream offstyledb registers only
	// osdb_*, so these are free - but they are aliases rather than the primary
	// names, in case a build that does claim them ever comes back.
	RegConsoleCmd("sm_os_undo", Command_ImportUndo, "Reverses the last offstyles.net import.");
	RegConsoleCmd("sm_os_keep", Command_ImportKeep, "Makes the last offstyles.net import permanent.");

	Import_SetInstanceKey();
	Import_EnsureDirs();
}

// Called once the database is up. Anything the last map left behind gets rolled
// back here, which covers a crash or a hard restart mid-import.
void Import_OnConfigsExecuted()
{
	Import_ConnectDatabase();

	if(gH_ImportDatabase == null)
	{
		return;
	}

	if(!gImportUndo.bAvailable && Import_ReadJournal())
	{
		LogMessage("[wros] Rolling back an import left over from %s.", gImportUndo.sMap);
		Import_Rollback(false);
	}
}

// wros treats shavit as optional, but GetTimerDatabaseHandle() and
// GetTimerSQLPrefix() both SetFailState rather than returning an error, so
// neither may be reached until shavit has proven it is there. The handle belongs
// to shavit, so it is re-fetched every map rather than cached for the session -
// reloading shavit-core mid-session would otherwise leave a dead handle behind.
void Import_ConnectDatabase()
{
	gH_ImportDatabase = null;

	// The whole feature is a leaderboard write, so shavit-wr is as required as
	// shavit-core is.
	if(GetFeatureStatus(FeatureType_Native, "Shavit_GetDatabase") != FeatureStatus_Available
	|| GetFeatureStatus(FeatureType_Native, "Shavit_GetWorldRecord") != FeatureStatus_Available
	|| GetFeatureStatus(FeatureType_Native, "Shavit_ReloadLeaderboards") != FeatureStatus_Available)
	{
		return;
	}

	char sFile[PLATFORM_MAX_PATH];
	BuildPath(Path_SM, sFile, sizeof(sFile), "configs/shavit-prefix.txt");

	if(!FileExists(sFile))
	{
		LogError("[wros] configs/shavit-prefix.txt is missing, so record imports stay disabled.");
		return;
	}

	Database db = Shavit_GetDatabase();

	if(db == null)
	{
		return;
	}

	gH_ImportDatabase = db;
	GetTimerSQLPrefix(gS_ImportPrefix, sizeof(gS_ImportPrefix));
}

// The replay half of an import needs shavit-replay-playback, which is a separate
// plugin from the timer and can be absent on a server that still has a
// leaderboard. Without it an import is a leaderboard entry and nothing more.
bool Import_CanTouchReplays()
{
	return (gB_ReplayPlayback
		&& GetFeatureStatus(FeatureType_Native, "Shavit_GetReplayFolderPath") == FeatureStatus_Available
		&& GetFeatureStatus(FeatureType_Native, "Shavit_ReloadReplay") == FeatureStatus_Available
		&& GetFeatureStatus(FeatureType_Native, "Shavit_DeleteReplay") == FeatureStatus_Available);
}

void Import_OnMapStart()
{
	Import_SetInstanceKey();
	Import_EnsureDirs();
	Import_DeleteStaging();
}

// The last chance to undo an ephemeral import. Run blocking: the threaded queue
// is not guaranteed to drain before the map tears down, and a rollback that only
// half happens is worse than one that costs a few milliseconds here. If it fails
// the journal stays on disk and Import_OnConfigsExecuted retries next map.
void Import_OnMapEnd()
{
	if(!gImportUndo.bAvailable || gImportUndo.bPersist)
	{
		return;
	}

	Import_Rollback(true);
}

void Import_OnPluginEnd()
{
	// Same reasoning as OnMapEnd - an unloaded plugin can no longer roll back, so
	// do it now or leave the journal for whoever loads next.
	if(gImportUndo.bAvailable && !gImportUndo.bPersist)
	{
		Import_Rollback(true);
	}
}

// --- Access and gating -------------------------------------------------------

bool Import_IsEnabled()
{
	return (gCV_ImportEnabled != null && gCV_ImportEnabled.BoolValue && gH_ImportDatabase != null);
}

// Deliberately a command override rather than a flag ConVar, so it sits next to
// sm_wros_getreplay in admin_overrides.cfg and is adjusted the same way.
bool Import_HasAccess(int client)
{
	if(client == 0)
	{
		return true;
	}

	return CheckCommandAccess(client, "sm_wros_import", ADMFLAG_RCON);
}

bool Import_IsStyleBlocked(int style)
{
	char sBlocked[128];
	gCV_ImportBlockedStyles.GetString(sBlocked, sizeof(sBlocked));

	if(sBlocked[0] == '\0')
	{
		return false;
	}

	char sParts[32][8];
	int iCount = ExplodeString(sBlocked, ",", sParts, sizeof(sParts), sizeof(sParts[]));

	for(int i = 0; i < iCount; i++)
	{
		TrimString(sParts[i]);

		if(sParts[i][0] != '\0' && StringToInt(sParts[i]) == style)
		{
			return true;
		}
	}

	return false;
}

bool Import_IsBusy()
{
	if(!gB_ImportInFlight)
	{
		return false;
	}

	if(GetTime() - gI_ImportLockedAt > IMPORT_LOCK_TIMEOUT)
	{
		LogMessage("[wros] Import lock was held for over %d seconds - releasing it. Check the error log for what stalled.", IMPORT_LOCK_TIMEOUT);
		gB_ImportInFlight = false;
		return false;
	}

	return true;
}

void Import_TakeLock()
{
	gB_ImportInFlight = true;
	gI_ImportLockedAt = GetTime();
}

void Import_TouchLock()
{
	gI_ImportLockedAt = GetTime();
}

void Import_Reply(const char[] phrase, any ...)
{
	int client = GetClientFromSerial(gI_ImportRequester);

	char buffer[256];

	if(client == 0)
	{
		SetGlobalTransTarget(LANG_SERVER);
		VFormat(buffer, sizeof(buffer), phrase, 2);
		PrintToServer("[wros] %s", buffer);
		return;
	}

	SetGlobalTransTarget(client);
	VFormat(buffer, sizeof(buffer), phrase, 2);
	CPrintToChat(client, "%s", buffer);
}

// --- Menu integration --------------------------------------------------------

// True when this record could be imported right now, which is what decides
// whether the item appears in the record info submenu at all.
bool Import_CanShowFor(int client, WROS_RecordInfo record)
{
	if(!Import_IsEnabled() || !Import_HasAccess(client))
	{
		return false;
	}

	// Importing writes a row keyed on the current map, so a record fetched for a
	// different map has nothing to write into.
	if(!StrEqual(record.map, gS_CurrentMap, false))
	{
		return false;
	}

	int iLocalStyle = WROS_ConvertStyle(record.style, WROS_Style_Offstyle, WROS_Style_Server);

	return (iLocalStyle != -1 && !Import_IsStyleBlocked(iLocalStyle));
}

// The import item doubles as the undo/keep item once an import is live for this
// record, so an admin never has to remember a command.
bool Import_IsUndoTargetFor(WROS_RecordInfo record)
{
	if(!gImportUndo.bAvailable)
	{
		return false;
	}

	return (gImportUndo.iAccountId == record.accountid
		&& gImportUndo.iLocalStyle == WROS_ConvertStyle(record.style, WROS_Style_Offstyle, WROS_Style_Server)
		&& StrEqual(gImportUndo.sMap, record.map, false));
}

void Import_AddMenuItems(Menu menu, int client, WROS_RecordInfo record)
{
	if(!Import_CanShowFor(client, record))
	{
		return;
	}

	char sInfo[PLATFORM_MAX_PATH], sDisplay[128];

	if(Import_IsUndoTargetFor(record))
	{
		if(!gImportUndo.bPersist)
		{
			FormatEx(sDisplay, sizeof(sDisplay), "%T", "RecordInfo_Item_ImportKeep", client);
			FormatEx(sInfo, sizeof(sInfo), "6%s", record._id);
			menu.AddItem(sInfo, sDisplay);
		}

		FormatEx(sDisplay, sizeof(sDisplay), "%T", "RecordInfo_Item_ImportUndo", client);
		FormatEx(sInfo, sizeof(sInfo), "7%s", record._id);
		menu.AddItem(sInfo, sDisplay);
		return;
	}

	FormatEx(sDisplay, sizeof(sDisplay), "%T",
		(record.replay_ref[0] != '\0') ? "RecordInfo_Item_Import" : "RecordInfo_Item_Import_NoReplay", client);
	FormatEx(sInfo, sizeof(sInfo), "5%s", record._id);
	menu.AddItem(sInfo, sDisplay);
}

// Entry point from MenuHandler_RecordInfo.
void Import_OnRecordSelected(int client, const char[] id)
{
	WROS_RecordInfo record;

	if(!GetRecordInfo(id, record))
	{
		CPrintToChat(client, "%T", "Chat_Record_NotFound", client);
		return;
	}

	if(!Import_CanShowFor(client, record))
	{
		CPrintToChat(client, "%T", "Chat_Import_NoAccess", client);
		return;
	}

	// A record with no replay still imports as a leaderboard entry, but the
	// style's replay bot keeps playing whatever it had. Worth a confirmation
	// rather than a surprise.
	if(record.replay_ref[0] == '\0')
	{
		Import_ShowNoReplayConfirm(client, record);
		return;
	}

	Import_Start(client, record);
}

void Import_ShowNoReplayConfirm(int client, WROS_RecordInfo record)
{
	char sTime[32];
	FormatSecondsEx(record.time, sTime, sizeof(sTime), 3);

	Menu menu = new Menu(MenuHandler_ImportNoReplay);
	menu.SetTitle("%T\n ", "Import_NoReplay_Title", client, sTime, record.name);

	char sInfo[PLATFORM_MAX_PATH], sDisplay[128];

	FormatEx(sDisplay, sizeof(sDisplay), "%T", "Import_NoReplay_Yes", client);
	FormatEx(sInfo, sizeof(sInfo), "%s", record._id);
	menu.AddItem(sInfo, sDisplay);

	FormatEx(sDisplay, sizeof(sDisplay), "%T", "Import_NoReplay_No", client);
	menu.AddItem("", sDisplay);

	menu.ExitButton = true;
	menu.Display(client, MENU_TIME_FOREVER);
}

public int MenuHandler_ImportNoReplay(Menu menu, MenuAction action, int client, int param2)
{
	switch(action)
	{
		case MenuAction_Select:
		{
			char sInfo[PLATFORM_MAX_PATH];
			menu.GetItem(param2, sInfo, sizeof(sInfo));

			if(sInfo[0] == '\0')
			{
				BuildWRMenu(client, gI_CurrentPagePosition[client]);
				return 0;
			}

			WROS_RecordInfo record;

			if(!GetRecordInfo(sInfo, record))
			{
				CPrintToChat(client, "%T", "Chat_Record_NotFound", client);
				return 0;
			}

			Import_Start(client, record);
		}

		case MenuAction_End: delete menu;
	}

	return 0;
}

// --- The import itself -------------------------------------------------------

void Import_Start(int client, WROS_RecordInfo record)
{
	if(Import_IsBusy())
	{
		CPrintToChat(client, "%T", "Chat_Import_Busy", client);
		return;
	}

	if(!Import_CanShowFor(client, record))
	{
		CPrintToChat(client, "%T", "Chat_Import_NoAccess", client);
		return;
	}

	// A live ephemeral import holds the only copy of the displaced replay in
	// backup/. Starting a second one before it is resolved would leave the first
	// unrecoverable, so it has to be settled first.
	if(gImportUndo.bAvailable && !gImportUndo.bPersist)
	{
		CPrintToChat(client, "%T", "Chat_Import_PendingFirst", client);
		return;
	}

	Import_TakeLock();

	gImportPending         = record;
	gI_ImportLocalStyle    = WROS_ConvertStyle(record.style, WROS_Style_Offstyle, WROS_Style_Server);
	gI_ImportRequester     = GetClientSerial(client);
	gF_ImportPreWR         = Shavit_GetWorldRecord(gI_ImportLocalStyle, IMPORT_TRACK);
	gB_ImportOverwrite     = false;
	gS_ImportReplayPath[0] = '\0';

	// Everything the write below would overwrite is selected here so the undo can
	// put the row back exactly as it was.
	char sQuery[512];
	FormatEx(sQuery, sizeof(sQuery),
		"SELECT time, jumps, date, strafes, sync, points, perfs FROM %splayertimes WHERE map = '%s' AND auth = %d AND style = %d AND track = %d;",
		gS_ImportPrefix, gImportPending.map, gImportPending.accountid, gI_ImportLocalStyle, IMPORT_TRACK);

	gH_ImportDatabase.Query(SQL_OnImportExistingChecked, sQuery, 0, DBPrio_High);
}

public void SQL_OnImportExistingChecked(Database db, DBResultSet results, const char[] error, any data)
{
	if(results == null)
	{
		LogError("[wros] Import: existing-time lookup failed: %s", error);
		Import_Reply("%t", "Chat_Import_Failed_Lookup");
		Import_Finish(false);
		return;
	}

	ImportUndo empty;
	gImportUndo = empty;

	if(results.FetchRow())
	{
		float fLocal = results.FetchFloat(0);

		// Never regress a leaderboard. An equal time is a no-op too.
		if(fLocal <= gImportPending.time)
		{
			char sLocal[32], sRemote[32];
			FormatSecondsEx(fLocal, sLocal, sizeof(sLocal), 3);
			FormatSecondsEx(gImportPending.time, sRemote, sizeof(sRemote), 3);

			Import_Reply("%t", "Chat_Import_Skipped", gImportPending.name, sLocal, sRemote);
			Import_Finish(false);
			return;
		}

		gB_ImportOverwrite = true;

		gImportUndo.fPrevTime    = fLocal;
		gImportUndo.iPrevJumps   = results.FetchInt(1);
		gImportUndo.iPrevDate    = results.FetchInt(2);
		gImportUndo.iPrevStrafes = results.FetchInt(3);
		gImportUndo.fPrevSync    = results.FetchFloat(4);
		gImportUndo.fPrevPoints  = results.FetchFloat(5);
		gImportUndo.fPrevPerfs   = results.FetchFloat(6);
	}

	gImportUndo.bWasUpdate    = gB_ImportOverwrite;
	gImportUndo.iAccountId    = gImportPending.accountid;
	gImportUndo.iLocalStyle   = gI_ImportLocalStyle;
	gImportUndo.fImportedTime = gImportPending.time;
	strcopy(gImportUndo.sMap,  sizeof(gImportUndo.sMap),  gImportPending.map);
	strcopy(gImportUndo.sName, sizeof(gImportUndo.sName), gImportPending.name);

	if(gImportPending.replay_ref[0] == '\0' || !Import_CanTouchReplays())
	{
		Import_WriteUser();
		return;
	}

	// wros caches replay downloads by replay_ref and reuses them for playback, so
	// the import borrows that file rather than fetching its own copy - and copies
	// it before patching, because the cached file has to stay usable for playback.
	char sCached[PLATFORM_MAX_PATH];
	Import_GetCachedReplayPath(gImportPending.replay_ref, sCached, sizeof(sCached));

	if(sCached[0] != '\0' && FileExists(sCached))
	{
		Import_StageReplay(sCached);
		Import_WriteUser();
		return;
	}

	if(!Import_QueueDownload())
	{
		Import_Reply("%t", "Chat_Import_NoReplayFile");
		Import_WriteUser();
	}
}

void Import_GetCachedReplayPath(const char[] replay_ref, char[] buffer, int maxlen)
{
	GetDownloadDirectory(buffer, maxlen);

	if(buffer[0] == '\0')
	{
		return;    // os_dl_directory is empty, the replay feature is off
	}

	Format(buffer, maxlen, "%s/%s.replay", buffer, replay_ref);
}

// Rides wros's own download queue so a replay fetched for an import is the same
// file playback would use afterwards, and a download already in flight is shared.
bool Import_QueueDownload()
{
	char sOutputFile[PLATFORM_MAX_PATH];
	Import_GetCachedReplayPath(gImportPending.replay_ref, sOutputFile, sizeof(sOutputFile));

	if(sOutputFile[0] == '\0' || !gB_DirExists)
	{
		return false;
	}

	bool bQueued = (gA_DownloadQueue != null && gA_DownloadQueue.FindString(sOutputFile, 0) != -1);

	if(gA_DownloadQueue == null)
	{
		gA_DownloadQueue = new ArrayList(sizeof(download_queue_t));
	}

	download_queue_t aQueue;
	aQueue.iRequester = gI_ImportRequester;
	aQueue.sPath      = sOutputFile;
	aQueue.sMap       = gS_CurrentMap;
	aQueue.iStyle     = gI_ImportLocalStyle;
	aQueue.sName      = gImportPending.name;
	aQueue.sReplayRef = gImportPending.replay_ref;
	aQueue.fTime      = bQueued ? 0.0 : GetEngineTime();
	aQueue.bImport    = true;
	gA_DownloadQueue.PushArray(aQueue);

	if(!bQueued)
	{
		DownloadReplay(aQueue);
	}

	Import_TouchLock();
	Import_Reply("%t", "Chat_Import_Downloading");

	return true;
}

// Resumes the pipeline from DownloadFinished when the queue entry was ours.
void Import_OnReplayDownloaded(bool success, download_queue_t queue)
{
	if(!gB_ImportInFlight)
	{
		return;    // lock expired or the import was already resolved
	}

	Import_TouchLock();

	if(!success || !FileExists(queue.sPath))
	{
		Import_Reply("%t", "Chat_Import_ReplayDownloadFailed");
	}
	else
	{
		Import_StageReplay(queue.sPath);
	}

	Import_WriteUser();
}

// Copies the cached replay somewhere private and patches the copy's header. The
// cached file is left byte-for-byte alone so wros can still play it.
void Import_StageReplay(const char[] cachedPath)
{
	char sStaging[PLATFORM_MAX_PATH];
	Import_GetStagingPath(sStaging, sizeof(sStaging));

	Import_DeleteFileIfExists(sStaging);

	if(!Import_CopyFile(cachedPath, sStaging))
	{
		LogError("[wros] Import: could not copy %s to %s", cachedPath, sStaging);
		Import_Reply("%t", "Chat_Import_ReplayUnusable", "copy failed");
		return;
	}

	char sError[128];

	if(!Import_PrepareReplay(sStaging, gImportPending.map, gI_ImportLocalStyle, sError, sizeof(sError)))
	{
		Import_DeleteFileIfExists(sStaging);
		Import_Reply("%t", "Chat_Import_ReplayUnusable", sError);
		return;
	}

	strcopy(gS_ImportReplayPath, sizeof(gS_ImportReplayPath), sStaging);
}

// A downloaded replay carries the source server's style and track indices, and
// shavit refuses to load one whose header doesn't match the style it is being
// loaded as (LoadReplayCache in shavit/replay-file.inc). wros sidesteps this by
// patching in memory and playing from a frame cache; a replay that has to live on
// disk and be loaded by shavit itself has to be patched for real.
bool Import_PrepareReplay(const char[] path, const char[] map, int localStyle, char[] error, int errlen)
{
	replay_header_t header;
	File fCheck = ReadReplayHeader(path, header, localStyle, IMPORT_TRACK);

	if(fCheck == null)
	{
		strcopy(error, errlen, "unreadable header");
		return false;
	}

	delete fCheck;

	if(!StrEqual(header.sReplayFormat, REPLAY_FORMAT_FINAL))
	{
		FormatEx(error, errlen, "unsupported format '%s'", header.sReplayFormat);
		return false;
	}

	// Below 0x03 the header has no map/style/track fields at all, so there is
	// nothing to verify or patch.
	if(header.iReplayVersion < 0x03)
	{
		FormatEx(error, errlen, "format 0x%02x is too old", header.iReplayVersion);
		return false;
	}

	if(header.iReplayVersion > REPLAY_FORMAT_SUBVERSION)
	{
		FormatEx(error, errlen, "format 0x%02x is newer than this shavit", header.iReplayVersion);
		return false;
	}

	if(!StrEqual(header.sMap, map, false))
	{
		FormatEx(error, errlen, "replay is for %s", header.sMap);
		return false;
	}

	if(header.iStyle == localStyle && header.iTrack == IMPORT_TRACK)
	{
		return true;
	}

	File fFile = OpenFile(path, "rb+");

	if(fFile == null)
	{
		strcopy(error, errlen, "could not open replay for writing");
		return false;
	}

	char sLine[64], sMapInFile[PLATFORM_MAX_PATH];

	// Header layout: "<version>:<format>\n", then the map as a null terminated
	// string, then the style and track bytes.
	if(!fFile.ReadLine(sLine, sizeof(sLine)) || fFile.ReadString(sMapInFile, sizeof(sMapInFile)) == -1)
	{
		delete fFile;
		strcopy(error, errlen, "header truncated");
		return false;
	}

	// Required by stdio: a read/write handle needs a seek between reading and
	// writing, even when the position is already correct.
	int iStylePos = fFile.Position;

	if(iStylePos == -1 || !fFile.Seek(iStylePos, SEEK_SET))
	{
		delete fFile;
		strcopy(error, errlen, "could not seek to the style byte");
		return false;
	}

	bool bWritten = fFile.WriteInt8(localStyle) && fFile.WriteInt8(IMPORT_TRACK);
	delete fFile;

	if(!bWritten)
	{
		strcopy(error, errlen, "could not write patched header");
		return false;
	}

	return true;
}

// playertimes.auth is a foreign key onto users.auth, so the user row has to exist
// first. IGNORE rather than upsert: an imported record must not clobber the name,
// ip or login timestamps of someone who actually plays here.
void Import_WriteUser()
{
	char sName[MAX_NAME_LENGTH * 2 + 1];
	SQL_EscapeString(gH_ImportDatabase, gImportPending.name, sName, sizeof(sName));

	char sDriver[16];
	SQL_ReadDriver(gH_ImportDatabase, sDriver, sizeof(sDriver));

	char sQuery[512];
	FormatEx(sQuery, sizeof(sQuery),
		"INSERT %s INTO %susers (auth, name, ip, lastlogin, firstlogin) VALUES (%d, '%s', 0, %d, %d);",
		StrEqual(sDriver, "mysql", false) ? "IGNORE" : "OR IGNORE",
		gS_ImportPrefix, gImportPending.accountid, sName,
		gImportPending.date, gImportPending.date);

	gH_ImportDatabase.Query(SQL_OnImportUserWritten, sQuery, 0, DBPrio_High);
}

public void SQL_OnImportUserWritten(Database db, DBResultSet results, const char[] error, any data)
{
	if(results == null)
	{
		LogError("[wros] Import: users insert failed: %s", error);
		Import_Reply("%t", "Chat_Import_Failed_User");
		Import_Finish(false);
		return;
	}

	Import_WriteTime();
}

void Import_WriteTime()
{
	// The API has no perfs field, and guessing one would poison the stat.
	float fPerfs  = 0.0;
	float fPoints = 0.0;

	if(GetFeatureStatus(FeatureType_Native, "Shavit_GuessPointsForTime") == FeatureStatus_Available)
	{
		fPoints = Shavit_GuessPointsForTime(IMPORT_TRACK, gI_ImportLocalStyle, -1, gImportPending.time, gF_ImportPreWR);
	}

	// wros stores -1.0 for "the API had no sync for this run"; playertimes.sync
	// has no such sentinel, so it becomes 0.
	float fSync = (gImportPending.sync < 0.0) ? 0.0 : gImportPending.sync;

	char sQuery[1024];

	if(gB_ImportOverwrite)
	{
		// completions is deliberately left alone - an import is not a new run.
		FormatEx(sQuery, sizeof(sQuery),
			"UPDATE %splayertimes SET time = %.9f, jumps = %d, date = %d, strafes = %d, sync = %.02f, points = %f, perfs = %.2f WHERE map = '%s' AND auth = %d AND style = %d AND track = %d;",
			gS_ImportPrefix, gImportPending.time, gImportPending.jumps, gImportPending.date,
			gImportPending.strafes, fSync, fPoints, fPerfs,
			gImportPending.map, gImportPending.accountid, gI_ImportLocalStyle, IMPORT_TRACK);
	}
	else
	{
		FormatEx(sQuery, sizeof(sQuery),
			"INSERT INTO %splayertimes (auth, map, time, jumps, date, style, strafes, sync, points, track, perfs) VALUES (%d, '%s', %.9f, %d, %d, %d, %d, %.02f, %f, %d, %.2f);",
			gS_ImportPrefix, gImportPending.accountid, gImportPending.map, gImportPending.time,
			gImportPending.jumps, gImportPending.date, gI_ImportLocalStyle,
			gImportPending.strafes, fSync, fPoints, IMPORT_TRACK, fPerfs);
	}

	gH_ImportDatabase.Query(SQL_OnImportTimeWritten, sQuery, 0, DBPrio_High);
}

public void SQL_OnImportTimeWritten(Database db, DBResultSet results, const char[] error, any data)
{
	if(results == null)
	{
		LogError("[wros] Import: playertimes write failed: %s", error);
		Import_Reply("%t", "Chat_Import_Failed_Write");
		Import_Finish(false);
		return;
	}

	char sTime[32];
	FormatSecondsEx(gImportPending.time, sTime, sizeof(sTime), 3);

	// shavit only keeps one replay per style and track - the WR's. A slower import
	// has nowhere to put its replay, so it stays a leaderboard entry only.
	bool bIsNewWR   = (gF_ImportPreWR == 0.0 || gImportPending.time < gF_ImportPreWR);
	bool bHasReplay = (gS_ImportReplayPath[0] != '\0');
	bool bInstalled = false;

	if(bHasReplay && bIsNewWR)
	{
		char sError[128];
		bInstalled = Import_InstallReplay(gI_ImportLocalStyle, gImportPending.map, gS_ImportReplayPath, sError, sizeof(sError));

		if(!bInstalled)
		{
			LogError("[wros] Import: replay install failed (%s)", sError);
			Import_Reply("%t", "Chat_Import_ReplayInstallFailed", sError);
		}
	}

	Shavit_ReloadLeaderboards();

	if(bInstalled)
	{
		Shavit_ReloadReplay(gI_ImportLocalStyle, IMPORT_TRACK, true);
	}

	gImportUndo.bReplayInstalled = bInstalled;
	gImportUndo.bAvailable       = true;
	gImportUndo.bPersist         = gCV_ImportPersistDefault.BoolValue;

	if(!gImportUndo.bPersist)
	{
		Import_WriteJournal();
	}

	stylestrings_t styleStrings;
	Shavit_GetStyleStringsStruct(gI_ImportLocalStyle, styleStrings);

	LogMessage("[wros] Imported %s by %s (%s, %s) from offstyles.net record %s%s.",
		sTime, gImportPending.name, gImportPending.map, styleStrings.sStyleName,
		gImportPending._id, bInstalled ? " with replay" : "");

	if(bInstalled)
	{
		Import_Reply("%t", "Chat_Import_Done_WR", sTime, gImportPending.name, styleStrings.sStyleName);
	}
	else if(bHasReplay && !bIsNewWR)
	{
		Import_Reply("%t", "Chat_Import_Done_NotWR", sTime, gImportPending.name, styleStrings.sStyleName);
	}
	else
	{
		Import_Reply("%t", "Chat_Import_Done_NoReplay", sTime, gImportPending.name, styleStrings.sStyleName);
	}

	Import_Reply("%t", gImportUndo.bPersist ? "Chat_Import_Hint_Undo" : "Chat_Import_Hint_Ephemeral");

	Import_Finish(bInstalled);
}

void Import_Finish(bool replayConsumed)
{
	if(!replayConsumed)
	{
		Import_DeleteFileIfExists(gS_ImportReplayPath);
	}

	gS_ImportReplayPath[0] = '\0';
	gI_ImportRequester     = 0;
	gF_ImportPreWR         = 0.0;
	gB_ImportOverwrite     = false;
	gB_ImportInFlight      = false;
}

// --- Replay file handling ----------------------------------------------------

// Moves the patched replay into shavit's replay folder, keeping whatever was
// there under backup/.
//
// An existing backup is never overwritten. If one is already sitting there, the
// import that put it there has not been rolled back yet, which makes it the real
// pre-import replay - the file being displaced now is a previous import's own
// artifact and is the one that can safely go.
bool Import_InstallReplay(int style, const char[] map, const char[] tmpPath, char[] error, int errlen)
{
	char sFolder[PLATFORM_MAX_PATH];
	Shavit_GetReplayFolderPath(sFolder, sizeof(sFolder));

	char sStyleDir[PLATFORM_MAX_PATH];
	FormatEx(sStyleDir, sizeof(sStyleDir), "%s/%d", sFolder, style);

	if(!DirExists(sStyleDir) && !CreateDirectory(sStyleDir, 511))
	{
		strcopy(error, errlen, "no replay folder for this style");
		return false;
	}

	// Mirrors Shavit_GetReplayFilePath() in shavit/replay-stocks.sp: only tracks
	// above 0 get a suffix, and imports are always track 0.
	char sDest[PLATFORM_MAX_PATH];
	FormatEx(sDest, sizeof(sDest), "%s/%s.replay", sStyleDir, map);

	gImportUndo.bHadBackup = false;

	if(FileExists(sDest))
	{
		char sBackup[PLATFORM_MAX_PATH];
		Import_GetBackupPath(style, map, sBackup, sizeof(sBackup));

		if(FileExists(sBackup))
		{
			LogMessage("[wros] Import: %s already held a backup, keeping it and discarding the displaced file.", sBackup);
			DeleteFile(sDest);
		}
		else if(!RenameFile(sBackup, sDest))
		{
			strcopy(error, errlen, "could not back up the existing replay");
			return false;
		}

		gImportUndo.bHadBackup = true;
	}

	if(!RenameFile(sDest, tmpPath))
	{
		strcopy(error, errlen, "could not move the replay into place");
		return false;
	}

	return true;
}

void Import_GetBackupPath(int style, const char[] map, char[] buffer, int maxlen)
{
	BuildPath(Path_SM, buffer, maxlen, IMPORT_DIR ... "/backup/%s_%d_%s.replay", gS_ImportInstance, style, map);
}

// Same layout as Shavit_GetReplayFilePath() for track 0.
void Import_GetReplayDestPath(int style, const char[] map, char[] buffer, int maxlen)
{
	char sFolder[PLATFORM_MAX_PATH];
	Shavit_GetReplayFolderPath(sFolder, sizeof(sFolder));

	FormatEx(buffer, maxlen, "%s/%d/%s.replay", sFolder, style, map);
}

bool Import_CopyFile(const char[] from, const char[] to)
{
	File fIn = OpenFile(from, "rb");

	if(fIn == null)
	{
		return false;
	}

	File fOut = OpenFile(to, "wb");

	if(fOut == null)
	{
		delete fIn;
		return false;
	}

	int  buffer[2048];
	int  iRead;
	bool bOk = true;

	while((iRead = fIn.Read(buffer, sizeof(buffer), 1)) > 0)
	{
		if(!fOut.Write(buffer, iRead, 1))
		{
			bOk = false;
			break;
		}
	}

	if(iRead < 0)
	{
		bOk = false;
	}

	delete fIn;
	delete fOut;

	if(!bOk)
	{
		DeleteFile(to);
	}

	return bOk;
}

void Import_DeleteFileIfExists(const char[] path)
{
	if(path[0] != '\0' && FileExists(path))
	{
		DeleteFile(path);
	}
}

void Import_DeleteStaging()
{
	char sPath[PLATFORM_MAX_PATH];
	Import_GetStagingPath(sPath, sizeof(sPath));
	Import_DeleteFileIfExists(sPath);
}

void Import_EnsureDirs()
{
	char sDir[PLATFORM_MAX_PATH];

	BuildPath(Path_SM, sDir, sizeof(sDir), IMPORT_DIR);

	if(!DirExists(sDir))
	{
		CreateDirectory(sDir, 511);
	}

	BuildPath(Path_SM, sDir, sizeof(sDir), IMPORT_DIR ... "/backup");

	if(!DirExists(sDir))
	{
		CreateDirectory(sDir, 511);
	}
}

// --- Undo and keep -----------------------------------------------------------

public Action Command_ImportUndo(int client, int args)
{
	if(!Import_HasAccess(client))
	{
		CReplyToCommand(client, "%T", "Chat_Import_NoAccess", client);
		return Plugin_Handled;
	}

	if(Import_IsBusy())
	{
		CReplyToCommand(client, "%T", "Chat_Import_Busy", client);
		return Plugin_Handled;
	}

	if(!gImportUndo.bAvailable)
	{
		CReplyToCommand(client, "%T", "Chat_Import_NothingToUndo", client);
		return Plugin_Handled;
	}

	gI_ImportRequester = (client == 0) ? 0 : GetClientSerial(client);
	Import_Rollback(false);

	return Plugin_Handled;
}

public Action Command_ImportKeep(int client, int args)
{
	if(!Import_HasAccess(client))
	{
		CReplyToCommand(client, "%T", "Chat_Import_NoAccess", client);
		return Plugin_Handled;
	}

	if(!gImportUndo.bAvailable || gImportUndo.bPersist)
	{
		CReplyToCommand(client, "%T", "Chat_Import_NothingToKeep", client);
		return Plugin_Handled;
	}

	char sName[MAX_NAME_LENGTH];
	strcopy(sName, sizeof(sName), gImportUndo.sName);

	Import_Persist();
	CReplyToCommand(client, "%T", "Chat_Import_Kept", client, sName);

	return Plugin_Handled;
}

// Makes the live import permanent. The displaced replay is archived rather than
// deleted - it is still the only copy of a run this server actually recorded.
void Import_Persist()
{
	if(gImportUndo.bHadBackup)
	{
		char sBackup[PLATFORM_MAX_PATH], sArchive[PLATFORM_MAX_PATH];
		Import_GetBackupPath(gImportUndo.iLocalStyle, gImportUndo.sMap, sBackup, sizeof(sBackup));

		if(FileExists(sBackup))
		{
			FormatEx(sArchive, sizeof(sArchive), "%s.superseded-%d", sBackup, GetTime());

			if(RenameFile(sArchive, sBackup))
			{
				LogMessage("[wros] Import kept; the replay it displaced is archived at %s", sArchive);
			}
			else
			{
				LogError("[wros] Import kept, but %s could not be archived. It will be treated as the backup for the next import on this map.", sBackup);
			}
		}

		gImportUndo.bHadBackup = false;
	}

	gImportUndo.bPersist = true;
	Import_DeleteJournal();
}

// Reverses the live import. blocking is set from OnMapEnd and OnPluginEnd, where
// the threaded queue is not guaranteed to drain before everything tears down.
void Import_Rollback(bool blocking)
{
	if(!gImportUndo.bAvailable)
	{
		return;
	}

	// The handle belongs to shavit and this is the path that must not fail
	// quietly, so take a fresh one rather than trusting the cached reference. If
	// shavit has gone the journal stays on disk and the next boot picks it up.
	Import_ConnectDatabase();

	if(gH_ImportDatabase == null)
	{
		LogError("[wros] Import rollback for %s could not reach shavit's database. The journal is kept, so it retries next map.", gImportUndo.sMap);
		return;
	}

	char sQuery[1024];

	if(gImportUndo.bWasUpdate)
	{
		// Put back exactly what Import_WriteTime overwrote.
		FormatEx(sQuery, sizeof(sQuery),
			"UPDATE %splayertimes SET time = %.9f, jumps = %d, date = %d, strafes = %d, sync = %.02f, points = %f, perfs = %.2f WHERE map = '%s' AND auth = %d AND style = %d AND track = %d;",
			gS_ImportPrefix, gImportUndo.fPrevTime, gImportUndo.iPrevJumps, gImportUndo.iPrevDate,
			gImportUndo.iPrevStrafes, gImportUndo.fPrevSync, gImportUndo.fPrevPoints, gImportUndo.fPrevPerfs,
			gImportUndo.sMap, gImportUndo.iAccountId, gImportUndo.iLocalStyle, IMPORT_TRACK);
	}
	else
	{
		// The row didn't exist before the import, so remove it. The users row is
		// deliberately left behind - other tables may reference it by now, and an
		// orphan user with no times is harmless.
		FormatEx(sQuery, sizeof(sQuery),
			"DELETE FROM %splayertimes WHERE map = '%s' AND auth = %d AND style = %d AND track = %d;",
			gS_ImportPrefix, gImportUndo.sMap, gImportUndo.iAccountId, gImportUndo.iLocalStyle, IMPORT_TRACK);
	}

	if(!blocking)
	{
		Import_TakeLock();
		gH_ImportDatabase.Query(SQL_OnUndoWritten, sQuery, 0, DBPrio_High);
		return;
	}

	SQL_LockDatabase(gH_ImportDatabase);
	DBResultSet hResults = SQL_Query(gH_ImportDatabase, sQuery);
	SQL_UnlockDatabase(gH_ImportDatabase);

	if(hResults == null)
	{
		char sError[256];
		SQL_GetError(gH_ImportDatabase, sError, sizeof(sError));
		LogError("[wros] Import rollback failed on shutdown (%s). The journal is kept, so it retries next map.", sError);
		return;
	}

	delete hResults;
	Import_FinishRollback(false);
}

public void SQL_OnUndoWritten(Database db, DBResultSet results, const char[] error, any data)
{
	// Released up front: everything below is synchronous, so nothing can
	// interleave, and a failure in it must not strand the lock.
	gB_ImportInFlight = false;

	if(results == null)
	{
		LogError("[wros] Import: rollback write failed: %s", error);
		Import_Reply("%t", "Chat_Import_UndoFailed");
		return;
	}

	Import_FinishRollback(true);
}

// The file half of the rollback, plus clearing the journal and the undo state.
//
// live is false when this runs from OnMapEnd or OnPluginEnd. There is nobody to
// tell and no replay bot worth restarting at that point, and poking one while the
// map tears down is asking for trouble.
void Import_FinishRollback(bool live)
{
	bool bReplayRestored = false;

	// A journal written before shavit-replay-playback was unloaded can still say
	// a replay was installed. Without the natives there is no safe way to put it
	// back, so leave the files alone and say where the backup is.
	if(gImportUndo.bReplayInstalled && !Import_CanTouchReplays())
	{
		LogError("[wros] Import rollback: shavit-replay-playback is not loaded, so the replay for %s style %d was left in place. The previous one is in %s",
			gImportUndo.sMap, gImportUndo.iLocalStyle, IMPORT_DIR ... "/backup");
	}
	else if(gImportUndo.bReplayInstalled)
	{
		char sDest[PLATFORM_MAX_PATH];
		Import_GetReplayDestPath(gImportUndo.iLocalStyle, gImportUndo.sMap, sDest, sizeof(sDest));

		if(gImportUndo.bHadBackup)
		{
			char sBackup[PLATFORM_MAX_PATH];
			Import_GetBackupPath(gImportUndo.iLocalStyle, gImportUndo.sMap, sBackup, sizeof(sBackup));

			if(FileExists(sBackup))
			{
				Import_DeleteFileIfExists(sDest);
				bReplayRestored = RenameFile(sDest, sBackup);
			}

			if(bReplayRestored)
			{
				Shavit_ReloadReplay(gImportUndo.iLocalStyle, IMPORT_TRACK, live);
			}
			else
			{
				LogError("[wros] Import rollback could not restore the previous replay for %s style %d - look in %s",
					gImportUndo.sMap, gImportUndo.iLocalStyle, IMPORT_DIR ... "/backup");

				if(live)
				{
					Import_Reply("%t", "Chat_Import_UndoNoReplay");
				}
			}
		}
		else
		{
			// Nothing was there before the import, so the replay goes entirely.
			// Shavit_DeleteReplay removes the file and unloads the cached frames -
			// deleting it ourselves and then reloading would point the replay bot
			// at a file that is no longer there.
			Shavit_DeleteReplay(gImportUndo.sMap, gImportUndo.iLocalStyle, IMPORT_TRACK);
			Import_DeleteFileIfExists(sDest);
		}
	}

	Shavit_ReloadLeaderboards();

	LogMessage("[wros] Rolled back the import of %s by %s (style %d)%s.",
		gImportUndo.sMap, gImportUndo.sName, gImportUndo.iLocalStyle,
		bReplayRestored ? ", previous replay restored" : "");

	if(live)
	{
		char sTime[32];
		FormatSecondsEx(gImportUndo.fImportedTime, sTime, sizeof(sTime), 3);
		Import_Reply("%t", "Chat_Import_Undone", sTime, gImportUndo.sName);
	}

	Import_DeleteJournal();

	ImportUndo empty;
	gImportUndo = empty;

	gI_ImportRequester = 0;
}

// --- Journal -----------------------------------------------------------------
//
// An ephemeral import that is never confirmed has to be reversed even if the
// server never reaches OnMapEnd. The journal is what makes that survive a crash:
// it is written when the import lands and removed the moment it is rolled back
// or kept, so its mere presence at boot means "there is an import to reverse".

void Import_WriteJournal()
{
	char sPath[PLATFORM_MAX_PATH];
	Import_GetJournalPath(sPath, sizeof(sPath));

	KeyValues kv = new KeyValues("wros_import");

	kv.SetString("map", gImportUndo.sMap);
	kv.SetString("name", gImportUndo.sName);
	kv.SetNum("auth", gImportUndo.iAccountId);
	kv.SetNum("style", gImportUndo.iLocalStyle);
	kv.SetFloat("imported_time", gImportUndo.fImportedTime);
	kv.SetNum("was_update", gImportUndo.bWasUpdate);
	kv.SetFloat("prev_time", gImportUndo.fPrevTime);
	kv.SetNum("prev_jumps", gImportUndo.iPrevJumps);
	kv.SetNum("prev_date", gImportUndo.iPrevDate);
	kv.SetNum("prev_strafes", gImportUndo.iPrevStrafes);
	kv.SetFloat("prev_sync", gImportUndo.fPrevSync);
	kv.SetFloat("prev_points", gImportUndo.fPrevPoints);
	kv.SetFloat("prev_perfs", gImportUndo.fPrevPerfs);
	kv.SetNum("replay_installed", gImportUndo.bReplayInstalled);
	kv.SetNum("had_backup", gImportUndo.bHadBackup);

	kv.Rewind();

	if(!kv.ExportToFile(sPath))
	{
		LogError("[wros] Could not write the import journal to %s. This import will not roll back automatically.", sPath);
	}

	delete kv;
}

bool Import_ReadJournal()
{
	char sPath[PLATFORM_MAX_PATH];
	Import_GetJournalPath(sPath, sizeof(sPath));

	if(!FileExists(sPath))
	{
		return false;
	}

	KeyValues kv = new KeyValues("wros_import");

	if(!kv.ImportFromFile(sPath))
	{
		delete kv;
		LogError("[wros] The import journal at %s is unreadable - removing it. Check %s by hand.", sPath, IMPORT_DIR ... "/backup");
		DeleteFile(sPath);
		return false;
	}

	ImportUndo undo;

	kv.GetString("map", undo.sMap, sizeof(undo.sMap));
	kv.GetString("name", undo.sName, sizeof(undo.sName));
	undo.iAccountId       = kv.GetNum("auth");
	undo.iLocalStyle      = kv.GetNum("style", -1);
	undo.fImportedTime    = kv.GetFloat("imported_time");
	undo.bWasUpdate       = view_as<bool>(kv.GetNum("was_update"));
	undo.fPrevTime        = kv.GetFloat("prev_time");
	undo.iPrevJumps       = kv.GetNum("prev_jumps");
	undo.iPrevDate        = kv.GetNum("prev_date");
	undo.iPrevStrafes     = kv.GetNum("prev_strafes");
	undo.fPrevSync        = kv.GetFloat("prev_sync");
	undo.fPrevPoints      = kv.GetFloat("prev_points");
	undo.fPrevPerfs       = kv.GetFloat("prev_perfs");
	undo.bReplayInstalled = view_as<bool>(kv.GetNum("replay_installed"));
	undo.bHadBackup       = view_as<bool>(kv.GetNum("had_backup"));

	delete kv;

	if(undo.sMap[0] == '\0' || undo.iLocalStyle == -1 || undo.iAccountId == 0)
	{
		LogError("[wros] The import journal at %s is incomplete - removing it. Check %s by hand.", sPath, IMPORT_DIR ... "/backup");
		DeleteFile(sPath);
		return false;
	}

	undo.bAvailable = true;
	undo.bPersist   = false;
	gImportUndo     = undo;

	return true;
}

void Import_DeleteJournal()
{
	char sPath[PLATFORM_MAX_PATH];
	Import_GetJournalPath(sPath, sizeof(sPath));
	Import_DeleteFileIfExists(sPath);
}
