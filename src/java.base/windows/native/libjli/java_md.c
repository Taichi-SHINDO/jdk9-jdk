/*
 * Copyright (c) 1997, 2014, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include <windows.h>
#include <io.h>
#include <process.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <wtypes.h>
#include <commctrl.h>

#include <jni.h>
#include "java.h"

#define JVM_DLL "jvm.dll"
#define JAVA_DLL "java.dll"

/*
 * Prototypes.
 */
static jboolean GetPublicJREHome(char *path, jint pathsize);
static jboolean GetJVMPath(const char *jrepath, const char *jvmtype,
                           char *jvmpath, jint jvmpathsize);
static jboolean GetJREPath(char *path, jint pathsize);

/* We supports warmup for UI stack that is performed in parallel
 * to VM initialization.
 * This helps to improve startup of UI application as warmup phase
 * might be long due to initialization of OS or hardware resources.
 * It is not CPU bound and therefore it does not interfere with VM init.
 * Obviously such warmup only has sense for UI apps and therefore it needs
 * to be explicitly requested by passing -Dsun.awt.warmup=true property
 * (this is always the case for plugin/javaws).
 *
 * Implementation launches new thread after VM starts and use it to perform
 * warmup code (platform dependent).
 * This thread is later reused as AWT toolkit thread as graphics toolkit
 * often assume that they are used from the same thread they were launched on.
 *
 * At the moment we only support warmup for D3D. It only possible on windows
 * and only if other flags do not prohibit this (e.g. OpenGL support requested).
 */
#undef ENABLE_AWT_PRELOAD
#ifndef JAVA_ARGS /* turn off AWT preloading for javac, jar, etc */
    /* CR6999872: fastdebug crashes if awt library is loaded before JVM is
     * initialized*/
    #if !defined(DEBUG)
        #define ENABLE_AWT_PRELOAD
    #endif
#endif

#ifdef ENABLE_AWT_PRELOAD
/* "AWT was preloaded" flag;
 * turned on by AWTPreload().
 */
int awtPreloaded = 0;

/* Calls a function with the name specified
 * the function must be int(*fn)(void).
 */
int AWTPreload(const char *funcName);
/* stops AWT preloading */
void AWTPreloadStop();

/* D3D preloading */
/* -1: not initialized; 0: OFF, 1: ON */
int awtPreloadD3D = -1;
/* command line parameter to swith D3D preloading on */
#define PARAM_PRELOAD_D3D "-Dsun.awt.warmup"
/* D3D/OpenGL management parameters */
#define PARAM_NODDRAW "-Dsun.java2d.noddraw"
#define PARAM_D3D "-Dsun.java2d.d3d"
#define PARAM_OPENGL "-Dsun.java2d.opengl"
/* funtion in awt.dll (src/windows/native/sun/java2d/d3d/D3DPipelineManager.cpp) */
#define D3D_PRELOAD_FUNC "preloadD3D"

/* Extracts value of a parameter with the specified name
 * from command line argument (returns pointer in the argument).
 * Returns NULL if the argument does not contains the parameter.
 * e.g.:
 * GetParamValue("theParam", "theParam=value") returns pointer to "value".
 */
const char * GetParamValue(const char *paramName, const char *arg) {
    int nameLen = JLI_StrLen(paramName);
    if (JLI_StrNCmp(paramName, arg, nameLen) == 0) {
        /* arg[nameLen] is valid (may contain final NULL) */
        if (arg[nameLen] == '=') {
            return arg + nameLen + 1;
        }
    }
    return NULL;
}

/* Checks if commandline argument contains property specified
 * and analyze it as boolean property (true/false).
 * Returns -1 if the argument does not contain the parameter;
 * Returns 1 if the argument contains the parameter and its value is "true";
 * Returns 0 if the argument contains the parameter and its value is "false".
 */
int GetBoolParamValue(const char *paramName, const char *arg) {
    const char * paramValue = GetParamValue(paramName, arg);
    if (paramValue != NULL) {
        if (JLI_StrCaseCmp(paramValue, "true") == 0) {
            return 1;
        }
        if (JLI_StrCaseCmp(paramValue, "false") == 0) {
            return 0;
        }
    }
    return -1;
}
#endif /* ENABLE_AWT_PRELOAD */


static jboolean _isjavaw = JNI_FALSE;


jboolean
IsJavaw()
{
    return _isjavaw;
}

