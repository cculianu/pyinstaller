/*
 * ****************************************************************************
 * Copyright (c) 2013-2018, PyInstaller Development Team.
 * Distributed under the terms of the GNU General Public License with exception
 * for distributing bootloader.
 *
 * The full license is in the file COPYING.txt, distributed with this software.
 * ****************************************************************************
 */

/*
 * Portable wrapper for some utility functions like getenv/setenv,
 * file path manipulation and other shared data types or functions.
 */

/* TODO: use safe string functions */
#define _CRT_SECURE_NO_WARNINGS 1

#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>  /* _mkdir, _rmdir */
    #include <io.h>      /* _finddata_t */
    #include <process.h> /* getpid */
    #include <signal.h>  /* signal */
#else
    #include <dirent.h>
/*
 * On AIX  RTLD_MEMBER  flag is only visible when _ALL_SOURCE flag is defined.
 *
 * There are quite a few issues with xlC compiler. GCC is much better,
 * Without flag _ALL_SOURCE gcc get stuck on the RTLD_MEMBER flax when
 * compiling the bootloader.
 * This fix was tested wigh gcc on AIX6.1.
 */
    #if defined(AIX) && !defined(_ALL_SOURCE)
        #define _ALL_SOURCE
        #include <dlfcn.h>
        #undef  _ALL_SOURCE
    #else
        #include <dlfcn.h>
    #endif
    #include <limits.h>  /* PATH_MAX */
    #include <signal.h>  /* kill, */
    #include <sys/wait.h>
    #include <unistd.h>  /* rmdir, unlink, mkdtemp */
#endif /* ifdef _WIN32 */
#ifndef SIGCLD
#define SIGCLD SIGCHLD /* not defined on OS X */
#endif
#ifndef sighandler_t
typedef void (*sighandler_t)(int);
#endif
#include <errno.h>
#include <stddef.h> /* ptrdiff_t */
#include <stdio.h>  /* FILE */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* struct stat */
#include <wchar.h>    /* wchar_t */
#if defined(__APPLE__) && defined(WINDOWED)
    #include <Carbon/Carbon.h>  /* AppleEventsT */
    #include <ApplicationServices/ApplicationServices.h> /* GetProcessForPID, etc */
    extern Boolean ConvertEventRefToEventRecord(EventRef inEvent, EventRecord *outEvent); /* Not declared in headers but exists in libs */
#endif

/*
 * Function 'mkdtemp' (make temporary directory) is missing on some *nix platforms:
 * - On Solaris function 'mkdtemp' is missing.
 * - On AIX 5.2 function 'mkdtemp' is missing. It is there in version 6.1 but we don't know
 *   the runtime platform at compile time, so we always include our own implementation on AIX.
 */
#if defined(SUNOS) || defined(AIX) || defined(HPUX)
    #if !defined(HAVE_MKDTEMP)
    #include "mkdtemp.h"
    #endif
#endif

/* PyInstaller headers. */
#include "pyi_global.h"
#include "pyi_path.h"
#include "pyi_archive.h"
#include "pyi_utils.h"
#include "pyi_win32_utils.h"

/*
 *  global variables that are used to copy argc/argv, so that PyIstaller can manipulate them
 *  if need be.  One case in which the incoming argc/argv is manipulated is in the case of
 *  Apple/Windowed, where we watch for AppleEvents in order to add files to the command line.
 *  (this is argv_emulation).  These variables must be of file global scope to be able to
 *  be accessed inside of the AppleEvents handlers.
 */
static char **argv_pyi = NULL;
static int argc_pyi = 0;

/*
 * Watch for OpenDocument AppleEvents and add the files passed in to the
 * sys.argv command line on the Python side.
 *
 * This allows on Mac OS X to open files when a file is dragged and dropped
 * on the App icon in the OS X dock.
 */
#if defined(__APPLE__) && defined(WINDOWED)
static void process_apple_events();
#endif

char *
pyi_strjoin(const char *first, const char *sep, const char *second){
    /* join first and second string, using sep as separator.
     * any of them may be either a null-terminated string or NULL.
     * sep will be only used if first and second string are not empty.
     * returns a null-terminated string which the caller is responsible
     * for freeing. Returns NULL if memory could not be allocated.
     */
    int first_len, sep_len, second_len;
    char *result;
    first_len = first ? strlen(first) : 0;
    sep_len = sep ? strlen(sep) : 0;
    second_len = second ? strlen(second) : 0;
    result = malloc(first_len + sep_len + second_len + 1);
    if (!result) {
        return NULL;
    }
    *result = '\0';
    if (first_len) {
        strcat(result, first);
    }
    if (sep_len && first_len && second_len) {
        strcat(result, sep);
    }
    if (second_len) {
        strcat(result, second);
    }
    return result;
}

