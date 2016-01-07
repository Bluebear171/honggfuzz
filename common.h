/*
 *
 * honggfuzz - core structures and macros
 * -----------------------------------------
 *
 * Author: Robert Swiecki <swiecki@google.com>
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

#ifndef _COMMON_H_
#define _COMMON_H_

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/param.h>
#include <sys/types.h>

#ifndef UNUSED
#define UNUSED __attribute__((unused))
#endif

#define PROG_NAME "honggfuzz"
#define PROG_VERSION "0.6rc"
#define PROG_AUTHORS "Robert Swiecki <swiecki@google.com> et al.,\nCopyright 2010-2015 by Google Inc. All Rights Reserved."

/* Name of the template which will be replaced with the proper name of the file */
#define _HF_FILE_PLACEHOLDER "___FILE___"

/* Default name of the report created with some architectures */
#define _HF_REPORT_FILE "HONGGFUZZ.REPORT.TXT"

/* Default stack-size of created threads. Must be bigger then _HF_DYNAMIC_FILE_MAX_SZ */
#define _HF_PTHREAD_STACKSIZE (1024 * 1024 * 8) /* 8MB */

/* Align to the upper-page boundary */
#define _HF_PAGE_ALIGN_UP(x)  (((size_t)x + (size_t)getpagesize() - (size_t)1) & ~((size_t)getpagesize() - (size_t)1))

/* String buffer size for function names in stack traces produced from libunwind */
#define _HF_FUNC_NAME_SZ    256 // Should be alright for mangled C++ procs too

/* Number of crash verifier iterations before tag crash as stable */
#define _HF_VERIFIER_ITER   5

/*
 * If enabled simplifier aborts on size mismatch between seed & crash. Otherwise
 * it tries to revert bytes up to offset of smaller file.
 */
#define __HF_ABORT_SIMPLIFIER_ON_SIZ_MISMATCH true

/* Maximum number of diff bytes to try reverting - skipping continus diff blobs */
#define __HF_ABORT_SIMPLIFIER_MAX_DIFF 30

/* Constant prefix used for single frame crashes stackhash masking */
#define _HF_SINGLE_FRAME_MASK  0xBADBAD0000000000

/* Size (in bytes) for report data to be stored in stack before written to file */
#define _HF_REPORT_SIZE 8192

/* 
 * Maximum number of iterations to keep same base seed file for dynamic preparation.
 * Maintained iterations counters is set to zero if unique crash is detected or
 * zero-set two MSB using following mask if crash is detected (might not be unique).
 */
#define _HF_MAX_DYNFILE_ITER 0x2000UL
#define _HF_DYNFILE_SUB_MASK 0xFFFUL    // Zero-set two MSB

/* 
 * SIGABRT is not a monitored signal (thus 'abort_on_error' is missing crashes when set)
 * for Android OS since it produces lots of useless crashes due to way Android process 
 * termination hacks work. Safest option is to register & monitor one of user signals. 
 * SIGUSR2 is used for sanitizer fuzzing in Android, although might need to be changed
 * if target uses it for other purposes.
 */
#define _HF_ANDROID_ASAN_EXIT_SIG   SIGUSR2

/* Bitmap size */
#define _HF_BITMAP_SIZE 0xAFFFFF

/* Directory in workspace to store sanitizer coverage data */
#define _HF_SANCOV_DIR "HF_SANCOV"

/* Uncomment/Comment to enable/disable debug */
#define _HF_DEBUG   1

typedef enum {
    _HF_DYNFILE_NONE = 0x0,
    _HF_DYNFILE_INSTR_COUNT = 0x1,
    _HF_DYNFILE_BRANCH_COUNT = 0x2,
    _HF_DYNFILE_UNIQUE_BLOCK_COUNT = 0x8,
    _HF_DYNFILE_UNIQUE_EDGE_COUNT = 0x10,
    _HF_DYNFILE_CUSTOM = 0x20,
} dynFileMethod_t;

typedef struct {
    uint64_t cpuInstrCnt;
    uint64_t cpuBranchCnt;
    uint64_t pcCnt;
    uint64_t pathCnt;
    uint64_t customCnt;
} hwcnt_t;

/* Sanitizer coverage specific data structures */
typedef struct {
    uint64_t hitPcCnt;
    uint64_t totalPcCnt;
    uint64_t dsoCnt;
    uint64_t iDsoCnt;
    uint64_t newPcCnt;
    uint64_t crashesCnt;
} sancovcnt_t;

typedef struct {
    uint32_t capacity;
    uint32_t *pChunks;
    uint32_t nChunks;
} bitmap_t;

/* Memory map struct */
typedef struct __attribute__ ((packed)) {
    uint64_t start;             // region start addr
    uint64_t end;               // region end addr
    uint64_t base;              // region base addr
    char mapName[NAME_MAX];     // bin/DSO name
    uint64_t pcCnt;
    uint64_t newPcCnt;
} memMap_t;