/*
 * Returns the arch path, to get the current arch use the
 * macro GetArch, nbits here is ignored for now.
 */
const char *
GetArchPath(int nbits)
{
#ifdef _M_AMD64
    return "amd64";
#elif defined(_M_IA64)
    return "ia64";
#else
    return "i386";
#endif
}

/*
 *
 */
void
CreateExecutionEnvironment(int *pargc, char ***pargv,
                           char *jrepath, jint so_jrepath,
                           char *jvmpath, jint so_jvmpath,
                           char *jvmcfg,  jint so_jvmcfg) {
    char * jvmtype;
    int i = 0;
    int running = CURRENT_DATA_MODEL;

    int wanted = running;

    char** argv = *pargv;
    for (i = 1; i < *pargc ; i++) {
        if (JLI_StrCmp(argv[i], "-J-d64") == 0 || JLI_StrCmp(argv[i], "-d64") == 0) {
            wanted = 64;
            continue;
        }
        if (JLI_StrCmp(argv[i], "-J-d32") == 0 || JLI_StrCmp(argv[i], "-d32") == 0) {
            wanted = 32;
            continue;
        }

        if (IsJavaArgs() && argv[i][0] != '-')
            continue;
        if (argv[i][0] != '-')
            break;
    }
    if (running != wanted) {
        JLI_ReportErrorMessage(JRE_ERROR2, wanted);
        exit(1);
    }

    /* Find out where the JRE is that we will be using. */
    if (!GetJREPath(jrepath, so_jrepath)) {
        JLI_ReportErrorMessage(JRE_ERROR1);
        exit(2);
    }

    JLI_Snprintf(jvmcfg, so_jvmcfg, "%s%slib%s%s%sjvm.cfg",
        jrepath, FILESEP, FILESEP, (char*)GetArch(), FILESEP);

    /* Find the specified JVM type */
    if (ReadKnownVMs(jvmcfg, JNI_FALSE) < 1) {
        JLI_ReportErrorMessage(CFG_ERROR7);
        exit(1);
    }

    jvmtype = CheckJvmType(pargc, pargv, JNI_FALSE);
    if (JLI_StrCmp(jvmtype, "ERROR") == 0) {
        JLI_ReportErrorMessage(CFG_ERROR9);
        exit(4);
    }

    jvmpath[0] = '\0';
    if (!GetJVMPath(jrepath, jvmtype, jvmpath, so_jvmpath)) {
        JLI_ReportErrorMessage(CFG_ERROR8, jvmtype, jvmpath);
        exit(4);
    }
    /* If we got here, jvmpath has been correctly initialized. */

    /* Check if we need preload AWT */
#ifdef ENABLE_AWT_PRELOAD
    argv = *pargv;
    for (i = 0; i < *pargc ; i++) {
        /* Tests the "turn on" parameter only if not set yet. */
        if (awtPreloadD3D < 0) {
            if (GetBoolParamValue(PARAM_PRELOAD_D3D, argv[i]) == 1) {
                awtPreloadD3D = 1;
            }
        }
        /* Test parameters which can disable preloading if not already disabled. */
        if (awtPreloadD3D != 0) {
            if (GetBoolParamValue(PARAM_NODDRAW, argv[i]) == 1
                || GetBoolParamValue(PARAM_D3D, argv[i]) == 0
                || GetBoolParamValue(PARAM_OPENGL, argv[i]) == 1)
            {
                awtPreloadD3D = 0;
                /* no need to test the rest of the parameters */
                break;
            }
        }
    }
#endif /* ENABLE_AWT_PRELOAD */
}


