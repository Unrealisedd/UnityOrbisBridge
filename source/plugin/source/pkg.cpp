#include "../headers/includes.hpp"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>

bool sceAppInst_done = false;
static bool s_bgft_initialized = false;
static struct bgft_init_params s_bgft_init_params;

int PKG_ERROR(const char *name, int ret)
{
    printAndLogFmt(3, "%s error: %x", name, ret);
    return ret;
}

int app_inst_util_init_result(void)
{
    if (sceAppInst_done)
        return 0;

    printAndLogFmt(0, "Initializing AppInstUtil...");
    const int ret = sceAppInstUtilInitialize();
    if (ret)
    {
        sceAppInst_done = false;
        return PKG_ERROR("sceAppInstUtilInitialize failed", ret);
    }

    sceAppInst_done = true;
    return 0;
}

bool app_inst_util_init(void)
{
    return app_inst_util_init_result() == 0;
}

void app_inst_util_fini(void)
{
    if (!sceAppInst_done)
        return;

    const int ret = sceAppInstUtilTerminate();
    if (ret)
        printAndLogFmt(4, "sceAppInstUtilTerminate failed: 0x%08X", ret);
    else
        sceAppInst_done = false;
}

int bgft_init_result(void)
{
    if (s_bgft_initialized)
        return 0;

    memset(&s_bgft_init_params, 0, sizeof(s_bgft_init_params));
    s_bgft_init_params.heapSize = BGFT_HEAP_SIZE;
    s_bgft_init_params.heap = static_cast<uint8_t *>(malloc(s_bgft_init_params.heapSize));
    if (!s_bgft_init_params.heap)
        return PKG_ERROR("No memory for BGFT heap", ENOMEM);

    memset(s_bgft_init_params.heap, 0, s_bgft_init_params.heapSize);
    printAndLogFmt(0, "Initializing BGFT...");

    const int ret = sceBgftServiceInit(&s_bgft_init_params);
    if (ret)
    {
        free(s_bgft_init_params.heap);
        memset(&s_bgft_init_params, 0, sizeof(s_bgft_init_params));
        s_bgft_initialized = false;
        return PKG_ERROR("sceBgftServiceInit failed", ret);
    }

    s_bgft_initialized = true;
    return 0;
}

bool bgft_init(void)
{
    return bgft_init_result() == 0;
}

void bgft_fini(void)
{
    if (!s_bgft_initialized)
        return;

    const int ret = sceBgftServiceTerm();
    if (ret)
    {
        printAndLogFmt(4, "sceBgftServiceTerm failed: 0x%08X", ret);
        return;
    }

    free(s_bgft_init_params.heap);
    memset(&s_bgft_init_params, 0, sizeof(s_bgft_init_params));
    s_bgft_initialized = false;
}

int displayDownloadProgress(void *arguments)
{
    struct install_args *args = static_cast<install_args *>(arguments);
    if (!args)
        return EINVAL;

    SceBgftTaskProgress progressInfo;
    memset(&progressInfo, 0, sizeof(progressInfo));
    const std::string message = std::string("Installing... [") + args->title_id + "] " + args->fname;
    printAndLogFmt(1, "%s", message.c_str());
    initiateProgressDialog(args->fname);

    struct stat packageInfo;
    const uint64_t expectedPackageSize =
        stat(args->path, &packageInfo) == 0 && packageInfo.st_size > 0
            ? static_cast<uint64_t>(packageInfo.st_size)
            : 0;

    int result = 0;
    for (;;)
    {
        memset(&progressInfo, 0, sizeof(progressInfo));
        result = sceBgftServiceDownloadGetProgress(args->task_id, &progressInfo);
        if (result)
        {
            PKG_ERROR("sceBgftServiceDownloadGetProgress", result);
            break;
        }
        if (progressInfo.error_result != 0)
        {
            result = PKG_ERROR("BGFT_ERROR", progressInfo.error_result);
            break;
        }

        const uint64_t totalLength = progressInfo.lengthTotal > 0
                                         ? static_cast<uint64_t>(progressInfo.lengthTotal)
                                         : static_cast<uint64_t>(progressInfo.length);
        const uint64_t transferred = progressInfo.lengthTotal > 0
                                         ? static_cast<uint64_t>(progressInfo.transferredTotal)
                                         : static_cast<uint64_t>(progressInfo.transferred);
        if (totalLength > 0)
        {
            const bool transferComplete = transferred >= totalLength;
            const bool sizeMatched = expectedPackageSize > 0 && totalLength == expectedPackageSize;
            const bool copyPhaseActive = progressInfo.localCopyPercent > 0;
            const bool installComplete =
                transferComplete &&
                ((sizeMatched && !copyPhaseActive) || progressInfo.localCopyPercent >= 100);

            int progress = transferComplete
                               ? (installComplete ? 100 : 99)
                               : static_cast<int>((static_cast<long double>(transferred) * 100.0L) /
                                                  static_cast<long double>(totalLength));
            if (progressInfo.preparingPercent > progress && progressInfo.preparingPercent < 100)
                progress = progressInfo.preparingPercent;
            if (progressInfo.localCopyPercent > progress && progressInfo.localCopyPercent < 100)
                progress = progressInfo.localCopyPercent;
            setProgressMsgText(progress, "%s", message.c_str());

            if (installComplete)
                break;
        }

        sceKernelUsleep(100000);
    }

    if (result == 0 && args->delete_pkg && unlink(args->path) != 0)
        printAndLogFmt(2, "Installed package but could not delete %s: %s", args->path, strerror(errno));

    sceMsgDialogTerminate();
    free(args->title_id);
    free(args->path);
    free(args->fname);
    free(args);
    bgft_fini();
    return result;
}

