#include "../Interface/Thread.h"
#include "../Interface/Mutex.h"
#include "../Interface/Pipe.h"
#include "../Interface/Database.h"
#include "../Interface/ThreadPool.h"
#include "../urbackupcommon/os_functions.h"
#include "clientdao.h"
#include <map>
#include "tokens.h"

#ifdef _WIN32
#ifndef VSS_XP
#ifndef VSS_S03
#include <Vss.h>
#include <VsWriter.h>
#include <VsBackup.h>
#else
#include <win2003/vss.h>
#include <win2003/vswriter.h>
#include <win2003/vsbackup.h>
#endif //VSS_S03
#else
#include <winxp/vss.h>
#include <winxp/vswriter.h>
#include <winxp/vsbackup.h>
#endif //VSS_XP
#endif

#ifndef _WIN32
#define VSS_ID GUID
#endif

#include "../fileservplugin/IFileServFactory.h"

#include <vector>
#include <string>
#include <fstream>
#include <sstream>

const int c_group_default = 0;
const int c_group_continuous = 1;
const int c_group_max = 99;
const int c_group_size = 100;

const unsigned int flag_end_to_end_verification = 2;
const unsigned int flag_with_scripts = 4;
const unsigned int flag_calc_checksums = 8;
const unsigned int flag_with_orig_path = 16;
const unsigned int flag_with_sequence = 32;
const unsigned int flag_with_proper_symlinks = 64;

class DirectoryWatcherThread;

class IdleCheckerThread : public IThread
{
public:
	void operator()(void);

	static bool getIdle(void);
	static bool getPause(void);
	static void setPause(bool b);

private:
	volatile static bool idle;
	volatile static bool pause;
};

struct SCRef
{
#ifdef _WIN32
	SCRef(void): backupcom(NULL), ok(false), dontincrement(false) {}

	IVssBackupComponents *backupcom;
#endif
	VSS_ID ssetid;
	std::wstring volpath;
	int64 starttime;
	std::wstring target;
	int save_id;
	bool ok;
	bool dontincrement;
	std::vector<std::string> starttokens;
};

struct SCDirs
{
	SCDirs(void): ref(NULL) {}
	std::wstring dir;
	std::wstring target;
	std::wstring orig_target;
	bool running;
	SCRef *ref;
	int64 starttime;
	bool fileserv;
};

struct SHashedFile
{
	SHashedFile(const std::wstring& path,
				_i64 filesize,
				_i64 modifytime,
				const std::string& hash)
				: path(path),
				  filesize(filesize), modifytime(modifytime),
				  hash(hash)
	{
	}

	std::wstring path;
	_i64 filesize;
	_i64 modifytime;
	std::string hash;
};

struct SShadowCopyContext
{
#ifdef _WIN32
	SShadowCopyContext()
		:backupcom(NULL)
	{

	}

	IVssBackupComponents *backupcom;
#endif
};

struct SVssLogItem
{
	std::string msg;
	int loglevel;
	int64 times;
};

struct SBackupScript
{
	std::wstring scriptname;
	std::wstring outputname;
	int64 size;
};

class ClientDAO;

class IndexThread : public IThread
{
public:
	static const char IndexThreadAction_StartFullFileBackup;
	static const char IndexThreadAction_StartIncrFileBackup;
	static const char IndexThreadAction_GetLog;
	static const char IndexThreadAction_PingShadowCopy;
	static const char IndexThreadAction_AddWatchdir;
	static const char IndexThreadAction_RemoveWatchdir;

	IndexThread(void);
	~IndexThread();

	void operator()(void);

	static IMutex* getFilelistMutex(void);
	static IPipe * getMsgPipe(void);
	static IFileServ *getFileSrv(void);

	static void stopIndex(void);

	static void shareDir(const std::wstring& token, std::wstring name, const std::wstring &path);
	static void removeDir(const std::wstring& token, std::wstring name);
	static std::wstring getShareDir(const std::wstring &name);
	static void share_dirs();
	static void unshare_dirs();
	
