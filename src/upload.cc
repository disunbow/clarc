#include <iostream>
using namespace std;

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <errno.h>
#include <sqlite3.h>

extern "C" {
#include "utils.h"
#include "base64.h"
#include "curlResponse.h"
#include <pwd.h>
#include <grp.h>
#include "logger.h"
	
// this is declared explicitly instead of including unistd.h because some
// linux distributions have clang broken with it. Fortunately, sleep() is 
// the same on all sane OS.
unsigned int sleep(unsigned int seconds);
}

#include "localFileList.h"
#include "fileListStorage.h"
#include "upload.h"
#include "settings.h"
#include "amazonCredentials.h"
#include "amzHeaders.h"


pthread_mutex_t uidMutex;

struct UploadProgress {
	char *path;
	double ullast;
	Uploader *uploader;
};

int progressFunction(struct UploadProgress *uploadProgress, double dltotal, double dlnow, double ultotal, double ulnow) {
	double uploadedBytes = ulnow - uploadProgress->ullast;
	if (uploadedBytes>0) {
		uploadProgress->ullast = ulnow;
		uploadProgress->uploader->progress(uploadProgress->path, uploadedBytes, ulnow, ultotal);
	}
	return 0;
}

void Uploader::progress(char *path, double uploadedBytes, double ulnow, double ultotal) {
	this->uploadedSize+=(uint64_t) uploadedBytes;
	if (this->showProgress) { 
		time_t timeDiff = time(NULL) - this->lastProgressUpdate;
		if (timeDiff>=1) {
			double percent = (double)this->uploadedSize / (double)this->totalSize;
			char *uploadedSizeHr = hrSize(this->uploadedSize);
			char *totalSizeHr = hrSize(this->totalSize);
			printf("\r[Upload] %.1f%% Done (%s out of %s)           \r", percent*100, uploadedSizeHr, totalSizeHr);
			fflush(stdout);
			free(uploadedSizeHr);
			free(totalSizeHr);
			this->lastProgressUpdate = time(NULL);
		}
	}
}

Uploader::Uploader(AmazonCredentials *amazonCredentials, FilePattern *excludeFilePattern) {
	this->amazonCredentials = amazonCredentials;
	this->excludeFilePattern = excludeFilePattern;
	this->totalSize=0;
	this->uploadedSize=0;
	this->lastProgressUpdate = 0;
	this->useRrs = 0;
	this->makeAllPublic = 0;
	this->useSsl = 1;
	this->dryRun = 0;
	this->connectTimeout = CONNECT_TIMEOUT;
	this->networkTimeout = LOW_SPEED_TIME;
	this->uploadThreads = UPLOAD_THREADS;
	this->failed=0;
	this->showProgress=0;
	this->threads = NULL;

	pthread_mutex_init(&uidMutex, NULL);
}

Uploader::~Uploader() {
	this->amazonCredentials = NULL;
	this->excludeFilePattern = NULL;

	pthread_mutex_destroy(&uidMutex);
}


void Uploader::extractMD5FromETagHeaders(char *headers, char *md5) {
	*md5=0;
	if (!headers) {
		return;
	}

	if (strlen(headers)<7+32) {
		return;
	}

	char *etag = strstr(headers, "ETag: ");
	if (etag) {
		char *pos = (etag+6);
		if (strlen(pos)>=34 && *pos==0x22 && *(pos+33)==0x22) { // double quotes
			strncpy(md5, (pos+1), 32);
			*(md5+32)=0;
		}
	}
}

void Uploader::addUidAndGidHeaders(uid_t uid, gid_t gid, AmzHeaders *amzHeaders) {
	pthread_mutex_lock(&uidMutex);
	struct passwd *uidS = getpwuid(uid);
	struct group *gidS = getgrgid(gid);

	char *uidHeader;
	char *gidHeader;
	asprintf(&uidHeader, (char *) "%" PRIu64 " %s", (uint64_t) uid, uidS->pw_name);
	asprintf(&gidHeader, (char *) "%" PRIu64 " %s", (uint64_t) gid, gidS->gr_name);

	pthread_mutex_unlock(&uidMutex);

	amzHeaders->add((char *) "x-amz-meta-uid", uidHeader);
	amzHeaders->add((char *) "x-amz-meta-gid", gidHeader);
	free(gidHeader);
	free(uidHeader);	
}