/* Return string copy of environment variable. */
char *
pyi_getenv(const char *variable)
{
    char *env = NULL;

#ifdef _WIN32
    wchar_t * wenv = NULL;
    wchar_t * wvar = NULL;
    wchar_t buf1[PATH_MAX], buf2[PATH_MAX];
    DWORD rc;

    wvar = pyi_win32_utils_from_utf8(NULL, variable, 0);
    rc = GetEnvironmentVariableW(wvar, buf1, sizeof(buf1));

    if (rc > 0) {
        wenv = buf1;
        /* Expand environment variables like %VAR% in value. */
        rc = ExpandEnvironmentStringsW(wenv, buf2, sizeof(buf2));

        if (rc > 0) {
            wenv = buf1;
        }
    }

    if (wenv) {
        env = pyi_win32_utils_to_utf8(NULL, wenv, 0);
    }
#else /*
       * ifdef _WIN32
       * Standard POSIX function.
       */
    env = getenv(variable);
#endif /* ifdef _WIN32 */

    /* If the Python program we are about to run invokes another PyInstaller
     * one-file program as subprocess, this subprocess must not be fooled into
     * thinking that it is already unpacked. Therefore, PyInstaller deletes
     * the _MEIPASS2 variable from the environment in pyi_main().
     *
     * However, on some platforms (e.g. AIX) the Python function 'os.unsetenv()'
     * does not always exist. In these cases we cannot delete the _MEIPASS2
     * environment variable from Python but only set it to the empty string.
     * The code below takes into account that a variable may exist while its
     * value is only the empty string.
     *
     * Return copy of string to avoid modification of the process environment.
     */
    return (env && env[0]) ? strdup(env) : NULL;
}

/* Set environment variable. */
int
pyi_setenv(const char *variable, const char *value)
{
    int rc;

#ifdef _WIN32
    wchar_t * wvar, *wval;

    wvar = pyi_win32_utils_from_utf8(NULL, variable, 0);
    wval = pyi_win32_utils_from_utf8(NULL, value, 0);

    // Not sure why, but SetEnvironmentVariableW() didn't work with _wtempnam()
    // Replaced it with _wputenv_s()
    rc = _wputenv_s(wvar, wval);

    free(wvar);
    free(wval);
#else
    rc = setenv(variable, value, true);
#endif
    return rc;
}

/* Unset environment variable. */
int
pyi_unsetenv(const char *variable)
{
    int rc;

#ifdef _WIN32
    wchar_t * wvar;
    wvar = pyi_win32_utils_from_utf8(NULL, variable, 0);
    rc = SetEnvironmentVariableW(wvar, NULL);
    free(wvar);
#else  /* _WIN32 */
    #if HAVE_UNSETENV
    rc = unsetenv(variable);
    #else /* HAVE_UNSETENV */
    rc = setenv(variable, "", true);
    #endif /* HAVE_UNSETENV */
#endif     /* _WIN32 */
    return rc;
}

#ifdef _WIN32

/* TODO rename fuction and revisit */
int
pyi_get_temp_path(char *buffer, char *runtime_tmpdir)
{
    int i;
    wchar_t *wchar_ret;
    wchar_t prefix[16];
    wchar_t wchar_buffer[PATH_MAX];
    char *original_tmpdir;
    char runtime_tmpdir_abspath[PATH_MAX + 1];

    if (runtime_tmpdir != NULL) {
      /*
       * Get original TMP environment variable so it can be restored
       * after this is done.
       */
      original_tmpdir = pyi_getenv("TMP");
      /*
       * Set TMP to runtime_tmpdir for _wtempnam() later
       */
      pyi_path_fullpath(runtime_tmpdir_abspath, PATH_MAX, runtime_tmpdir);
      pyi_setenv("TMP", runtime_tmpdir_abspath);
    }

    GetTempPathW(PATH_MAX, wchar_buffer);

    swprintf(prefix, 16, L"_MEI%d", getpid());

    /*
     * Windows does not have a race-free function to create a temporary
     * directory. Thus, we rely on _tempnam, and simply try several times
     * to avoid stupid race conditions.
     */
    for (i = 0; i < 5; i++) {
        /* TODO use race-free fuction - if any exists? */
        wchar_ret = _wtempnam(wchar_buffer, prefix);

        if (_wmkdir(wchar_ret) == 0) {
            pyi_win32_utils_to_utf8(buffer, wchar_ret, PATH_MAX);
            free(wchar_ret);
            if (runtime_tmpdir != NULL) {
              /*
               * Restore TMP to what it was
               */
              if (original_tmpdir != NULL) {
                pyi_setenv("TMP", original_tmpdir);
                free(original_tmpdir);
              } else {
                pyi_unsetenv("TMP");
              }
            }
            return 1;
        }
        free(wchar_ret);
    }
    if (runtime_tmpdir != NULL) {
      /*
       * Restore TMP to what it was
       */
      if (original_tmpdir != NULL) {
        pyi_setenv("TMP", original_tmpdir);
        free(original_tmpdir);
      } else {
        pyi_unsetenv("TMP");
      }
    }
    return 0;
}

#else /* ifdef _WIN32 */

/* TODO Is this really necessary to test for temp path? Why not just use mkdtemp()? */
int
pyi_test_temp_path(char *buff)
{
    /*
     * If path does not end with directory separator - append it there.
     * On OSX the value from $TMPDIR ends with '/'.
     */
    if (buff[strlen(buff) - 1] != PYI_SEP) {
        strcat(buff, PYI_SEPSTR);
    }
    strcat(buff, "_MEIXXXXXX");

    if (mkdtemp(buff)) {
        return 1;
    }
    return 0;
}

