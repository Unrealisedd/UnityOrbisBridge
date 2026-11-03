#include "../headers/includes.hpp"

#include <cerrno>
#include <cstdlib>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>
#include <strings.h>
#include <sys/stat.h>

namespace
{
enum DirectDownloadState
{
    DirectDownloadIdle = 0,
    DirectDownloadRunning = 1,
    DirectDownloadSucceeded = 2,
    DirectDownloadFailed = 3,
    DirectDownloadCancelled = 4
};

struct TransferResult
{
    int state;
    int error;

    TransferResult(int transferState, int transferError)
        : state(transferState), error(transferError)
    {
    }
};

struct ProgressContext
{
    CURL *curl;
    uint64_t resumeBytes;
    bool foreground;
    bool hasUnsatisfiedRangeTotal;
    uint64_t unsatisfiedRangeTotal;
    int responseStatus;
    bool hasContentRangeStart;
    uint64_t contentRangeStart;
    uint64_t contentRangeEnd;
    uint64_t contentRangeTotal;
    std::string displayName;
};

struct ByteBuffer
{
    std::vector<char> bytes;
};

const size_t MaxByteDownloadSize = 64U * 1024U * 1024U;

std::atomic<int> g_downloadState(DirectDownloadIdle);
std::atomic<int> g_downloadError(CURLE_OK);
std::atomic<int> g_downloadHttpStatus(0);
std::atomic<uint64_t> g_downloadTotalBytes(0);
std::atomic<uint64_t> g_downloadCurrentBytes(0);
std::atomic<uint64_t> g_downloadSpeedBytesPerSecond(0);
std::atomic<bool> g_cancelRequested(false);
std::atomic<bool> g_byteCancelRequested(false);

std::mutex g_transitionMutex;
std::thread g_downloadWorker;
std::mutex g_httpConfigMutex;
std::string g_caBundlePath;
std::mutex g_byteBufferMutex;
std::mutex g_byteTransferMutex;
char *g_byteBuffer = NULL;
size_t g_byteBufferSize = 0;
std::mutex g_infoMutex;
char g_speedText[32] = {0};
char g_downloadedText[32] = {0};
char g_totalText[32] = {0};
char g_progressText[16] = {0};
std::once_flag g_curlInitOnce;
CURLcode g_curlInitResult = CURLE_FAILED_INIT;
bool g_curlInitialized = false;

struct HttpShutdown
{
    ~HttpShutdown()
    {
        g_cancelRequested.store(true);
        g_byteCancelRequested.store(true);
        if (g_downloadWorker.joinable())
            g_downloadWorker.join();
        free(g_byteBuffer);
        if (g_curlInitialized)
            curl_global_cleanup();
    }
};

HttpShutdown g_httpShutdown;

CURLcode InitializeCurl()
{
    std::call_once(g_curlInitOnce, []()
                   {
                       g_curlInitResult = curl_global_init(CURL_GLOBAL_DEFAULT);
                       g_curlInitialized = g_curlInitResult == CURLE_OK;
                   });
    return g_curlInitResult;
}

bool IsHttpUrl(const char *url)
{
    return url && strncasecmp(url, "http://", 7) == 0;
}

bool IsHttpsUrl(const char *url)
{
    return url && strncasecmp(url, "https://", 8) == 0;
}

std::string GetConfiguredCaBundle()
{
    std::lock_guard<std::mutex> lock(g_httpConfigMutex);
    return g_caBundlePath;
}

CURLcode ApplyHttpSecurity(CURL *handle, const char *url, const std::string &caBundle)
{
    if (!handle || (!IsHttpUrl(url) && !IsHttpsUrl(url)))
        return CURLE_UNSUPPORTED_PROTOCOL;

    if (IsHttpsUrl(url) && caBundle.empty())
        return CURLE_SSL_CACERT_BADFILE;

    CURLcode result;
    const long protocols = CURLPROTO_HTTP | CURLPROTO_HTTPS;
    const long redirectProtocols = IsHttpsUrl(url) ? CURLPROTO_HTTPS : CURLPROTO_HTTP;

    result = curl_easy_setopt(handle, CURLOPT_PROTOCOLS, protocols);
    if (result != CURLE_OK)
        return result;
    result = curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS, redirectProtocols);
    if (result != CURLE_OK)
        return result;
    result = curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    if (result != CURLE_OK)
        return result;
    result = curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    if (result != CURLE_OK)
        return result;
    result = curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    if (result != CURLE_OK)
        return result;
    result = curl_easy_setopt(handle, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    if (result != CURLE_OK)
        return result;
    result = curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    if (result != CURLE_OK)
        return result;
    result = curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    if (result != CURLE_OK)
        return result;
    result = curl_easy_setopt(handle, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    if (result != CURLE_OK)
        return result;
    if (!caBundle.empty())
    {
        result = curl_easy_setopt(handle, CURLOPT_CAINFO, caBundle.c_str());
        if (result != CURLE_OK)
            return result;
    }

    return CURLE_OK;
}

uint64_t SaturatingAdd(uint64_t left, uint64_t right)
{
    if (right > std::numeric_limits<uint64_t>::max() - left)
        return std::numeric_limits<uint64_t>::max();
    return left + right;
}

int CalculateProgress(uint64_t current, uint64_t total)
{
    if (total == 0)
        return 0;
    if (current >= total)
        return 100;
    return static_cast<int>((static_cast<long double>(current) * 100.0L) /
                            static_cast<long double>(total));
}

int UpdateDownloadProgress(void *userData, curl_off_t totalSize, curl_off_t downloadedSize,
                           curl_off_t, curl_off_t)
{
    ProgressContext *context = static_cast<ProgressContext *>(userData);
    if (g_cancelRequested.load())
        return 1;

    const uint64_t transferred = downloadedSize > 0 ? static_cast<uint64_t>(downloadedSize) : 0;
    const uint64_t current = SaturatingAdd(context->resumeBytes, transferred);
    const uint64_t total = totalSize > 0
                               ? SaturatingAdd(context->resumeBytes, static_cast<uint64_t>(totalSize))
                               : 0;

    g_downloadCurrentBytes.store(current);
    g_downloadTotalBytes.store(total);

    double speed = 0.0;
    if (curl_easy_getinfo(context->curl, CURLINFO_SPEED_DOWNLOAD, &speed) == CURLE_OK && speed > 0.0)
    {
        const double maximum = static_cast<double>(std::numeric_limits<uint64_t>::max());
        g_downloadSpeedBytesPerSecond.store(speed >= maximum
                                                ? std::numeric_limits<uint64_t>::max()
                                                : static_cast<uint64_t>(speed));
    }
    else
        g_downloadSpeedBytesPerSecond.store(0);

    if (context->foreground)
    {
        const int progress = CalculateProgress(current, total);
        if (context->displayName.empty() || context->displayName == "NULL")
            setProgressMsgText(progress, "Downloading...");
        else
            setProgressMsgText(progress, "Downloading \"%s\"...", context->displayName.c_str());
    }

    return 0;
}

int UpdateByteTransferProgress(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return g_byteCancelRequested.load() ? 1 : 0;
}

size_t DownloadAsBytesCallback(void *ptr, size_t size, size_t count, void *userData)
{
    ByteBuffer *buffer = static_cast<ByteBuffer *>(userData);
    if (g_byteCancelRequested.load())
        return 0;
    if (!buffer || (size != 0 && count > std::numeric_limits<size_t>::max() / size))
        return 0;

    const size_t byteCount = size * count;
    if (byteCount > MaxByteDownloadSize - buffer->bytes.size())
        return 0;

    const size_t oldSize = buffer->bytes.size();
    try
    {
        buffer->bytes.resize(oldSize + byteCount);
    }
    catch (...)
    {
        return 0;
    }

    if (byteCount != 0)
        memcpy(&buffer->bytes[oldSize], ptr, byteCount);
    return byteCount;
}

TransferResult RunDirectTransfer(const std::string &url, const std::string &finalPath,
                                 bool foreground, const std::string &displayName)
{
    const std::string resumePath = finalPath + ".resume";
    FILE *file = fopen(resumePath.c_str(), "a+b");
    if (!file)
        return TransferResult(DirectDownloadFailed, CURLE_WRITE_ERROR);

    if (fseeko(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return TransferResult(DirectDownloadFailed, CURLE_WRITE_ERROR);
    }

    const off_t resumeOffsetValue = ftello(file);
    if (resumeOffsetValue < 0)
    {
        fclose(file);
        return TransferResult(DirectDownloadFailed, CURLE_WRITE_ERROR);
    }

    const uint64_t resumeBytes = static_cast<uint64_t>(resumeOffsetValue);
    g_downloadCurrentBytes.store(resumeBytes);

    const CURLcode initializationResult = InitializeCurl();
    if (initializationResult != CURLE_OK)
    {
        fclose(file);
        return TransferResult(DirectDownloadFailed, initializationResult);
    }
    CURL *handle = curl_easy_init();
    if (!handle)
    {
        fclose(file);
        return TransferResult(DirectDownloadFailed, CURLE_FAILED_INIT);
    }

    const std::string caBundle = GetConfiguredCaBundle();
    const std::string userAgent = "UnityOrbisBridge | FW: " + std::string(GetFWVersion());
    CURLcode result = ApplyHttpSecurity(handle, url.c_str(), caBundle);
    ProgressContext progress = {handle, resumeBytes, foreground, false, 0, 0, false, 0, 0, 0, displayName};

    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_USERAGENT, userAgent.c_str());
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, fwrite);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_WRITEDATA, file);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, UpdateDownloadProgress);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &progress);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 60L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, HeaderCallback);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_HEADERDATA, &progress);
    if (result == CURLE_OK && resumeBytes > 0)
        result = curl_easy_setopt(handle, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(resumeBytes));

    if (result == CURLE_OK)
        result = curl_easy_perform(handle);
    long httpStatus = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &httpStatus);
    if (resumeBytes > 0)
    {
        if (httpStatus == 416)
        {
            const bool complete = progress.hasUnsatisfiedRangeTotal &&
                                  progress.unsatisfiedRangeTotal == resumeBytes;
            result = complete ? CURLE_OK : CURLE_RANGE_ERROR;
            if (complete)
                g_downloadTotalBytes.store(resumeBytes);
        }
        else if (httpStatus == 206)
        {
            const bool completeRange =
                progress.responseStatus == 206 && progress.hasContentRangeStart &&
                progress.contentRangeStart == resumeBytes &&
                progress.contentRangeEnd != std::numeric_limits<uint64_t>::max() &&
                progress.contentRangeEnd + 1 == progress.contentRangeTotal;
            if (!completeRange)
                result = CURLE_RANGE_ERROR;
        }
        else
            result = CURLE_RANGE_ERROR;
    }
    g_downloadHttpStatus.store(httpStatus > INT_MAX ? INT_MAX : static_cast<int>(httpStatus));
    curl_easy_cleanup(handle);

    if (g_cancelRequested.load())
    {
        fclose(file);
        return TransferResult(DirectDownloadCancelled, CURLE_ABORTED_BY_CALLBACK);
    }

    if (result != CURLE_OK)
    {
        fclose(file);
        printAndLogFmt(3, "Download failed with error: %s", curl_easy_strerror(result));
        return TransferResult(DirectDownloadFailed, result);
    }

    const int flushResult = fflush(file);
    if (flushResult != 0)
    {
        fclose(file);
        return TransferResult(DirectDownloadFailed, CURLE_WRITE_ERROR);
    }

    if (resumeBytes > 0 && httpStatus == 206)
    {
        if (fseeko(file, 0, SEEK_END) != 0)
        {
            fclose(file);
            return TransferResult(DirectDownloadFailed, CURLE_WRITE_ERROR);
        }
        const off_t finalLength = ftello(file);
        if (finalLength < 0 ||
            static_cast<uint64_t>(finalLength) != progress.contentRangeTotal)
        {
            fclose(file);
            return TransferResult(DirectDownloadFailed, CURLE_RANGE_ERROR);
        }
    }

    const int closeResult = fclose(file);
    if (closeResult != 0)
        return TransferResult(DirectDownloadFailed, CURLE_WRITE_ERROR);

    if (rename(resumePath.c_str(), finalPath.c_str()) != 0)
    {
        printAndLogFmt(3, "Failed to rename resume file to final filename: %s", strerror(errno));
        return TransferResult(DirectDownloadFailed, CURLE_WRITE_ERROR);
    }

    return TransferResult(DirectDownloadSucceeded, CURLE_OK);
}

