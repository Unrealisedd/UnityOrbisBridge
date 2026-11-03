#include "../headers/includes.hpp"

#include <mutex>

jbc_cred g_Creds, g_RootCreds;

static bool messageDialogModuleLoaded = false;
static bool commonDialogInitialized = false;
static bool nativeDialogInitialized = false;
static bool hasBrokenFromTheSandbox = false;
static bool appInstModuleLoaded = false;
static bool bgftModuleLoaded = false;
static std::mutex nativeServicesMutex;

bool PS5_Jailbreak()
{
    struct sockaddr_in address;
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(9028);
    memset(&address.sin_zero, 0, sizeof(address.sin_zero));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr.s_addr);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return false;

    if (connect(sock, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        puts("Failed to connect to daemon");
        close(sock);
        return false;
    }

    HijackerCommand cmd;
    cmd.PID = getpid();
    cmd.cmd = JAILBREAK_CMD;

    if (send(sock, (void *)&cmd, sizeof(cmd), MSG_NOSIGNAL) == -1)
    {
        puts("failed to send command");
        close(sock);
        return false;
    }

    recv(sock, reinterpret_cast<void *>(&cmd), sizeof(cmd), MSG_NOSIGNAL);
    close(sock);

    if (cmd.ret != 0 && cmd.ret != -1337)
    {
        puts("Jailbreak has failed");
        return false;
    }

    return true;
}

bool PS5_WhitelistJailbreak()
{
    std::string json = "{\"PID\":\"" + std::to_string(getpid()) + "\"}";

    remove("/download0/etahen_jailbreak");

    sceKernelUsleep(500);

    std::ofstream outFile("/download0/etahen_jailbreak");
    if (!outFile.is_open())
        return false;

    outFile << json;
    outFile.close();

    showDialogMessage((char *)"Waiting for etaHEN... (MAX 30 secs)");
    auto start_time = std::chrono::high_resolution_clock::now();

    void *addr = nullptr;

    while (true)
    {
        int handle = sceKernelLoadStartModule("libSceSystemService.sprx", 0, 0, 0, 0, 0);
        sceKernelDlsym(handle, "sceSystemServiceLaunchApp", (void **)&addr);

        if (addr != nullptr)
            break;

        auto time_now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(time_now - start_time).count() > 30)
        {
            sceMsgDialogTerminate();
            return false;
        }

        sceKernelUsleep(500);
    }

    sceMsgDialogTerminate();
    return true;
}

bool IsFreeOfSandbox()
{
    if (hasBrokenFromTheSandbox)
        return true;

    FILE *filePtr = fopen("/user/.jailbreak", "w");
    if (!filePtr)
        return false;

    fclose(filePtr);
    remove("/user/.jailbreak");

    return true;
}

void EnterSandbox()
{
    if (IsPlayStation5())
        return;

    if (IsFreeOfSandbox())
        jbc_set_cred(&g_Creds);

    hasBrokenFromTheSandbox = false;
}

void BreakFromSandbox()
{
    if (IsFreeOfSandbox())
        return;

    if (IsPlayStation5())
    {
        if (!PS5_WhitelistJailbreak())
            PS5_Jailbreak();
    }
    else
    {
        jbc_get_cred(&g_Creds);
        g_RootCreds = g_Creds;
        jbc_jailbreak_cred(&g_RootCreds);
        jbc_set_cred(&g_RootCreds);
    }

    hasBrokenFromTheSandbox = true;
}

void MountInSandbox(const char *systemPath, const char *mountName)
{
    if (IsPlayStation5())
        return;

    if (jbc_mount_in_sandbox(systemPath, mountName) == 0)
        PrintToConsole("Mount: Passed", 0);
    else
        PrintToConsole("Mount: Failed", 2);
}

void UnmountFromSandbox(const char *mountName)
{
    if (IsPlayStation5())
        return;

    if (jbc_unmount_in_sandbox(mountName) == 0)
        PrintToConsole("Unmount: Passed", 0);
    else
        PrintToConsole("Unmount: Failed", 2);
}

int InitializePackageServices()
{
    std::lock_guard<std::mutex> lock(nativeServicesMutex);

    int ret;
    if (!messageDialogModuleLoaded)
    {
        ret = sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG);
        if (ret)
            return ret;
        messageDialogModuleLoaded = true;
    }
    if (!commonDialogInitialized)
    {
        ret = sceCommonDialogInitialize();
        if (ret)
            return ret;
        commonDialogInitialized = true;
    }
    if (nativeDialogInitialized && sceMsgDialogGetStatus() == ORBIS_COMMON_DIALOG_STATUS_NONE)
        nativeDialogInitialized = false;
    if (!nativeDialogInitialized)
    {
        ret = sceMsgDialogInitialize();
        if (ret)
            return ret;
        nativeDialogInitialized = true;
    }

    if (!appInstModuleLoaded)
    {
        const int32_t handle = static_cast<int32_t>(
            sceKernelLoadStartModule("/system/common/lib/libSceAppInstUtil.sprx", 0, 0, 0, 0, 0));
        if (handle < 0)
            return handle;
        appInstModuleLoaded = true;
    }
    if (!bgftModuleLoaded)
    {
        const int32_t handle = static_cast<int32_t>(
            sceKernelLoadStartModule("/system/common/lib/libSceBgft.sprx", 0, 0, 0, 0, 0));
        if (handle < 0)
            return handle;
        bgftModuleLoaded = true;
    }

    ret = app_inst_util_init_result();
    if (ret)
        return ret;
    return bgft_init_result();
}