int installPKG(const char *fullpath, const char *name, const char *iconURI, bool deleteAfter)
{
    if (!fullpath || !*fullpath || !name)
        return EINVAL;

    printAndLogFmt(0, "Checking if file exists: %s", fullpath);
    struct stat packageInfo;
    if (stat(fullpath, &packageInfo) != 0)
        return PKG_ERROR("Failed to stat package file", errno ? errno : ENOENT);
    if ((packageInfo.st_mode & S_IFREG) == 0 || packageInfo.st_size <= 0)
        return PKG_ERROR("Invalid package file", EINVAL);

    int ret = app_inst_util_init_result();
    if (ret)
        return ret;

    ret = bgft_init_result();
    if (ret)
        return ret;

    char titleId[16] = {0};
    int isApp = 0;
    ret = sceAppInstUtilGetTitleIdFromPkg(fullpath, titleId, &isApp);
    if (ret)
    {
        bgft_fini();
        return PKG_ERROR("sceAppInstUtilGetTitleIdFromPkg failed", ret);
    }

    if (!iconURI)
        iconURI = "";

    struct bgft_download_param_ex downloadParams;
    memset(&downloadParams, 0, sizeof(downloadParams));
    downloadParams.param.user_id = 0;
    downloadParams.param.entitlement_type = 5;
    downloadParams.param.id = "";
    downloadParams.param.content_url = fullpath;
    downloadParams.param.content_ex_url = "";
    downloadParams.param.content_name = name;
    downloadParams.param.icon_path = iconURI;
    downloadParams.param.sku_id = "";
    downloadParams.param.option = BGFT_TASK_OPTION_DISABLE_CDN_QUERY_PARAM;
    downloadParams.param.playgo_scenario_id = "0";
    downloadParams.param.release_date = "";
    downloadParams.param.package_type = "";
    downloadParams.param.package_sub_type = "";
    downloadParams.param.package_size = static_cast<unsigned long>(packageInfo.st_size);
    downloadParams.slot = 0;

    int taskId = -1;
    ret = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&downloadParams, &taskId);
    if (ret == static_cast<int>(0x80990088) || ret == static_cast<int>(0x80990015))
    {
        printAndLogFmt(2, "Conflicting installation detected. Uninstalling existing title: [%s] %s!", titleId, name);
        ret = sceAppInstUtilAppUnInstall(titleId);
        if (ret)
        {
            bgft_fini();
            return PKG_ERROR("sceAppInstUtilAppUnInstall failed", ret);
        }
        ret = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&downloadParams, &taskId);
    }

    if (ret == static_cast<int>(0x80990086))
    {
        TextNotify(222, "Already queued in notifications\nplease cancel it and retry.");
        bgft_fini();
        return ret;
    }
    if (ret)
    {
        bgft_fini();
        return PKG_ERROR("sceBgftServiceIntDownloadRegisterTaskByStorageEx failed", ret);
    }

    ret = sceBgftServiceDownloadStartTask(taskId);
    if (ret)
    {
        bgft_fini();
        return PKG_ERROR("sceBgftServiceDownloadStartTask failed", ret);
    }

    struct install_args *args = static_cast<install_args *>(calloc(1, sizeof(struct install_args)));
    if (!args)
    {
        sceBgftServiceDownloadStopTask(taskId);
        bgft_fini();
        return PKG_ERROR("Memory allocation failed", ENOMEM);
    }

    args->title_id = strdup(titleId);
    args->path = strdup(fullpath);
    args->fname = strdup(name);
    args->task_id = taskId;
    args->delete_pkg = deleteAfter;
    if (!args->title_id || !args->path || !args->fname)
    {
        free(args->title_id);
        free(args->path);
        free(args->fname);
        free(args);
        sceBgftServiceDownloadStopTask(taskId);
        bgft_fini();
        return PKG_ERROR("Memory allocation failed", ENOMEM);
    }

    return displayDownloadProgress(args);
}

