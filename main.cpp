#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>

// 1. اسم مكتبة اللعبة الأساسية
static const char* TARGET_GAME_LIB = "libmain.so"; 

// دالة تسجيل دقيقة تحفظ كل حركة في ملف TXT داخل مجلد المودات فوراً
static void LogTrace(const char* fmt, ...)
{
    char logFilePath[512];
    snprintf(logFilePath, sizeof(logFilePath), "%s/aml_trace_log.txt", g_szModsDir);

    FILE* file = fopen(logFilePath, "a");
    if (!file) return;

    // إضافة التوقيت بالثواني والملي ثانية
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
    fflush(file); // تفريغ التخزين المؤقت فوراً لضمان الكتابة على القرص
    fclose(file);
}

// 2. دالة تنفذ داخل الخيط المستقل لفحص الذاكرة
static void* LibraryCheckThread(void* arg)
{
    const char* libName = (const char*)arg;
    LogTrace("LibraryCheckThread: Started. Monitoring memory for %s...", libName);

    unsigned long attempts = 0;
    while (true)
    {
        attempts++;
        void* handle = dlopen(libName, RTLD_NOLOAD);
        if (handle)
        {
            dlclose(handle);
            LogTrace("LibraryCheckThread: Target library %s detected and stable after %lu attempts!", libName, attempts);
            break;
        }
        
        if (attempts % 10 == 0) // تسجيل تنبيه كل ثانية لعدم إغراق الملف
        {
            LogTrace("LibraryCheckThread: Still waiting for %s... (Attempt %lu)", libName, attempts);
        }

        usleep(100000); // 100ms
    }

    LogTrace("LibraryCheckThread: Exiting check thread successfully.");
    pthread_exit(NULL);
    return NULL;
}

// 3. دالة مساعدة لإنشاء الخيط
static void WaitForLibraryInThread(const char* libName)
{
    LogTrace("WaitForLibraryInThread: Creating POSIX thread...");
    pthread_t threadId;
    if (pthread_create(&threadId, NULL, LibraryCheckThread, (void*)libName) == 0)
    {
        LogTrace("WaitForLibraryInThread: Thread created successfully (ID: %lu). Waiting for join...", (unsigned long)threadId);
        pthread_join(threadId, NULL);
        LogTrace("WaitForLibraryInThread: Thread rejoined main execution.");
    }
    else
    {
        LogTrace("WaitForLibraryInThread: ERROR - Failed to create thread! Falling back to inline loop.");
        while (!dlopen(libName, RTLD_NOLOAD))
        {
            usleep(100000);
        }
        LogTrace("WaitForLibraryInThread: Inline loop detected library.");
    }
}

// 4. دالة StartAMLRightNow الرئيسية
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

    /* Create Directories */
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

    // البدء الفعلي لتسجيل الملف بعد التأكد من إنشاء المجلدات
    LogTrace("--- Starting New Launch Session ---");
    LogTrace("Environment Initialized. App: %s", g_szAppName);
    LogTrace("Mods directory path: %s", g_szModsDir);

    /* root/data/data Folder */
    jobject filesDir = GetFilesDir(g_env, appContext);
    jstring filesPath = GetAbsolutePath(g_env, filesDir);
    if(!CopyJStringUTF(g_env, filesPath, g_szDataDirPath, sizeof(g_szDataDirPath)))
    {
        LogTrace("FATAL: Failed to determine app data path.");
        if(filesPath) g_env->DeleteLocalRef(filesPath);
        if(filesDir) g_env->DeleteLocalRef(filesDir);
        return;
    }
    if(filesPath) g_env->DeleteLocalRef(filesPath);
    if(filesDir) g_env->DeleteLocalRef(filesDir);

    /* AML Config */
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

    cfg->Save();

    /* Handlers & Log Hooks */
    if(cfg->GetBool("SignalHandler", true))
    {
        g_pAML->AddFeature("SIGNAL");
        StartSignalHandler();
        LogTrace("SignalHandler registered.");
    }

    bAndroidLog_OnlyImportant = !cfg->GetBool("PrintLogsToFile_Verbose", false);
    bAndroidLog_NoAfter = cfg->GetBool("PrintLogsToFile_NoLogCat", false);
    if(cfg->GetBool("PrintLogsToFile", false))
    {
        g_pAML->AddFeature("LOGHOOK");
        HookALog();
        LogTrace("LogHook registered.");
    }

    // =================================================================
    // تشغيل خيط الفحص والانتظار مع تسجيلة في الملف
    // =================================================================
    LogTrace("Initiating library waiting process...");
    WaitForLibraryInThread(TARGET_GAME_LIB);
    LogTrace("Library check passed. Resuming AML operations.");

    /* Mods loading */
    LogTrace("Starting mods loading process...");
    #ifdef __IL2CPPUTILS
        LogTrace("IL2CPP: Initializing IL2CPP-Utils...");
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
        LogTrace("Mods loading skipped (DontLoadMods option is set).");
    }

    /* Dependencies check */
    LogTrace("Processing mod dependencies...");
    modlist->ProcessDependencies();
    
    if(bEnableUpdater)
    {
        LogTrace("Processing mod updates...");
        g_pAML->AddFeature("UPDATER");
        modlist->ProcessUpdater();
    }

    if(!g_bNoMods)
    {
        LogTrace("Executing ProcessPreLoading...");
        modlist->ProcessPreLoading();
        
        LogTrace("Executing ProcessLoading...");
        modlist->ProcessLoading();
        
        LogTrace("Executing OnAllModsLoaded...");
        modlist->OnAllModsLoaded();
        LogTrace("All mods loaded and initialized successfully!");
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

    LogTrace("AML startup process completed.");
    g_bAMLStarted = true;
}