	static void execute_postbackup_hook(void);

	static void doStop(void);
	
	static bool backgroundBackupsEnabled(const std::string& clientsubname);

	static std::vector<std::wstring> parseExcludePatterns(const std::wstring& val);
	static std::vector<std::wstring> parseIncludePatterns(const std::wstring& val, std::vector<int>& include_depth,
		std::vector<std::wstring>& include_prefix);

	static bool isExcluded(const std::vector<std::wstring>& exlude_dirs, const std::wstring &path);
	static bool isIncluded(const std::vector<std::wstring>& include_dirs, const std::vector<int>& include_depth,
		const std::vector<std::wstring>& include_prefix, const std::wstring &path, bool *adding_worthless);

	static std::wstring mapScriptOutputName(const std::wstring& fn);

	static std::string getSHA256(const std::wstring& fn);
	static std::string getSHA512Binary(const std::wstring& fn);

private:

	bool readBackupDirs(void);
	bool readBackupScripts();

    bool getAbsSymlinkTarget(const std::wstring& symlink, const std::wstring& orig_path, std::wstring& target);
	void addSymlinkBackupDir(const std::wstring& target);
	bool backupNameInUse(const std::wstring& name);
	void removeUnconfirmedSymlinkDirs();

	std::vector<SFileAndHash> convertToFileAndHash(const std::wstring& orig_dir, const std::vector<SFile> files, const std::wstring& fn_filter);

	bool initialCheck(std::wstring orig_dir, std::wstring dir, std::wstring named_path, std::fstream &outfile, bool first, int flags, bool use_db, bool symlinked);

	void indexDirs(void);

	void updateDirs(void);

	static std::wstring sanitizePattern(const std::wstring &p);
	void readPatterns(bool &pattern_changed, bool update_saved_patterns);	

	std::vector<SFileAndHash> getFilesProxy(const std::wstring &orig_path, std::wstring path, const std::wstring& named_path, bool use_db, const std::wstring& fn_filter);

	bool start_shadowcopy(SCDirs *dir, bool *onlyref=NULL, bool allow_restart=false, std::vector<SCRef*> no_restart_refs=std::vector<SCRef*>(), bool for_imagebackup=false, bool *stale_shadowcopy=NULL);

	bool find_existing_shadowcopy(SCDirs *dir, bool *onlyref, bool allow_restart, const std::wstring& wpath, const std::vector<SCRef*>& no_restart_refs, bool for_imagebackup, bool *stale_shadowcopy,
		bool consider_only_own_tokens);
	bool release_shadowcopy(SCDirs *dir, bool for_imagebackup=false, int save_id=-1, SCDirs *dontdel=NULL);
	bool cleanup_saved_shadowcopies(bool start=false);
	std::string lookup_shadowcopy(int sid);
#ifdef _WIN32
	bool start_shadowcopy_win( SCDirs * dir, std::wstring &wpath, bool for_imagebackup, bool * &onlyref );
	bool wait_for(IVssAsync *vsasync);
	std::string GetErrorHResErrStr(HRESULT res);
	bool check_writer_status(IVssBackupComponents *backupcom, std::wstring& errmsg, int loglevel, bool* retryable_error);
	bool checkErrorAndLog(BSTR pbstrWriter, VSS_WRITER_STATE pState, HRESULT pHrResultFailure, std::wstring& errmsg, int loglevel, bool* retryable_error);
#else
	bool start_shadowcopy_lin( SCDirs * dir, std::wstring &wpath, bool for_imagebackup, bool * &onlyref );
#endif

	bool deleteShadowcopy(SCDirs *dir);
	bool deleteSavedShadowCopy(SShadowCopy& scs, SShadowCopyContext& context);
	void clearContext( SShadowCopyContext& context);

	
	void VSSLog(const std::string& msg, int loglevel);
	void VSSLog(const std::wstring& msg, int loglevel);
	void VSSLogLines(const std::string& msg, int loglevel);

