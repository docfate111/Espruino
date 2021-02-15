#include <ftw.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "jslex.h"
#include "jsparse.h"
#include "jsvar.h"
#include "jswrap_json.h"

#include "jshardware.h"
#include "jsinteractive.h"
#include "jswrapper.h"

#define TEST_DIR "tests/"
#define CMD_NAME "espruino"
#define REPRL_MAX_DATA_SIZE (16*1024*1024)
bool isRunning = true;
struct filelist test_files;

void warning(const char *, ...) __attribute__((__format__(__warning__, 1, 2)));
void fatal(int, const char *, ...)
    __attribute__((__format__(__warning__, 2, 3)));

void warning(const char *fmt, ...) {
  va_list ap;

  fflush(stdout);
  fprintf(stderr, "%s: ", CMD_NAME);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\r', stderr);
  fputc('\n', stderr);
}

void fatal(int errcode, const char *fmt, ...) {
  va_list ap;

  fflush(stdout);
  fprintf(stderr, "%s: ", CMD_NAME);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\r', stderr);
  fputc('\n', stderr);

  exit(errcode);
}

void perror_exit(int errcode, const char *s) {
  fflush(stdout);
  fprintf(stderr, "%s: ", CMD_NAME);
  perror(s);
  exit(errcode);
}

struct filelist {
  char **array;
  size_t count;
  size_t size;
};