static jboolean
LoadMSVCRT()
{
    // Only do this once
    static int loaded = 0;
    char crtpath[MAXPATHLEN];

    if (!loaded) {
        /*
         * The Microsoft C Runtime Library needs to be loaded first.  A copy is
         * assumed to be present in the "JRE path" directory.  If it is not found
         * there (or "JRE path" fails to resolve), skip the explicit load and let
         * nature take its course, which is likely to be a failure to execute.
         * This is clearly completely specific to the exact compiler version
         * which isn't very nice, but its hardly the only place.
         * No attempt to look for compiler versions in between 2003 and 2010
         * as we aren't supporting building with those.
         */
#ifdef _MSC_VER
#if _MSC_VER < 1400
#define CRT_DLL "msvcr71.dll"
#endif
#if _MSC_VER >= 1600
#define CRT_DLL "msvcr100.dll"
#endif
#ifdef CRT_DLL
        if (GetJREPath(crtpath, MAXPATHLEN)) {
            if (JLI_StrLen(crtpath) + JLI_StrLen("\\bin\\") +
                    JLI_StrLen(CRT_DLL) >= MAXPATHLEN) {
                JLI_ReportErrorMessage(JRE_ERROR11);
                return JNI_FALSE;
            }
            (void)JLI_StrCat(crtpath, "\\bin\\" CRT_DLL);   /* Add crt dll */
            JLI_TraceLauncher("CRT path is %s\n", crtpath);
            if (_access(crtpath, 0) == 0) {
                if (LoadLibrary(crtpath) == 0) {
                    JLI_ReportErrorMessage(DLL_ERROR4, crtpath);
                    return JNI_FALSE;
                }
            }
        }
#endif /* CRT_DLL */
#endif /* _MSC_VER */
        loaded = 1;
    }
    return JNI_TRUE;
}


/*
 * Find path to JRE based on .exe's location or registry settings.
 */
jboolean
GetJREPath(char *path, jint pathsize)
{
    char javadll[MAXPATHLEN];
    struct stat s;

    if (GetApplicationHome(path, pathsize)) {
        /* Is JRE co-located with the application? */
        JLI_Snprintf(javadll, sizeof(javadll), "%s\\bin\\" JAVA_DLL, path);
        if (stat(javadll, &s) == 0) {
            JLI_TraceLauncher("JRE path is %s\n", path);
            return JNI_TRUE;
        }

        /* Does this app ship a private JRE in <apphome>\jre directory? */
        JLI_Snprintf(javadll, sizeof (javadll), "%s\\jre\\bin\\" JAVA_DLL, path);
        if (stat(javadll, &s) == 0) {
            JLI_StrCat(path, "\\jre");
            JLI_TraceLauncher("JRE path is %s\n", path);
            return JNI_TRUE;
        }
    }

    /* Look for a public JRE on this machine. */
    if (GetPublicJREHome(path, pathsize)) {
        JLI_TraceLauncher("JRE path is %s\n", path);
        return JNI_TRUE;
    }

    JLI_ReportErrorMessage(JRE_ERROR8 JAVA_DLL);
    return JNI_FALSE;

}

/*
 * Given a JRE location and a JVM type, construct what the name the
 * JVM shared library will be.  Return true, if such a library
 * exists, false otherwise.
 */
static jboolean
GetJVMPath(const char *jrepath, const char *jvmtype,
           char *jvmpath, jint jvmpathsize)
{
    struct stat s;
    if (JLI_StrChr(jvmtype, '/') || JLI_StrChr(jvmtype, '\\')) {
        JLI_Snprintf(jvmpath, jvmpathsize, "%s\\" JVM_DLL, jvmtype);
    } else {
        JLI_Snprintf(jvmpath, jvmpathsize, "%s\\bin\\%s\\" JVM_DLL,
                     jrepath, jvmtype);
    }
    if (stat(jvmpath, &s) == 0) {
        return JNI_TRUE;
    } else {
        return JNI_FALSE;
    }
}

/*
 * Load a jvm from "jvmpath" and initialize the invocation functions.
 */