CURLcode Uploader::uploadFile(
	char *localPath, 
	char *remotePath, 
	char *url,
	char *contentType, 
	struct stat *fileInfo, 	
	uint32_t *httpStatusCode, 
	char *md5,
	char *errorResult
) {
	char method[4] = "PUT";

	FILE *fin = fopen(localPath, "rb");
	if (!fin) {
		sprintf(errorResult, "Cannot open file %s: %s", localPath, strerror(errno));
		return UPLOAD_FILE_FUNCTION_FAILED;
	}

	char *date = getIsoDate();

	CURL *curl = curl_easy_init();
	char *escapedRemotePath=curl_easy_escape(curl, remotePath, 0);

	AmzHeaders *amzHeaders = new AmzHeaders();

	Uploader::addUidAndGidHeaders(fileInfo->st_uid, fileInfo->st_gid, amzHeaders);

	amzHeaders->add((char *) "x-amz-acl", this->makeAllPublic ? (char *) "public-read" : (char *) "private");

	if (this->useRrs) {
		amzHeaders->add((char *) "x-amz-storage-class",(char *) "REDUCED_REDUNDANCY");
	}

	amzHeaders->add((char *) "x-amz-meta-mode", (char *) "%o", fileInfo->st_mode);
	amzHeaders->add((char *) "x-amz-meta-mtime", (char *) "%llu", (uint64_t) fileInfo->st_mtime);

	char *amzHeadersToSign = amzHeaders->serializeIntoStringToSign();

	char *canonicalizedResource;
	asprintf(&canonicalizedResource, "/%s/%s", amazonCredentials->bucket, escapedRemotePath);
	
	char *stringToSign = (char *)malloc(strlen(canonicalizedResource) + strlen(amzHeadersToSign) + 1024); // 1k is enough to hold other headers
	sprintf(stringToSign, "%s\n%s\n%s\n%s\n", method, "", contentType, date);
	strcat(stringToSign, amzHeadersToSign);
	strcat(stringToSign, canonicalizedResource);

	char *authorization = amazonCredentials->createAuthorizationHeader(stringToSign); 

	free(stringToSign);
	free(amzHeadersToSign);
	free(canonicalizedResource);

	if (authorization==NULL) {
		free(date);
		curl_easy_cleanup(curl);
		free(escapedRemotePath);
		strcpy(errorResult, "Error in auth");
		return UPLOAD_FILE_FUNCTION_FAILED;
	}

	char *postUrl;
	if (url) {
		postUrl = strdup(url);
	} else { 
		postUrl = amazonCredentials->generateUrl(escapedRemotePath, this->useSsl); 
	}
	free(escapedRemotePath);

	struct CurlResponse curlResponse;
	CurlResponseInit(&curlResponse);

	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
	curl_easy_setopt(curl, CURLOPT_PUT, 1L);
	//curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);

	curl_easy_setopt(curl, CURLOPT_URL, postUrl);
	free(postUrl);

	curl_easy_setopt(curl, CURLOPT_READDATA, fin);

	struct curl_slist *slist = NULL;

	slist = AmzHeaders::addHeader(slist, "Date", date);
	free(date);

	slist = AmzHeaders::addHeader(slist, "Authorization", authorization);
	free(authorization);

	slist = AmzHeaders::addHeader(slist, "Content-type", contentType);

	slist = curl_slist_append(slist, "Expect:");

	slist = amzHeaders->serializeIntoCurl(slist);
	delete amzHeaders;

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, this->connectTimeout);
	curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, MAXCONNECTS);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, this->networkTimeout);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, LOW_SPEED_LIMIT);

	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&curlResponse);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlResponseBodyCallback);

	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &CurlResponseHeadersCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEHEADER, (void *)&curlResponse); 

	struct UploadProgress *uploadProgress = (struct UploadProgress *)malloc(sizeof(struct UploadProgress));
	uploadProgress->path = remotePath;
	uploadProgress->ullast = 0;
	uploadProgress->uploader = this;
	curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, &progressFunction);
	curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, (void *) uploadProgress);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);

	curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t) fileInfo->st_size);

	char *curlErrors = (char *) malloc(CURL_ERROR_SIZE);
	curlErrors[0]=0;
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrors);


	CURLcode res = curl_easy_perform(curl);

	fclose(fin); 

	curl_slist_free_all(slist);
	free(uploadProgress);

	*md5=0;
	*httpStatusCode = 0;

	if (res==CURLE_OK) {
		this->progress(remotePath, 0, (double) fileInfo->st_size, (double) fileInfo->st_size);

		long _httpStatus = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &_httpStatus);
		*httpStatusCode = (uint32_t) _httpStatus;

		if (_httpStatus==307) {
			extractLocationFromHeaders(curlResponse.headers, errorResult);
		} else if (_httpStatus==200) { 
			Uploader::extractMD5FromETagHeaders(curlResponse.headers, md5);
		}

		curl_easy_cleanup(curl);
		free(curlErrors);
		CurlResponseFree(&curlResponse);

		return CURLE_OK;

	} else { 
		strcpy(errorResult, curlErrors);

		curl_easy_cleanup(curl);
		free(curlErrors);
		CurlResponseFree(&curlResponse);

		return res;
	}
}