void DirectDownloadWorker(std::string url, std::string finalPath, bool foreground,
                          std::string displayName)
{
    if (foreground)
        initiateProgressDialog((char *)"Starting download...");

    const TransferResult result = RunDirectTransfer(url, finalPath, foreground, displayName);

    if (foreground)
        sceMsgDialogTerminate();

    std::lock_guard<std::mutex> lock(g_transitionMutex);
    g_downloadError.store(result.error);
    g_downloadState.store(result.state);
}

int StartDirectDownloadInternal(const char *url, const char *finalPath, bool foreground,
                                const char *displayName)
{
    std::lock_guard<std::mutex> lock(g_transitionMutex);

    if (g_downloadState.load() != DirectDownloadIdle || g_downloadWorker.joinable())
        return EBUSY;

    if (!url || !finalPath || !*finalPath || (!IsHttpUrl(url) && !IsHttpsUrl(url)))
    {
        g_downloadError.store(CURLE_URL_MALFORMAT);
        g_downloadHttpStatus.store(0);
        g_downloadState.store(DirectDownloadFailed);
        return EINVAL;
    }

    g_cancelRequested.store(false);
    g_downloadError.store(CURLE_OK);
    g_downloadHttpStatus.store(0);
    g_downloadTotalBytes.store(0);
    g_downloadCurrentBytes.store(0);
    g_downloadSpeedBytesPerSecond.store(0);
    g_downloadState.store(DirectDownloadRunning);

    try
    {
        g_downloadWorker = std::thread(DirectDownloadWorker, std::string(url), std::string(finalPath),
                                       foreground, displayName ? std::string(displayName) : std::string());
    }
    catch (...)
    {
        g_downloadError.store(CURLE_FAILED_INIT);
        g_downloadState.store(DirectDownloadFailed);
        return EAGAIN;
    }

    return 0;
}