jboolean
LoadJavaVM(const char *jvmpath, InvocationFunctions *ifn)
{
    HINSTANCE handle;

    JLI_TraceLauncher("JVM path is %s\n", jvmpath);

    /*
     * The Microsoft C Runtime Library needs to be loaded first.  A copy is
     * assumed to be present in the "JRE path" directory.  If it is not found
     * there (or "JRE path" fails to resolve), skip the explicit load and let
     * nature take its course, which is likely to be a failure to execute.
     *
     */
    LoadMSVCRT();

    /* Load the Java VM DLL */
    if ((handle = LoadLibrary(jvmpath)) == 0) {
        JLI_ReportErrorMessage(DLL_ERROR4, (char *)jvmpath);
        return JNI_FALSE;
    }

    /* Now get the function addresses */
    ifn->CreateJavaVM =
        (void *)GetProcAddress(handle, "JNI_CreateJavaVM");
    ifn->GetDefaultJavaVMInitArgs =
        (void *)GetProcAddress(handle, "JNI_GetDefaultJavaVMInitArgs");
    if (ifn->CreateJavaVM == 0 || ifn->GetDefaultJavaVMInitArgs == 0) {
        JLI_ReportErrorMessage(JNI_ERROR1, (char *)jvmpath);
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

/*
 * If app is "c:\foo\bin\javac", then put "c:\foo" into buf.
 */
jboolean
GetApplicationHome(char *buf, jint bufsize)
{
    char *cp;
    GetModuleFileName(0, buf, bufsize);
    *JLI_StrRChr(buf, '\\') = '\0'; /* remove .exe file name */
    if ((cp = JLI_StrRChr(buf, '\\')) == 0) {
        /* This happens if the application is in a drive root, and
         * there is no bin directory. */
        buf[0] = '\0';
        return JNI_FALSE;
    }
    *cp = '\0';  /* remove the bin\ part */
    return JNI_TRUE;
}

/*
 * Helpers to look in the registry for a public JRE.
 */
                    /* Same for 1.5.0, 1.5.1, 1.5.2 etc. */
#define JRE_KEY     "Software\\JavaSoft\\Java Runtime Environment"

static jboolean
GetStringFromRegistry(HKEY key, const char *name, char *buf, jint bufsize)
{
    DWORD type, size;

    if (RegQueryValueEx(key, name, 0, &type, 0, &size) == 0
        && type == REG_SZ
        && (size < (unsigned int)bufsize)) {
        if (RegQueryValueEx(key, name, 0, 0, buf, &size) == 0) {
            return JNI_TRUE;
        }
    }
    return JNI_FALSE;
}

static jboolean
GetPublicJREHome(char *buf, jint bufsize)
{
    HKEY key, subkey;
    char version[MAXPATHLEN];

    /*
     * Note: There is a very similar implementation of the following
     * registry reading code in the Windows java control panel (javacp.cpl).
     * If there are bugs here, a similar bug probably exists there.  Hence,
     * changes here require inspection there.
     */

    /* Find the current version of the JRE */
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, JRE_KEY, 0, KEY_READ, &key) != 0) {
        JLI_ReportErrorMessage(REG_ERROR1, JRE_KEY);
        return JNI_FALSE;
    }

    if (!GetStringFromRegistry(key, "CurrentVersion",
                               version, sizeof(version))) {
        JLI_ReportErrorMessage(REG_ERROR2, JRE_KEY);
        RegCloseKey(key);
        return JNI_FALSE;
    }

    if (JLI_StrCmp(version, GetDotVersion()) != 0) {
        JLI_ReportErrorMessage(REG_ERROR3, JRE_KEY, version, GetDotVersion()
        );
        RegCloseKey(key);
        return JNI_FALSE;
    }

    /* Find directory where the current version is installed. */
    if (RegOpenKeyEx(key, version, 0, KEY_READ, &subkey) != 0) {
        JLI_ReportErrorMessage(REG_ERROR1, JRE_KEY, version);
        RegCloseKey(key);
        return JNI_FALSE;
    }

    if (!GetStringFromRegistry(subkey, "JavaHome", buf, bufsize)) {
        JLI_ReportErrorMessage(REG_ERROR4, JRE_KEY, version);
        RegCloseKey(key);
        RegCloseKey(subkey);
        return JNI_FALSE;
    }

    if (JLI_IsTraceLauncher()) {
        char micro[MAXPATHLEN];
        if (!GetStringFromRegistry(subkey, "MicroVersion", micro,
                                   sizeof(micro))) {
            printf("Warning: Can't read MicroVersion\n");
            micro[0] = '\0';
        }
        printf("Version major.minor.micro = %s.%s\n", version, micro);
    }

    RegCloseKey(key);
    RegCloseKey(subkey);
    return JNI_TRUE;
}

/*
 * Support for doing cheap, accurate interval timing.
 */
static jboolean counterAvailable = JNI_FALSE;
static jboolean counterInitialized = JNI_FALSE;
static LARGE_INTEGER counterFrequency;

jlong CounterGet()
{
    LARGE_INTEGER count;

    if (!counterInitialized) {
        counterAvailable = QueryPerformanceFrequency(&counterFrequency);
        counterInitialized = JNI_TRUE;
    }
    if (!counterAvailable) {
        return 0;
    }
    QueryPerformanceCounter(&count);
    return (jlong)(count.QuadPart);
}