#define filelist_foreach(list, file)                                           \
  const char *file = NULL;                                                     \
  size_t idx##list = 0;                                                        \
  for (idx##list = 0;                                                          \
       (idx##list < list->count ? (file = list->array[idx##list]) : 0);        \
       ++idx##list)

int filelist_free(struct filelist *fl) {
  while (fl->count > 0) {
    free(fl->array[--fl->count]);
  }
  free(fl->array);
  fl->array = NULL;
  fl->size = 0;
  return 0;
}

int filelist_add(struct filelist *fl, const char *name) {
  if (fl->count == fl->size) {
    size_t newsize = fl->size + (fl->size >> 1) + 4;
    char **a = realloc(fl->array, sizeof(fl->array[0]) * newsize);
    if (!a)
      return -1;
    fl->array = a;
    fl->size = newsize;
  }
  char absname[PATH_MAX];
  realpath(name, absname);
  fl->array[fl->count] = strdup(absname);
  fl->count++;
  return 0;
}

int filelist_load(struct filelist *fl, const char *filename) {
  char buf[1024];
  FILE *f;

  f = fopen(filename, "rb");
  if (!f) {
    return -1;
  }

  while (fgets(buf, sizeof(buf), f) != NULL) {
    if (buf[0] == '#' || buf[0] == ';' || buf[0] == '\0')
      continue;
    filelist_add(fl, buf);
  }

  fclose(f);
  return 0;
}

void addNativeFunction(const char *name, void (*callbackPtr)(void)) {
  jsvObjectSetChildAndUnLock(execInfo.root, name,
                             jsvNewNativeFunction(callbackPtr, JSWAT_VOID));
}

void nativeQuit() { isRunning = false; }

void nativeInterrupt() { jspSetInterrupted(true); }

static char *read_file(const char *filename) {
  FILE *f;
  char *buf;
  size_t buf_len;
  long lret;

  f = fopen(filename, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) < 0)
    goto fail;
  lret = ftell(f);
  if (lret < 0)
    goto fail;
  if (lret == LONG_MAX) { // it's a directory
    errno = EISDIR;
    goto fail;
  }
  buf_len = lret;
  if (fseek(f, 0, SEEK_SET) < 0)
    goto fail;
  buf = malloc(buf_len + 1);
  if (!buf)
    goto fail;
  if (fread(buf, 1, buf_len, f) != buf_len) {
    errno = EIO;
    free(buf);
  fail:
    fclose(f);
    return NULL;
  }
  buf[buf_len] = '\0';
  fclose(f);
  return buf;
}

bool run_test(const char *filename) {
  warning("----------------------------------");
  warning("----------------------------- TEST %s", filename);
  char *buffer = read_file(filename);
  if (!buffer)
  {
    warning("cannot load %s: %s", filename, strerror(errno));
    return 0;
  }

  jshInit();
  jsvInit(0);
  jsiInit(false /* do not autoload!!! */);

  addNativeFunction("quit", nativeQuit);
  addNativeFunction("interrupt", nativeInterrupt);

  jsvUnLock(jspEvaluate(buffer, false));

  isRunning = true;
  bool isBusy = true;
  while (isRunning && (jsiHasTimers() || isBusy))
    isBusy = jsiLoop();

  JsVar *result = jsvObjectGetChild(execInfo.root, "result", 0 /*no create*/);
  bool pass = jsvGetBool(result);
  jsvUnLock(result);

  if (pass)
    warning("----------------------------- PASS %s", filename);
  else {
    warning("----------------------------------");
    warning("----------------------------- FAIL %s -------", filename);
    jsvTrace(execInfo.root, 0);
    warning("----------------------------- FAIL %s -------", filename);
    warning("----------------------------------");
  }
  warning("BEFORE: %d Memory Records Used", jsvGetMemoryUsage());
  // jsvTrace(execInfo.root, 0);
  jsiKill();
  warning("AFTER: %d Memory Records Used", jsvGetMemoryUsage());
  jsvGarbageCollect();
  unsigned int unfreed = jsvGetMemoryUsage();
  warning("AFTER GC: %d Memory Records Used (should be 0!)", unfreed);
  jsvShowAllocated();
  jsvKill();
  jshKill();

  if (unfreed) {
    warning("FAIL because of unfreed memory.");
    pass = false;
  }

  // jsvDottyOutput();

  free(buffer);
  return pass;
}

static int has_suffix(const char *str, const char *suffix) {
  size_t len = strlen(str);
  size_t slen = strlen(suffix);
  return (len >= slen && !memcmp(str + len - slen, suffix, slen));
}

static int add_test_file(const char *filename, const struct stat *ptr,
                         int flags) {
  struct filelist *fl = &test_files;
  if (has_suffix(filename, ".js"))
    filelist_add(fl, filename);
  return 0;
}

static void enumerate_tests(const char *path) { ftw(path, add_test_file, 100); }

bool run_test_list(struct filelist *fl) {
  size_t passed = 0;
  struct filelist fails;
  memset(&fails, 0, sizeof(fails));

  if (fl->count == 0) {
    warning("No tests found");
    return 0;
  }

  filelist_foreach(fl, fn) {
    if (run_test(fn))
      passed++;
    else {
      filelist_add(&fails, fn);
    }
  }

  warning("--------------------------------------------------");
  warning(" %d of %d tests passed", passed, fl->count);
  if (passed != fl->count) {
    warning("FAILS:");
    struct filelist *faili = &fails;
    filelist_foreach(faili, ffn) { warning("%s", ffn); }
  }
  warning("--------------------------------------------------");
  filelist_free(&fails);
  return passed == fl->count;
}

bool run_all_tests() {
  bool rc;
  enumerate_tests(TEST_DIR);
  rc = run_test_list(&test_files);
  filelist_free(&test_files);
  return rc;
}

bool run_memory_test(const char *fn, int vars) {
  unsigned int i;
  unsigned int min = 20;
  unsigned int max = 100;
  if (vars > 0) {
    min = (unsigned)vars;
    max = (unsigned)vars + 1;
  }
  for (i = min; i < max; i++) {
    jsvSetMaxVarsUsed(i);
    warning("----------------------------------------------------- MEMORY TEST "
            "WITH %d VARS",
            i);
    run_test(fn);
  }
  return true;
}

bool run_memory_tests(struct filelist *fl, int vars) {

  filelist_foreach(fl, file) { run_memory_test(file, vars); }

  return true;
}

void sig_handler(int sig) {
  // warning("Got Signal %d\n",sig);fflush(stdout);
  if (sig == SIGINT)
    jspSetInterrupted(true);
}

//
// BEGIN FUZZING CODE
//

#define REPRL_CRFD 100
#define REPRL_CWFD 101
#define REPRL_DRFD 102
#define REPRL_DWFD 103

#define SHM_SIZE 0x100000
#define MAX_EDGES ((SHM_SIZE - 4) * 8)

#define CHECK(cond) if (!(cond)) { fprintf(stderr, "\"" #cond "\" failed\n"); _exit(-1); }

struct shmem_data {
    uint32_t num_edges;
    unsigned char edges[];
};

struct shmem_data* __shmem;
uint32_t *__edges_start, *__edges_stop;

void __sanitizer_cov_reset_edgeguards() {
    uint64_t N = 0;
    for (uint32_t *x = __edges_start; x < __edges_stop && N < MAX_EDGES; x++)
        *x = ++N;
}

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop) {
    // Avoid duplicate initialization
    if (start == stop || *start)
        return;

    if (__edges_start != NULL || __edges_stop != NULL) {
        fprintf(stderr, "Coverage instrumentation is only supported for a single module\n");
        _exit(-1);
    }

    __edges_start = start;
    __edges_stop = stop;

    // Map the shared memory region
    const char* shm_key = getenv("SHM_ID");
    if (!shm_key) {
        puts("[COV] no shared memory bitmap available, skipping");
        __shmem = (struct shmem_data*) malloc(SHM_SIZE);
    } else {
        int fd = shm_open(shm_key, O_RDWR, S_IREAD | S_IWRITE);
        if (fd <= -1) {
            fprintf(stderr, "Failed to open shared memory region: %s\n", strerror(errno));
            _exit(-1);
        }

        __shmem = (struct shmem_data*) mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (__shmem == MAP_FAILED) {
            fprintf(stderr, "Failed to mmap shared memory region\n");
            _exit(-1);
        }
    }

    __sanitizer_cov_reset_edgeguards();

    __shmem->num_edges = stop - start;
    printf("[COV] edge counters initialized. Shared memory: %s with %u edges\n", shm_key, __shmem->num_edges);
}