char *DownloadBytesInternal(const char *url, const char *range, size_t *outSize)
{
    if (outSize)
        *outSize = 0;
    if (!url || !outSize || (!IsHttpUrl(url) && !IsHttpsUrl(url)))
        return NULL;

    std::lock_guard<std::mutex> transferLock(g_byteTransferMutex);
    g_byteCancelRequested.store(false);

    if (InitializeCurl() != CURLE_OK)
        return NULL;

    CURL *handle = curl_easy_init();
    if (!handle)
        return NULL;

    ByteBuffer buffer;
    const std::string caBundle = GetConfiguredCaBundle();
    const std::string userAgent = "UnityOrbisBridge | FW: " + std::string(GetFWVersion());
    CURLcode result = ApplyHttpSecurity(handle, url, caBundle);

    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_URL, url);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_USERAGENT, userAgent.c_str());
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, DownloadAsBytesCallback);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_WRITEDATA, &buffer);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, UpdateByteTransferProgress);
    if (result == CURLE_OK && range)
        result = curl_easy_setopt(handle, CURLOPT_RANGE, range);
    if (result == CURLE_OK)
        result = curl_easy_perform(handle);

    curl_easy_cleanup(handle);
    if (result != CURLE_OK)
    {
        printAndLogFmt(3, "Download failed with error: %s", curl_easy_strerror(result));
        return NULL;
    }

    std::lock_guard<std::mutex> lock(g_byteBufferMutex);
    char *replacement = static_cast<char *>(malloc(buffer.bytes.size() + 1));
    if (!replacement)
        return NULL;

    if (!buffer.bytes.empty())
        memcpy(replacement, &buffer.bytes[0], buffer.bytes.size());
    replacement[buffer.bytes.size()] = '\0';

    free(g_byteBuffer);
    g_byteBuffer = replacement;
    g_byteBufferSize = buffer.bytes.size();
    *outSize = g_byteBufferSize;
    return g_byteBuffer;
}
} // namespace