int Uploader::uploadFileWithRetry(
	char *localPath, 
	char *remotePath, 
	char *contentType, 
	struct stat *fileInfo,
	uint32_t *httpStatusCode, 
	char *md5,
	char *errorResult
) {
	int cUploads=0;
	char *url = NULL;
	do {
		errorResult[0]=0;

		CURLcode res=this->uploadFile(localPath, remotePath, url, contentType, fileInfo, httpStatusCode, md5, errorResult);
		
		if (url!=NULL) {
			free(url);
			url=NULL;
		}

		if (*httpStatusCode==307) {
			url = strdup(errorResult); 
			cUploads=0;
			LOG(LOG_INFO, "[Upload] Retrying %s: Amazon asked to reupload to: %s", remotePath, errorResult);
			continue;
		}

		if (res==CURLE_OK) {
			return UPLOAD_SUCCESS;
		}

		if (HTTP_SHOULD_RETRY_ON(res)) {
			LOG(LOG_WARN, "[Upload] Retrying %s after a short sleep", remotePath);
			int sleepTime = cUploads*RETRY_SLEEP_TIME; 
			sleep(sleepTime);
		} else { 
			return UPLOAD_FAILED;
		}
		cUploads++;
	} while (cUploads<RETRY_FAIL_AFTER); 

	return UPLOAD_FAILED;
}

int Uploader::uploadFileWithRetryAndStore(
	FileListStorage *fileListStorage, 
	char *localPath, 
	char *remotePath, 
	char *contentType, 
	struct stat *fileInfo,
	char *errorResult
) {
	uint32_t httpStatus=0;
	char *remoteMd5 = (char *) malloc(330);
	remoteMd5[0]=0;

	int res = this->uploadFileWithRetry(localPath, remotePath, contentType, fileInfo, &httpStatus, remoteMd5, errorResult);

	if (res!=UPLOAD_SUCCESS) {
		free(remoteMd5);
		return UPLOAD_FAILED;
	}

	if (httpStatus==200) {
		int toReturn=0;
		if (strlen(remoteMd5)==32) {
			int sqlRes = fileListStorage->store(remotePath, remoteMd5, (uint32_t) fileInfo->st_mtime);
			if (sqlRes==STORAGE_SUCCESS) {
				toReturn = UPLOAD_SUCCESS;
			} else {
				sprintf(errorResult, "Oops, database error");
				toReturn = UPLOAD_FAILED;
			}
		} else { 
			sprintf(errorResult, "Amazon did not return MD5");
			toReturn = UPLOAD_FAILED;
		}
		
		free(remoteMd5);
		return toReturn;
		
	} else {
		sprintf(errorResult, "Amazon returned HTTP status %d", httpStatus);
		free(remoteMd5);
		return UPLOAD_FAILED;
	}
}