int installWebPKG(const char *url, const char *name, const char *titleId, const char *iconURI)
{
    if (!url || !name || !titleId || !iconURI)
        return EINVAL;

    int ret = app_inst_util_init_result();
    if (ret)
        return ret;
    ret = bgft_init_result();
    if (ret)
        return ret;

    struct bgft_download_param downloadParams;
    memset(&downloadParams, 0, sizeof(downloadParams));
    downloadParams.user_id = 0;
    downloadParams.entitlement_type = 5;
    downloadParams.id = "";
    downloadParams.content_url = url;
    downloadParams.content_ex_url = "";
    downloadParams.content_name = name;
    downloadParams.icon_path = iconURI;
    downloadParams.sku_id = "";
    downloadParams.option = BGFT_TASK_OPTION_DISABLE_CDN_QUERY_PARAM;
    downloadParams.playgo_scenario_id = "0";
    downloadParams.release_date = "";
    downloadParams.package_type = "";
    downloadParams.package_sub_type = "";
    downloadParams.package_size = 0;

    int taskId = -1;
    ret = sceBgftServiceIntDebugDownloadRegisterPkg(&downloadParams, &taskId);
    if (ret == static_cast<int>(0x80990088) || ret == static_cast<int>(0x80990015))
    {
        printAndLogFmt(2, "Conflicting installation detected. Uninstalling existing title: [%s] %s!", titleId, name);
        ret = sceAppInstUtilAppUnInstall(titleId);
        if (ret)
        {
            bgft_fini();
            return PKG_ERROR("sceAppInstUtilAppUnInstall failed", ret);
        }
        ret = sceBgftServiceIntDebugDownloadRegisterPkg(&downloadParams, &taskId);
    }

    if (ret == static_cast<int>(0x80F00633))
    {
        TextNotify(222, "Please change NP Environment\nin Debug Settings from NP to SP-INT.");
        bgft_fini();
        return PKG_ERROR("Incorrect NP environment", ret);
    }
    if (ret == static_cast<int>(0x80990086))
    {
        TextNotify(222, "Already queued in notifications\nplease cancel it and retry.");
        bgft_fini();
        return ret;
    }
    if (ret)
    {
        bgft_fini();
        return PKG_ERROR("sceBgftServiceIntDebugDownloadRegisterPkg failed", ret);
    }

    ret = sceBgftServiceDownloadStartTask(taskId);
    if (ret)
    {
        bgft_fini();
        return PKG_ERROR("sceBgftServiceDownloadStartTask failed", ret);
    }

    return 0;
}

bool SendInstallRequestForPS5(const char *url)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        printAndLogFmt(3, "Socket server failed to creation for DPI.");
        return false;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(9090);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
    {
        printAndLogFmt(3, "Failed to connect to the DPI server.");
        close(sock);
        return false;
    }

    std::string request = std::string("{ \"url\" : \"") + url + "\" }";
    printAndLogFmt(0, "Sending install request: %s\n", request.c_str());

    if (send(sock, request.c_str(), request.size(), 0) < 0)
    {
        printAndLogFmt(3, "Failed to send install request to DPI.");
        close(sock);
        return false;
    }

    char buffer[1024] = {0};
    int bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0);
    close(sock);

    if (bytesRead <= 0)
    {
        printAndLogFmt(3, "No response or failed to read response from DPI.");
        return false;
    }

    buffer[bytesRead] = '\0';
    printAndLogFmt(0, "Response: %s", buffer);
    return std::string(buffer).find("\"res\" : \"0\"") != std::string::npos;
}