char *GetDownloadInfo(const char *info)
{
    if (!info)
        return NULL;

    std::lock_guard<std::mutex> lock(g_infoMutex);
    if (strcmp(info, "speed") == 0)
    {
        snprintf(g_speedText, sizeof(g_speedText), "%llu",
                 static_cast<unsigned long long>(g_downloadSpeedBytesPerSecond.load()));
        return g_speedText;
    }
    if (strcmp(info, "downloaded") == 0)
    {
        snprintf(g_downloadedText, sizeof(g_downloadedText), "%llu",
                 static_cast<unsigned long long>(g_downloadCurrentBytes.load()));
        return g_downloadedText;
    }
    if (strcmp(info, "filesize") == 0)
    {
        snprintf(g_totalText, sizeof(g_totalText), "%llu",
                 static_cast<unsigned long long>(g_downloadTotalBytes.load()));
        return g_totalText;
    }
    if (strcmp(info, "progress") == 0)
    {
        snprintf(g_progressText, sizeof(g_progressText), "%d",
                 CalculateProgress(g_downloadCurrentBytes.load(), g_downloadTotalBytes.load()));
        return g_progressText;
    }
    return NULL;
}

bool HasDownloadCompleted()
{
    const int state = g_downloadState.load();
    return state == DirectDownloadSucceeded || state == DirectDownloadFailed ||
           state == DirectDownloadCancelled;
}