struct ThreadCommand {
	uint8_t threadNumber;
	Uploader *self;
	FileListStorage *fileListStorage;
	char *realLocalPath;
	char *path;
	char *contentType;
	struct stat *fileInfo;
};

void *uploader_runOverThreadFunc(void *arg) {
	struct ThreadCommand *threadCommand = (struct ThreadCommand *)arg;

	threadCommand->self->runOverThread(
		threadCommand->threadNumber, 
		threadCommand->fileListStorage,
		threadCommand->realLocalPath,
		threadCommand->path,
		threadCommand->contentType,
		threadCommand->fileInfo
	);

	threadCommand->self->threads->markFree(threadCommand->threadNumber);

	free(threadCommand->realLocalPath);
	free(threadCommand->fileInfo);
	free(arg); 

	pthread_exit(NULL);
}

void Uploader::runOverThread(
	uint8_t threadNumber, 
	FileListStorage *fileListStorage, 
	char *realLocalPath, 
	char *path, 
	char *contentType, 
	struct stat *fileInfo
) {
	char errorResult[1024*20];
	errorResult[0]=0;

	int res = this->uploadFileWithRetryAndStore(
		fileListStorage, realLocalPath, path, contentType, 
		fileInfo,
		errorResult
	);
	if (res==UPLOAD_FAILED) {
		LOG(LOG_FATAL, "[Upload] FAIL %s: %s                  ", path, errorResult);
		this->failed=1;
	} else {
		LOG(LOG_INFO, "[Upload] Uploaded %s              ", path);
	}	

	this->threads->markFree(threadNumber);
}

char *Uploader::createRealLocalPath(char *prefix, char *path) {
	uint64_t len = strlen(prefix) + strlen(path) + 2;
	char *realLocalPath = (char *) malloc(len);
	realLocalPath[0]=0;
	strcat(realLocalPath, prefix);
	strcat(realLocalPath, "/");
	strcat(realLocalPath, path);
	return realLocalPath;
}

