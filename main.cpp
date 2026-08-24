#ifndef DONT_USE_STB
    #include <mod/thirdparty/stb_sprintf.h>
    #define sprintf stbsp_sprintf
    #define snprintf stbsp_snprintf
#endif
#include <string>
#include <unordered_map>
#include <vector>
#include <string_view>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h> // mkdir
#include <sys/sendfile.h> // sendfile
#include <sys/system_properties.h> // __system_property_get
#include <fcntl.h> // "open" flags
#include <android/looper.h> // ALooper
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>
#include <stdarg.h>
#include <time.h>

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

pid_t g_MainThreadID = 0;
bool g_bShowUpdatedToast, g_bShowUpdateFailedToast, g_bEnableFileDownloads;
bool g_bCrashAML = false, g_bNoMods = false, g_bSimplerCrashLog = false, g_bNoSPInLog = false,
     g_bNoModsInLog = false, g_bMLSOnlyManualSaves = false, g_bDumpAllThreads = true,
     g_bMoreRegsInfo = true, g_bDumpThreadRegisters = true;
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

// ============================================================================
// نظام تسجيل الأحداث إلى ملف النص aml_log.txt
// ============================================================================
void LogToFile(const char* fmt, ...)
{
    if (g_szModsDir[0] == 0) return;

    char logPath[512];
    snprintf(logPath, sizeof(logPath), "%s/aml_log.txt", g_szModsDir);

    FILE* f = fopen(logPath, "a");
    if (!f) return;

    time_t rawtime;
    struct tm* timeinfo;
    char timeBuffer[32];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    if (timeinfo)
    {
        strftime(timeBuffer, sizeof(timeBuffer), "[%H:%M:%S] ", timeinfo);
        fputs(timeBuffer, f);
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fputc('\n', f);
    fflush(f);
    fclose(f);
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

    LogToFile("----------------------------------------");
    LogToFile("[SCAN START] Checking directory: %s", path);

    if (dir != NULL)
    {
        logger->Info("Loading mods from %s", path);
        struct dirent *diread; void* handle;
        const char* gameName = HasFakeAppName() ? g_szFakeAppName : g_szAppName;
        LogToFile("[INFO] Target Game/Package Name: %s", gameName);

        int filesCount = 0;

        while ((diread = readdir(dir)) != NULL)
        {
            if(diread->d_name[0] == '.' &&
                (diread->d_name[1] == '.' || diread->d_name[1] == 0)) continue;

            filesCount++;
            LogToFile("[FILE FOUND] Processing: %s", diread->d_name);

            if(!EndsWithSO(diread->d_name))
            {
                LogToFile("[SKIP] %s is not a .so library", diread->d_name);
                continue;
            }
            int srcLen = snprintf(buf, sizeof(buf), "%s/%s", path, diread->d_name);
            int tmpLen = snprintf(dataBuf, sizeof(dataBuf), "%s/%s", g_szDataDir, diread->d_name);
            if(srcLen < 0 || srcLen >= (int)sizeof(buf) || tmpLen < 0 || tmpLen >= (int)sizeof(dataBuf))
            {
                logger->Error("Skipping mod %s: path is too long", diread->d_name);
                LogToFile("[ERROR] Skipping mod %s: path is too long", diread->d_name);
                continue;
            }

            chmod(dataBuf, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP);
            remove(dataBuf);

            if(!CopyFileFaster(buf, dataBuf) && !CopyFile(buf, dataBuf))
            {
                logger->Error("File %s is failed to be copied! :(", diread->d_name);
                LogToFile("[ERROR] Failed to copy %s to data directory | Errno: %d (%s)", diread->d_name, errno, strerror(errno));
                continue;
            }
            chmod(dataBuf, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP);

            handle = dlopen(dataBuf, RTLD_NOW);
            if(!handle)
            {
                logger->Error("Failed to load mod %s: %s", diread->d_name, dlerror());
                LogToFile("[CRITICAL ERROR] dlopen failed for %s! Cause: %s", diread->d_name, dlerror());
                remove(dataBuf);
                continue;
            }

            LogToFile("[SUCCESS] dlopen loaded %s successfully", diread->d_name);
            
            bool keepLoaded = false;
            modInfoFn = (GetModInfoFn)dlsym(handle, "__GetModInfo");
            if(modInfoFn != NULL)
            {
                pModInfo = modInfoFn();
                if(pModInfo == NULL)
                {
                    logger->Error("Mod %s returned NULL from __GetModInfo!", diread->d_name);
                    LogToFile("[ERROR] Mod %s returned NULL from __GetModInfo!", diread->d_name);
                }
                else
                {
                    LogToFile("[MOD INFO] GUID: %s | Name: %s | Version: %s", pModInfo->GUID(), pModInfo->Name(), pModInfo->VersionString());

                    maybeINeedAGame = (SpecificGameFn)dlsym(handle, "__INeedASpecificGame");
                    const char* requiredGame = maybeINeedAGame ? maybeINeedAGame() : NULL;
                    if(requiredGame != NULL && requiredGame[0] != 0 && strcmp(requiredGame, gameName) != 0)
                    {
                        logger->Error("Mod (GUID %s) built for the game %s!", pModInfo->GUID(), requiredGame);
                        LogToFile("[REJECTED] Mod %s requires game: %s (Current: %s)", pModInfo->GUID(), requiredGame, gameName);
                    }
                    else if(!modlist->AddMod(pModInfo, handle, buf))
                    {
                        logger->Error("Mod (GUID %s) is already loaded!", pModInfo->GUID());
                        LogToFile("[REJECTED] Mod %s is already loaded!", pModInfo->GUID());
                    }
                    else
                    {
                        logger->Info("Mod (GUID %s) has been pre-processed.", pModInfo->GUID());
                        LogToFile("[LOADED] Mod %s (%s) registered successfully!", diread->d_name, pModInfo->GUID());
                        keepLoaded = true;
                    }
                }
            }
            else
            {
                LogToFile("[ERROR] %s missing symbol '__GetModInfo'. Not a valid AML mod!", diread->d_name);
            }

            if(!keepLoaded)
            {
                dlclose(handle);
            }

            int removeStatus = remove(dataBuf);
            if(removeStatus != 0) logger->Error("Failed to remove temp mod file! Error %d", removeStatus);
        }
        closedir(dir);
        LogToFile("[SCAN END] Total items scanned in %s: %d", path, filesCount);
    }
    else
    {
        logger->Error("Failed to load mods: unable to open directory");
        LogToFile("[ERROR] Unable to open directory: %s | Errno: %d (%s)", path, errno, strerror(errno));
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

std::vector<void*> g_vLibrariesToBeLoaded;
template <typename Func> void AML_SplitText(const char* txt, Func action)
{
    if(!txt) return;

    std::string_view text(txt);
    size_t start = 0;
    size_t end = text.find(',');

    while(end != std::string_view::npos)
    {
        action(text.substr(start, end - start));
        
        start = end + 1;
        end = text.find(',', start);
    }

    if(start < text.size()) action(text.substr(start));
}
void AML_InitLibs(const char* libsArray)
{
    g_vLibrariesToBeLoaded.clear();
    if(!libsArray || !libsArray[0]) return;
    
    AML_SplitText(libsArray, [](std::string_view item)
    {
        std::string lib(item);
        if(lib.size() < 4 || ( lib[0] != 'l' || lib[1] != 'i' || lib[2] != 'b') )
        {
            lib = "lib" + lib + ".so";
        }
        void* libHandle = dlopen(lib.c_str(), RTLD_NOW);
        if(libHandle) g_vLibrariesToBeLoaded.push_back(libHandle);
    });
}
void AML_PostLoadLibs()
{
    for(void* libHandle : g_vLibrariesToBeLoaded)
    {
        auto libEntry = (void(*)(JavaVM*, void*))dlsym(libHandle, "JNI_OnLoad");
        if(libEntry) libEntry(g_pJavaVM, g_pJavaReserved);
    }
}

void StartAMLRightNow(const char* libsArray = NULL)
{
    if(g_bAMLStarted)
    {
        logger->Error("Something was trying to boot-up AML again.");
        LogToFile("[WARNING] AML Boot-up was called multiple times!");
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
    jobject localContext = ::GetGlobalContext(g_env);
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

    // ========================================================================
    // تهيئة ملف aml_log.txt ومسح السجل القديم
    // ========================================================================
    char mainLogPath[512];
    snprintf(mainLogPath, sizeof(mainLogPath), "%s/aml_log.txt", g_szModsDir);
    FILE* fInit = fopen(mainLogPath, "w");
    if(fInit)
    {
        fputs("=== AML Log System Started ===\n", fInit);
        fclose(fInit);
    }

    LogToFile("[AML INIT] Storage Path: %s", g_szInternalStoragePath);
    LogToFile("[AML INIT] Package Name: %s", g_szAppName);
    LogToFile("[AML INIT] Mods Folder: %s", g_szModsDir);

    // ========================================================================
    // التحقق المباشر من وجود مكتبة GTA SA في الذاكرة
    // ========================================================================
    void* hGTASA = dlopen("libgtasa.so", RTLD_NOLOAD);
    if (hGTASA)
    {
        LogToFile("[GAME CHECK] Found 'libgtasa.so' loaded in RAM at: %p", hGTASA);
        dlclose(hGTASA);
    }
    else
    {
        LogToFile("[GAME CHECK] 'libgtasa.so' is NOT loaded in memory yet (Normal if AML starts early).");
    }

    // Preload libs
    AML_InitLibs(libsArray);
    
    /* Must Have for mods */
    modlist->AddMod(amlmodinfo, 0, "localpath (core)");
    interfaces->Register("AMLInterface", aml);
    interfaces->Register("AMLConfig", icfg);

    /* root/data/data Folder */
    jobject filesDir = GetFilesDir(g_env, appContext);
    jstring filesPath = GetAbsolutePath(g_env, filesDir);
    if(!CopyJStringUTF(g_env, filesPath, g_szDataDirPath, sizeof(g_szDataDirPath)))
    {
        logger->Error("Failed to determine app data path.");
        LogToFile("[ERROR] Failed to determine app data path!");
        if(filesPath) g_env->DeleteLocalRef(filesPath);
        if(filesDir) g_env->DeleteLocalRef(filesDir);
        return;
    }
    if(filesPath) g_env->DeleteLocalRef(filesPath);
    if(filesDir) g_env->DeleteLocalRef(filesDir);

    /* AML Config */
    logger->Info("Reading core config...");
    LogToFile("[CONFIG] Reading ModLoaderCore config...");
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
    g_bDumpThreadRegisters = cfg->GetBool("CrashLogFromAllThreads", true, "DevTools");
    g_bMoreRegsInfo = cfg->GetBool("MoreRegistersInfo", true, "DevTools");

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
    LogToFile("[SYSTEM] Android SDK Version detected: %d", g_nAndroidSDKVersion);

    /* Catch the fish! */
    if(cfg->GetBool("SignalHandler", true))
    {
        g_pAML->AddFeature("SIGNAL");
        StartSignalHandler();
    }

    /* Catch another fish! */
    bAndroidLog_OnlyImportant = !cfg->GetBool("PrintLogsToFile_Verbose", false);
    bAndroidLog_NoAfter = cfg->GetBool("PrintLogsToFile_NoLogCat", false);
    if(cfg->GetBool("PrintLogsToFile", false))
    {
        g_pAML->AddFeature("LOGHOOK");
        HookALog();
    }

    /* Mods? */
    logger->Info("Working with mods...");
    LogToFile("[MODS ENGINE] Starting mod scan process...");

    #ifdef __IL2CPPUTILS
        logger->Info("IL2CPP: Attempting to initialize IL2CPP-Utils");
        LogToFile("[IL2CPP] Initializing IL2CPP Hooks...");
        IL2CPP::Func::HookFunctions();
    #endif

    if(!g_bNoMods)
    {
        MLS::LoadFile();
        if(g_szInternalModsDir[0] != 0)
        {
            LoadMods(internalModsPriority ? g_szInternalModsDir : g_szModsDir);
            LoadMods(internalModsPriority ? g_szModsDir : g_szInternalModsDir);
        }
        else
        {
            LoadMods(g_szModsDir);
        }
    }
    else
    {
        LogToFile("[WARNING] Loading mods disabled by DevTools config (DontLoadMods=true)");
    }

    /* Dependencies Check */
    logger->Info("Checking for dependencies...");
    LogToFile("[MODS ENGINE] Checking mod dependencies...");
    modlist->ProcessDependencies();
    
    /* Features */
    #ifdef __XDL
        g_pAML->AddFeature("XDL");
    #endif
    #ifdef __IL2CPPUTILS
        g_pAML->AddFeature("IL2CPP");
    #endif
    if(g_pAML->IsGameFaked()) g_pAML->AddFeature("FAKEGAME");
    if(bHasChangedCfgAuthor) g_pAML->AddFeature("STEALER");
    if(!logger->HasOutput()) g_pAML->AddFeature("NOLOGGING");
    
    /* Load news */
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
    
    /* Execution Stages */
    if(bEnableUpdater)
    {
        g_pAML->AddFeature("UPDATER");
        modlist->ProcessUpdater();
        logger->Info("Mods were updated!");
        LogToFile("[UPDATER] Mods updater processed.");
    }
    if(!g_bNoMods)
    {
        LogToFile("[LAUNCH] Executing ProcessPreLoading()...");
        modlist->ProcessPreLoading();
        LogToFile("[LAUNCH] Executing ProcessLoading()...");
        modlist->ProcessLoading();
        LogToFile("[LAUNCH] Executing OnAllModsLoaded()...");
        modlist->OnAllModsLoaded();
        logger->Info("Mods were launched!");
        LogToFile("[SUCCESS] All mods successfully loaded and launched!");
    }
    pLastModProcessed = NULL;

    #ifdef AML32
        snprintf(g_szUserAgent, sizeof(g_szUserAgent), "AndroidModLoader/%s (Android; ARM; ARM32)", amlmodinfo->VersionString());
    #else
        snprintf(g_szUserAgent, sizeof(g_szUserAgent), "AndroidModLoader/%s (Android; ARM; ARM64)", amlmodinfo->VersionString());
    #endif

    AML_PostLoadLibs();

    g_bAMLStarted = true;
    LogToFile("=== AML Engine Fully Started ===");
}

extern "C" JNIEXPORT void JNICALL Java_net_rusjj_amlcore_launchAMLCore(JNIEnv *env, jclass clazz, jstring libsArray)
{
    const char* szLibsArray = (libsArray ? env->GetStringUTFChars(libsArray, NULL) : NULL);

    StartAMLRightNow(szLibsArray);

    if(szLibsArray) env->ReleaseStringUTFChars(libsArray, szLibsArray);
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
    LogToFile("[AML] Engine Unloading...");
    modlist->ProcessUnloading();
    ClearIniPointers();
}