/* TODO merge this function with windows version. */
static int
pyi_get_temp_path(char *buff, char *runtime_tmpdir)
{
    if (runtime_tmpdir != NULL) {
      strcpy(buff, runtime_tmpdir);
      if (pyi_test_temp_path(buff))
        return 1;
    } else {
      /* On OSX the variable TMPDIR is usually defined. */
      static const char *envname[] = {
          "TMPDIR", "TEMP", "TMP", 0
      };
      static const char *dirname[] = {
          "/tmp", "/var/tmp", "/usr/tmp", 0
      };
      int i;
      char *p;

      for (i = 0; envname[i]; i++) {
          p = pyi_getenv(envname[i]);

          if (p) {
              strcpy(buff, p);

              if (pyi_test_temp_path(buff)) {
                  return 1;
              }
          }
      }

      for (i = 0; dirname[i]; i++) {
          strcpy(buff, dirname[i]);

          if (pyi_test_temp_path(buff)) {
              return 1;
          }
      }
    }
    return 0;
}

#endif /* ifdef _WIN32 */

/*
 * Creates a temporany directory if it doesn't exists
 * and properly sets the ARCHIVE_STATUS members.
 */
int
pyi_create_temp_path(ARCHIVE_STATUS *status)
{
    char *runtime_tmpdir = NULL;

    if (status->has_temp_directory != true) {
        runtime_tmpdir = pyi_arch_get_option(status, "pyi-runtime-tmpdir");
        if(runtime_tmpdir != NULL) {
          VS("LOADER: Found runtime-tmpdir %s\n", runtime_tmpdir);
        }

        if (!pyi_get_temp_path(status->temppath, runtime_tmpdir)) {
            FATALERROR("INTERNAL ERROR: cannot create temporary directory!\n");
            return -1;
        }
        /* Set flag that temp directory is created and available. */
        status->has_temp_directory = true;
    }
    return 0;
}

/* TODO merge unix/win versions of remove_one() and pyi_remove_temp_path() */
#ifdef _WIN32
static void
remove_one(wchar_t *wfnm, size_t pos, struct _wfinddata_t wfinfo)
{
    char fnm[PATH_MAX + 1];

    if (wcscmp(wfinfo.name, L".") == 0  || wcscmp(wfinfo.name, L"..") == 0) {
        return;
    }
    wfnm[pos] = PYI_NULLCHAR;
    wcscat(wfnm, wfinfo.name);

    if (wfinfo.attrib & _A_SUBDIR) {
        /* Use recursion to remove subdirectories. */
        pyi_win32_utils_to_utf8(fnm, wfnm, PATH_MAX);
        pyi_remove_temp_path(fnm);
    }
    else if (_wremove(wfnm)) {
        /* HACK: Possible concurrency issue... spin a little while */
        Sleep(100);
        _wremove(wfnm);
    }
}

/* TODO Find easier and more portable implementation of removing directory recursively. */
/*     e.g. */
void
pyi_remove_temp_path(const char *dir)
{
    wchar_t wfnm[PATH_MAX + 1];
    wchar_t wdir[PATH_MAX + 1];
    struct _wfinddata_t wfinfo;
    intptr_t h;
    size_t dirnmlen;

    pyi_win32_utils_from_utf8(wdir, dir, PATH_MAX);
    wcscpy(wfnm, wdir);
    dirnmlen = wcslen(wfnm);

    if (wfnm[dirnmlen - 1] != L'/' && wfnm[dirnmlen - 1] != L'\\') {
        wcscat(wfnm, L"\\");
        dirnmlen++;
    }
    wcscat(wfnm, L"*");
    h = _wfindfirst(wfnm, &wfinfo);

    if (h != -1) {
        remove_one(wfnm, dirnmlen, wfinfo);

        while (_wfindnext(h, &wfinfo) == 0) {
            remove_one(wfnm, dirnmlen, wfinfo);
        }
        _findclose(h);
    }
    _wrmdir(wdir);
}
#else /* ifdef _WIN32 */
static void
remove_one(char *pnm, int pos, const char *fnm)
{
    struct stat sbuf;

    if (strcmp(fnm, ".") == 0  || strcmp(fnm, "..") == 0) {
        return;
    }
    pnm[pos] = PYI_NULLCHAR;
    strcat(pnm, fnm);

    if (stat(pnm, &sbuf) == 0) {
        if (S_ISDIR(sbuf.st_mode) ) {
            /* Use recursion to remove subdirectories. */
            pyi_remove_temp_path(pnm);
        }
        else {
            unlink(pnm);
        }
    }
}

void
pyi_remove_temp_path(const char *dir)
{
    char fnm[PATH_MAX + 1];
    DIR *ds;
    struct dirent *finfo;
    int dirnmlen;

    /* Leave 1 char for PY_SEP if needed */
    strncpy(fnm, dir, PATH_MAX);
    dirnmlen = strlen(fnm);

    if (fnm[dirnmlen - 1] != PYI_SEP) {
        strcat(fnm, PYI_SEPSTR);
        dirnmlen++;
    }
    ds = opendir(dir);
    finfo = readdir(ds);

    while (finfo) {
        remove_one(fnm, dirnmlen, finfo->d_name);
        finfo = readdir(ds);
    }
    closedir(ds);
    rmdir(dir);
}
#endif /* ifdef _WIN32 */