void InitializeNativeDialogs()
{
    const int result = InitializePackageServices();
    if (result != 0)
        printAndLogFmt(4, "Native service initialization failed: 0x%08X", result);
}

void ExitApplication()
{
    EnterSandbox();

    sceSystemServiceLoadExec("exit", 0);
}

void UpdateViaHomebrewStore(const char *query)
{
    int32_t userId = 0;
    char titleIdBuffer[16] = {0};
    char *ownedTitleId = nullptr;
    const char *titleId = nullptr;

    BreakFromSandbox();

    for (int attempt = 0; attempt < 300 && !IsFreeOfSandbox(); ++attempt)
        sceKernelUsleep(100000);
    if (!IsFreeOfSandbox())
    {
        printAndLogFmt(3, "Timed out while leaving the sandbox.");
        return;
    }

    InitializeNativeDialogs();

    if (!CheckIfAppExists("NPXS39041"))
    {
        printAndLogFmt(1, "The Homebrew Store currently isnt installed...");

        const char *storeUrl = IsPlayStation5()
                                   ? "https://pkg-zone.com/update/Store-R2-PS5.pkg"
                                   : "https://pkg-zone.com/update/Store-R2.pkg";

        printAndLogFmt(1, "Attempting to download the Homebrew Store...");

        const int startResult = StartDirectDownload(storeUrl, "/user/app/store_download.pkg");
        if (startResult != 0)
        {
            printAndLogFmt(3, "Failed to start Homebrew Store download: %d", startResult);
            return;
        }

        while (GetDirectDownloadState() == DIRECT_DOWNLOAD_RUNNING)
            sceKernelUsleep(100000);

        const int downloadState = GetDirectDownloadState();
        const int downloadError = GetDirectDownloadErrorCode();
        const int httpStatus = GetDirectDownloadHttpStatus();
        if (ResetDirectDownload() != 0)
        {
            printAndLogFmt(3, "Homebrew Store download worker did not reach a resettable state.");
            return;
        }

        if (downloadState != DIRECT_DOWNLOAD_SUCCEEDED)
        {
            printAndLogFmt(3, "Failed to download the Homebrew Store (curl=%d, HTTP=%d).",
                           downloadError, httpStatus);
            return;
        }

        printAndLogFmt(1, "Now installing the Homebrew Store...");
        const int installResult =
            InstallLocalPackageResult("/user/app/store_download.pkg", "Homebrew Store", "", true);
        if (installResult != 0)
        {
            printAndLogFmt(3, "Failed to install the Homebrew Store: 0x%08X", installResult);
            return;
        }

        if (IsPlayStation5())
        {
            while (!CheckIfAppExists("NPXS39041"))
                sceKernelSleep(1);
        }
    }
    else
        printAndLogFmt(1, "Homebrew Store is installed, launching...");

    if (!IsPlayStation5())
    {
        int storeId = sceSystemServiceGetAppIdOfMiniApp();
        if ((storeId & ~0xFFFFFF) == 0x60000000 && if_exists("/mnt/sandbox/pfsmnt/NPXS39041-app0/"))
        {
            printAndLogFmt(0, "Closing Homebrew Store for re-launch!...");

            sceSystemServiceKillApp(storeId, -1, 0, 0);
        }
    }

    if (!IsPlayStation5())
    {
        if (query != nullptr && query[0] != '\0')
        {
            ownedTitleId = strdup(query);
            if (!ownedTitleId)
            {
                printAndLogFmt(3, "Failed to allocate Homebrew Store launch query.");
                return;
            }
            titleId = ownedTitleId;
        }
        else
        {
            const int result =
                sceLncUtilGetAppTitleId(sceSystemServiceGetAppIdOfBigApp(), titleIdBuffer);
            if (result != 0)
            {
                printAndLogFmt(3, "Failed to resolve current application title ID: 0x%08X", result);
                return;
            }
            titleId = titleIdBuffer;
        }
    }
    else
    {
        if (query != nullptr && query[0] != '\0')
        {
            ownedTitleId = strdup(query);
            if (!ownedTitleId)
            {
                printAndLogFmt(3, "Failed to allocate Homebrew Store launch query.");
                return;
            }
            titleId = ownedTitleId;
        }
        else
        {
            printAndLogFmt(1, "No query provided to launch Homebrew Store...");
            return;
        }
    }

    const char *argv[] = {titleId, nullptr};

    LncAppParam param;
    param.size = sizeof(LncAppParam);
    param.user_id = userId;
    param.app_opt = 0;
    param.crash_report = 0;
    param.LaunchAppCheck_flag = LaunchApp_SkipSystemUpdate;

    printAndLogFmt(0, "Attempting to launch Homebrew Store with \"%s\" query...", titleId);

    uint32_t res = sceLncUtilLaunchApp("NPXS39041", argv, &param);
    if ((res & 0x80000000) && res != 2157182993)
        printAndLogFmt(3, "App launch failed with error code: %u", res);

    if (res == 2157182993)
        printAndLogFmt(1, "Homebrew Store has launched successfully!");

    free(ownedTitleId);
}