jlong Counter2Micros(jlong counts)
{
    if (!counterAvailable || !counterInitialized) {
        return 0;
    }
    return (counts * 1000 * 1000)/counterFrequency.QuadPart;
}
/*
 * windows snprintf does not guarantee a null terminator in the buffer,
 * if the computed size is equal to or greater than the buffer size,
 * as well as error conditions. This function guarantees a null terminator
 * under all these conditions. An unreasonable buffer or size will return
 * an error value. Under all other conditions this function will return the
 * size of the bytes actually written minus the null terminator, similar
 * to ansi snprintf api. Thus when calling this function the caller must
 * ensure storage for the null terminator.
 */
int
JLI_Snprintf(char* buffer, size_t size, const char* format, ...) {
    int rc;
    va_list vl;
    if (size == 0 || buffer == NULL)
        return -1;
    buffer[0] = '\0';
    va_start(vl, format);
    rc = vsnprintf(buffer, size, format, vl);
    va_end(vl);
    /* force a null terminator, if something is amiss */
    if (rc < 0) {
        /* apply ansi semantics */
        buffer[size - 1] = '\0';
        return size;
    } else if (rc == size) {
        /* force a null terminator */
        buffer[size - 1] = '\0';
    }
    return rc;
}

void
JLI_ReportErrorMessage(const char* fmt, ...) {
    va_list vl;
    va_start(vl,fmt);

    if (IsJavaw()) {
        char *message;

        /* get the length of the string we need */
        int n = _vscprintf(fmt, vl);

        message = (char *)JLI_MemAlloc(n + 1);
        _vsnprintf(message, n, fmt, vl);
        message[n]='\0';
        MessageBox(NULL, message, "Java Virtual Machine Launcher",
            (MB_OK|MB_ICONSTOP|MB_APPLMODAL));
        JLI_MemFree(message);
    } else {
        vfprintf(stderr, fmt, vl);
        fprintf(stderr, "\n");
    }
    va_end(vl);
}

/*
 * Just like JLI_ReportErrorMessage, except that it concatenates the system
 * error message if any, its upto the calling routine to correctly
 * format the separation of the messages.
 */
void
JLI_ReportErrorMessageSys(const char *fmt, ...)
{
    va_list vl;

    int save_errno = errno;
    DWORD       errval;
    jboolean freeit = JNI_FALSE;
    char  *errtext = NULL;

    va_start(vl, fmt);

    if ((errval = GetLastError()) != 0) {               /* Platform SDK / DOS Error */
        int n = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM|
            FORMAT_MESSAGE_IGNORE_INSERTS|FORMAT_MESSAGE_ALLOCATE_BUFFER,
            NULL, errval, 0, (LPTSTR)&errtext, 0, NULL);
        if (errtext == NULL || n == 0) {                /* Paranoia check */
            errtext = "";
            n = 0;
        } else {
            freeit = JNI_TRUE;
            if (n > 2) {                                /* Drop final CR, LF */
                if (errtext[n - 1] == '\n') n--;
                if (errtext[n - 1] == '\r') n--;
                errtext[n] = '\0';
            }
        }
    } else {   /* C runtime error that has no corresponding DOS error code */
        errtext = strerror(save_errno);
    }

    if (IsJavaw()) {
        char *message;
        int mlen;
        /* get the length of the string we need */
        int len = mlen =  _vscprintf(fmt, vl) + 1;
        if (freeit) {
           mlen += (int)JLI_StrLen(errtext);
        }

        message = (char *)JLI_MemAlloc(mlen);
        _vsnprintf(message, len, fmt, vl);
        message[len]='\0';

        if (freeit) {
           JLI_StrCat(message, errtext);
        }

        MessageBox(NULL, message, "Java Virtual Machine Launcher",
            (MB_OK|MB_ICONSTOP|MB_APPLMODAL));

        JLI_MemFree(message);
    } else {
        vfprintf(stderr, fmt, vl);
        if (freeit) {
           fprintf(stderr, "%s", errtext);
        }
    }
    if (freeit) {
        (void)LocalFree((HLOCAL)errtext);
    }
    va_end(vl);
}

void  JLI_ReportExceptionDescription(JNIEnv * env) {
    if (IsJavaw()) {
       /*
        * This code should be replaced by code which opens a window with
        * the exception detail message, for now atleast put a dialog up.
        */
        MessageBox(NULL, "A Java Exception has occurred.", "Java Virtual Machine Launcher",
               (MB_OK|MB_ICONSTOP|MB_APPLMODAL));
    } else {
        (*env)->ExceptionDescribe(env);
    }
}

jboolean
ServerClassMachine() {
    return (GetErgoPolicy() == ALWAYS_SERVER_CLASS) ? JNI_TRUE : JNI_FALSE;
}

