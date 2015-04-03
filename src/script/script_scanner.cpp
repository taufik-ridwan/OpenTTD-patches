/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file script_scanner.cpp Allows scanning for scripts. */

#include "../stdafx.h"
#include "../debug.h"
#include "../string.h"
#include "../settings_type.h"

#include "../script/squirrel.hpp"
#include "script_scanner.hpp"
#include "script_info.hpp"

bool ScriptScanner::AddFile(const char *filename, size_t basepath_length, const char *tar_filename)
{
	free(this->main_script);

	const char *sep = strrchr (filename, PATHSEPCHAR);
	if (sep == NULL) {
		this->main_script = xstrdup ("main.nut");
	} else {
		this->main_script = str_fmt ("%.*smain.nut", (int)(sep - filename + 1), filename);
	}
	if (this->main_script == NULL) return false;

	free(this->tar_file);
	if (tar_filename != NULL) {
		this->tar_file = xstrdup(tar_filename);
	} else {
		this->tar_file = NULL;
	}

	if (!FioCheckFileExists(filename, this->subdir) || !FioCheckFileExists(this->main_script, this->subdir)) return false;

	this->ResetEngine();
	this->engine->LoadScript(filename);

	return true;
}

ScriptScanner::ScriptScanner() :
	engine(NULL),
	main_script(NULL),
	tar_file(NULL)
{
}

void ScriptScanner::ResetEngine()
{
	this->engine->Reset();
	this->engine->SetGlobalPointer(this);
	this->RegisterAPI(this->engine);
}

void ScriptScanner::Initialize(const char *name)
{
	this->engine = new Squirrel(name);

	this->RescanDir();

	this->ResetEngine();
}

ScriptScanner::~ScriptScanner()
{
	this->Reset();

	free(this->main_script);
	free(this->tar_file);
	delete this->engine;
}

void ScriptScanner::RescanDir()
{
	/* Forget about older scans */
	this->Reset();

	/* Scan for scripts */
	this->Scan(this->GetFileName(), this->GetDirectory());
}

void ScriptScanner::Reset()
{
	ScriptInfoList::iterator it = this->info_list.begin();
	for (; it != this->info_list.end(); it++) {
		free((*it).first);
		delete (*it).second;
	}
	it = this->info_single_list.begin();
	for (; it != this->info_single_list.end(); it++) {
		free((*it).first);
	}

	this->info_list.clear();
	this->info_single_list.clear();
}

