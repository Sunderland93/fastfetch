#include "FFPlatform_private.h"
#include "util/stringUtils.h"
#include "fastfetch_config.h"
#include "common/io/io.h"

#include <unistd.h>
#include <pwd.h>
#include <limits.h>
#include <sys/utsname.h>

#ifdef __APPLE__
    #include <libproc.h>
    #include <sys/sysctl.h>
#elif defined(__FreeBSD__)
    #include <sys/sysctl.h>
#endif

static void getExePath(FFPlatform* platform)
{
    FFstrbuf* const exePath = &platform->exePath;
    ffStrbufEnsureFree(exePath, PATH_MAX);
    #ifdef __linux__
        ssize_t exePathLen = readlink("/proc/self/exe", exePath->chars, exePath->allocated);
    #elif defined(__APPLE__)
        int exePathLen = proc_pidpath((int) getpid(), exePath->chars, exePath->allocated);
    #elif defined(__FreeBSD__)
        size_t exePathLen = exePath->allocated;
        if(sysctl(
            (int[]){CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, (int) getpid()}, 4,
            exePath->chars, &exePathLen,
            NULL, 0
        ))
            exePathLen = 0;
    #endif
    if (exePathLen > 0)
        exePath->length = (uint32_t) exePathLen;
}

static void platformPathAddEnv(FFlist* dirs, const char* env)
{
    const char* envValue = getenv(env);
    if(!ffStrSet(envValue))
        return;

    FF_STRBUF_AUTO_DESTROY value = ffStrbufCreateA(64);
    ffStrbufAppendS(&value, envValue);

    uint32_t startIndex = 0;
    while (startIndex < value.length)
    {
        uint32_t colonIndex = ffStrbufNextIndexC(&value, startIndex, ':');
        value.chars[colonIndex] = '\0';

        if(!ffStrSet(value.chars + startIndex))
        {
            startIndex = colonIndex + 1;
            continue;
        }

        ffPlatformPathAddAbsolute(dirs, value.chars + startIndex);

        startIndex = colonIndex + 1;
    }
}

static void getHomeDir(FFPlatform* platform, const struct passwd* pwd)
{
    const char* home = pwd ? pwd->pw_dir : getenv("HOME");
    ffStrbufAppendS(&platform->homeDir, home);
    ffStrbufEnsureEndsWithC(&platform->homeDir, '/');
}

static void getCacheDir(FFPlatform* platform)
{
    const char* cache = getenv("XDG_CACHE_HOME");
    if(ffStrSet(cache))
    {
        ffStrbufAppendS(&platform->cacheDir, cache);
        ffStrbufEnsureEndsWithC(&platform->cacheDir, '/');
    }
    else
    {
        ffStrbufAppend(&platform->cacheDir, &platform->homeDir);
        ffStrbufAppendS(&platform->cacheDir, ".cache/");
    }
}

static void getConfigDirs(FFPlatform* platform)
{
    platformPathAddEnv(&platform->configDirs, "XDG_CONFIG_HOME");
    ffPlatformPathAddHome(&platform->configDirs, platform, ".config/");

    #if defined(__APPLE__)
        ffPlatformPathAddHome(&platform->configDirs, platform, "Library/Preferences/");
        ffPlatformPathAddHome(&platform->configDirs, platform, "Library/Application Support/");
    #endif

    ffPlatformPathAddHome(&platform->configDirs, platform, "");
    platformPathAddEnv(&platform->configDirs, "XDG_CONFIG_DIRS");

    #if !defined(__APPLE__)
        ffPlatformPathAddAbsolute(&platform->configDirs, FASTFETCH_TARGET_DIR_ETC "/xdg/");
    #endif

    ffPlatformPathAddAbsolute(&platform->configDirs, FASTFETCH_TARGET_DIR_ETC "/");
    ffPlatformPathAddAbsolute(&platform->configDirs, FASTFETCH_TARGET_DIR_INSTALL_SYSCONF "/");
}

static void getDataDirs(FFPlatform* platform)
{
    platformPathAddEnv(&platform->dataDirs, "XDG_DATA_HOME");
    ffPlatformPathAddHome(&platform->dataDirs, platform, ".local/share/");

    // Add ${currentExePath}/../share
    if (platform->exePath.length > 0)
    {
        FF_STRBUF_AUTO_DESTROY path = ffStrbufCreateCopy(&platform->exePath);
        ffStrbufSubstrBeforeLastC(&path, '/');
        ffStrbufSubstrBeforeLastC(&path, '/');
        ffStrbufAppendS(&path, "/share");
        ffPlatformPathAddAbsolute(&platform->dataDirs, path.chars);
    }

    #ifdef __APPLE__
        ffPlatformPathAddHome(&platform->dataDirs, platform, "Library/Application Support/");
    #endif

    ffPlatformPathAddHome(&platform->dataDirs, platform, "");
    platformPathAddEnv(&platform->dataDirs, "XDG_DATA_DIRS");
    ffPlatformPathAddAbsolute(&platform->dataDirs, FASTFETCH_TARGET_DIR_USR "/local/share/");
    ffPlatformPathAddAbsolute(&platform->dataDirs, FASTFETCH_TARGET_DIR_USR "/share/");
}

static void getUserName(FFPlatform* platform, const struct passwd* pwd)
{
    const char* user = getenv("USER");
    if(!ffStrSet(user) && pwd)
        user = pwd->pw_name;

    ffStrbufAppendS(&platform->userName, user);
}

static void getHostName(FFPlatform* platform, const struct utsname* uts)
{
    ffStrbufAppendS(&platform->hostName, uts->nodename);
}

static void getUserShell(FFPlatform* platform, const struct passwd* pwd)
{
    const char* shell = getenv("SHELL");
    if(!ffStrSet(shell) && pwd)
        shell = pwd->pw_shell;

    ffStrbufAppendS(&platform->userShell, shell);
}

static void getPageSize(FFPlatform* platform)
{
    #if defined(__FreeBSD__) || defined(__APPLE__)
    size_t length = sizeof(platform->pageSize);
    sysctl((int[]){ CTL_HW, HW_PAGESIZE }, 2, &platform->pageSize, &length, NULL, 0);
    #else
    platform->pageSize = (uint32_t) sysconf(_SC_PAGESIZE);
    #endif
}

void ffPlatformInitImpl(FFPlatform* platform)
{
    struct passwd* pwd = getpwuid(getuid());

    struct utsname uts;
    if(uname(&uts) != 0)
        memset(&uts, 0, sizeof(uts));

    getExePath(platform);
    getHomeDir(platform, pwd);
    getCacheDir(platform);
    getConfigDirs(platform);
    getDataDirs(platform);

    getUserName(platform, pwd);
    getHostName(platform, &uts);
    getUserShell(platform, pwd);

    ffStrbufAppendS(&platform->systemName, uts.sysname);
    ffStrbufAppendS(&platform->systemRelease, uts.release);
    ffStrbufAppendS(&platform->systemVersion, uts.version);
    ffStrbufAppendS(&platform->systemArchitecture, uts.machine);
    ffStrbufInit(&platform->systemDisplayVersion);

    getPageSize(platform);
}
