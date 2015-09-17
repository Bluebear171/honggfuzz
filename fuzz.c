/*
 *
 * honggfuzz - fuzzing routines
 * -----------------------------------------
 *
 * Author:
 * Robert Swiecki <swiecki@google.com>
 * Felix Gröbert <groebert@google.com>
 *
 * Copyright 2010-2015 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "common.h"
#include "fuzz.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "arch.h"
#include "display.h"
#include "files.h"
#include "log.h"
#include "mangle.h"
#include "report.h"
#include "util.h"

#if defined(__ANDROID__) && !defined(__NR_fork)
#include <sys/syscall.h>

pid_t honggfuzz_aarch64_fork(void)
{
    return syscall(__NR_clone, SIGCHLD, 0, 0, 0);
}

#define fork honggfuzz_aarch64_fork
#endif

static int fuzz_sigReceived = 0;

#ifdef EXTENSION_ENABLED
// Definitions of extension interface functions
typedef void (*MangleResizeCallback) (honggfuzz_t *, uint8_t *, size_t *);
typedef void (*MangleCallback) (honggfuzz_t *, uint8_t *, size_t, int);
typedef void (*PostMangleCallback) (honggfuzz_t *, uint8_t *, size_t);

extern void __hf_MangleResizeCallback(honggfuzz_t * hfuzz, uint8_t * buf, size_t * bufSz);
extern void __hf_MangleCallback(honggfuzz_t * hfuzz, uint8_t * buf, size_t bufSz, int rnd_index);
extern void __hf_PostMangleCallback(honggfuzz_t * hfuzz, uint8_t * buf, size_t bufSz);

// Function pointer variables
#ifdef _HF_MANGLERESIZECALLBACK
static MangleResizeCallback UserMangleResizeCallback = &__hf_MangleResizeCallback;
#endif                          /* defined(_HF_MANGLERESIZECALLBACK) */
#ifdef _HF_MANGLECALLBACK
static MangleCallback UserMangleCallback = &__hf_MangleCallback;
#endif                          /* defined(_HF_MANGLECALLBACK) */
#ifdef _HF_POSTMANGLECALLBACK
static PostMangleCallback UserPostMangleCallback = &__hf_PostMangleCallback;
#endif                          /* defined(_HF_POSTMANGLECALLBACK) */
#endif                          /* defined(EXTENSION_ENABLED) */

static void fuzz_sigHandler(int sig)
{
    /* We should not terminate upon SIGALRM delivery */
    if (sig == SIGALRM) {
        return;
    }

    fuzz_sigReceived = sig;
}

static void fuzz_getFileName(honggfuzz_t * hfuzz, char *fileName)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    snprintf(fileName, PATH_MAX, "%s/.honggfuzz.%d.%lu.%llx.%s", hfuzz->workDir, (int)getpid(),
             (unsigned long int)tv.tv_sec, (unsigned long long int)util_rndGet(0, 1ULL << 62),
             hfuzz->fileExtn);

    return;
}