bool HasDownloadErrorOccured()
{
    return g_downloadState.load() == DirectDownloadFailed;
}

void ResetDownloadVars()
{
    if (ResetDirectDownload() != 0)
        return;
    std::lock_guard<std::mutex> transferLock(g_byteTransferMutex);
    g_byteCancelRequested.store(false);
    std::lock_guard<std::mutex> bufferLock(g_byteBufferMutex);
    free(g_byteBuffer);
    g_byteBuffer = NULL;
    g_byteBufferSize = 0;
}

void CancelDownload()
{
    CancelDirectDownload();
    g_byteCancelRequested.store(true);
}

size_t HeaderCallback(void *ptr, size_t size, size_t nmemb, void *userData)
{
    if (size != 0 && nmemb > std::numeric_limits<size_t>::max() / size)
        return 0;

    const size_t byteCount = size * nmemb;
    if (!ptr || byteCount == 0)
        return byteCount;

    const std::string header(static_cast<char *>(ptr), byteCount);
    ProgressContext *progress = static_cast<ProgressContext *>(userData);
    if (progress && progress->resumeBytes > 0 && header.compare(0, 5, "HTTP/") == 0)
    {
        int status = 0;
        if (sscanf(header.c_str(), "HTTP/%*s %d", &status) == 1)
        {
            progress->responseStatus = status;
            progress->hasContentRangeStart = false;
            progress->hasUnsatisfiedRangeTotal = false;
            if (status == 200)
            {
                return 0;
            }
        }
    }

    if (progress && progress->resumeBytes > 0 &&
        strncasecmp(header.c_str(), "Content-Range:", 14) == 0)
    {
        const char *value = header.c_str() + 14;
        while (*value == ' ' || *value == '\t')
            ++value;

        if (progress->responseStatus == 206 && strncasecmp(value, "bytes ", 6) == 0)
        {
            unsigned long long start = 0;
            unsigned long long end = 0;
            unsigned long long total = 0;
            if (sscanf(value + 6, "%llu-%llu/%llu", &start, &end, &total) == 3 &&
                end >= start && total > end)
            {
                progress->hasContentRangeStart = true;
                progress->contentRangeStart = static_cast<uint64_t>(start);
                progress->contentRangeEnd = static_cast<uint64_t>(end);
                progress->contentRangeTotal = static_cast<uint64_t>(total);
            }
        }
        else if (progress->responseStatus == 416)
        {
            const char *totalStart = strstr(value, "*/");
            if (totalStart)
            {
                char *end = NULL;
                const unsigned long long total = strtoull(totalStart + 2, &end, 10);
                if (end != totalStart + 2)
                {
                    progress->hasUnsatisfiedRangeTotal = true;
                    progress->unsatisfiedRangeTotal = static_cast<uint64_t>(total);
                }
            }
        }
    }

    if (progress && progress->resumeBytes > 0 &&
        (header == "\r\n" || header == "\n"))
    {
        if (progress->responseStatus == 206 &&
            (!progress->hasContentRangeStart ||
             progress->contentRangeStart != progress->resumeBytes))
            return 0;
        if (progress->responseStatus == 416 &&
            (!progress->hasUnsatisfiedRangeTotal ||
             progress->unsatisfiedRangeTotal != progress->resumeBytes))
            return 0;
    }

    if (header.find("Content-Disposition: attachment") != std::string::npos)
        printAndLogFmt(0, "Server is sending the file as an attachment.");
    return byteCount;
}