	SCDirs* getSCDir(const std::wstring path);

	void execute_prebackup_hook(void);
	void execute_postindex_hook(void);
	std::string execute_script(const std::wstring& cmd);

	void start_filesrv(void);

	bool skipFile(const std::wstring& filepath, const std::wstring& namedpath);

	bool addMissingHashes(std::vector<SFileAndHash>* dbfiles, std::vector<SFileAndHash>* fsfiles, const std::wstring &orig_path, const std::wstring& filepath, const std::wstring& namedpath);

	void modifyFilesInt(std::wstring path, const std::vector<SFileAndHash> &data);
	size_t calcBufferSize( std::wstring &path, const std::vector<SFileAndHash> &data );

	void commitModifyFilesBuffer();

	void addFilesInt(std::wstring path, const std::vector<SFileAndHash> &data);
	void commitAddFilesBuffer();
	std::string getShaBinary(const std::wstring& fn);

	std::wstring removeDirectorySeparatorAtEnd(const std::wstring& path);
	std::string getSHA256Binary(const std::wstring& fn);
	std::wstring addDirectorySeparatorAtEnd(const std::wstring& path);

	void resetFileEntries(void);

	static void addFileExceptions(std::vector<std::wstring>& exlude_dirs);

	void handleHardLinks(const std::wstring& bpath, const std::wstring& vsspath);

	std::string escapeListName(const std::string& listname);

	std::string escapeDirParam(const std::string& dir);
	std::string escapeDirParam(const std::wstring& dir);

	void writeTokens();

	void setFlags(unsigned int flags);

	void writeDir(std::fstream& out, const std::wstring& name, bool with_change, int64 change_identicator, const std::string& extra=std::string());
	bool addBackupScripts(std::fstream& outfile);
	std::string starttoken;

	std::vector<SBackupDir> backup_dirs;

	std::vector<std::wstring> changed_dirs;
	std::vector<std::wstring> open_files;

	static IMutex *filelist_mutex;
	static IMutex *filesrv_mutex;

	static IPipe* msgpipe;

	IPipe *contractor;

	ClientDAO *cd;

	IDatabase *db;

	static IFileServ *filesrv;

	DirectoryWatcherThread *dwt;
	THREADPOOL_TICKET dwt_ticket;

	std::map<std::string, std::map<std::wstring, SCDirs*> > scdirs;
	std::vector<SCRef*> sc_refs;

	int index_c_db;
	int index_c_fs;
	int index_c_db_update;

	static volatile bool stop_index;

	std::vector<std::wstring> exlude_dirs;
	std::vector<std::wstring> include_dirs;
	std::vector<int> include_depth;
	std::vector<std::wstring> include_prefix;

	int64 last_transaction_start;

	static std::map<std::wstring, std::wstring> filesrv_share_dirs;

	std::vector< std::pair<std::wstring, std::vector<SFileAndHash> > > modify_file_buffer;
	size_t modify_file_buffer_size;
	std::vector< std::pair<std::wstring, std::vector<SFileAndHash> > > add_file_buffer;
	size_t add_file_buffer_size;

	int64 last_file_buffer_commit_time;

	int index_group;
	int index_flags;
	std::string index_clientsubname;

	SCDirs* index_scd;

	bool end_to_end_file_backup_verification;
	bool calculate_filehashes_on_client;
	bool with_scripts;
	bool with_orig_path;
	bool with_sequence;
	bool with_proper_symlinks;

	int64 last_tmp_update_time;

	std::wstring index_root_path;
	bool index_error;

	std::vector<SVssLogItem> vsslog;

	std::vector<SBackupScript> scripts;

	_i64 last_filebackup_filetime;

	tokens::TokenCache token_cache;
	int sha_version;
};

std::wstring add_trailing_slash(const std::wstring &strDirName);