/* TODO is this function still used? Could it be removed? */
/*
 * If binaries were extracted, this should be called
 * to remove them
 */
void
cleanUp(ARCHIVE_STATUS *status)
{
    if (status->temppath[0]) {
        pyi_remove_temp_path(status->temppath);
    }
}

/*
 * helper for extract2fs
 * which may try multiple places
 */
/* TODO find better name for function. */
FILE *
pyi_open_target(const char *path, const char* name_)
{

#ifdef _WIN32
    wchar_t wchar_buffer[PATH_MAX];
    struct _stat sbuf;
#else
    struct stat sbuf;
#endif
    char fnm[PATH_MAX];
    char name[PATH_MAX];
    char *dir;
    size_t len;

    strncpy(fnm, path, PATH_MAX);
    strncpy(name, name_, PATH_MAX);

    /* Check if the path names could be copied */
    if (fnm[PATH_MAX-1] != '\0' || name[PATH_MAX-1] != '\0') {
        return NULL;
    }

    len = strlen(fnm);
    dir = strtok(name, PYI_SEPSTR);

    while (dir != NULL) {
        len += strlen(dir) + strlen(PYI_SEPSTR);
        /* Check if fnm does not exceed the buffer size */
        if (len >= PATH_MAX-1) {
            return NULL;
        }
        strcat(fnm, PYI_SEPSTR);
        strcat(fnm, dir);
        dir = strtok(NULL, PYI_SEPSTR);

        if (!dir) {
            break;
        }

#ifdef _WIN32
        pyi_win32_utils_from_utf8(wchar_buffer, fnm, PATH_MAX);

        if (_wstat(wchar_buffer, &sbuf) < 0) {
            _wmkdir(wchar_buffer);
        }
#else

        if (stat(fnm, &sbuf) < 0) {
            mkdir(fnm, 0700);
        }
#endif
    }

#ifdef _WIN32
    pyi_win32_utils_from_utf8(wchar_buffer, fnm, PATH_MAX);

    if (_wstat(wchar_buffer, &sbuf) == 0) {
        OTHERERROR("WARNING: file already exists but should not: %s\n", fnm);
    }
#else

    if (stat(fnm, &sbuf) == 0) {
        OTHERERROR("WARNING: file already exists but should not: %s\n", fnm);
    }
#endif
    /*
     * pyi_path_fopen() wraps different fopen names. On Windows it uses
     * wide-character version of fopen.
     */
    return pyi_path_fopen(fnm, "wb");
}

/* Copy the file src to dst 4KB per time */
int
pyi_copy_file(const char *src, const char *dst, const char *filename)
{
    FILE *in = pyi_path_fopen(src, "rb");
    FILE *out = pyi_open_target(dst, filename);
    char buf[4096];
    int error = 0;

    if (in == NULL || out == NULL) {
        if (in) {
            fclose(in);
        }
        if (out) {
            fclose(out);
        }
        return -1;
    }

    while (!feof(in)) {
        if (fread(buf, 4096, 1, in) == -1) {
            if (ferror(in)) {
                clearerr(in);
                error = -1;
                break;
            }
        }
        else {
            int rc = fwrite(buf, 4096, 1, out);
            if (rc <= 0 || ferror(out)) {
                clearerr(out);
                error = -1;
                break;
            }
        }
    }
#ifndef WIN32
    fchmod(fileno(out), S_IRUSR | S_IWUSR | S_IXUSR);
#endif
    fclose(in);
    fclose(out);

    return error;
}

