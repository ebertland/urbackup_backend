/*************************************************************************
*    UrBackup - Client/Server backup system
*    Copyright (C) 2011  Martin Raiber
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include "../vld.h"
#ifdef _WIN32
#define DLLEXPORT extern "C" __declspec (dllexport)
#else
#define DLLEXPORT extern "C"
#endif

#include <vector>

#define DEF_SERVER
#include "../Interface/Server.h"
IServer *Server;

#include "../Interface/Action.h"
#include "../Interface/Database.h"
#include "../Interface/SessionMgr.h"
#include "../Interface/Pipe.h"
#include "../Interface/Query.h"
#include "../Interface/Thread.h"
#include "../Interface/File.h"

#include "../fsimageplugin/IFSImageFactory.h"
#include "../cryptoplugin/ICryptoFactory.h"

#include "database.h"

#include "ClientService.h"
#include "client.h"
#include "../stringtools.h"
#include "ServerIdentityMgr.h"
#include "../urbackupcommon/os_functions.h"
#ifdef _WIN32
#include "DirectoryWatcherThread.h"
#endif
#include "InternetClient.h"
#include <stdlib.h>

#ifdef _WIN32
#include <Windows.h>
#include <Sddl.h>
#include <Aclapi.h>
#endif

PLUGIN_ID filesrv_pluginid;
IFSImageFactory *image_fak;
ICryptoFactory *crypto_fak;
std::string server_identity;
std::string server_token;

const unsigned short serviceport=35623;

void init_mutex1(void);
bool testEscape(void);
void do_restore(void);
void restore_wizard(void);
void upgrade(void);
bool upgrade_client(void);

std::string lang="en";
std::string time_format_str_de="%d.%m.%Y %H:%M";
std::string time_format_str="%m/%d/%Y %H:%M";

#ifdef _WIN32
const std::string pw_file="pw.txt";
const std::string pw_change_file="pw_change.txt";
const std::string new_file="new.txt";
#else
const std::string pw_file="urbackup/pw.txt";
const std::string pw_change_file="urbackup/pw_change.txt";
const std::string new_file="urbackup/new.txt";
#endif

THREADPOOL_TICKET indexthread_ticket;
THREADPOOL_TICKET internetclient_ticket;

namespace
{

bool copy_file(const std::wstring &src, const std::wstring &dst)
{
	IFile *fsrc=Server->openFile(src, MODE_READ);
	if(fsrc==NULL) return false;
	IFile *fdst=Server->openFile(dst, MODE_WRITE);
	if(fdst==NULL)
	{
		Server->destroy(fsrc);
		return false;
	}
	char buf[4096];
	size_t rc;
	while( (rc=(_u32)fsrc->Read(buf, 4096))>0)
	{
		fdst->Write(buf, (_u32)rc);
	}
	
	Server->destroy(fsrc);
	Server->destroy(fdst);
	return true;
}

#ifdef _WIN32

const TCHAR * szSD = TEXT("D:")       // Discretionary ACL
        //TEXT("(D;OICI;GA;;;WD)")     // Deny access to 
                                     // built-in guests
        TEXT("(A;OICI;GA;;;SY)") // Deny access to 
                                     // to authenticated 
                                     // users
        TEXT("(A;OICI;GA;;;BA)");    // Allow full control 
                                     // to administrators

bool change_file_permissions(const std::string& filename)
{
	PSECURITY_DESCRIPTOR pSDCNV = NULL;

     BOOL b=ConvertStringSecurityDescriptorToSecurityDescriptor(
                szSD,
                SDDL_REVISION_1,
				&pSDCNV,
                NULL);

	 if(!b)
	 {
		 Server->Log("Error creating security descriptor", LL_ERROR);
		 return false;
	 }

	SECURITY_DESCRIPTOR sd = {};
	 DWORD sd_size = sizeof(sd);
	 PACL pDACL = NULL;
	 DWORD dwDACLSize = 0;
	 PACL pSACL = NULL;
	 DWORD dwSACLSize = 0;
	 DWORD dwOwnerSIDSize = 0;
	 DWORD dwGroupSIDSize = 0;


	 if (! MakeAbsoluteSD(pSDCNV, &sd, &sd_size, 
           pDACL, &dwDACLSize, 
           pSACL, &dwSACLSize, 
           NULL, &dwOwnerSIDSize, 
           NULL, &dwGroupSIDSize) ) {
 
 
		  pDACL = (PACL) GlobalAlloc(GPTR, dwDACLSize);
		  pSACL = (PACL) GlobalAlloc(GPTR, dwSACLSize);
 
		  if (! MakeAbsoluteSD(pSDCNV, &sd, &sd_size, pDACL, &dwDACLSize, 
				   pSACL, &dwSACLSize, NULL, &dwOwnerSIDSize, 
				   NULL, &dwGroupSIDSize) ) {
			Server->Log("Error: MakeAbsoluteSD", LL_ERROR);
			LocalFree(pSDCNV);
			GlobalFree(pDACL);
			GlobalFree(pSACL);
			return false;
		  }
	 }


	 bool ret=true;

	 DWORD rc = SetNamedSecurityInfoA(const_cast<char*>(filename.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL, pDACL, NULL);
	 if(rc!=ERROR_SUCCESS)
	 {
		 Server->Log("Error setting security information. rc: "+nconvert((int)rc), LL_ERROR);
		 ret=false;
	 }

	 GlobalFree(pDACL);
	 GlobalFree(pSACL);

	 LocalFree(pSDCNV);

	 return ret;
}

bool write_pw_change_file()
{
	SECURITY_ATTRIBUTES  sa;      
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = FALSE;


     BOOL b=ConvertStringSecurityDescriptorToSecurityDescriptor(
                szSD,
                SDDL_REVISION_1,
				&(sa.lpSecurityDescriptor),
                NULL);

	 if(!b)
	 {
		 Server->Log("Error creating security descriptor", LL_ERROR);
		 return false;
	 }

	 HANDLE file = CreateFileA(pw_change_file.c_str(),
		 GENERIC_READ | GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, 0, NULL);

	 if(file==INVALID_HANDLE_VALUE)
	 {
		 Server->Log("Error creating pw file", LL_ERROR);
		 LocalFree(sa.lpSecurityDescriptor);
		 return false;
	 }

	 std::string pw = wnarrow(Server->getSessionMgr()->GenerateSessionIDWithUser(L"",L""));

	 DWORD written=0;
	 while(written<pw.size())
	 {
		b = WriteFile(file, pw.data()+written, static_cast<DWORD>(pw.size())-written, &written, NULL);
		if(!b)
		{
			Server->Log("Error writing to pw file", LL_ERROR);
			CloseHandle(file);
			LocalFree(sa.lpSecurityDescriptor);
			return false;
		}
	 }

	 CloseHandle(file);
	 LocalFree(sa.lpSecurityDescriptor);
	 return true;
}

#else

bool write_pw_change_file()
{
	int fd = open(pw_change_file.c_str(), O_CREAT, S_IRWXU);

	if(fd==-1)
	{
		Server->Log("Error opening pw file", LL_ERROR);
		return false;
	}

	std::string pw = wnarrow(Server->getSessionMgr()->GenerateSessionIDWithUser(L"",L""));

	ssize_t rc = write(fd, pw.data(), pw.size());

	if(rc<pw.size())
	{
		Server->Log("Error writing to pw file", LL_ERROR);
		close(fd);
		return false;
	}
	
	close(fd);
	return true;
}

bool change_file_permissions(const std::string& filename)
{
	if(chmod(filename.c_str(), S_IRWXU)!=0)
	{
		Server->Log("Error setting file permissions", LL_ERROR);
		return false;
	}
	return true;
}

#endif

}

DLLEXPORT void LoadActions(IServer* pServer)
{
	Server=pServer;
	
	std::string rmtest=Server->getServerParameter("rmtest");
	if(!rmtest.empty())
	{
		os_remove_nonempty_dir(widen(rmtest));
		return;
	}

#ifdef _WIN32
	char t_lang[20];
	GetLocaleInfoA(LOCALE_SYSTEM_DEFAULT,LOCALE_SISO639LANGNAME ,t_lang,sizeof(t_lang));
	lang=t_lang;
#endif

	if(lang=="de")
	{
		time_format_str=time_format_str_de;
	}

	//writeZeroblockdata();

	if(Server->getServerParameter("restore_mode")=="true")
	{
		Server->setServerParameter("max_worker_clients", "1");
	}
	if(Server->getServerParameter("restore")=="true")
	{
		do_restore();
		exit(10);
		return;
	}
	if(Server->getServerParameter("restore_wizard")=="true")
	{
		restore_wizard();
		exit(10);
		return;
	}


	{
		str_map params;
		image_fak=(IFSImageFactory *)Server->getPlugin(Server->getThreadID(), Server->StartPlugin("fsimageplugin", params));
		if( image_fak==NULL )
		{
			Server->Log("Error loading fsimageplugin", LL_ERROR);
		}
	}



	ServerIdentityMgr::init_mutex();
#ifdef _WIN32
	DirectoryWatcherThread::init_mutex();
#endif

	if(getFile(pw_file).size()<5)
	{
		writestring(wnarrow(Server->getSessionMgr()->GenerateSessionIDWithUser(L"",L"")), pw_file);
	}
	if(getFile(pw_change_file).size()<5)
	{
		write_pw_change_file();
	}

	if( !FileExists("urbackup/backup_client.db") && FileExists("urbackup/backup_client.db.template") )
	{
		//Copy file
		copy_file(L"urbackup/backup_client.db.template", L"urbackup/backup_client.db");
	}

	change_file_permissions("urbackup/backup_client.db");

	if(! Server->openDatabase("urbackup/backup_client.db", URBACKUPDB_CLIENT) )
	{
		Server->Log("Couldn't open Database backup_client.db", LL_ERROR);
		return;
	}

#ifdef _WIN32
	if( !FileExists("prefilebackup.bat") && FileExists("prefilebackup_new.bat") )
	{
		copy_file(L"prefilebackup_new.bat", L"prefilebackup.bat");
		Server->deleteFile("prefilebackup_new.bat");
	}
#endif

	if( !FileExists("urbackup/data/settings.cfg") && FileExists("initial_settings.cfg") )
	{
		copy_file(L"initial_settings.cfg", L"urbackup/data/settings.cfg");
		Server->deleteFile("initial_settings.cfg");
	}

	if(FileExists("urbackup/data/settings.cfg"))
	{
		change_file_permissions("urbackup/data/settings.cfg");
	}

	if(FileExists("urbackup/data/filelist.ub"))
	{
		change_file_permissions("urbackup/data/filelist.ub");
	}

	change_file_permissions("urbackup");
	change_file_permissions("urbackup/data");

#ifndef _DEBUG
	if(FileExists(new_file) )
#endif
	{
		Server->Log("Upgrading...", LL_WARNING);
		Server->deleteFile(new_file);
		if(!upgrade_client())
		{
			IDatabase *db=Server->getDatabase(Server->getThreadID(), URBACKUPDB_CLIENT);
			db->Write("DELETE FROM files");			
			db->Write("CREATE TABLE IF NOT EXISTS logdata (id INTEGER PRIMARY KEY,logid INTEGER,loglevel INTEGER,message TEXT,idx INTEGER);");
			db->Write("CREATE TABLE IF NOT EXISTS  logs ( id INTEGER PRIMARY KEY, ttime DATE DEFAULT CURRENT_TIMESTAMP);");
			db->Write("CREATE TABLE IF NOT EXISTS shadowcopies ( id INTEGER PRIMARY KEY, vssid BLOB, ssetid BLOB, target TEXT, path TEXT);");
			db->Write("CREATE TABLE IF NOT EXISTS mdirs_backup ( name TEXT );");
			db->Write("ALTER TABLE shadowcopies ADD tname TEXT;");
			db->Write("ALTER TABLE shadowcopies ADD orig_target TEXT;");
			db->Write("ALTER TABLE shadowcopies ADD filesrv INTEGER;");
			db->Write("CREATE TABLE IF NOT EXISTS journal_ids ( id INTEGER PRIMARY KEY, device_name TEXT, journal_id INTEGER, last_record INTEGER);");
			db->Write("ALTER TABLE journal_ids ADD index_done INTEGER;");
			db->Write("UPDATE journal_ids SET index_done=0 WHERE index_done IS NULL");
			db->Write("CREATE TABLE IF NOT EXISTS map_frn ( id INTEGER PRIMARY KEY, name TEXT, pid INTEGER, frn INTEGER, rid INTEGER)");
			db->Write("CREATE INDEX IF NOT EXISTS frn_index ON map_frn( frn ASC )");
			db->Write("CREATE INDEX IF NOT EXISTS frn_pid_index ON map_frn( pid ASC )");
			db->Write("CREATE TABLE IF NOT EXISTS journal_data ( id INTEGER PRIMARY KEY, device_name TEXT, journal_id INTEGER, usn INTEGER, reason INTEGER, filename TEXT, frn INTEGER, parent_frn INTEGER, next_usn INTEGER)");
			db->Write("DELETE FROM journal_ids");
			db->Write("DELETE FROM journal_data");
			db->Write("DELETE FROM map_frn");
			db->Write("CREATE INDEX IF NOT EXISTS logdata_index ON logdata( logid ASC )");
			db->Write("ALTER TABLE logdata ADD ltime DATE;");
			db->Write("CREATE TABLE IF NOT EXISTS del_dirs ( name TEXT );");
			db->Write("CREATE TABLE IF NOT EXISTS del_dirs_backup ( name TEXT );");
			db->Write("ALTER TABLE journal_data ADD attributes INTEGER;");
			db->Write("ALTER TABLE backupdirs ADD server_default INTEGER;");
			db->Write("UPDATE backupdirs SET server_default=0 WHERE server_default IS NULL");
			db->Write("CREATE TABLE IF NOT EXISTS misc (tkey TEXT, tvalue TEXT);");
			db->Write("INSERT INTO misc (tkey, tvalue) VALUES ('db_version', '1');");
			upgrade_client();
		}
	}

	bool do_leak_check=(Server->getServerParameter("leak_check")=="true");

	ClientConnector::init_mutex();
	Server->StartCustomStreamService(new ClientService(), "urbackupserver", serviceport);

	str_map params;
	filesrv_pluginid=Server->StartPlugin("fileserv", params);

	crypto_fak=(ICryptoFactory *)Server->getPlugin(Server->getThreadID(), Server->StartPlugin("cryptoplugin", params));
	if( crypto_fak==NULL )
	{
		Server->Log("Error loading Cryptoplugin", LL_ERROR);
	}

	IndexThread *it=new IndexThread();
	if(!do_leak_check)
	{
		Server->createThread(it);
	}
	else
	{
		indexthread_ticket=Server->getThreadPool()->execute(it);
	}

	internetclient_ticket=InternetClient::start(do_leak_check);

	Server->Log("Started UrBackupClient Backend...", LL_INFO);
	Server->wait(1000);	
}

DLLEXPORT void UnloadActions(void)
{
	if(Server->getServerParameter("leak_check")=="true")
	{
		IndexThread::doStop();
		Server->getThreadPool()->waitFor(indexthread_ticket);
		ServerIdentityMgr::destroy_mutex();

		InternetClient::stop(internetclient_ticket);

		ClientConnector::destroy_mutex();

		Server->destroyAllDatabases();
	}
}

void upgrade_client1_2(IDatabase *db)
{
	db->Write("ALTER TABLE shadowcopies ADD vol TEXT");
}

void upgrade_client2_3(IDatabase *db)
{
	db->Write("ALTER TABLE shadowcopies ADD refs INTEGER");
	db->Write("ALTER TABLE shadowcopies ADD starttime DATE");
}

void upgrade_client3_4(IDatabase *db)
{
	db->Write("ALTER TABLE shadowcopies ADD starttoken TEXT");
}

void upgrade_client4_5(IDatabase *db)
{
	db->Write("DROP TABLE mdirs");
	db->Write("CREATE TABLE mdirs ( id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT )");
	db->Write("DROP TABLE mdirs_backup");
	db->Write("CREATE TABLE mdirs_backup ( id INTEGER, name TEXT )");
	db->Write("CREATE TABLE mfiles ( dir_id INTEGER, name TEXT );");
	db->Write("CREATE TABLE mfiles_backup ( dir_id INTEGER, name TEXT );");
	db->Write("CREATE INDEX IF NOT EXISTS mfiles_backup_idx ON mfiles_backup( dir_id ASC )");
	db->Write("DELETE FROM files");	
}

void upgrade_client5_6(IDatabase *db)
{
	db->Write("DELETE FROM files");
}

void upgrade_client6_7(IDatabase *db)
{
	db->Write("DELETE FROM files");
}

void upgrade_client7_8(IDatabase *db)
{
	db->Write("DELETE FROM files");
}

void upgrade_client8_9(IDatabase *db)
{
	db->Write("DELETE FROM files");
}

void upgrade_client9_10(IDatabase *db)
{
	db->Write("CREATE TABLE filehashes (name TEXT, filesize INTEGER, modifytime INTEGER, hashdata BLOB)");
	db->Write("CREATE UNIQUE INDEX filehashes_idx ON filehashes (name ASC)");
}

void update_client10_11(IDatabase *db)
{
	db->Write("DROP TABLE filehashes");
	db->Write("DROP INDEX filehashes_idx");
	db->Write("DELETE FROM files");
}

bool upgrade_client(void)
{
	IDatabase *db=Server->getDatabase(Server->getThreadID(), URBACKUPDB_CLIENT);
	IQuery *q=db->Prepare("SELECT tvalue FROM misc WHERE tkey='db_version'");
	if(q==NULL)
		return false;
	db_results res_v=q->Read();
	if(res_v.empty())
		return false;
	
	int ver=watoi(res_v[0][L"tvalue"]);
	int old_v;
	
	IQuery *q_update=db->Prepare("UPDATE misc SET tvalue=? WHERE tkey='db_version'");
	do
	{
		db->BeginTransaction();

		old_v=ver;
		switch(ver)
		{
			case 1:
				upgrade_client1_2(db);
				++ver;
				break;
			case 2:
				upgrade_client2_3(db);
				++ver;
				break;
			case 3:
				upgrade_client3_4(db);
				++ver;
				break;
			case 4:
				upgrade_client4_5(db);
				++ver;
				break;
			case 5:
				upgrade_client5_6(db);
				++ver;
				break;
			case 6:
				upgrade_client6_7(db);
				++ver;
				break;
			case 7:
				upgrade_client7_8(db);
				++ver;
				break;
			case 8:
				upgrade_client8_9(db);
				++ver;
				break;
			case 9:
				upgrade_client9_10(db);
				++ver;
				break;
			case 10:
				update_client10_11(db);
				++ver;
				break;
			default:
				break;
		}
		
		if(ver!=old_v)
		{
			q_update->Bind(ver);
			q_update->Write();
			q_update->Reset();
		}

		db->EndTransaction();
	}
	while(old_v<ver);
	
	db->destroyAllQueries();
	return true;
}
