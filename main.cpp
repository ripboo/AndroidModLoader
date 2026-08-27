#ifndef DONT_USE_STB
    #include <mod/thirdparty/stb_sprintf.h>
    #define sprintf stbsp_sprintf
    #define snprintf stbsp_snprintf
#endif
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h> // mkdir
#include <sys/sendfile.h> // sendfile
#include <fcntl.h> // "open" flags
#include <android/looper.h> // ALooper
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>

#include <aml.h>
#include <defines.h>
#include <modpaks.h>
#include <mls.h>
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>

#include <jnifn.h>

#ifdef __IL2CPPUTILS
    #include <il2cpp/functions.h>
#endif

// Should be after config.h in main.cpp
#include <icfg_desc.h>
// Should be after config.h in main.cpp

#include <interfaces.h>
#include <modslist.h>

#ifndef AML_PATH_MAX
    #ifdef PATH_MAX
        #define AML_PATH_MAX PATH_MAX
    #else
        #define AML_PATH_MAX 4096
    #endif
#endif

// تعريف RTLD_NOLOAD للتحقق من المكتبة إن لم تكن معرفة في الـ NDK
#ifndef RTLD_NOLOAD
    #define RTLD_NOLOAD 0x00004
#endif

// 1. اسم مكتبة اللعبة المستهدفة
static const char* TARGET_GAME_LIB = "libGTASA.so"; 

pid_t g_MainThreadID = 0;
bool g_bShowUpdatedToast, g_bShowUpdateFailedToast, g_bEnableFileDownloads;
// تم إرجاع المتغير g_bDumpThreadRegisters هنا لحل خطأ الربط (Linker Error)
bool g_bCrashAML, g_bNoMods, g_bSimplerCrashLog = false, g_bNoSPInLog, g_bNoModsInLog, g_bMLSOnlyManualSaves, g_bDumpAllThreads, g_bEHUnwind, g_bMoreRegsInfo, g_bDumpThreadRegisters = false, g_bUnixBacktrace = false;
int g_nEnableNews, g_nDownloadTimeout;
int g_nAndroidSDKVersion = 0, g_nFailedToLoad = 0, g_nLatestDownloadErrorCode = 0;
ConfigEntry* g_pLastNewsId;
char g_szInternalStoragePath[256]{0},
     g_szAppName[256]{0},
     g_szFakeAppName[256]{0},
     g_szNativeLibPath[512]{0},
     g_szModsDir[256]{0},
     g_szInternalModsDir[256]{0},
     g_szAndroidDataRootDir[256]{0},
     g_szAndroidDataDir[256]{0},
     g_szCfgPath[256]{0},
     g_szFastman92Android[256]{0},
     g_szDataDirPath[256]{0},
     g_szNewsString[512]{0};
const char* g_szDataDir = g_szDataDirPath;
char g_szUserAgent[256]{0};

jobject appContext = NULL;
JNIEnv* g_env = NULL;
std::unordered_map<std::string, jobject> g_InjectedInstances;

// Main
static ModInfo modinfoLocal("net.rusjj.aml", "AML Core", "1.4.1", "RusJJ aka [-=KILL MAN=-]");
ModInfo* amlmodinfo = &modinfoLocal;

static Config cfgLocal("ModLoaderCore");
Config* cfg = &cfgLocal;
static CFG icfgLocal; ICFG* icfg = &icfgLocal;

// دالة التسجيل في ملف txt بعد تعريف g_szModsDir والمكتبات
static void LogTrace(const char* fmt, ...)
{
    if (g_szModsDir[0] == 0) return;

    char logFilePath[512];
    snprintf(logFilePath, sizeof(logFilePath), "%s/aml_trace_log.txt", g_szModsDir);

    FILE* file = fopen(logFilePath, "a");
    if (!file) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm* tm_info = localtime(&ts.tv_sec);
    char timeBuffer[32];
    strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", tm_info);

    fprintf(file, "[%s.%03ld] ", timeBuffer, ts.tv_nsec / 1000000);

    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);

    fprintf(file, "\n");
    fflush(file);
    fclose(file);
}

inline size_t __strlen(const char *str)
{
    const char* s = str;
    while(*s) ++s;
    return (s - str);
}

inline bool __ispathdel(char s)
{
    return (s == '\\' || s == '/');
}

inline void __pathback(char *str)
{
    const char* s = str;
    uint16_t i = 0;
    while(*s) ++s;
    while(s != str)
    {
        if(!__ispathdel(*(--s))) break;
    }
    while(s != str)
    {
        if(__ispathdel(*(--s)))
        {
            i = (uint16_t)(s - str);
        }
        else if(i != 0) break;
    }
    if(i > 0) str[i] = 0;
}