/* TODO use dlclose() when exiting. */
/* Load the shared dynamic library (DLL) */
dylib_t
pyi_utils_dlopen(const char *dllpath)
{

#ifdef _WIN32
    wchar_t * dllpath_w;
    dylib_t ret;
#else
    int dlopenMode = RTLD_NOW | RTLD_GLOBAL;
#endif

#ifdef AIX
    /* Append the RTLD_MEMBER to the open mode for 'dlopen()'
     * in order to load shared object member from library.
     */
    dlopenMode |= RTLD_MEMBER;
#endif

#ifdef _WIN32
    dllpath_w = pyi_win32_utils_from_utf8(NULL, dllpath, 0);
    ret = LoadLibraryExW(dllpath_w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    free(dllpath_w);
    return ret;
#else
    return dlopen(dllpath, dlopenMode);
#endif

}

/* ////////////////////////////////////////////////////////////////// */
/* TODO better merging of the following platform specific functions. */
/* ////////////////////////////////////////////////////////////////// */

#ifdef _WIN32

int
pyi_utils_set_environment(const ARCHIVE_STATUS *status)
{
    return 0;
}

int
pyi_utils_create_child(const char *thisfile, const ARCHIVE_STATUS* status,
                       const int argc, char *const argv[])
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    int rc = 0;
    wchar_t buffer[PATH_MAX];

    /* TODO is there a replacement for this conversion or just use wchar_t everywhere? */
    /* Convert file name to wchar_t from utf8. */
    pyi_win32_utils_from_utf8(buffer, thisfile, PATH_MAX);

    /* the parent process should ignore all signals it can */
    signal(SIGABRT, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGBREAK, SIG_IGN);

    VS("LOADER: Setting up to run child\n");
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    GetStartupInfoW(&si);
    si.lpReserved = NULL;
    si.lpDesktop = NULL;
    si.lpTitle = NULL;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_NORMAL;
    si.hStdInput = (void*)_get_osfhandle(fileno(stdin));
    si.hStdOutput = (void*)_get_osfhandle(fileno(stdout));
    si.hStdError = (void*)_get_osfhandle(fileno(stderr));

    VS("LOADER: Creating child process\n");

    if (CreateProcessW(
            buffer,            /* Pointer to name of executable module. */
            GetCommandLineW(), /* pointer to command line string */
            &sa,               /* pointer to process security attributes */
            NULL,              /* pointer to thread security attributes */
            TRUE,              /* handle inheritance flag */
            0,                 /* creation flags */
            NULL,              /* pointer to new environment block */
            NULL,              /* pointer to current directory name */
            &si,               /* pointer to STARTUPINFO */
            &pi                /* pointer to PROCESS_INFORMATION */
            )) {
        VS("LOADER: Waiting for child process to finish...\n");
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, (unsigned long *)&rc);
    }
    else {
        FATAL_WINERROR("CreateProcessW", "Error creating child process!\n");
        rc = -1;
    }
    return rc;
}

#else /* ifdef _WIN32 */

static int
set_dynamic_library_path(const char* path)
{
    int rc = 0;
    char *env_var, *env_var_orig;
    char *new_path, *orig_path;

    #ifdef AIX
    /* LIBPATH is used to look up dynamic libraries on AIX. */
    env_var = "LIBPATH";
    env_var_orig = "LIBPATH_ORIG";
    #else
    /* LD_LIBRARY_PATH is used on other *nix platforms (except Darwin). */
    env_var = "LD_LIBRARY_PATH";
    env_var_orig = "LD_LIBRARY_PATH_ORIG";
    #endif /* AIX */

    /* keep original value in a new env var so the application can restore it
     * before forking subprocesses. This is important so that e.g. a forked
     * (system installed) ssh can find the matching (system installed) ssh
     * related libraries - not the potentially different versions of same libs
     * that we have bundled.
     */
    orig_path = pyi_getenv(env_var);
    if (orig_path) {
        pyi_setenv(env_var_orig, orig_path);
        VS("LOADER: %s=%s\n", env_var_orig, orig_path);
    }
    /* prepend our path to the original path, pyi_strjoin can deal with orig_path being NULL or empty string */
    new_path = pyi_strjoin(path, ":", orig_path);
    rc = pyi_setenv(env_var, new_path);
    VS("LOADER: %s=%s\n", env_var, new_path);
    free(new_path);
    return rc;
}

int
pyi_utils_set_environment(const ARCHIVE_STATUS *status)
{
    int rc = 0;

    #ifdef __APPLE__
    /* On Mac OS X we do not use environment variables DYLD_LIBRARY_PATH
     * or others to tell OS where to look for dynamic libraries.
     * There were some issues with this approach. In some cases some
     * system libraries were trying to load incompatible libraries from
     * the dist directory. For instance this was experienced with macprots
     * and PyQt4 applications.
     *
     * To tell the OS where to look for dynamic libraries we modify
     * .so/.dylib files to use relative paths to other dependend
     * libraries starting with @executable_path.
     *
     * For more information see:
     * http://blogs.oracle.com/dipol/entry/dynamic_libraries_rpath_and_mac
     * http://developer.apple.com/library/mac/#documentation/DeveloperTools/  \
     *     Conceptual/DynamicLibraries/100-Articles/DynamicLibraryUsageGuidelines.html
     */
    /* For environment variable details see 'man dyld'. */
    pyi_unsetenv("DYLD_FRAMEWORK_PATH");
    pyi_unsetenv("DYLD_FALLBACK_FRAMEWORK_PATH");
    pyi_unsetenv("DYLD_VERSIONED_FRAMEWORK_PATH");
    pyi_unsetenv("DYLD_LIBRARY_PATH");
    pyi_unsetenv("DYLD_FALLBACK_LIBRARY_PATH");
    pyi_unsetenv("DYLD_VERSIONED_LIBRARY_PATH");
    pyi_unsetenv("DYLD_ROOT_PATH");

    #else

    /* Set library path to temppath. This is only for onefile mode.*/
    if (status->temppath[0] != PYI_NULLCHAR) {
        rc = set_dynamic_library_path(status->temppath);
    }
    /* Set library path to homepath. This is for default onedir mode.*/
    else {
        rc = set_dynamic_library_path(status->homepath);
    }
    #endif /* ifdef __APPLE__ */

    return rc;
}