void BeginDownload(const char *url, const char *pathWithFile)
{
    if (ResetDirectDownload() != 0)
        return;

    if (StartDirectDownloadInternal(url, pathWithFile, true, "NULL") != 0)
    {
        ResetDirectDownload();
        return;
    }

    while (GetDirectDownloadState() == DirectDownloadRunning)
        sceKernelUsleep(100000);
    ResetDirectDownload();
}

void DownloadWebFile(const char *url, const char *pathWithFile, bool bgDL, const char *name)
{
    int result = ResetDirectDownload();
    if (result == 0)
        result = StartDirectDownloadInternal(url, pathWithFile, !bgDL, name);
    if (result != 0)
    {
        ResetDirectDownload();
        printAndLogFmt(3, "Unable to start download: %d", result);
        return;
    }

    if (!bgDL)
    {
        while (GetDirectDownloadState() == DirectDownloadRunning)
            sceKernelUsleep(100000);
        ResetDirectDownload();
    }

    printAndLogFmt(1, bgDL ? "Background download has begun." : "Foreground download has finished.");
    printAndLogFmt(0, "Downloading file and writing to: %s", pathWithFile);
}

char *DownloadAsBytesThread(const char *url, size_t *outSize)
{
    return DownloadBytesInternal(url, NULL, outSize);
}

char *DownloadAsBytes(const char *url, size_t *outSize)
{
    return DownloadAsBytesThread(url, outSize);
}

char *DownloadAsBytesRangeThread(const char *url, uint32_t offset, uint32_t size, size_t *outSize)
{
    std::string range;
    if (size == 0)
        range = std::to_string(offset) + "-";
    else
        range = std::to_string(offset) + "-" +
                std::to_string(static_cast<uint64_t>(offset) + static_cast<uint64_t>(size) - 1);
    return DownloadBytesInternal(url, range.c_str(), outSize);
}

char *DownloadAsBytesRange(const char *url, uint32_t offset, uint32_t size, size_t *outSize)
{
    return DownloadAsBytesRangeThread(url, offset, size, outSize);
}