inline bool EndsWith(const char* base, const char* str)
{
    int blen = strlen(base), slen = strlen(str);
    return (blen >= slen) && (!strcmp(base + blen - slen, str));
}

inline bool EndsWithSO(const char* base)
{
    int blen = strlen(base);
    return (blen >= 3) && (!strcmp(base + blen - 3, ".so"));
}

inline bool CopyFileFaster(const char* file, const char* dest)
{
    int inFd = open(file, O_RDONLY | O_CLOEXEC);
    if(inFd < 0) return false;
    struct stat statBuf;
    fstat(inFd, &statBuf);
    int outFd = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, statBuf.st_mode);
    if(outFd < 0)
    {
        close(inFd);
        return false;
    }

    off_t bytesLeft = statBuf.st_size;
    unsigned char errors = 0;
    while(bytesLeft > 0)
    {
        ssize_t bytesCopied = sendfile(outFd, inFd, NULL, bytesLeft);
        if(bytesCopied > 0) bytesLeft -= bytesCopied;
        else if(bytesCopied == 0) break;
        else
        {
            ++errors;
            if(errors < 5 && (errno == EINTR || errno == EAGAIN) ) continue;
            break;
        }
    }

    close(inFd);
    close(outFd);
    return (bytesLeft <= 0);
}

inline bool CopyFile(const char* file, const char* dest)
{
    FILE* source = fopen(file, "rb");
    if(source == NULL) return false;
    FILE* target = fopen(dest, "wb");
    if(target == NULL) 
    {
        fclose(source);
        return false;
    }

    int ch;
    while((ch = fgetc(source)) != EOF)
    {
        if(fputc(ch, target) == EOF)
        {
            fclose(source);
            fclose(target);
            return false;
        }
    }

    bool success = !ferror(source) && !ferror(target) && fclose(target) == 0;
    fclose(source);
    return success;
}

bool AML_CopyFile(const char* file, const char* dest)
{
    return (CopyFileFaster(file, dest) || CopyFile(file, dest));
}

inline bool HasFakeAppName()
{
    return (g_szFakeAppName[0] != 0 && strlen(g_szFakeAppName) > 5);
}