static bool fuzz_prepareFileDynamically(honggfuzz_t * hfuzz, fuzzer_t * fuzzer, int rnd_index)
{
    MX_LOCK(&hfuzz->dynamicFile_mutex);

    if (hfuzz->inputFile && hfuzz->hwCnts.cpuInstrCnt == 0ULL && hfuzz->hwCnts.cpuBranchCnt == 0ULL
        && hfuzz->hwCnts.pcCnt == 0ULL && hfuzz->hwCnts.pathCnt == 0ULL
        && hfuzz->hwCnts.customCnt == 0ULL) {
        size_t fileSz = files_readFileToBufMax(hfuzz->files[rnd_index], hfuzz->dynamicFileBest,
                                               hfuzz->maxFileSz);
        if (fileSz == 0) {
            MX_UNLOCK(&hfuzz->dynamicFile_mutex);
            LOGMSG(l_ERROR, "Couldn't read '%s'", hfuzz->files[rnd_index]);
            return false;
        }
        hfuzz->dynamicFileBestSz = fileSz;
    }

    if (hfuzz->dynamicFileBestSz > hfuzz->maxFileSz) {
        LOGMSG(l_FATAL, "Current BEST file Sz > maxFileSz (%zu > %zu)", hfuzz->dynamicFileBestSz,
               hfuzz->maxFileSz);
    }

    fuzzer->dynamicFileSz = hfuzz->dynamicFileBestSz;
    memcpy(fuzzer->dynamicFile, hfuzz->dynamicFileBest, hfuzz->dynamicFileBestSz);

    MX_UNLOCK(&hfuzz->dynamicFile_mutex);

    /* The first pass should be on an empty/initial file */
    if (hfuzz->hwCnts.cpuInstrCnt > 0 || hfuzz->hwCnts.cpuBranchCnt > 0 || hfuzz->hwCnts.pcCnt > 0
        || hfuzz->hwCnts.pathCnt > 0 || hfuzz->hwCnts.customCnt > 0) {

#if defined(EXTENSION_ENABLED) && defined(_HF_MANGLERESIZECALLBACK)
        UserMangleResizeCallback(hfuzz, fuzzer->dynamicFile, &fuzzer->dynamicFileSz);
#else
        mangle_Resize(hfuzz, fuzzer->dynamicFile, &fuzzer->dynamicFileSz);
#endif
#if defined(EXTENSION_ENABLED) && defined(_HF_MANGLECALLBACK)
        UserMangleCallback(hfuzz, fuzzer->dynamicFile, fuzzer->dynamicFileSz, rnd_index);
#else
        mangle_mangleContent(hfuzz, fuzzer->dynamicFile, fuzzer->dynamicFileSz);
#endif
#if defined(EXTENSION_ENABLED) && defined(_HF_POSTMANGLECALLBACK)
        UserPostMangleCallback(hfuzz, fuzzer->dynamicFile, fuzzer->dynamicFileSz);
#endif

    }

    if (files_writeBufToFile
        (fuzzer->fileName, fuzzer->dynamicFile, fuzzer->dynamicFileSz,
         O_WRONLY | O_CREAT | O_EXCL | O_TRUNC) == false) {
        LOGMSG(l_ERROR, "Couldn't write buffer to file '%s'", fuzzer->fileName);
        return false;
    }

    return true;
}

static bool fuzz_prepareFile(honggfuzz_t * hfuzz, fuzzer_t * fuzzer, int rnd_index)
{
    size_t fileSz =
        files_readFileToBufMax(hfuzz->files[rnd_index], fuzzer->dynamicFile, hfuzz->maxFileSz);
    if (fileSz == 0UL) {
        LOGMSG(l_ERROR, "Couldn't read contents of '%s'", hfuzz->files[rnd_index]);
        return false;
    }
#if defined(EXTENSION_ENABLED) && defined(_HF_MANGLERESIZECALLBACK)
    UserMangleResizeCallback(hfuzz, fuzzer->dynamicFile, &fileSz);
#else
    mangle_Resize(hfuzz, fuzzer->dynamicFile, &fileSz);
#endif
#if defined(EXTENSION_ENABLED) && defined(_HF_MANGLECALLBACK)
    UserMangleCallback(hfuzz, fuzzer->dynamicFile, fileSz, rnd_index);
#else
    mangle_mangleContent(hfuzz, fuzzer->dynamicFile, fileSz);
#endif
#if defined(EXTENSION_ENABLED) && defined(_HF_POSTMANGLECALLBACK)
    UserPostMangleCallback(hfuzz, fuzzer->dynamicFile, fileSz);
#endif

    if (files_writeBufToFile
        (fuzzer->fileName, fuzzer->dynamicFile, fileSz, O_WRONLY | O_CREAT | O_EXCL) == false) {
        LOGMSG(l_ERROR, "Couldn't write buffer to file '%s'", fuzzer->fileName);
        return false;
    }

    return true;
}