/*
 * If the program is actived by a systemd socket, systemd will set
 * LISTEN_PID, LISTEN_FDS environment variable for that process.
 *
 * LISTEN_PID is set to the pid of the parent process of bootloader,
 * which is forked by systemd.
 *
 * Bootloader will duplicate LISTEN_FDS to child process, but the
 * LISTEN_PID environment variable remains unchanged.
 *
 * Here we change the LISTEN_PID to the child pid in child process.
 * So the application can detecte it and use the LISTEN_FDS created
 * by systemd.
 */
int
set_systemd_env()
{
    const char * env_var = "LISTEN_PID";
    if(pyi_getenv(env_var) != NULL) {
        /* the ULONG_STRING_SIZE is roughly equal to log10(max number)
         * but can be calculated in compile time.
         * The idea is from an answer on stackoverflow,
         * https://stackoverflow.com/questions/8257714/
         */
        #define ULONG_STRING_SIZE (sizeof (unsigned long) * CHAR_BIT / 3 + 2)
        char pid_str[ULONG_STRING_SIZE];
        snprintf(pid_str, ULONG_STRING_SIZE, "%ld", (unsigned long)getpid());
        return pyi_setenv(env_var, pid_str);
    }
    return 0;
}

/* Remember child process id. It allows sending a signal to child process.
 * Frozen application always runs in a child process. Parent process is used
 * to setup environment for child process and clean the environment when
 * child exited.
 */
pid_t child_pid = 0;

static void
_ignoring_signal_handler(int signum)
{
    VS("LOADER: Ignoring signal %d\n", signum);
}

static void
_signal_handler(int signum)
{
    VS("LOADER: Forwarding signal %d to child pid %d\n", signum, child_pid);
    kill(child_pid, signum);
}

/* Start frozen application in a subprocess. The frozen application runs
 * in a subprocess.
 */
int
pyi_utils_create_child(const char *thisfile, const ARCHIVE_STATUS* status,
                       const int argc, char *const argv[])
{
    pid_t pid = 0;
    int rc = 0;
    int i;

    /* cause nonzero return unless this is overwritten
     * with a successful return code from wait() */
    int wait_rc = -1;

    /* As indicated in signal(7), signal numbers range from 1-31 (standard)
     * and 32-64 (Linux real-time). */
    const size_t num_signals = 65;

    sighandler_t handler;
    int ignore_signals;
    int signum;

    argv_pyi = (char**)calloc(argc + 1, sizeof(char*));
    argc_pyi = 0;

    for (i = 0; i < argc; i++) {
    #if defined(__APPLE__) && defined(WINDOWED)

        /* if we are on a Mac, it passes a strange -psnxxx argument.  Filter it out. */
        if (strstr(argv[i], "-psn") == argv[i]) {
            /* skip */
        }
        else
    #endif
        {
            argv_pyi[argc_pyi++] = strdup(argv[i]);
        }
    }

    #if defined(__APPLE__) && defined(WINDOWED)
    process_apple_events();
    #endif

    pid = fork();
    if (pid < 0) {
        VS("LOADER: failed to fork child process: %s\n", strerror(errno));
        goto cleanup;
    }

    /* Child code. */
    if (pid == 0) {
        /* Replace process by starting a new application. */
        if (set_systemd_env() != 0) {
            VS("WARNING: Application is started by systemd socket,"
               "but we can't set proper LISTEN_PID on it.\n");
        }
        if (execvp(thisfile, argv_pyi) < 0) {
            VS("Failed to exec: %s\n", strerror(errno));
            goto cleanup;
        }
        /* NOTREACHED */
    }

    /* From here to end-of-function is parent code (since the child exec'd).
     * The exception is the `cleanup` block that frees argv_pyi; in the child,
     * wait_rc is -1, so the child exit code checking is skipped. */

    child_pid = pid;
    ignore_signals = (pyi_arch_get_option(status, "pyi-bootloader-ignore-signals") != NULL);
    handler = ignore_signals ? &_ignoring_signal_handler : &_signal_handler;

    /* Redirect all signals received by parent to child process. */
    if (ignore_signals) {
        VS("LOADER: Ignoring all signals in parent\n");
    } else {
        VS("LOADER: Registering signal handlers\n");
    }
    for (signum = 0; signum < num_signals; ++signum) {
        // don't mess with SIGCHLD/SIGCLD; it affects our ability
        // to wait() for the child to exit
        if (signum != SIGCHLD && signum != SIGCLD) {
            signal(signum, handler);
        }
    }

    #if defined(__APPLE__) && defined(WINDOWED)
    /* MacOS code -- forward events to child! */
    do {
        /* The below waitpid() call will run about once every second on Apple, waiting on the event queue most of that time. */
        wait_rc = waitpid(child_pid, &rc, WNOHANG);
        if (wait_rc == 0)
            /* Child not ready yet -- process apple events with 1 second timeout, forwarding them to child. */
            process_apple_events();
    } while (!wait_rc);
    #else
    wait_rc = waitpid(child_pid, &rc, 0);
    #endif
    if (wait_rc < 0) {
        VS("LOADER: failed to wait for child process: %s\n", strerror(errno));
    }

    /* When child process exited, reset signal handlers to default values. */
    VS("LOADER: Restoring signal handlers\n");
    for (signum = 0; signum < num_signals; ++signum) {
        signal(signum, SIG_DFL);
    }

  cleanup:
    VS("LOADER: freeing args\n");
    for (i = 0; i < argc_pyi; i++) {
        free(argv_pyi[i]);
    }
    free(argv_pyi);

    /* Either wait() failed, or we jumped to `cleanup` and
     * didn't wait() at all. Either way, exit with error,
     * because rc does not contain a valid process exit code. */
    if (wait_rc < 0) {
        VS("LOADER: exiting early\n");
        return 1;
    }

    if (WIFEXITED(rc)) {
        VS("LOADER: returning child exit status %d\n", WEXITSTATUS(rc));
        return WEXITSTATUS(rc);
    }

    /* Process ended abnormally */
    if (WIFSIGNALED(rc)) {
        VS("LOADER: re-raising child signal %d\n", WTERMSIG(rc));
        /* Mimick the signal the child received */
        raise(WTERMSIG(rc));
    }
    return 1;
}