static bool CopyJStringUTF(JNIEnv* env, jstring str, char* out, size_t outLen, bool lowercase = false)
{
    if(!env || !str || !out || outLen == 0) return false;

    const char* tmp = env->GetStringUTFChars(str, NULL);
    if(!tmp)
    {
        if(env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }

    size_t i = 0;
    if(lowercase)
    {
        for(; tmp[i] != 0 && i < outLen - 1; ++i)
        {
            out[i] = (char)tolower((unsigned char)tmp[i]);
        }
        out[i] = 0;
    }
    else
    {
        snprintf(out, outLen, "%s", tmp);
    }

    env->ReleaseStringUTFChars(str, tmp);
    return true;
}

typedef const char* (*SpecificGameFn)();
void LoadMods(const char* path)
{
    ModInfo* pModInfo = NULL;
    SpecificGameFn maybeINeedAGame = NULL;
    GetModInfoFn modInfoFn = NULL;

    char buf[AML_PATH_MAX], dataBuf[AML_PATH_MAX];
    DIR* dir = opendir(path);
    if (dir != NULL)
    {
        logger->Info("Loading mods from %s", path);
        LogTrace("LoadMods: Scanning directory %s", path);
        struct dirent *diread; void* handle;
        const char* gameName = HasFakeAppName() ? g_szFakeAppName : g_szAppName;
        while ((diread = readdir(dir)) != NULL)
        {
            if(diread->d_name[0] == '.' &&
                (diread->d_name[1] == '.' || diread->d_name[1] == 0)) continue;
            if(!EndsWithSO(diread->d_name))
            {
                continue;
            }
            int srcLen = snprintf(buf, sizeof(buf), "%s/%s", path, diread->d_name);
            int tmpLen = snprintf(dataBuf, sizeof(dataBuf), "%s/%s", g_szDataDir, diread->d_name);
            if(srcLen < 0 || srcLen >= (int)sizeof(buf) || tmpLen < 0 || tmpLen >= (int)sizeof(dataBuf))
            {
                logger->Error("Skipping mod %s: path is too long", diread->d_name);
                LogTrace("LoadMods: Skipping %s - Path too long", diread->d_name);
                continue;
            }

            chmod(dataBuf, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP);
            int removeStatus = remove(dataBuf);
            if(!CopyFileFaster(buf, dataBuf) && !CopyFile(buf, dataBuf))
            {
                logger->Error("File %s is failed to be copied! :(", diread->d_name);
                LogTrace("LoadMods: Failed to copy mod file %s", diread->d_name);
                continue;
            }
            chmod(dataBuf, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP);

            handle = dlopen(dataBuf, RTLD_NOW);
            if(!handle)
            {
                logger->Error("Failed to load mod %s: %s", diread->d_name, dlerror());
                LogTrace("LoadMods: Failed dlopen %s - Error: %s", diread->d_name, dlerror());
                remove(dataBuf);
                continue;
            }
            
            bool keepLoaded = false;
            modInfoFn = (GetModInfoFn)dlsym(handle, "__GetModInfo");
            if(modInfoFn != NULL)
            {
                pModInfo = modInfoFn();
                if(pModInfo == NULL)
                {
                    logger->Error("Mod %s returned NULL from __GetModInfo!", diread->d_name);
                    LogTrace("LoadMods: Mod %s returned NULL __GetModInfo", diread->d_name);
                }
                else
                {
                    maybeINeedAGame = (SpecificGameFn)dlsym(handle, "__INeedASpecificGame");
                    const char* requiredGame = maybeINeedAGame ? maybeINeedAGame() : NULL;
                    if(requiredGame != NULL && requiredGame[0] != 0 && strcmp(requiredGame, gameName) != 0)
                    {
                        logger->Error("Mod (GUID %s) built for the game %s!", pModInfo->GUID(), requiredGame);
                        LogTrace("LoadMods: Mod GUID %s mismatch required game %s", pModInfo->GUID(), requiredGame);
                    }
                    else if(!modlist->AddMod(pModInfo, handle, buf))
                    {
                        logger->Error("Mod (GUID %s) is already loaded!", pModInfo->GUID());
                        LogTrace("LoadMods: Mod GUID %s already loaded", pModInfo->GUID());
                    }
                    else
                    {
                        logger->Info("Mod (GUID %s) has been preprocessed.", pModInfo->GUID());
                        LogTrace("LoadMods: Mod GUID %s successfully registered", pModInfo->GUID());
                        keepLoaded = true;
                    }
                }
            }
            if(!keepLoaded)
            {
                dlclose(handle);
            }
            removeStatus = remove(dataBuf);
            if(removeStatus != 0) logger->Error("Failed to remove temp mod file! Error %d", removeStatus);
        }
        closedir(dir);
    }
    else
    {
        logger->Error("Failed to load mods: unable to open directory");
        LogTrace("LoadMods: Unable to open directory %s", path);
    }
}

extern ModDesc* pLastModProcessed;

void StartSignalHandler();
void HookALog();
JavaVM *g_pJavaVM = NULL;
void *g_pJavaReserved = NULL;

extern bool bAndroidLog_OnlyImportant, bAndroidLog_NoAfter, bAML_HasFastmanModified;
bool g_bAMLStarted = false;

pthread_key_t g_JNIThreadKey;
static pthread_once_t g_JNIKeyOnce = PTHREAD_ONCE_INIT;
static void DetachJNIOnThreadExit(void* env)
{
    if(env && g_pJavaVM) g_pJavaVM->DetachCurrentThread();
}
static void CreateJNIThreadKey()
{
    pthread_key_create(&g_JNIThreadKey, DetachJNIOnThreadExit);
}
JNIEnv* GetCurrentJNI()
{
    pthread_once(&g_JNIKeyOnce, CreateJNIThreadKey);
    
    JNIEnv* env = (JNIEnv*)pthread_getspecific(g_JNIThreadKey);
    if(env) return env;

    if(g_pJavaVM)
    {
        if(g_pJavaVM->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK && env) return env;
        if(g_pJavaVM->AttachCurrentThread(&env, NULL) == 0 && env)
        {
            pthread_setspecific(g_JNIThreadKey, env);
            return env;
        }
    }
    return NULL;
}

jobject g_GlobalContext = NULL;
jobject GetCurrentContext()
{
    if(g_GlobalContext) return g_GlobalContext;

    JNIEnv* env = GetCurrentJNI();
    if(!env) return NULL;

    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if(!activityThread) { if(env->ExceptionCheck()) env->ExceptionClear(); return NULL; }
    jmethodID currentActivityThread = env->GetStaticMethodID(activityThread, "currentActivityThread", "()Landroid/app/ActivityThread;");
    if(!currentActivityThread) { if(env->ExceptionCheck()) env->ExceptionClear(); env->DeleteLocalRef(activityThread); return NULL; }
    jobject activityThreadObj = env->CallStaticObjectMethod(activityThread, currentActivityThread);
    if(env->ExceptionCheck()) env->ExceptionClear();
    if(!activityThreadObj) { env->DeleteLocalRef(activityThread); return NULL; }
    jmethodID getApplication = env->GetMethodID(activityThread, "getApplication", "()Landroid/app/Application;");
    if(!getApplication) { if(env->ExceptionCheck()) env->ExceptionClear(); env->DeleteLocalRef(activityThreadObj); env->DeleteLocalRef(activityThread); return NULL; }

    jobject localContext = env->CallObjectMethod(activityThreadObj, getApplication);
    if(env->ExceptionCheck()) env->ExceptionClear();
    if(localContext) g_GlobalContext = env->NewGlobalRef(localContext);
    if(localContext) env->DeleteLocalRef(localContext);
    env->DeleteLocalRef(activityThreadObj);
    env->DeleteLocalRef(activityThread);
    return g_GlobalContext;
}

jobject g_GlobalActivity = NULL;
jobject GetCurrentActivity()
{
    if(g_GlobalActivity) return g_GlobalActivity;

    JNIEnv* env = GetCurrentJNI();
    if(!env) return NULL;

    jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
    jmethodID currentActivityThreadMethod = env->GetStaticMethodID(activityThreadClass, "currentActivityThread", "()Landroid/app/ActivityThread;");
    jobject activityThreadObj = env->CallStaticObjectMethod(activityThreadClass, currentActivityThreadMethod);
    if(!activityThreadObj)
    {
        env->DeleteLocalRef(activityThreadClass);
        return NULL;
    }
    
    jfieldID mActivitiesField = env->GetFieldID(activityThreadClass, "mActivities", "Landroid/util/ArrayMap;");
    if(!mActivitiesField)
    {
        env->ExceptionClear();
        env->DeleteLocalRef(activityThreadObj);
        env->DeleteLocalRef(activityThreadClass);
        return NULL;
    }
    
    jobject mActivities = env->GetObjectField(activityThreadObj, mActivitiesField);
    if(!mActivities)
    {
        env->DeleteLocalRef(activityThreadObj);
        env->DeleteLocalRef(activityThreadClass);
        return NULL;
    }

    jclass arrayMapClass = env->GetObjectClass(mActivities);
    jmethodID isEmptyMethod = env->GetMethodID(arrayMapClass, "isEmpty", "()Z");
    
    if(env->CallBooleanMethod(mActivities, isEmptyMethod))
    {
        env->DeleteLocalRef(arrayMapClass);
        env->DeleteLocalRef(mActivities);
        env->DeleteLocalRef(activityThreadObj);
        env->DeleteLocalRef(activityThreadClass);
        return NULL;
    }

    jmethodID valueAtMethod = env->GetMethodID(arrayMapClass, "valueAt", "(I)Ljava/lang/Object;");
    jobject activityClientRecord = env->CallObjectMethod(mActivities, valueAtMethod, 0);
    if(!activityClientRecord)
    {
        env->DeleteLocalRef(arrayMapClass);
        env->DeleteLocalRef(mActivities);
        env->DeleteLocalRef(activityThreadObj);
        env->DeleteLocalRef(activityThreadClass);
        return NULL;
    }

    jclass acrClass = env->GetObjectClass(activityClientRecord);
    jfieldID activityField = env->GetFieldID(acrClass, "activity", "Landroid/app/Activity;");
    g_GlobalActivity = env->GetObjectField(activityClientRecord, activityField);
    if(g_GlobalActivity) g_GlobalActivity = env->NewGlobalRef(g_GlobalActivity);

    env->DeleteLocalRef(arrayMapClass);
    env->DeleteLocalRef(mActivities);
    env->DeleteLocalRef(activityClientRecord);
    env->DeleteLocalRef(acrClass);
    env->DeleteLocalRef(activityThreadObj);
    env->DeleteLocalRef(activityThreadClass);

    return g_GlobalActivity;
}

AAssetManager* g_AssetManager = NULL;
AAssetManager* GetCurrentAssetManager()
{
    if(g_AssetManager) return g_AssetManager;

    JNIEnv* env = GetCurrentJNI();
    if(!env) return NULL;
    
    return ( g_AssetManager = GetAssetManager(env) );
}

struct LibraryLoadData
{
    LibraryLoadData() : initMembersList(NULL), initMembersCount(0) { }
    typedef void(*initArrayMember)();

    initArrayMember* initMembersList;
    int initMembersCount;
};
static LibraryLoadData g_LoadDatas[2];
void AML_PostLoadLib(int libNum)
{
    libNum--;

    const int size = g_LoadDatas[libNum].initMembersCount;
    for(int i = 0; i < size; ++i)
    {
        g_LoadDatas[libNum].initMembersList[i]();
    }
}

void* AML_dlopen(const char* lib, int libNum)
{
    return dlopen(lib, RTLD_NOW);
}

// 2. دالة تنفذ داخل الخيط المستقل لفحص الذاكرة فقط
static void* LibraryCheckThread(void* arg)
{
    const char* libName = (const char*)arg;
    LogTrace("LibraryCheckThread: Thread started. Monitoring memory for %s...", libName);

    unsigned long attempts = 0;
    while (true)
    {
        attempts++;
        void* handle = dlopen(libName, RTLD_NOLOAD);
        if (handle)
        {
            dlclose(handle);
            LogTrace("LibraryCheckThread: Target library %s loaded & stable after %lu attempts!", libName, attempts);
            break;
        }
        
        if (attempts % 10 == 0)
        {
            LogTrace("LibraryCheckThread: Still waiting for %s... (Attempt %lu)", libName, attempts);
        }

        usleep(100000); // 100ms
    }

    LogTrace("LibraryCheckThread: Finished checking, exiting thread.");
    pthread_exit(NULL);
    return NULL;
}

// 3. دالة مساعدة لإنشاء الخيط
static void WaitForLibraryInThread(const char* libName)
{
    LogTrace("WaitForLibraryInThread: Spawning thread to monitor %s...", libName);
    pthread_t threadId;
    if (pthread_create(&threadId, NULL, LibraryCheckThread, (void*)libName) == 0)
    {
        LogTrace("WaitForLibraryInThread: Thread created (ID: %lu). Joining...", (unsigned long)threadId);
        pthread_join(threadId, NULL);
        LogTrace("WaitForLibraryInThread: Thread rejoined successfully.");
    }
    else
    {
        LogTrace("WaitForLibraryInThread: ERROR - Thread creation failed! Fallback to inline wait.");
        while (!dlopen(libName, RTLD_NOLOAD))
        {
            usleep(100000);
        }
        LogTrace("WaitForLibraryInThread: Inline wait finished.");
    }
}

void StartAMLRightNow(const char* libName1 = NULL, const char* libName2 = NULL)
{
    if(g_bAMLStarted)
    {
        logger->Error("Something was trying to boot-up AML again.");
        return;
    }

    logger->SetTag("AndroidModLoader");
    jstring jTmp;

    /* JNI Environment */
    if (g_pJavaVM->GetEnv(reinterpret_cast<void**>(&g_env), JNI_VERSION_1_6) != JNI_OK)
    {
        logger->Error("Cannot get JNI Environment!");
        return;
    }

    /* Application Context */
    jobject localContext = GetGlobalContext(g_env);
    if(localContext == NULL)
    {
        logger->Error("Failed to resolve Android application context.");
        return;
    }
    appContext = g_env->NewGlobalRef(localContext);
    g_env->DeleteLocalRef(localContext);
    if(appContext == NULL)
    {
        logger->Error("AML Library should be loaded in \"onCreate\" or by injecting it directly into the main game library!");
        return;
    }

    // Preload libs
    void *lib1 = NULL, *lib2 = NULL;
    if(libName1 && libName1[0]) lib1 = AML_dlopen(libName1, 1);
    if(libName2 && libName2[0]) lib2 = AML_dlopen(libName2, 2);
    
    /* Must Have for mods */
    modlist->AddMod(amlmodinfo, 0, "localpath (core)");
    interfaces->Register("AMLInterface", aml);
    interfaces->Register("AMLConfig", icfg);

    /* Internal Storage */
    jobject storageDir = GetStorageDir(g_env);
    jTmp = GetAbsolutePath(g_env, storageDir);
    if(!CopyJStringUTF(g_env, jTmp, g_szInternalStoragePath, sizeof(g_szInternalStoragePath)))
    {
        logger->Error("Failed to determine internal storage path.");
        if(jTmp) g_env->DeleteLocalRef(jTmp);
        if(storageDir) g_env->DeleteLocalRef(storageDir);
        return;
    }
    if(jTmp) g_env->DeleteLocalRef(jTmp);
    if(storageDir) g_env->DeleteLocalRef(storageDir);

    /* Package Name */
    jTmp = GetPackageName(g_env, appContext);
    if(!CopyJStringUTF(g_env, jTmp, g_szAppName, sizeof(g_szAppName), true))
    {
        logger->Error("Failed to determine package name.");
        if(jTmp) g_env->DeleteLocalRef(jTmp);
        return;
    }
    if(jTmp) g_env->DeleteLocalRef(jTmp);
    logger->Info("Determined app info: %s", g_szAppName);

  #ifdef FASTMAN92_CODE
    bAML_HasFastmanModified = GetExternalFilesDir_FLA(g_env, appContext, g_szFastman92Android, sizeof(g_szFastman92Android));
    __pathback(g_szFastman92Android);

    snprintf(g_szAndroidDataRootDir, sizeof(g_szAndroidDataRootDir), "%s/", g_szFastman92Android);
    snprintf(g_szModsDir, sizeof(g_szModsDir), "%s/mods/", g_szFastman92Android);
    mkdir(g_szModsDir, 0777);
    snprintf(g_szAndroidDataDir, sizeof(g_szAndroidDataDir), "%s/files/", g_szFastman92Android);
    mkdir(g_szAndroidDataDir, 0777);
    snprintf(g_szCfgPath, sizeof(g_szCfgPath), "%s/configs/", g_szFastman92Android);
    mkdir(g_szCfgPath, 0777);
  #else
    snprintf(g_szAndroidDataRootDir, sizeof(g_szAndroidDataRootDir), "%s/Android/data/%s/", g_szInternalStoragePath, g_szAppName);
    DIR* dir = opendir(g_szAndroidDataRootDir);
    if(dir != NULL) closedir(dir);
    else GetExternalFilesDir(g_env, appContext);

    snprintf(g_szModsDir, sizeof(g_szModsDir), "%s/Android/data/%s/mods/", g_szInternalStoragePath, g_szAppName);
    mkdir(g_szModsDir, 0777);

    snprintf(g_szAndroidDataDir, sizeof(g_szAndroidDataDir), "%s/Android/data/%s/files/", g_szInternalStoragePath, g_szAppName);
    mkdir(g_szAndroidDataDir, 0777);

    snprintf(g_szCfgPath, sizeof(g_szCfgPath), "%s/Android/data/%s/configs/", g_szInternalStoragePath, g_szAppName);
    mkdir(g_szCfgPath, 0777);
  #endif

    // بدء السجل التتبعي بعد إنشاء المجلدات
    LogTrace("--- Starting New AML Launch Session ---");
    LogTrace("Target Library set to: %s", TARGET_GAME_LIB);
    LogTrace("App Name: %s", g_szAppName);
    LogTrace("Mods Dir: %s", g_szModsDir);

    /* root/data/data Folder */
    jobject filesDir = GetFilesDir(g_env, appContext);
    jstring filesPath = GetAbsolutePath(g_env, filesDir);
    if(!CopyJStringUTF(g_env, filesPath, g_szDataDirPath, sizeof(g_szDataDirPath)))
    {
        logger->Error("Failed to determine app data path.");
        LogTrace("FATAL: Failed to determine app data path.");
        if(filesPath) g_env->DeleteLocalRef(filesPath);
        if(filesDir) g_env->DeleteLocalRef(filesDir);
        return;
    }
    if(filesPath) g_env->DeleteLocalRef(filesPath);
    if(filesDir) g_env->DeleteLocalRef(filesDir);

    /* AML Config */
    logger->Info("Reading core config...");
    LogTrace("Reading core config...");
    cfg->Init();
    cfg->Bind("Author", "")->SetString("RusJJ aka [-=KILL MAN=-]"); cfg->ClearLast();
    cfg->Bind("Discord", "")->SetString("https://discord.gg/2MY7W39kBg"); cfg->ClearLast();
    bool bHasChangedCfgAuthor = cfg->IsValueChanged();
    cfg->Bind("Version", "")->SetString(amlmodinfo->VersionString()); cfg->ClearLast();
    cfg->Bind("LaunchedTimeStamp", 0)->SetInt((int)time(NULL)); cfg->ClearLast();
    cfg->Bind("FakePackageName", "")->GetString(g_szFakeAppName, sizeof(g_szFakeAppName)); cfg->ClearLast();
    snprintf(g_szInternalModsDir, sizeof(g_szInternalModsDir), "%s/%s/%s", g_szInternalStoragePath, cfg->Bind("InternalModsFolder", "AMLMods")->GetString(), g_szAppName); cfg->ClearLast();
    bool internalModsPriority = cfg->GetBool("InternalModsFirst", true);
    logger->ToggleOutput(cfg->GetBool("EnableLogcats", true));
    bool bEnableUpdater = cfg->GetBool("EnableUpdater", true);
    g_bShowUpdatedToast = cfg->GetBool("ShowUpdaterToast", true);
    g_bShowUpdateFailedToast = cfg->GetBool("ShowUpdaterFailedToast", true);
    g_bEnableFileDownloads = cfg->GetBool("EnableModFileDownloads", true);
    g_nEnableNews = clampint(0, 3, cfg->GetInt("ShowNewsForFewTimes", 3));
    g_pLastNewsId = cfg->Bind("LastNewsIdShowed", 0, "Savings");
    g_nDownloadTimeout = clampint(1, 5, cfg->GetInt("DownloadTimeout", 2));

    g_bCrashAML = cfg->GetBool("CrashAML", false, "DevTools");
    g_bNoMods = cfg->GetBool("DontLoadMods", false, "DevTools");
    g_bNoSPInLog = cfg->GetBool("NoStackInCrashLog", false, "DevTools");
    g_bNoModsInLog = cfg->GetBool("NoModsInCrashLog", false, "DevTools");
    g_bMLSOnlyManualSaves = cfg->GetBool("MLSOnlyManualSaves", false, "DevTools");
    g_bDumpAllThreads = cfg->GetBool("CrashLogFromAllThreads", true, "DevTools");
    g_bEHUnwind = cfg->GetBool("EHUnwindCrashLog", false, "DevTools");
    g_bMoreRegsInfo = cfg->GetBool("MoreRegistersInfo", true, "DevTools");
    g_bDumpThreadRegisters = cfg->GetBool("DumpThreadRegisters", false, "DevTools");

    cfg->Save();

    /* Android version */
    char sdk_ver_str[92];
    if(__system_property_get("ro.build.version.sdk", sdk_ver_str))
    {
        g_nAndroidSDKVersion = atoi(sdk_ver_str);
    }
    else if(g_nAndroidSDKVersion == 0)
    {
        if(__system_property_get("ro.build.version.release", sdk_ver_str))
        {
            g_nAndroidSDKVersion = atoi(sdk_ver_str);
        }
        else
        {
            jclass versionClass = g_env->FindClass("android/os/Build$VERSION");
            jfieldID sdkIntFieldID = g_env->GetStaticFieldID(versionClass, "SDK_INT", "I");
            g_nAndroidSDKVersion = g_env->GetStaticIntField(versionClass, sdkIntFieldID);
            g_env->DeleteLocalRef(versionClass);
        }
    }

    /* Handlers & Log Hooks */
    if(cfg->GetBool("SignalHandler", true))
    {
        g_pAML->AddFeature("SIGNAL");
        StartSignalHandler();
        LogTrace("SignalHandler active.");
    }

    bAndroidLog_OnlyImportant = !cfg->GetBool("PrintLogsToFile_Verbose", false);
    bAndroidLog_NoAfter = cfg->GetBool("PrintLogsToFile_NoLogCat", false);
    if(cfg->GetBool("PrintLogsToFile", false))
    {
        g_pAML->AddFeature("LOGHOOK");
        HookALog();
        LogTrace("LogHook active.");
    }

    // =================================================================
    // انتظار تحميل libGTASA.so داخل الخيط المستقل
    // =================================================================
    LogTrace("Waiting for target library (%s) stability...", TARGET_GAME_LIB);
    WaitForLibraryInThread(TARGET_GAME_LIB);
    LogTrace("Target library confirmed. Loading mods...");

    /* Mods loading */
    logger->Info("Working with mods...");
    #ifdef __IL2CPPUTILS
        logger->Info("IL2CPP: Attempting to initialize IL2CPP-Utils");
        LogTrace("Initializing IL2CPP-Utils...");
        IL2CPP::Func::HookFunctions();
    #endif

    if(!g_bNoMods)
    {
        MLS::LoadFile();
        if(g_szInternalModsDir[0] != 0)
        {
            LogTrace("Loading mods from internal and external directories...");
            LoadMods(internalModsPriority ? g_szInternalModsDir : g_szModsDir);
            LoadMods(internalModsPriority ? g_szModsDir : g_szInternalModsDir);
        }
        else
        {
            LogTrace("Loading mods from primary directory...");
            LoadMods(g_szModsDir);
        }
    }
    else
    {
        LogTrace("Skipping mods loading (DontLoadMods option active).");
    }

    /* Dependencies check */
    logger->Info("Checking for dependencies...");
    LogTrace("Processing mod dependencies...");
    modlist->ProcessDependencies();
    
    #ifdef __XDL
        g_pAML->AddFeature("XDL");
    #endif
    #ifdef __IL2CPPUTILS
        g_pAML->AddFeature("IL2CPP");
    #endif
    if(g_pAML->IsGameFaked()) g_pAML->AddFeature("FAKEGAME");
    if(bHasChangedCfgAuthor) g_pAML->AddFeature("STEALER");
    if(!logger->HasOutput()) g_pAML->AddFeature("NOLOGGING");
    
    if(g_nEnableNews > 0)
    {
        char newsBuf[24]{0};
        if(aml->DownloadFileToData("https://raw.githubusercontent.com/RusJJ/AndroidModLoader/main/news.txt", g_szNewsString, sizeof(g_szNewsString)) && g_szNewsString[0])
        {
            memcpy(newsBuf, g_szNewsString, 16);
            if(strncmp(g_pLastNewsId->GetString(), newsBuf, 16) != 0)
            {
                for(int i = 0; i < g_nEnableNews; ++i)
                {
                    aml->ShowToast(true, "%s", g_szNewsString);
                }

                newsBuf[16] = '|';
                newsBuf[17] = 0;
                g_pLastNewsId->SetString(newsBuf);
                cfg->Save();
            }
            delete g_pLastNewsId;
        }
    }
    
    if(bEnableUpdater)
    {
        g_pAML->AddFeature("UPDATER");
        LogTrace("Updating mods via updater...");
        modlist->ProcessUpdater();
        logger->Info("Mods were updated!");
    }
    if(!g_bNoMods)
    {
        LogTrace("Executing ProcessPreLoading...");
        modlist->ProcessPreLoading();
        
        LogTrace("Executing ProcessLoading...");
        modlist->ProcessLoading();
        
        LogTrace("Executing OnAllModsLoaded...");
        modlist->OnAllModsLoaded();
        logger->Info("Mods were launched!");
        LogTrace("All mods loaded and launched successfully!");
    }
    pLastModProcessed = NULL;

    #ifdef AML32
        snprintf(g_szUserAgent, sizeof(g_szUserAgent), "AndroidModLoader/%s (Android; ARM; ARM32)", amlmodinfo->VersionString());
    #else
        snprintf(g_szUserAgent, sizeof(g_szUserAgent), "AndroidModLoader/%s (Android; ARM; ARM64)", amlmodinfo->VersionString());
    #endif

    if(lib1)
    {
        LogTrace("Triggering JNI_OnLoad for lib1...");
        AML_PostLoadLib(1);
        auto libEntry = (void(*)(JavaVM*, void*))dlsym(lib1, "JNI_OnLoad");
        if(libEntry) libEntry(g_pJavaVM, g_pJavaReserved);
    }
    if(lib2)
    {
        LogTrace("Triggering JNI_OnLoad for lib2...");
        AML_PostLoadLib(2);
        auto libEntry = (void(*)(JavaVM*, void*))dlsym(lib2, "JNI_OnLoad");
        if(libEntry) libEntry(g_pJavaVM, g_pJavaReserved);
    }

    LogTrace("AML core started successfully.");
    g_bAMLStarted = true;
}

extern "C" JNIEXPORT void JNICALL Java_net_rusjj_amlcore_launchAMLCore(JNIEnv *env, jclass clazz)
{
    StartAMLRightNow();
}
extern "C" JNIEXPORT void JNICALL Java_net_rusjj_amlcore_earlyLaunchAMLCore(JNIEnv *env, jclass clazz, jstring lib1, jstring lib2)
{
    const char* szLib1 = lib1 ? env->GetStringUTFChars(lib1, NULL) : NULL;
    const char* szLib2 = lib2 ? env->GetStringUTFChars(lib2, NULL) : NULL;

    StartAMLRightNow(szLib1, szLib2);

    if(lib1 && szLib1) env->ReleaseStringUTFChars(lib1, szLib1);
    if(lib2 && szLib2) env->ReleaseStringUTFChars(lib2, szLib2);
}

static ALooper* g_pJavaUILooper = NULL;
static int g_aJavaUIPipes[2];
struct JavaUIThreadTask { void (*fn)(void*); void* data; };
static int JavaUIThreadLoop(int fd, int events, void* data)
{
    JavaUIThreadTask task;
    if(read(fd, &task, sizeof(JavaUIThreadTask)) == sizeof(JavaUIThreadTask) && task.fn != NULL)
    {
        task.fn(task.data);
    }
    return 1;
}
static void InitJavaUIThreadLooper()
{
    g_pJavaUILooper = ALooper_forThread();
    if(g_pJavaUILooper)
    {
        ALooper_acquire(g_pJavaUILooper);
        pipe(g_aJavaUIPipes);
        ALooper_addFd(g_pJavaUILooper, g_aJavaUIPipes[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, JavaUIThreadLoop, NULL);
    }
}
bool PushToJavaUIThread(void (*fn)(void*), void* data)
{
    if(!g_pJavaUILooper || !fn) return false;

    JavaUIThreadTask task = { fn, data };
    return ( write(g_aJavaUIPipes[1], &task, sizeof(task)) == sizeof(task) );
}
JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    g_pJavaVM = vm;
    g_pJavaReserved = reserved;

    g_MainThreadID = gettid();
    InitJavaUIThreadLooper();
    
    StartAMLRightNow();
    
    return JNI_VERSION_1_6;
}

void ClearIniPointers();
JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* reserved)
{
    modlist->ProcessUnloading();
    ClearIniPointers();
}
