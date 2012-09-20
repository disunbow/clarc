#ifndef _FILELISTSTORAGE_H
#define _FILELISTSTORAGE_H

#include <iostream>
using namespace std;

#include "sqlite3.h"
#include "remoteListOfFiles.h"
#include "localFileList.h"

#define STORAGE_FAILED 0
#define STORAGE_SUCCESS 1

class FileListStorage
{
private:
	sqlite3 *sqlite;
	int createTable();
	int putDbVersion();
	int storeRemoteListOfFilesChunk(RemoteListOfFiles *remoteListOfFiles, uint32_t from, uint32_t to);

public:
	FileListStorage(char *path, char *errorResult);
	~FileListStorage();

	int lookup(char *remotePath, char *md5, uint64_t *mtime);
	int store(char *remotePath, char *md5, uint64_t mtime);
	int storeRemoteListOfFiles(RemoteListOfFiles *remoteListOfFiles);
	int truncate();
	LocalFileList * calculateListOfFilesToDelete(LocalFileList *localFileList);
	int storeDeletedBatch(char **batch, uint32_t batchCount);
};

#endif