/*
 * On Mac OS X this converts files from kAEOpenDocuments events into sys.argv.
 * After startup, it also forwards kAEOpenDocuments events at runtime to the child process.
 */
#if defined(__APPLE__) && defined(WINDOWED)

static Boolean gQuit = false,
               apple_event_is_open_doc = 0; /* flag set to indicate the current AppleEvent is an OpenDoc event */

static pascal OSErr handle_open_doc_ae(const AppleEvent *theAppleEvent, AppleEvent *reply, SRefCon handlerRefcon)
{
    if (apple_event_is_open_doc) {
        VS("LOADER [AppleEvent]: OpenDocument handler called.\n");

        if (!child_pid) {
            /* Child process is not up yet -- so we pick up kAEOpenDoc events and append them to args. */
            AEDescList docList;
            long index;
            long count = 0;
            Size actualSize;
            DescType returnedType;
            AEKeyword keywd;
            char buf[4096];

            VS("LOADER [AppleEvent ARGV_EMU]: Processing args for forward...\n");

            OSErr err = AEGetParamDesc(theAppleEvent, keyDirectObject, typeAEList, &docList);
            if (err != noErr) return err;

            err = AECountItems(&docList, &count);
            if (err != noErr) return err;

            for (index = 1; index <= count; index++)
            {
                err = AEGetNthPtr(&docList, index, /*typeFileURL*/typeUTF8Text, &keywd, &returnedType, buf, sizeof(buf), &actualSize);
                if (err != noErr) {
                    VS("LOADER [AppleEvent ARGV_EMU]: err[%d] = %d\n",(int)index, (int)err);
                } else {
                    VS("LOADER [AppleEvent ARGV_EMU]: arg[%d] = %s\n",(int)index, buf);
                    argv_pyi = (char**)realloc(argv_pyi,(argc_pyi+2)*sizeof(char*));
                    argv_pyi[argc_pyi++] = strdup(buf);
                    argv_pyi[argc_pyi] = NULL;
                    VS("LOADER [AppleEvent ARGV_EMU]: argv entry appended.\n");
                }
            }

            err = AEDisposeDesc(&docList);

            return err;
        } else {
            /* The child process exists.. so we forward kAEOpenDoc events to it */
            ProcessSerialNumber psn;
            AppleEvent evtCopy;
            AEAddressDesc target;
            OSStatus err = noErr;
            AEDescList docList;

            VS("LOADER [AppleEvent EVT_FWD]: Will forward kAEOpenDoc event to child pid %ld...\n",(long)child_pid);

            err = GetProcessForPID(child_pid, &psn);
            if (err != noErr) return (OSErr)err;
            VS("LOADER [AppleEvent EVT_FWD]: Creating desc.\n");
            err = AECreateDesc(typeProcessSerialNumber, &psn, sizeof(psn), &target); /* re-target the event to our child process */
            if (err != noErr) return (OSErr)err;
            VS("LOADER [AppleEvent EVT_FWD]: Creating dupe event.\n");
            err = AECreateAppleEvent(kCoreEventClass, kAEOpenDocuments, &target, kAutoGenerateReturnID, kAnyTransactionID, &evtCopy);
            if (err != noErr) goto cleanup2;
            VS("LOADER [AppleEvent EVT_FWD]: Getting param.\n");
            err = AEGetParamDesc(theAppleEvent, keyDirectObject, typeAEList, &docList);
            if (err != noErr) goto cleanup1;
            VS("LOADER [AppleEvent EVT_FWD]: Putting param.\n");
            err = AEPutParamDesc(&evtCopy, keyDirectObject, &docList);
            AEDisposeDesc(&docList); /* dispose apple doc list, no longer needed after put */
            if (err != noErr) goto cleanup1;

            VS("LOADER [AppleEvent EVT_FWD]: Sending message...\n");
            err = AESendMessage(&evtCopy, reply, /*kAEWaitReply*/kAENoReply|kAECanInteract, 90 /* 90 = about 1.5 seconds *//*kAEDefaultTimeout*/ );
            VS("LOADER [AppleEvent EVT_FWD]: OpenDocument handler forwarded message to child process.\n");
            if (err != noErr) goto cleanup1;

        cleanup1:
            AEDisposeDesc(&evtCopy); /* dispose apple event copy */
        cleanup2:
            AEDisposeDesc(&target);
            if (err) VS("LOADER [AppleEvent EVT_FWD]: OpenDocument handler got error %d\n", (int)err);
            return (OSErr)err;
        }
    } else {
        VS("LOADER [AppleEvent]: OpenDocument handler ignoring non-kAEOpenDoc event.\n");
    }
    return noErr;
}

