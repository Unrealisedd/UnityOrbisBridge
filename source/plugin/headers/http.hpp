#pragma once

#include <string>

size_t HeaderCallback(void *ptr, size_t size, size_t nmemb, void *data);
void BeginDownload(const char *url, const char *pathWithFile);
char *DownloadAsBytesThread(const char *url, size_t *outSize);
char *DownloadAsBytesRangeThread(const char *url, uint32_t offset, uint32_t size, size_t *outSize);
int ResolveRedirects(const char *url, std::string &resolvedUrl);
const char *FollowRedirects(const char *url);

extern "C"
{
    enum DirectDownloadStateValue
    {
        DIRECT_DOWNLOAD_IDLE = 0,
        DIRECT_DOWNLOAD_RUNNING = 1,
        DIRECT_DOWNLOAD_SUCCEEDED = 2,
        DIRECT_DOWNLOAD_FAILED = 3,
        DIRECT_DOWNLOAD_CANCELLED = 4
    };

    int StartDirectDownload(const char *url, const char *finalPath);
    int GetDirectDownloadState();
    int GetDirectDownloadErrorCode();
    int GetDirectDownloadHttpStatus();
    uint64_t GetDirectDownloadTotalBytes();
    uint64_t GetDirectDownloadCurrentBytes();
    uint64_t GetDirectDownloadSpeedBytesPerSecond();
    void CancelDirectDownload();
    int ResetDirectDownload();
    int ConfigureHttp(const char *caBundlePath);

    char *GetDownloadInfo(const char *info);
    bool HasDownloadCompleted();
    bool HasDownloadErrorOccured();
    void ResetDownloadVars();
    void CancelDownload();
    void DownloadWebFile(const char *url, const char *pathWithFile, bool bgDL, const char *name);
    char *DownloadAsBytes(const char *url, size_t *outSize);
    char *DownloadAsBytesRange(const char *url, uint32_t offset, uint32_t size, size_t *outSize);
}