/*
 * Wrapper for platform dependent unsetenv function.
 */
int
UnsetEnv(char *name)
{
    int ret;
    char *buf = JLI_MemAlloc(JLI_StrLen(name) + 2);
    buf = JLI_StrCat(JLI_StrCpy(buf, name), "=");
    ret = _putenv(buf);
    JLI_MemFree(buf);
    return (ret);
}

/* --- Splash Screen shared library support --- */

static const char* SPLASHSCREEN_SO = "\\bin\\splashscreen.dll";

static HMODULE hSplashLib = NULL;

void* SplashProcAddress(const char* name) {
    char libraryPath[MAXPATHLEN]; /* some extra space for JLI_StrCat'ing SPLASHSCREEN_SO */

    if (!GetJREPath(libraryPath, MAXPATHLEN)) {
        return NULL;
    }
    if (JLI_StrLen(libraryPath)+JLI_StrLen(SPLASHSCREEN_SO) >= MAXPATHLEN) {
        return NULL;
    }
    JLI_StrCat(libraryPath, SPLASHSCREEN_SO);

    if (!hSplashLib) {
        hSplashLib = LoadLibrary(libraryPath);
    }
    if (hSplashLib) {
        return GetProcAddress(hSplashLib, name);
    } else {
        return NULL;
    }
}

void SplashFreeLibrary() {
    if (hSplashLib) {
        FreeLibrary(hSplashLib);
        hSplashLib = NULL;
    }
}

const char *
jlong_format_specifier() {
    return "%I64d";
}

/*
 * Block current thread and continue execution in a new thread
 */
int
ContinueInNewThread0(int (JNICALL *continuation)(void *), jlong stack_size, void * args) {
    int rslt = 0;
    unsigned thread_id;

#ifndef STACK_SIZE_PARAM_IS_A_RESERVATION
#define STACK_SIZE_PARAM_IS_A_RESERVATION  (0x10000)
#endif

    /*
     * STACK_SIZE_PARAM_IS_A_RESERVATION is what we want, but it's not
     * supported on older version of Windows. Try first with the flag; and
     * if that fails try again without the flag. See MSDN document or HotSpot
     * source (os_win32.cpp) for details.
     */
    HANDLE thread_handle =
      (HANDLE)_beginthreadex(NULL,
                             (unsigned)stack_size,
                             continuation,
                             args,
                             STACK_SIZE_PARAM_IS_A_RESERVATION,
                             &thread_id);
    if (thread_handle == NULL) {
      thread_handle =
      (HANDLE)_beginthreadex(NULL,
                             (unsigned)stack_size,
                             continuation,
                             args,
                             0,
                             &thread_id);
    }

    /* AWT preloading (AFTER main thread start) */
#ifdef ENABLE_AWT_PRELOAD
    /* D3D preloading */
    if (awtPreloadD3D != 0) {
        char *envValue;
        /* D3D routines checks env.var J2D_D3D if no appropriate
         * command line params was specified
         */
        envValue = getenv("J2D_D3D");
        if (envValue != NULL && JLI_StrCaseCmp(envValue, "false") == 0) {
            awtPreloadD3D = 0;
        }
        /* Test that AWT preloading isn't disabled by J2D_D3D_PRELOAD env.var */
        envValue = getenv("J2D_D3D_PRELOAD");
        if (envValue != NULL && JLI_StrCaseCmp(envValue, "false") == 0) {
            awtPreloadD3D = 0;
        }
        if (awtPreloadD3D < 0) {
            /* If awtPreloadD3D is still undefined (-1), test
             * if it is turned on by J2D_D3D_PRELOAD env.var.
             * By default it's turned OFF.
             */
            awtPreloadD3D = 0;
            if (envValue != NULL && JLI_StrCaseCmp(envValue, "true") == 0) {
                awtPreloadD3D = 1;
            }
         }
    }
    if (awtPreloadD3D) {
        AWTPreload(D3D_PRELOAD_FUNC);
    }
#endif /* ENABLE_AWT_PRELOAD */

    if (thread_handle) {
      WaitForSingleObject(thread_handle, INFINITE);
      GetExitCodeThread(thread_handle, &rslt);
      CloseHandle(thread_handle);
    } else {
      rslt = continuation(args);
    }

#ifdef ENABLE_AWT_PRELOAD
    if (awtPreloaded) {
        AWTPreloadStop();
    }
#endif /* ENABLE_AWT_PRELOAD */

    return rslt;
}