void __sanitizer_cov_trace_pc_guard(uint32_t *guard) {
    // There's a small race condition here: if this function executes in two threads for the same
    // edge at the same time, the first thread might disable the edge (by setting the guard to zero)
    // before the second thread fetches the guard value (and thus the index). However, our
    // instrumentation ignores the first edge (see libcoverage.c) and so the race is unproblematic.
    uint32_t index = *guard;
    // If this function is called before coverage instrumentation is properly initialized we want to return early.
    if (!index) return;
    __shmem->edges[index / 8] |= 1 << (index % 8);
    *guard = 0;
}

//
// END FUZZING CODE
//
void show_help() {
  warning("Usage:");
  warning("   ./espruino                           : JavaScript immediate mode "
          "(REPL)");
  warning("   ./espruino script.js                 : Load and run script.js");
  warning("   ./espruino -e \"print('Hello World')\" : Print 'Hello World'");
  warning("");
  warning("Options:");
  warning("   -h, --help              Print this help screen");
  warning("   -e, --eval script       Evaluate the JavaScript supplied on the "
          "command-line");
  warning("  --fuzzilli               Mode for Fuzzilli javascript engine fuzzer");
#ifdef USE_TELNET
  warning(
      "   --telnet                Enable internal telnet server on port 2323");
#endif
  warning("   --test-all              Run all tests (in 'tests' directory)");
  warning("   --test-dir dir          Run all tests in directory 'dir'");
  warning("   --test test.js          Run the supplied test");
  warning("   --test test.js          Run the supplied test");
  warning("   --test-mem-all          Run all Exhaustive Memory crash tests");
  warning("   --test-mem test.js      Run the supplied Exhaustive Memory crash "
          "test");
  warning("   --test-mem-n test.js #  Run the supplied Exhaustive Memory crash "
          "test with # vars");
}

void die(const char *txt) {
  warning("%s", txt);
  exit(1);
}

int handleErrors() {
  int e = 0;
  JsVar *exception = jspGetException();
  if (exception) {
    jsiConsolePrintf("Uncaught %v\n", exception);
    jsvUnLock(exception);
    e = 1;
  }

  if (jspIsInterrupted()) {
    jsiConsoleRemoveInputLine();
    jsiConsolePrint("Execution Interrupted.\n");
    jspSetInterrupted(false);
    e = 1;
  }
  return e;
}