static OSStatus evt_handler_proc(EventHandlerCallRef href, EventRef eref, void *data) {
    VS("LOADER [AppleEvent]: App event handler proc called.\n");
    Boolean release = false;
    EventRecord eventRecord;

    // Events of type kEventAppleEvent must be removed from the queue
    // before being passed to AEProcessAppleEvent.
    if (IsEventInQueue(GetMainEventQueue(), eref))
    {
        // RemoveEventFromQueue will release the event, which will
        //  destroy it if we don't retain it first.
        VS("LOADER [AppleEvent]: Event was in queue, will release.\n");
        RetainEvent(eref);
        release = true;
        RemoveEventFromQueue(GetMainEventQueue(), eref);
    }
    // Convert the event ref to the type AEProcessAppleEvent expects.
    ConvertEventRefToEventRecord(eref, &eventRecord);
    VS("LOADER [AppleEvent]: what=%hu message=%lx modifiers=%hu\n", eventRecord.what, eventRecord.message, eventRecord.modifiers);
    apple_event_is_open_doc = eventRecord.what == kHighLevelEvent && eventRecord.message == 0x4755524c;
    OSStatus err = AEProcessAppleEvent(&eventRecord);
    apple_event_is_open_doc = 0;
    if (err) VS("LOADER [AppleEvent]: Failed to forward event to handle_open_doc_ae!\n");
    if (release)
        ReleaseEvent(eref);
    return noErr;
}

static void process_apple_events()
{
    static EventHandlerUPP handler;
    static AEEventHandlerUPP handler_open_doc;
    static Boolean did_install = false;
    static EventHandlerRef handler_ref;
    EventTypeSpec event_types[1];  /*  List of event types to handle. */
    event_types[0].eventClass = kEventClassAppleEvent;
    event_types[0].eventKind = kEventAppleEvent;

    VS("LOADER [AppleEvent]: Processing...\n");

    if (!did_install) {
        handler = NewEventHandlerUPP(evt_handler_proc);
        handler_open_doc = NewAEEventHandlerUPP(handle_open_doc_ae);
        /* NB must use wildcards because for some unknown reason the specific kAEOpenDocuments event is never propagated
           to our custom handler! */
        OSStatus err = AEInstallEventHandler(/*kCoreEventClass*/typeWildCard, typeWildCard/*kAEOpenDocuments*/, handler_open_doc, 0, false);
        if (!err)
            err = InstallApplicationEventHandler(handler, 1, event_types, NULL, &handler_ref);

        if (err != noErr) {
            /* Remove handler_ref reference when we are done with EventHandlerUPP. */
            //AERemoveEventHandler(kCoreEventClass, kAEOpenDocuments, handler_open_doc, false);
            /* Carbon Event Manager does not do this automatically. */
            DisposeEventHandlerUPP(handler);
            DisposeAEEventHandlerUPP(handler_open_doc);
            VS("LOADER [AppleEvent]: Disposed handlers.\n");
        } else {
            VS("LOADER [AppleEvent]: Installed handlers.\n");
            did_install = true;
        }
    }

    if (did_install) {
        OSStatus rcv_status;
        OSStatus pcs_status = 0;
        EventRef event_ref;          /* Event that caused ReceiveNextEvent to return. */
        EventTimeout timeout = 1.0;  /* number of seconds */


        while(!gQuit) {
            VS("LOADER [AppleEvent]: Calling ReceiveNextEvent\n");

            rcv_status = ReceiveNextEvent(1, event_types, timeout, kEventRemoveFromQueue /*kEventLeaveInQueue*/, &event_ref);

            if (rcv_status == eventLoopTimedOutErr) {
                VS("LOADER [AppleEvent]: ReceiveNextEvent timed out\n");
                break;
            }
            else if (rcv_status != 0) {
                VS("LOADER [AppleEvent]: ReceiveNextEvent fetching events failed\n");
                break;
            }
            else
            {
                /* We actually pulled an event off the queue, so process it.
                 We now 'own' the event_ref and must release it. */
                VS("LOADER [AppleEvent]: ReceiveNextEvent got an EVENT\n");

                VS("LOADER [AppleEvent]: Dispatching event...\n");
                pcs_status = SendEventToEventTarget(event_ref, /*GetApplicationEventTarget()*/GetEventDispatcherTarget());

                ReleaseEvent(event_ref);
                event_ref = NULL;
                if (pcs_status != 0) {
                    VS("LOADER [AppleEvent]: processing events failed\n");
                    break;
                }
            }
        }

        VS("LOADER [AppleEvent]: Out of the event loop.\n");

    }
    else {
        VS("LOADER [AppleEvent]: ERROR installing handler.\n");
    }
}

#endif /* if defined(__APPLE__) && defined(WINDOWED) */

#endif  /* WIN32 */