static bool fuzz_prepareFileExternally(honggfuzz_t * hfuzz, fuzzer_t * fuzzer, int rnd_index)
{
    int dstfd = open(fuzzer->fileName, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (dstfd == -1) {
        LOGMSG_P(l_ERROR, "Couldn't create a temporary file '%s'", fuzzer->fileName);
        return false;
    }

    LOGMSG(l_DEBUG, "Created '%f' as an input file", fuzzer->fileName);

    if (hfuzz->inputFile) {
        size_t fileSz =
            files_readFileToBufMax(hfuzz->files[rnd_index], fuzzer->dynamicFile, hfuzz->maxFileSz);
        if (fileSz == 0UL) {
            LOGMSG(l_ERROR, "Couldn't read '%s'", hfuzz->files[rnd_index]);
            unlink(fuzzer->fileName);
            return false;
        }
        // In case of external mangling only enable PostMangle callback
#if defined(EXTENSION_ENABLED) && defined(_HF_POSTMANGLECALLBACK)
        UserPostMangleCallback(hfuzz, fuzzer->dynamicFile, fileSz);
#endif

        if (files_writeToFd(dstfd, fuzzer->dynamicFile, fileSz) == false) {
            close(dstfd);
            unlink(fuzzer->fileName);
            return false;
        }
    }

    close(dstfd);

    pid_t pid = fork();
    if (pid == -1) {
        LOGMSG_P(l_ERROR, "Couldn't vfork");
        return false;
    }

    if (!pid) {
        /*
         * child performs the external file modifications
         */
        execl(hfuzz->externalCommand, hfuzz->externalCommand, fuzzer->fileName, NULL);
        LOGMSG_P(l_FATAL, "Couldn't execute '%s %s'", hfuzz->externalCommand, fuzzer->fileName);
        return false;
    }

    /*
     * parent waits until child is done fuzzing the input file
     */
    int childStatus;
    int flags = 0;
#if defined(__WNOTHREAD)
    flags |= __WNOTHREAD;
#endif                          /* defined(__WNOTHREAD) */
    while (wait4(pid, &childStatus, flags, NULL) != pid) ;
    if (WIFEXITED(childStatus)) {
        LOGMSG(l_DEBUG, "External command exited with status %d", WEXITSTATUS(childStatus));
        return true;
    }
    if (WIFSIGNALED(childStatus)) {
        LOGMSG(l_ERROR, "External command terminated with signal %d", WTERMSIG(childStatus));
        return false;
    }
    LOGMSG(l_FATAL, "External command terminated abnormally, status: %d", childStatus);
    return false;

    abort();                    /* NOTREACHED */
}

static void fuzz_fuzzLoop(honggfuzz_t * hfuzz)
{
    fuzzer_t fuzzer = {
        .pid = 0,
        .timeStartedMillis = util_timeNowMillis(),
        .pc = 0ULL,
        .backtrace = 0ULL,
        .access = 0ULL,
        .exception = 0,
        .dynamicFileSz = 0,
        .dynamicFile = malloc(hfuzz->maxFileSz),
        .hwCnts = {
                   .cpuInstrCnt = 0ULL,
                   .cpuBranchCnt = 0ULL,
                   .pcCnt = 0ULL,
                   .pathCnt = 0ULL,
                   .customCnt = 0ULL,
                   },
        .report = {'\0'}
    };
    if (fuzzer.dynamicFile == NULL) {
        LOGMSG(l_FATAL, "malloc(%zu) failed", hfuzz->maxFileSz);
    }

    int rnd_index = util_rndGet(0, hfuzz->fileCnt - 1);
    strncpy(fuzzer.origFileName, files_basename(hfuzz->files[rnd_index]), PATH_MAX);
    fuzz_getFileName(hfuzz, fuzzer.fileName);

    if (hfuzz->dynFileMethod != _HF_DYNFILE_NONE) {
        if (!fuzz_prepareFileDynamically(hfuzz, &fuzzer, rnd_index)) {
            exit(EXIT_FAILURE);
        }
    } else if (hfuzz->externalCommand != NULL) {
        if (!fuzz_prepareFileExternally(hfuzz, &fuzzer, rnd_index)) {
            exit(EXIT_FAILURE);
        }
    } else {
        if (!fuzz_prepareFile(hfuzz, &fuzzer, rnd_index)) {
            exit(EXIT_FAILURE);
        }
    }

#if defined(_HF_ARCH_LINUX) && defined(__NR_fork)
#include <unistd.h>
#include <sys/syscall.h>
    fuzzer.pid = syscall(__NR_fork);
#else                           /* defined(_HF_ARCH_LINUX) */
    fuzzer.pid = fork();
#endif                          /* defined(_HF_ARCH_LINUX) */

    if (fuzzer.pid == -1) {
        LOGMSG_P(l_FATAL, "Couldn't fork");
        exit(EXIT_FAILURE);
    }

    if (!fuzzer.pid) {
        /*
         * Ok, kill the parent if this fails
         */
        if (!arch_launchChild(hfuzz, fuzzer.fileName)) {
            LOGMSG(l_ERROR, "Error launching child process, killing parent");
            exit(EXIT_FAILURE);
        }
    }

    LOGMSG(l_DEBUG, "Launched new process, pid: %d, (concurrency: %d)", fuzzer.pid,
           hfuzz->threadsMax);

    arch_reapChild(hfuzz, &fuzzer);
    unlink(fuzzer.fileName);

    if (hfuzz->dynFileMethod != _HF_DYNFILE_NONE) {
        LOGMSG(l_DEBUG,
               "File size (New/Best): %zu/%zu, Perf feedback (instr/branch/block/block-edge/custom): Best: [%"
               PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "] / New: [%" PRIu64 ",%"
               PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]", fuzzer.dynamicFileSz,
               hfuzz->dynamicFileBestSz, hfuzz->hwCnts.cpuInstrCnt, hfuzz->hwCnts.cpuBranchCnt,
               hfuzz->hwCnts.pcCnt, hfuzz->hwCnts.pathCnt, hfuzz->hwCnts.customCnt,
               fuzzer.hwCnts.cpuInstrCnt, fuzzer.hwCnts.cpuBranchCnt, fuzzer.hwCnts.pcCnt,
               fuzzer.hwCnts.pathCnt, fuzzer.hwCnts.customCnt);

        MX_LOCK(&hfuzz->dynamicFile_mutex);

        int64_t diff0 = hfuzz->hwCnts.cpuInstrCnt - fuzzer.hwCnts.cpuInstrCnt;
        int64_t diff1 = hfuzz->hwCnts.cpuBranchCnt - fuzzer.hwCnts.cpuBranchCnt;
        int64_t diff2 = hfuzz->hwCnts.pcCnt - fuzzer.hwCnts.pcCnt;
        int64_t diff3 = hfuzz->hwCnts.pathCnt - fuzzer.hwCnts.pathCnt;
        int64_t diff4 = hfuzz->hwCnts.customCnt - fuzzer.hwCnts.customCnt;

        if (diff0 <= 0 && diff1 <= 0 && diff2 <= 0 && diff3 <= 0 && diff4 <= 0) {

            LOGMSG(l_INFO,
                   "New BEST feedback: File Size (New/Old): %zu/%zu', Perf feedback (Curr, High): %"
                   PRId64 "/%" PRId64 "/%" PRId64 "/%" PRId64 "/%" PRId64 ",%" PRId64 "/%" PRId64
                   "/%" PRId64 "/%" PRId64 "/%" PRId64, fuzzer.dynamicFileSz,
                   hfuzz->dynamicFileBestSz, hfuzz->hwCnts.cpuInstrCnt, hfuzz->hwCnts.cpuBranchCnt,
                   hfuzz->hwCnts.pcCnt, hfuzz->hwCnts.pathCnt, hfuzz->hwCnts.customCnt,
                   fuzzer.hwCnts.cpuInstrCnt, fuzzer.hwCnts.cpuBranchCnt, fuzzer.hwCnts.pcCnt,
                   fuzzer.hwCnts.pathCnt, fuzzer.hwCnts.customCnt);

            memcpy(hfuzz->dynamicFileBest, fuzzer.dynamicFile, fuzzer.dynamicFileSz);

            hfuzz->dynamicFileBestSz = fuzzer.dynamicFileSz;
            hfuzz->hwCnts.cpuInstrCnt = fuzzer.hwCnts.cpuInstrCnt;
            hfuzz->hwCnts.cpuBranchCnt = fuzzer.hwCnts.cpuBranchCnt;
            hfuzz->hwCnts.pcCnt = fuzzer.hwCnts.pcCnt;
            hfuzz->hwCnts.pathCnt = fuzzer.hwCnts.pathCnt;
            hfuzz->hwCnts.customCnt = fuzzer.hwCnts.customCnt;

            char currentBest[PATH_MAX], currentBestTmp[PATH_MAX];
            snprintf(currentBest, PATH_MAX, "%s/CURRENT_BEST", hfuzz->workDir);
            snprintf(currentBestTmp, PATH_MAX, "%s/.tmp.CURRENT_BEST", hfuzz->workDir);

            if (files_writeBufToFile
                (currentBestTmp, fuzzer.dynamicFile, fuzzer.dynamicFileSz,
                 O_WRONLY | O_CREAT | O_TRUNC)) {
                rename(currentBestTmp, currentBest);
            }
        }
        MX_UNLOCK(&hfuzz->dynamicFile_mutex);
    }

    report_Report(hfuzz, fuzzer.report);
    free(fuzzer.dynamicFile);

}