/* Unix only, empty on windows. */
void SetJavaLauncherPlatformProps() {}

/*
 * The implementation for finding classes from the bootstrap
 * class loader, refer to java.h
 */
static FindClassFromBootLoader_t *findBootClass = NULL;

jclass FindBootStrapClass(JNIEnv *env, const char *classname)
{
   HMODULE hJvm;

   if (findBootClass == NULL) {
       hJvm = GetModuleHandle(JVM_DLL);
       if (hJvm == NULL) return NULL;
       /* need to use the demangled entry point */
       findBootClass = (FindClassFromBootLoader_t *)GetProcAddress(hJvm,
            "JVM_FindClassFromBootLoader");
       if (findBootClass == NULL) {
          JLI_ReportErrorMessage(DLL_ERROR4, "JVM_FindClassFromBootLoader");
          return NULL;
       }
   }
   return findBootClass(env, classname);
}

void
InitLauncher(boolean javaw)
{
    INITCOMMONCONTROLSEX icx;

    /*
     * Required for javaw mode MessageBox output as well as for
     * HotSpot -XX:+ShowMessageBoxOnError in java mode, an empty
     * flag field is sufficient to perform the basic UI initialization.
     */
    memset(&icx, 0, sizeof(INITCOMMONCONTROLSEX));
    icx.dwSize = sizeof(INITCOMMONCONTROLSEX);
    InitCommonControlsEx(&icx);
    _isjavaw = javaw;
    JLI_SetTraceLauncher();
}


/* ============================== */
/* AWT preloading */
#ifdef ENABLE_AWT_PRELOAD

typedef int FnPreloadStart(void);
typedef void FnPreloadStop(void);
static FnPreloadStop *fnPreloadStop = NULL;
static HMODULE hPreloadAwt = NULL;

/*
 * Starts AWT preloading
 */
int AWTPreload(const char *funcName)
{
    int result = -1;
    /* load AWT library once (if several preload function should be called) */
    if (hPreloadAwt == NULL) {
        /* awt.dll is not loaded yet */
        char libraryPath[MAXPATHLEN];
        int jrePathLen = 0;
        HMODULE hJava = NULL;
        HMODULE hVerify = NULL;

        while (1) {
            /* awt.dll depends on jvm.dll & java.dll;
             * jvm.dll is already loaded, so we need only java.dll;
             * java.dll depends on MSVCRT lib & verify.dll.
             */
            if (!GetJREPath(libraryPath, MAXPATHLEN)) {
                break;
            }

            /* save path length */
            jrePathLen = JLI_StrLen(libraryPath);

            if (jrePathLen + JLI_StrLen("\\bin\\verify.dll") >= MAXPATHLEN) {
              /* jre path is too long, the library path will not fit there;
               * report and abort preloading
               */
              JLI_ReportErrorMessage(JRE_ERROR11);
              break;
            }

            /* load msvcrt 1st */
            LoadMSVCRT();

            /* load verify.dll */
            JLI_StrCat(libraryPath, "\\bin\\verify.dll");
            hVerify = LoadLibrary(libraryPath);
            if (hVerify == NULL) {
                break;
            }

            /* restore jrePath */
            libraryPath[jrePathLen] = 0;
            /* load java.dll */
            JLI_StrCat(libraryPath, "\\bin\\" JAVA_DLL);
            hJava = LoadLibrary(libraryPath);
            if (hJava == NULL) {
                break;
            }

            /* restore jrePath */
            libraryPath[jrePathLen] = 0;
            /* load awt.dll */
            JLI_StrCat(libraryPath, "\\bin\\awt.dll");
            hPreloadAwt = LoadLibrary(libraryPath);
            if (hPreloadAwt == NULL) {
                break;
            }

            /* get "preloadStop" func ptr */
            fnPreloadStop = (FnPreloadStop *)GetProcAddress(hPreloadAwt, "preloadStop");

            break;
        }
    }

    if (hPreloadAwt != NULL) {
        FnPreloadStart *fnInit = (FnPreloadStart *)GetProcAddress(hPreloadAwt, funcName);
        if (fnInit != NULL) {
            /* don't forget to stop preloading */
            awtPreloaded = 1;

            result = fnInit();
        }
    }

    return result;
}

/*
 * Terminates AWT preloading
 */
