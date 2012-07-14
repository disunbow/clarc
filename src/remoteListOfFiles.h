#ifndef _REMOTELISTOFFILES_H
#define _REMOTELISTOFFILES_H

#include <iostream>
using namespace std;

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include "amazonCredentials.h"

class RemoteListOfFiles
{
private:
	AmazonCredentials *amazonCredentials;
	uint32_t allocCount;

	char *extractMd5FromEtag(char *etag);
	void extractMtimeFromHeaders(char *headers, uint32_t *mtime);

	int parseListOfFiles(char *body, uint64_t bodySize, uint8_t *isTruncated, char *lastKey, char *errorResult);
	int performGetOnBucket(char *marker, int setLocationHeader, char *body, uint64_t *bodySize, uint32_t *statusCode, char *errorResult);
	int performHeadOnFile(char *remotePath, uint32_t *remoteMtime, uint32_t *statusCode, char *errorResult);

public:
	RemoteListOfFiles(AmazonCredentials *amazonCredentials);
	~RemoteListOfFiles();
	
	void add(char *path, char *md5);
	int downloadList();
	int resolveMtimes();

	int checkAuth();

	char **paths;
	char **md5s;
	uint32_t *mtimes;
	uint32_t count;
};

#define LIST_FAILED 0
#define LIST_SUCCESS 1

#define HEAD_FAILED 0
#define HEAD_SUCCESS 1

#define AUTH_FAILED 0
#define AUTH_FAILED_BUCKET_DOESNT_EXISTS -1
#define AUTH_SUCCESS 1

int checkAuth(AmazonCredentials *amazonCredentials);

#endif