static void *fuzz_threadNew(void *arg)
{
    honggfuzz_t *hfuzz = (honggfuzz_t *) arg;
    for (;;) {
        if ((__sync_fetch_and_add(&hfuzz->mutationsCnt, 1UL) >= hfuzz->mutationsMax)
            && hfuzz->mutationsMax) {
            __sync_fetch_and_add(&hfuzz->threadsFinished, 1UL);
            // Wake-up the main process
            kill(getpid(), SIGALRM);
            return NULL;
        }

        fuzz_fuzzLoop(hfuzz);
    }
}

static void fuzz_runThread(honggfuzz_t * hfuzz, void *(*thread) (void *))
{
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, _HF_PTHREAD_STACKSIZE);
    pthread_attr_setguardsize(&attr, (size_t) sysconf(_SC_PAGESIZE));

    pthread_t t;
    if (pthread_create(&t, &attr, thread, (void *)hfuzz) < 0) {
        LOGMSG_P(l_FATAL, "Couldn't create a new thread");
    }

    return;
}

bool fuzz_setupTimer(void)
{
    struct itimerval it = {
        .it_value = {.tv_sec = 0,.tv_usec = 1},
        .it_interval = {.tv_sec = 1,.tv_usec = 0},
    };
    if (setitimer(ITIMER_REAL, &it, NULL) == -1) {
        LOGMSG_P(l_ERROR, "setitimer(ITIMER_REAL)");
        return false;
    }
    return true;
}