void ScriptScanner::RegisterScript (ScriptInfo *info, const char *name, bool dev_only)
{
	sstring<1024> script_name;
	script_name.copy (name);
	script_name.tolower();
	size_t original_length = script_name.length();
	script_name.append_fmt (".%d", info->GetVersion());

	/* Check if GetShortName follows the rules */
	if (strlen(info->GetShortName()) != 4) {
		DEBUG(script, 0, "The script '%s' returned a string from GetShortName() which is not four characaters. Unable to load the script.", info->GetName());
		delete info;
		return;
	}

	ScriptInfoList::iterator iter = this->info_list.find (script_name.c_str());
	if (iter != this->info_list.end()) {
		/* This script was already registered */
		const char *old_main = iter->second->GetMainScript();
		const char *new_main = info->GetMainScript();
#ifdef WIN32
		/* Windows doesn't care about the case */
		if (strcasecmp (old_main, new_main) != 0) {
#else
		if (strcmp (old_main, new_main) != 0) {
#endif
			DEBUG(script, 1, "Registering two scripts with the same name and version");
			DEBUG(script, 1, "  1: %s", old_main);
			DEBUG(script, 1, "  2: %s", new_main);
			DEBUG(script, 1, "The first is taking precedence.");
		}

		delete info;
		return;
	}

	this->info_list[xstrdup(script_name.c_str())] = info;

	script_name.truncate (original_length);

	if (!dev_only || _settings_client.gui.ai_developer_tools) {
		/* Add the script to the 'unique' script list, where only the highest version
		 *  of the script is registered. */
		ScriptInfoList::iterator iter = this->info_single_list.find (script_name.c_str());
		if (iter == this->info_single_list.end()) {
			this->info_single_list[xstrdup(script_name.c_str())] = info;
		} else if (iter->second->GetVersion() < info->GetVersion()) {
			iter->second = info;
		}
	}
}

void ScriptScanner::GetConsoleList (stringb *buf, bool newest_only) const
{
	buf->append_fmt ("List of %s:\n", this->GetScannerName());
	const ScriptInfoList &list = newest_only ? this->info_single_list : this->info_list;
	ScriptInfoList::const_iterator it = list.begin();
	for (; it != list.end(); it++) {
		ScriptInfo *i = (*it).second;
		buf->append_fmt ("%10s (v%d): %s\n", i->GetName(), i->GetVersion(), i->GetDescription());
	}
	buf->append ('\n');
}

#if defined(ENABLE_NETWORK)
#include "../network/network_content.h"
#include "../3rdparty/md5/md5.h"
#include "../tar_type.h"

/** Helper for creating a MD5sum of all files within of a script. */
struct ScriptFileChecksumCreator : FileScanner {
	byte md5sum[16];  ///< The final md5sum.
	Subdirectory dir; ///< The directory to look in.

	/**
	 * Initialise the md5sum to be all zeroes,
	 * so we can easily xor the data.
	 */
	ScriptFileChecksumCreator(Subdirectory dir)
	{
		this->dir = dir;
		memset(this->md5sum, 0, sizeof(this->md5sum));
	}

	/* Add the file and calculate the md5 sum. */
	virtual bool AddFile(const char *filename, size_t basepath_length, const char *tar_filename)
	{
		Md5 checksum;
		uint8 buffer[1024];
		size_t len, size;
		byte tmp_md5sum[16];

		/* Open the file ... */
		FILE *f = FioFOpenFile(filename, "rb", this->dir, &size);
		if (f == NULL) return false;

		/* ... calculate md5sum... */
		while ((len = fread(buffer, 1, (size > sizeof(buffer)) ? sizeof(buffer) : size, f)) != 0 && size != 0) {
			size -= len;
			checksum.Append(buffer, len);
		}
		checksum.Finish(tmp_md5sum);

		FioFCloseFile(f);

		/* ... and xor it to the overall md5sum. */
		for (uint i = 0; i < sizeof(md5sum); i++) this->md5sum[i] ^= tmp_md5sum[i];

		return true;
	}
};

/**
 * Check whether the script given in info is the same as in ci based
 * on the shortname and md5 sum.
 * @param ci The information to compare to.
 * @param md5sum Whether to check the MD5 checksum.
 * @param info The script to get the shortname and md5 sum from.
 * @return True iff they're the same.
 */
static bool IsSameScript(const ContentInfo *ci, bool md5sum, ScriptInfo *info, Subdirectory dir)
{
	uint32 id = 0;
	const char *str = info->GetShortName();
	for (int j = 0; j < 4 && *str != '\0'; j++, str++) id |= *str << (8 * j);

	if (id != ci->unique_id) return false;
	if (!md5sum) return true;

	ScriptFileChecksumCreator checksum(dir);
	const char *tar_filename = info->GetTarFile();
	TarList::iterator iter;
	if (tar_filename != NULL && (iter = TarCache::cache[dir].tars.find(tar_filename)) != TarCache::cache[dir].tars.end()) {
		/* The main script is in a tar file, so find all files that
		 * are in the same tar and add them to the MD5 checksumming. */
		TarFileList::iterator tar;
		FOR_ALL_TARS(tar, dir) {
			/* Not in the same tar. */
			if (tar->second.tar_filename != iter->first) continue;

			/* Check the extension. */
			const char *ext = strrchr(tar->first.c_str(), '.');
			if (ext == NULL || strcasecmp(ext, ".nut") != 0) continue;

			checksum.AddFile(tar->first.c_str(), 0, tar_filename);
		}
	} else {
		char path[MAX_PATH];
		bstrcpy (path, info->GetMainScript());
		/* There'll always be at least 1 path separator character in a script
		 * main script name as the search algorithm requires the main script to
		 * be in a subdirectory of the script directory; so <dir>/<path>/main.nut. */
		*strrchr(path, PATHSEPCHAR) = '\0';
		checksum.Scan(".nut", path);
	}

	return memcmp(ci->md5sum, checksum.md5sum, sizeof(ci->md5sum)) == 0;
}

ScriptInfo *ScriptScanner::FindScript (const ContentInfo *ci, bool md5sum)
{
	for (ScriptInfoList::iterator it = this->info_list.begin(); it != this->info_list.end(); it++) {
		if (IsSameScript (ci, md5sum, it->second, this->GetDirectory())) return it->second;
	}
	return NULL;
}

bool ScriptScanner::HasScript(const ContentInfo *ci, bool md5sum)
{
	return this->FindScript (ci, md5sum) != NULL;
}

const char *ScriptScanner::FindMainScript(const ContentInfo *ci, bool md5sum)
{
	ScriptInfo *info = this->FindScript (ci, md5sum);
	return (info != NULL) ? info->GetMainScript() : NULL;
}

#endif /* ENABLE_NETWORK */