void AWTPreloadStop() {
    if (fnPreloadStop != NULL) {
        fnPreloadStop();
    }
}

#endif /* ENABLE_AWT_PRELOAD */

int
JVMInit(InvocationFunctions* ifn, jlong threadStackSize,
        int argc, char **argv,
        int mode, char *what, int ret)
{
    ShowSplashScreen();
    return ContinueInNewThread(ifn, threadStackSize, argc, argv, mode, what, ret);
}

void
PostJVMInit(JNIEnv *env, jstring mainClass, JavaVM *vm)
{
    // stubbed out for windows and *nixes.
}

void
RegisterThread()
{
    // stubbed out for windows and *nixes.
}

/*
 * on windows, we return a false to indicate this option is not applicable
 */
jboolean
ProcessPlatformOption(const char *arg)
{
    return JNI_FALSE;
}

/*
 * At this point we have the arguments to the application, and we need to
 * check with original stdargs in order to compare which of these truly
 * needs expansion. cmdtoargs will specify this if it finds a bare
 * (unquoted) argument containing a glob character(s) ie. * or ?
 */
jobjectArray
CreateApplicationArgs(JNIEnv *env, char **strv, int argc)
{
    int i, j, idx, tlen;
    jobjectArray outArray, inArray;
    char *ostart, *astart, **nargv;
    jboolean needs_expansion = JNI_FALSE;
    jmethodID mid;
    int stdargc;
    StdArg *stdargs;
    jclass cls = GetLauncherHelperClass(env);
    NULL_CHECK0(cls);

    if (argc == 0) {
        return NewPlatformStringArray(env, strv, argc);
    }
    // the holy grail we need to compare with.
    stdargs = JLI_GetStdArgs();
    stdargc = JLI_GetStdArgc();

    // sanity check, this should never happen
    if (argc > stdargc) {
        JLI_TraceLauncher("Warning: app args is larger than the original, %d %d\n", argc, stdargc);
        JLI_TraceLauncher("passing arguments as-is.\n");
        return NewPlatformStringArray(env, strv, argc);
    }

    // sanity check, match the args we have, to the holy grail
    idx = stdargc - argc;
    ostart = stdargs[idx].arg;
    astart = strv[0];
    // sanity check, ensure that the first argument of the arrays are the same
    if (JLI_StrCmp(ostart, astart) != 0) {
        // some thing is amiss the args don't match
        JLI_TraceLauncher("Warning: app args parsing error\n");
        JLI_TraceLauncher("passing arguments as-is\n");
        return NewPlatformStringArray(env, strv, argc);
    }

    // make a copy of the args which will be expanded in java if required.
    nargv = (char **)JLI_MemAlloc(argc * sizeof(char*));
    for (i = 0, j = idx; i < argc; i++, j++) {
        jboolean arg_expand = (JLI_StrCmp(stdargs[j].arg, strv[i]) == 0)
                                ? stdargs[j].has_wildcard
                                : JNI_FALSE;
        if (needs_expansion == JNI_FALSE)
            needs_expansion = arg_expand;

        // indicator char + String + NULL terminator, the java method will strip
        // out the first character, the indicator character, so no matter what
        // we add the indicator
        tlen = 1 + JLI_StrLen(strv[i]) + 1;
        nargv[i] = (char *) JLI_MemAlloc(tlen);
        if (JLI_Snprintf(nargv[i], tlen, "%c%s", arg_expand ? 'T' : 'F',
                         strv[i]) < 0) {
            return NULL;
        }
        JLI_TraceLauncher("%s\n", nargv[i]);
    }

    if (!needs_expansion) {
        // clean up any allocated memory and return back the old arguments
        for (i = 0 ; i < argc ; i++) {
            JLI_MemFree(nargv[i]);
        }
        JLI_MemFree(nargv);
        return NewPlatformStringArray(env, strv, argc);
    }
    NULL_CHECK0(mid = (*env)->GetStaticMethodID(env, cls,
                                                "expandArgs",
                                                "([Ljava/lang/String;)[Ljava/lang/String;"));

    // expand the arguments that require expansion, the java method will strip
    // out the indicator character.
    NULL_CHECK0(inArray = NewPlatformStringArray(env, nargv, argc));
    outArray = (*env)->CallStaticObjectMethod(env, cls, mid, inArray);
    for (i = 0; i < argc; i++) {
        JLI_MemFree(nargv[i]);
    }
    JLI_MemFree(nargv);
    return outArray;
}