/* Trie node data struct */
typedef struct __attribute__ ((packed)) {
    bitmap_t *pBM;
} trieData_t;

/* Trie node struct */
typedef struct __attribute__ ((packed)) node {
    char key;
    trieData_t data;
    struct node *next;
    struct node *prev;
    struct node *children;
    struct node *parent;
} node_t;

/* EOF Sanitizer coverage specific data structures */

typedef struct {
    char **cmdline;
    char *inputFile;
    bool nullifyStdio;
    bool fuzzStdin;
    bool saveUnique;
    bool useScreen;
    bool useVerifier;
    bool saveMaps;
    bool useSimplifier;
    char *fileExtn;
    char *workDir;
    double flipRate;
    char *externalCommand;
    const char *dictionaryFile;
    const char **dictionary;
    const char *blacklistFile;
    uint64_t *blacklist;
    size_t blacklistCnt;
    const char *symbolsBlacklistFile;
    const char **symbolsBlacklist;
    size_t symbolsBlacklistCnt;
    const char *symbolsWhitelistFile;
    const char **symbolsWhitelist;
    size_t symbolsWhitelistCnt;
    long tmOut;
    size_t dictionaryCnt;
    size_t mutationsMax;
    size_t threadsMax;
    size_t threadsFinished;
    size_t maxFileSz;
    char *reportFile;
    uint64_t asLimit;
    char **files;
    size_t fileCnt;
    size_t lastCheckedFileIndex;
    pid_t pid;
    char *envs[128];

    time_t timeStart;
    size_t mutationsCnt;
    size_t crashesCnt;
    size_t uniqueCrashesCnt;
    size_t verifiedCrashesCnt;
    size_t blCrashesCnt;
    size_t timeoutedCnt;

    /* For the linux/ code */
    uint8_t *dynamicFileBest;
    size_t dynamicFileBestSz;
    dynFileMethod_t dynFileMethod;
    hwcnt_t hwCnts;
    sancovcnt_t sanCovCnts;
    uint64_t dynamicCutOffAddr;
    pthread_mutex_t dynamicFile_mutex;
    bool disableRandomization;
    bool msanReportUMRS;
    void *ignoreAddr;
    bool useSanCov;
    node_t *covMetadata;
    bool clearCovMetadata;
    size_t dynFileIterExpire;
    pthread_mutex_t sanCov_mutex;
    pthread_mutex_t workersBlock_mutex;
#ifdef _HF_DEBUG
    long maxSpentInSanCov;
#endif
#if defined(EXTENSION_ENABLED)
    void **userData;
#endif
} honggfuzz_t;

typedef struct fuzzer_t {
    pid_t pid;
    int64_t timeStartedMillis;
    char origFileName[PATH_MAX];
    char fileName[PATH_MAX];
    char crashFileName[PATH_MAX];
    uint64_t pc;
    uint64_t backtrace;
    uint64_t access;
    int exception;
    char report[_HF_REPORT_SIZE];
    bool mainWorker;

    /* For linux/ code */
    uint8_t *dynamicFile;
    hwcnt_t hwCnts;
    sancovcnt_t sanCovCnts;
    size_t dynamicFileSz;
    bool isDynFileLocked;
} fuzzer_t;

#define _HF_MAX_FUNCS 80
typedef struct {
    void *pc;
    char func[_HF_FUNC_NAME_SZ];
    size_t line;
} funcs_t;

#define ARRAYSIZE(x) (sizeof(x) / sizeof(*x))

#ifdef _HF_DEBUG
#include <time.h>
#if defined(_HF_ARCH_DARWIN)
#include <mach/clock.h>
#include <mach/mach.h>
#endif

static inline void currentUtcTime(struct timespec *ts)
{
#if defined(_HF_ARCH_DARWIN)
    clock_serv_t cclock;
    mach_timespec_t mts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);
    ts->tv_sec = mts.tv_sec;
    ts->tv_nsec = mts.tv_nsec;
#else
    clock_gettime(CLOCK_REALTIME, ts);
#endif
}

static inline struct timespec startTimer()
{
    struct timespec startTime;
    currentUtcTime(&startTime);
    return startTime;
}

static inline long endTimer(struct timespec startTime)
{
    struct timespec endTime;
    currentUtcTime(&endTime);
    long diffNs = endTime.tv_nsec - startTime.tv_nsec;
    return diffNs;
}

#define _HF_START_TIMER struct timespec t = startTimer();
#define _HF_END_TIMER   long diff = endTimer(t);
#define _HF_PRINT_TIMER LOG_I("Time taken: %ld ns", diff);
#define _HF_GET_TIME    diff
#endif

#endif