void *STACK_BASE; ///< used for jsuGetFreeStack on Linux

int main(int argc, char **argv) {
  int i, args = 0;

  STACK_BASE = (void *)&i; // used for jsuGetFreeStack on Linux

  const char *singleArg = 0;
  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      // option
      char *a = argv[i];
      if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
        show_help();
        exit(1);
      } else if (!strcmp(a, "-e") || !strcmp(a, "--eval")) {
        if (i + 1 >= argc)
          fatal(1, "Expecting an extra argument");
        jshInit();
        jsvInit(0);
        jsiInit(true);
        addNativeFunction("quit", nativeQuit);
        jsvUnLock(jspEvaluate(argv[i + 1], false));
        int errCode = handleErrors();
        isRunning = !errCode;
        bool isBusy = true;
        while (isRunning && (jsiHasTimers() || isBusy))
          isBusy = jsiLoop();
        jsiKill();
        jsvKill();
        jshKill();
        exit(errCode);
#ifdef USE_TELNET
      } else if (!strcmp(a, "--telnet")) {
        extern bool telnetEnabled;
        telnetEnabled = true;
#endif
      } else if (!strcmp(a, "--test")) {
        bool ok;
        if (i + 1 >= argc) {
          fatal(1, "Expecting an extra arguments");
        } else if (i + 2 == argc) {
          ok = run_test(argv[i + 1]);
        } else {
          i++;
          while (i<argc) filelist_add(&test_files, argv[i++]);
          ok = run_test_list(&test_files);
        }
        exit(ok ? 0 : 1);
      } else if (!strcmp(a, "--test-dir")) {
        enumerate_tests(argv[i + 1]);
        bool ok = run_test_list(&test_files);
        filelist_free(&test_files);
        exit(ok ? 0 : 1);
      } else if (!strcmp(a, "--test-all")) {
        bool ok = run_all_tests();
        exit(ok ? 0 : 1);
      } else if (!strcmp(a, "--test-mem-all")) {
        enumerate_tests(argv[i + 1]);
        bool ok = run_memory_tests(&test_files, 0);
        filelist_free(&test_files);
        exit(ok ? 0 : 1);
      } else if (!strcmp(a, "--test-mem")) {
        if (i + 1 >= argc)
          fatal(1, "Expecting an extra argument");
        bool ok = run_memory_test(argv[i + 1], 0);
        exit(ok ? 0 : 1);
      } else if (!strcmp(a, "--test-mem-n")) {
        if (i + 2 >= argc)
          die("Expecting an extra 2 arguments\n");
        bool ok = run_memory_test(argv[i + 1], atoi(argv[i + 2]));
        exit(ok ? 0 : 1);

      } else if (!strcmp(a, "--fuzzilli")) {
        /*
        if REPRL_MODE on commandline:
        */
          char helo[] = "HELO";
          //  write "HELO" on REPRL_CWFD and read 4 bytes on REPRL_CRFD
          if (write(REPRL_CWFD, helo, 4) != 4 || read(REPRL_CRFD, helo, 4) != 4) {
              printf("Invalid HELO response from parent\n");
          }
          //    break if 4 read bytes do not equal "HELO"
          if (memcmp(helo, "HELO", 4) != 0) {
              printf("Invalid response from parent\n");
              exit(-1);
          }
          //  optionally, mmap the REPRL_DRFD with size REPRL_MAX_DATA_SIZE
          //  char* reprl_input_data = (char*)mmap(0, REPRL_MAX_DATA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, REPRL_DRFD, 0);
          //  while true:
          while(1){
            // read 4 bytes on REPRL_CRFD
            unsigned action;
            if(read(REPRL_CRFD, &action, 4) != 4){
              fprintf(stderr, "Did not get 4 bytes");
              exit(-1);
            }
            // break if 4 read bytes do not equal "cexe"
            if (action != 'cexe') {
             fprintf(stderr, "[REPRL] Unknown action: %u\n", action);
             exit(-1);
            }
            size_t script_size = 0;
            // read 8 bytes on REPRL_CRFD, store as unsigned 64 bit integer size
            if(read(REPRL_CRFD, &script_size, 8) != 8){
              fprintf(stderr, "Failed to read size of buffer");
              _exit(-1);
            }
            // allocate size+1 bytes
            char *buffer = (char *)malloc(script_size+1);
            // read size bytes from REPRL_DRFD into allocated buffer
            char *ptr = buffer;
            size_t remaining = script_size;
            while (remaining > 0) {
              ssize_t rv = read(REPRL_DRFD, ptr, remaining);
              if (rv <= 0) {
                fprintf(stderr, "Failed to load script\n");
                _exit(-1);
              }
              remaining -= rv;
              ptr += rv;
            }
            buffer[script_size] = '\0';
            // Execute buffer as javascript code
            char *cmd = buffer;
            if (cmd[0] == '#') {
              while (cmd[0] && cmd[0] != '\n')
                cmd++;
              if (cmd[0] == '\n')
                cmd++;
            }
            jshInit();
            jsvInit(0);
            jsiInit(false /* do not autoload!!! */);
            addNativeFunction("quit", nativeQuit);
            // addNativeFunction("fuzzilli", fuzzilli);
            jsvUnLock(jspEvaluate(cmd, false));
            //  Store return value from JS execution
            int result = handleErrors();
            free(buffer);
            // Reset the Javascript engine
            jsiKill();
            jsvKill();
            jshKill();
            //  Flush stdout and stderr
            fflush(stdout);
            fflush(stderr);
            //  Mask return value with 0xff and shift it left by 8
            int status = (result & 0xff) << 8;
            // then write that value over REPRL_CWFD
            CHECK(write(REPRL_CWFD, &status, 4) == 4);
            // Call __sanitizer_cov_reset_edgeguards to reset coverage 
            __sanitizer_cov_reset_edgeguards();
          }
      } else {
        warning("Unknown Argument %s", a);
        show_help();
        exit(1);
      }
    } else {
      args++;
      singleArg = argv[i];
    }
  }

  if (args == 0) {
    warning("Interactive mode.");
  } else if (args == 1) {
    // single file - just run it
    char *buffer = read_file(singleArg);
    if (!buffer)
      perror_exit(1, singleArg);
    // check for '#' as the first char, and if so, skip the first line
    char *cmd = buffer;
    if (cmd[0] == '#') {
      while (cmd[0] && cmd[0] != '\n')
        cmd++;
      if (cmd[0] == '\n')
        cmd++;
    }
    jshInit();
    jsvInit(0);
    jsiInit(false /* do not autoload!!! */);
    addNativeFunction("quit", nativeQuit);
    jsvUnLock(jspEvaluate(cmd, false));
    int errCode = handleErrors();
    free(buffer);
    isRunning = !errCode;
    bool isBusy = true;
    while (isRunning && (jsiHasTimers() || isBusy))
      isBusy = jsiLoop();
    jsiKill();
    jsvKill();
    jshKill();
    exit(errCode);
  } else {
    warning("Unknown arguments!");
    show_help();
    exit(1);
  }

  warning("Size of JsVar is now %d bytes", (int)sizeof(JsVar));
  warning("Size of JsVarRef is now %d bytes", (int)sizeof(JsVarRef));

#ifndef __MINGW32__
  struct sigaction sa;
  sa.sa_handler = sig_handler;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGINT, &sa, NULL) == -1)
    warning("Adding SIGINT hook failed");
  else
    warning("Added SIGINT hook");
  if (sigaction(SIGHUP, &sa, NULL) == -1)
    warning("Adding SIGHUP hook failed");
  else
    warning("Added SIGHUP hook");
  if (sigaction(SIGTERM, &sa, NULL) == -1)
    warning("Adding SIGTERM hook failed");
  else
    warning("Added SIGTERM hook");
#endif //!__MINGW32__

  jshInit();
  jsvInit(0);
  jsiInit(true);

  addNativeFunction("quit", nativeQuit);
  addNativeFunction("interrupt", nativeInterrupt);
  while (isRunning) {
    jsiLoop();
  }
  jsiConsolePrint("");
  jsiKill();
  jsvGarbageCollect();
  jsvShowAllocated();
  jsvKill();
  jshKill();

  return 0;
}
