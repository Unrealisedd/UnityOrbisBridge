#pragma once

#define BGFT_HEAP_SIZE (1 * 1024 * 1024)

extern bool sceAppInst_done;

int PKG_ERROR(const char *name, int ret);

int app_inst_util_init_result(void);
bool app_inst_util_init(void);
void app_inst_util_fini(void);

int bgft_init_result(void);
bool bgft_init(void);
void bgft_fini(void);

int initiateProgressDialog(const char *format, ...);
void setProgressMsgText(int prog, const char *fmt, ...);
int displayDownloadProgress(void *argument);

int installPKG(const char *fullpath, const char *name, const char *iconURI, bool deleteAfter = false);
int installWebPKG(const char *url, const char *name, const char *titleId, const char *iconURI);

bool SendInstallRequestForPS5(const char *url);