int Uploader::uploadFiles(FileListStorage *fileListStorage, char *prefix) {
	LocalFileList *files = new LocalFileList(this->excludeFilePattern);
	files->recurseIn((char *) "", prefix);

	this->totalSize = files->calculateTotalSize();
	char *hrSizeString = hrSize(this->totalSize);
	LOG(LOG_INFO, "[Upload] Total size of files: %s", hrSizeString);
	free(hrSizeString);

	this->failed=0;

	this->threads = new Threads(this->uploadThreads);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	for (uint32_t i=0;i<files->count;i++) {
		if (this->failed) {
			break;
		}

		char *path = (files->paths[i]+1);
		char *realLocalPath = Uploader::createRealLocalPath(prefix, path);
		
		struct stat *fileInfo = (struct stat *) malloc(sizeof(struct stat));;

		if (lstat(realLocalPath, fileInfo)<0) {
			LOG(LOG_ERR, "[Upload] FAIL %s: Cannot open file: %s", path, strerror(errno));
			free(realLocalPath);
			free(fileInfo);
			this->uploadedSize+=files->sizes[i];
			continue;
		}
		
		if (fileInfo->st_size==0) {
			LOG(LOG_WARN, "[Upload] WARNING %s: Zero sized file, skipped", path);
			free(realLocalPath);
			free(fileInfo);
			this->uploadedSize+=files->sizes[i];
			continue;
		}
		
		if (fileInfo->st_size>=MAX_S3_FILE_SIZE) {
			LOG(LOG_WARN, "[Upload] WARNING %s: File too large (%" PRIu64 " bytes while only %" PRIu64 " allowed), skipped", 
				path, (uint64_t) fileInfo->st_size, (uint64_t) MAX_S3_FILE_SIZE);
			free(realLocalPath);
			free(fileInfo);
			this->uploadedSize+=files->sizes[i];
			continue;
		}
	
		char md5[33] = "";
		uint64_t mtime=0;
		int res = fileListStorage->lookup(path, md5, &mtime);
		if (res!=STORAGE_SUCCESS) {
			LOG(LOG_FATAL, "[Upload] FAIL %s: Oops, database query failed", path);
			free(realLocalPath);
			free(fileInfo);
			return UPLOAD_FAILED;
		}

		if (res>0 && mtime == (uint64_t) fileInfo->st_mtime) {
			LOG(LOG_DBG, "[Upload] %s not changed", path);
			this->uploadedSize+=files->sizes[i];
			free(fileInfo);
			free(realLocalPath);
			continue;
		}

		if (this->dryRun) {
			LOG(LOG_INFO, "[Upload] [dry] Uploaded %s                  ", path);
			free(fileInfo);
			free(realLocalPath);
		} else { 

#ifndef SINGLETHREAD
			int threadNumber = threads->sleepTillThreadFree();
			threads->markBusy(threadNumber);
#endif
			struct ThreadCommand *threadCommand = (struct ThreadCommand *) malloc(sizeof(struct ThreadCommand));

			threadCommand->self = this;
			threadCommand->fileInfo = fileInfo; 
			threadCommand->contentType = guessContentType(path);
			threadCommand->path = path;
			threadCommand->realLocalPath = realLocalPath;
			threadCommand->fileListStorage = fileListStorage;

#ifdef SINGLETHREAD
			threadCommand->threadNumber = 1; 
			uploader_runOverThreadFunc(threadCommand);
#else 
			threadCommand->threadNumber = threadNumber;

			pthread_t threadId;
			int rc = pthread_create(&threadId, &attr, uploader_runOverThreadFunc, (void *)threadCommand);
			threads->setThreadId(threadNumber, threadId);
			if (rc) {
				LOG(LOG_FATAL, "Return code from pthread_create() is %d, exit", rc);
				exit(-1);
			}
#endif
		}
	}

	threads->sleepTillAllThreadsFree();
	delete this->threads;

	pthread_attr_destroy(&attr);

	LOG(LOG_INFO, "[Upload] Finished %s                     ", this->failed ? "with errors" : "successfully");

	delete files;

	return this->failed ? UPLOAD_FAILED : UPLOAD_SUCCESS;
}

int Uploader::uploadDatabase(char *databasePath, char *databaseFilename) { 
	struct stat fileInfo;
	if (lstat(databasePath, &fileInfo)<0) {
		LOG(LOG_FATAL, "[Upload] Oops database upload failed: file %s doesn't exists", databaseFilename);
		return UPLOAD_FAILED;
	}

	this->showProgress = 0;
	
	char md5[33] = "";
	uint32_t httpStatusCode=0;

	char errorResult[1024*100];
	int res = this->uploadFileWithRetry(databasePath, databaseFilename, 
		(char *)"application/octet-stream", &fileInfo, &httpStatusCode, md5, errorResult);

	if (res==UPLOAD_SUCCESS && httpStatusCode!=200) {
		LOG(LOG_ERR, "[Upload] Database upload failed with HTTP status %d", httpStatusCode);

	} if (res==UPLOAD_SUCCESS && httpStatusCode==200) {
		LOG(LOG_INFO, "[Upload] Database uploaded");

	} else { 
		LOG(LOG_ERR, "[Upload] Database upload failed: %s", errorResult);
	}

	return res;
}