int ResolveRedirects(const char *url, std::string &resolvedUrl)
{
    resolvedUrl.clear();
    if (!url || (!IsHttpUrl(url) && !IsHttpsUrl(url)))
        return CURLE_URL_MALFORMAT;

    CURLcode result = InitializeCurl();
    if (result != CURLE_OK)
        return result;

    CURL *handle = curl_easy_init();
    if (!handle)
        return CURLE_FAILED_INIT;

    const std::string caBundle = GetConfiguredCaBundle();
    const std::string userAgent = "UnityOrbisBridge | FW: " + std::string(GetFWVersion());
    result = ApplyHttpSecurity(handle, url, caBundle);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_URL, url);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_USERAGENT, userAgent.c_str());
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
    if (result == CURLE_OK)
        result = curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    if (result == CURLE_OK)
        result = curl_easy_perform(handle);

    if (result == CURLE_OK)
    {
        char *effectiveUrl = NULL;
        result = curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
        if (result == CURLE_OK && effectiveUrl)
            resolvedUrl = effectiveUrl;
        else if (result == CURLE_OK)
            result = CURLE_URL_MALFORMAT;
    }

    curl_easy_cleanup(handle);
    return result;
}

const char *FollowRedirects(const char *url)
{
    static thread_local char *resolvedBuffer = NULL;
    static thread_local size_t resolvedCapacity = 0;

    std::string resolvedUrl;
    if (ResolveRedirects(url, resolvedUrl) != CURLE_OK)
        resolvedUrl = url ? url : "";

    const size_t required = resolvedUrl.size() + 1;
    if (required > resolvedCapacity)
    {
        void *grown = realloc(resolvedBuffer, required);
        if (!grown)
            return url ? url : "";
        resolvedBuffer = static_cast<char *>(grown);
        resolvedCapacity = required;
    }

    memcpy(resolvedBuffer, resolvedUrl.c_str(), required);
    return resolvedBuffer;
}

extern "C"
{
int StartDirectDownload(const char *url, const char *finalPath)
{
    return StartDirectDownloadInternal(url, finalPath, false, NULL);
}

int GetDirectDownloadState()
{
    return g_downloadState.load();
}

int GetDirectDownloadErrorCode()
{
    return g_downloadError.load();
}

int GetDirectDownloadHttpStatus()
{
    return g_downloadHttpStatus.load();
}

uint64_t GetDirectDownloadTotalBytes()
{
    return g_downloadTotalBytes.load();
}

uint64_t GetDirectDownloadCurrentBytes()
{
    return g_downloadCurrentBytes.load();
}

uint64_t GetDirectDownloadSpeedBytesPerSecond()
{
    return g_downloadSpeedBytesPerSecond.load();
}

void CancelDirectDownload()
{
    std::lock_guard<std::mutex> lock(g_transitionMutex);
    if (g_downloadState.load() == DirectDownloadRunning)
        g_cancelRequested.store(true);
}

int ResetDirectDownload()
{
    std::lock_guard<std::mutex> lock(g_transitionMutex);
    if (g_downloadState.load() == DirectDownloadRunning)
        return EBUSY;

    if (g_downloadWorker.joinable())
        g_downloadWorker.join();

    g_cancelRequested.store(false);
    g_downloadError.store(CURLE_OK);
    g_downloadHttpStatus.store(0);
    g_downloadTotalBytes.store(0);
    g_downloadCurrentBytes.store(0);
    g_downloadSpeedBytesPerSecond.store(0);
    g_downloadState.store(DirectDownloadIdle);
    return 0;
}

int ConfigureHttp(const char *caBundlePath)
{
    if (!caBundlePath || !*caBundlePath)
        return EINVAL;

    struct stat info;
    if (stat(caBundlePath, &info) != 0)
        return errno ? errno : ENOENT;
    if ((info.st_mode & S_IFREG) == 0 || access(caBundlePath, R_OK) != 0)
        return errno ? errno : EACCES;

    std::lock_guard<std::mutex> lock(g_httpConfigMutex);
    g_caBundlePath = caBundlePath;
    return 0;
}
}