void fuzz_main(honggfuzz_t * hfuzz)
{
    struct sigaction sa = {
        .sa_handler = fuzz_sigHandler,
        .sa_flags = 0,
    };
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        LOGMSG_P(l_FATAL, "sigaction(SIGTERM) failed");
    }
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        LOGMSG_P(l_FATAL, "sigaction(SIGINT) failed");
    }
    if (sigaction(SIGQUIT, &sa, NULL) == -1) {
        LOGMSG_P(l_FATAL, "sigaction(SIGQUIT) failed");
    }
    if (sigaction(SIGALRM, &sa, NULL) == -1) {
        LOGMSG_P(l_FATAL, "sigaction(SIGQUIT) failed");
    }
    if (fuzz_setupTimer() == false) {
        LOGMSG(l_FATAL, "fuzz_setupTimer()");
    }

    if (!arch_archInit(hfuzz)) {
        LOGMSG(l_FATAL, "Couldn't prepare arch for fuzzing");
    }

    for (size_t i = 0; i < hfuzz->threadsMax; i++) {
        fuzz_runThread(hfuzz, fuzz_threadNew);
    }

    for (;;) {
        if (hfuzz->useScreen) {
            display_display(hfuzz);
        }
        if (fuzz_sigReceived > 0) {
            break;
        }
        if (__sync_fetch_and_add(&hfuzz->threadsFinished, 0UL) >= hfuzz->threadsMax) {
            break;
        }
        pause();
    }

    if (fuzz_sigReceived > 0) {
        LOGMSG(l_INFO, "Signal %d received, terminating", fuzz_sigReceived);
    }

    free(hfuzz->files);
    free(hfuzz->dynamicFileBest);

    _exit(EXIT_SUCCESS);
}
