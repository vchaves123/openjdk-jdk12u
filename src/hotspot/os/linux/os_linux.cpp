/*
 * Copyright (c) 1999, 2017, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
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
 *
 */

// no precompiled headers
#include "jvm.h"
#include "classfile/classLoader.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/icBuffer.hpp"
#include "code/vtableStubs.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/disassembler.hpp"
#include "interpreter/interpreter.hpp"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/filemap.hpp"
#include "oops/oop.inline.hpp"
#include "os_linux.inline.hpp"
#include "os_share_linux.hpp"
#include "osContainer_linux.hpp"
#include "prims/jniFastGetField.hpp"
#include "prims/jvm_misc.hpp"
#include "runtime/arguments.hpp"
#include "runtime/atomic.hpp"
#include "runtime/extendedPC.hpp"
#include "runtime/globals.hpp"
#include "runtime/interfaceSupport.hpp"
#include "runtime/init.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/orderAccess.inline.hpp"
#include "runtime/osThread.hpp"
#include "runtime/perfMemory.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/statSampler.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/threadCritical.hpp"
#include "runtime/timer.hpp"
#include "semaphore_posix.hpp"
#include "services/attachListener.hpp"
#include "services/memTracker.hpp"
#include "services/runtimeService.hpp"
#include "utilities/align.hpp"
#include "utilities/decoder.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/events.hpp"
#include "utilities/elfFile.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"
#include "utilities/vmError.hpp"

// put OS-includes here
# include <sys/types.h>
# include <sys/mman.h>
# include <sys/stat.h>
# include <sys/select.h>
# include <pthread.h>
# include <signal.h>
# include <errno.h>
# include <dlfcn.h>
# include <stdio.h>
# include <unistd.h>
# include <sys/resource.h>
# include <pthread.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <sys/times.h>
# include <sys/utsname.h>
# include <sys/socket.h>
# include <sys/wait.h>
# include <pwd.h>
# include <poll.h>
# include <semaphore.h>
# include <fcntl.h>
# include <string.h>
# include <syscall.h>
# include <sys/sysinfo.h>
# include <gnu/libc-version.h>
# include <sys/ipc.h>
# include <sys/shm.h>
# include <link.h>
# include <stdint.h>
# include <inttypes.h>
# include <sys/ioctl.h>

#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
  #include <sched.h>
  #undef _GNU_SOURCE
#else
  #include <sched.h>
#endif

// if RUSAGE_THREAD for getrusage() has not been defined, do it here. The code calling
// getrusage() is prepared to handle the associated failure.
#ifndef RUSAGE_THREAD
  #define RUSAGE_THREAD   (1)               /* only the calling thread */
#endif

#define MAX_PATH    (2 * K)

#define MAX_SECS 100000000

// for timer info max values which include all bits
#define ALL_64_BITS CONST64(0xFFFFFFFFFFFFFFFF)

#define LARGEPAGES_BIT (1 << 6)
////////////////////////////////////////////////////////////////////////////////
// global variables
julong os::Linux::_physical_memory = 0;

address   os::Linux::_initial_thread_stack_bottom = NULL;
uintptr_t os::Linux::_initial_thread_stack_size   = 0;

int (*os::Linux::_clock_gettime)(clockid_t, struct timespec *) = NULL;
int (*os::Linux::_pthread_getcpuclockid)(pthread_t, clockid_t *) = NULL;
int (*os::Linux::_pthread_setname_np)(pthread_t, const char*) = NULL;
Mutex* os::Linux::_createThread_lock = NULL;
pthread_t os::Linux::_main_thread;
int os::Linux::_page_size = -1;
bool os::Linux::_supports_fast_thread_cpu_time = false;
uint32_t os::Linux::_os_version = 0;
const char * os::Linux::_glibc_version = NULL;
const char * os::Linux::_libpthread_version = NULL;

static jlong initial_time_count=0;

static int clock_tics_per_sec = 100;

// For diagnostics to print a message once. see run_periodic_checks
static sigset_t check_signal_done;
static bool check_signals = true;

// Signal number used to suspend/resume a thread

// do not use any signal number less than SIGSEGV, see 4355769
static int SR_signum = SIGUSR2;
sigset_t SR_sigset;

// utility functions

static int SR_initialize();

julong os::available_memory() {
  return Linux::available_memory();
}

julong os::Linux::available_memory() {
  // values in struct sysinfo are "unsigned long"
  struct sysinfo si;
  julong avail_mem;

  if (OSContainer::is_containerized()) {
    jlong mem_limit, mem_usage;
    if ((mem_limit = OSContainer::memory_limit_in_bytes()) > 0) {
      if ((mem_usage = OSContainer::memory_usage_in_bytes()) > 0) {
        if (mem_limit > mem_usage) {
          avail_mem = (julong)mem_limit - (julong)mem_usage;
        } else {
          avail_mem = 0;
        }
        log_trace(os)("available container memory: " JULONG_FORMAT, avail_mem);
        return avail_mem;
      } else {
        log_debug(os,container)("container memory usage call failed: " JLONG_FORMAT, mem_usage);
      }
    } else {
      log_debug(os,container)("container memory unlimited or failed: " JLONG_FORMAT, mem_limit);
    }
  }

  sysinfo(&si);
  avail_mem = (julong)si.freeram * si.mem_unit;
  log_trace(os)("available memory: " JULONG_FORMAT, avail_mem);
  return avail_mem;
}

julong os::physical_memory() {
  if (OSContainer::is_containerized()) {
    jlong mem_limit;
    if ((mem_limit = OSContainer::memory_limit_in_bytes()) > 0) {
      log_trace(os)("total container memory: " JLONG_FORMAT, mem_limit);
      return (julong)mem_limit;
    } else {
      if (mem_limit == OSCONTAINER_ERROR) {
        log_debug(os,container)("container memory limit call failed");
      }
      if (mem_limit == -1) {
        log_debug(os,container)("container memory unlimited, using host value");
      }
    }
  }

  jlong phys_mem = Linux::physical_memory();
  log_trace(os)("total system memory: " JLONG_FORMAT, phys_mem);
  return phys_mem;
}

// Return true if user is running as root.

bool os::have_special_privileges() {
  static bool init = false;
  static bool privileges = false;
  if (!init) {
    privileges = (getuid() != geteuid()) || (getgid() != getegid());
    init = true;
  }
  return privileges;
}


#ifndef SYS_gettid
// i386: 224, ia64: 1105, amd64: 186, sparc 143
  #ifdef __ia64__
    #define SYS_gettid 1105
  #else
    #ifdef __i386__
      #define SYS_gettid 224
    #else
      #ifdef __amd64__
        #define SYS_gettid 186
      #else
        #ifdef __sparc__
          #define SYS_gettid 143
        #else
          #error define gettid for the arch
        #endif
      #endif
    #endif
  #endif
#endif


// pid_t gettid()
//
// Returns the kernel thread id of the currently running thread. Kernel
// thread id is used to access /proc.
pid_t os::Linux::gettid() {
  int rslt = syscall(SYS_gettid);
  assert(rslt != -1, "must be."); // old linuxthreads implementation?
  return (pid_t)rslt;
}

// Most versions of linux have a bug where the number of processors are
// determined by looking at the /proc file system.  In a chroot environment,
// the system call returns 1.  This causes the VM to act as if it is
// a single processor and elide locking (see is_MP() call).
static bool unsafe_chroot_detected = false;
static const char *unstable_chroot_error = "/proc file system not found.\n"
                     "Java may be unstable running multithreaded in a chroot "
                     "environment on Linux when /proc filesystem is not mounted.";

void os::Linux::initialize_system_info() {
  set_processor_count(sysconf(_SC_NPROCESSORS_CONF));
  if (processor_count() == 1) {
    pid_t pid = os::Linux::gettid();
    char fname[32];
    jio_snprintf(fname, sizeof(fname), "/proc/%d", pid);
    FILE *fp = fopen(fname, "r");
    if (fp == NULL) {
      unsafe_chroot_detected = true;
    } else {
      fclose(fp);
    }
  }
  _physical_memory = (julong)sysconf(_SC_PHYS_PAGES) * (julong)sysconf(_SC_PAGESIZE);
  assert(processor_count() > 0, "linux error");
}

void os::init_system_properties_values() {
  // The next steps are taken in the product version:
  //
  // Obtain the JAVA_HOME value from the location of libjvm.so.
  // This library should be located at:
  // <JAVA_HOME>/lib/{client|server}/libjvm.so.
  //
  // If "/jre/lib/" appears at the right place in the path, then we
  // assume libjvm.so is installed in a JDK and we use this path.
  //
  // Otherwise exit with message: "Could not create the Java virtual machine."
  //
  // The following extra steps are taken in the debugging version:
  //
  // If "/jre/lib/" does NOT appear at the right place in the path
  // instead of exit check for $JAVA_HOME environment variable.
  //
  // If it is defined and we are able to locate $JAVA_HOME/jre/lib/<arch>,
  // then we append a fake suffix "hotspot/libjvm.so" to this path so
  // it looks like libjvm.so is installed there
  // <JAVA_HOME>/jre/lib/<arch>/hotspot/libjvm.so.
  //
  // Otherwise exit.
  //
  // Important note: if the location of libjvm.so changes this
  // code needs to be changed accordingly.

  // See ld(1):
  //      The linker uses the following search paths to locate required
  //      shared libraries:
  //        1: ...
  //        ...
  //        7: The default directories, normally /lib and /usr/lib.
#if defined(AMD64) || (defined(_LP64) && defined(SPARC)) || defined(PPC64) || defined(S390)
  #define DEFAULT_LIBPATH "/usr/lib64:/lib64:/lib:/usr/lib"
#else
  #define DEFAULT_LIBPATH "/lib:/usr/lib"
#endif

// Base path of extensions installed on the system.
#define SYS_EXT_DIR     "/usr/java/packages"
#define EXTENSIONS_DIR  "/lib/ext"

  // Buffer that fits several sprintfs.
  // Note that the space for the colon and the trailing null are provided
  // by the nulls included by the sizeof operator.
  const size_t bufsize =
    MAX2((size_t)MAXPATHLEN,  // For dll_dir & friends.
         (size_t)MAXPATHLEN + sizeof(EXTENSIONS_DIR) + sizeof(SYS_EXT_DIR) + sizeof(EXTENSIONS_DIR)); // extensions dir
  char *buf = (char *)NEW_C_HEAP_ARRAY(char, bufsize, mtInternal);

  // sysclasspath, java_home, dll_dir
  {
    char *pslash;
    os::jvm_path(buf, bufsize);

    // Found the full path to libjvm.so.
    // Now cut the path to <java_home>/jre if we can.
    pslash = strrchr(buf, '/');
    if (pslash != NULL) {
      *pslash = '\0';            // Get rid of /libjvm.so.
    }
    pslash = strrchr(buf, '/');
    if (pslash != NULL) {
      *pslash = '\0';            // Get rid of /{client|server|hotspot}.
    }
    Arguments::set_dll_dir(buf);

    if (pslash != NULL) {
      pslash = strrchr(buf, '/');
      if (pslash != NULL) {
        *pslash = '\0';        // Get rid of /lib.
      }
    }
    Arguments::set_java_home(buf);
    set_boot_path('/', ':');
  }

  // Where to look for native libraries.
  //
  // Note: Due to a legacy implementation, most of the library path
  // is set in the launcher. This was to accomodate linking restrictions
  // on legacy Linux implementations (which are no longer supported).
  // Eventually, all the library path setting will be done here.
  //
  // However, to prevent the proliferation of improperly built native
  // libraries, the new path component /usr/java/packages is added here.
  // Eventually, all the library path setting will be done here.
  {
    // Get the user setting of LD_LIBRARY_PATH, and prepended it. It
    // should always exist (until the legacy problem cited above is
    // addressed).
    const char *v = ::getenv("LD_LIBRARY_PATH");
    const char *v_colon = ":";
    if (v == NULL) { v = ""; v_colon = ""; }
    // That's +1 for the colon and +1 for the trailing '\0'.
    char *ld_library_path = (char *)NEW_C_HEAP_ARRAY(char,
                                                     strlen(v) + 1 +
                                                     sizeof(SYS_EXT_DIR) + sizeof("/lib/") + sizeof(DEFAULT_LIBPATH) + 1,
                                                     mtInternal);
    sprintf(ld_library_path, "%s%s" SYS_EXT_DIR "/lib:" DEFAULT_LIBPATH, v, v_colon);
    Arguments::set_library_path(ld_library_path);
    FREE_C_HEAP_ARRAY(char, ld_library_path);
  }

  // Extensions directories.
  sprintf(buf, "%s" EXTENSIONS_DIR ":" SYS_EXT_DIR EXTENSIONS_DIR, Arguments::get_java_home());
  Arguments::set_ext_dirs(buf);

  FREE_C_HEAP_ARRAY(char, buf);

#undef DEFAULT_LIBPATH
#undef SYS_EXT_DIR
#undef EXTENSIONS_DIR
}

////////////////////////////////////////////////////////////////////////////////
// breakpoint support

void os::breakpoint() {
  BREAKPOINT;
}

extern "C" void breakpoint() {
  // use debugger to set breakpoint here
}

////////////////////////////////////////////////////////////////////////////////
// signal support

debug_only(static bool signal_sets_initialized = false);
static sigset_t unblocked_sigs, vm_sigs;

bool os::Linux::is_sig_ignored(int sig) {
  struct sigaction oact;
  sigaction(sig, (struct sigaction*)NULL, &oact);
  void* ohlr = oact.sa_sigaction ? CAST_FROM_FN_PTR(void*,  oact.sa_sigaction)
                                 : CAST_FROM_FN_PTR(void*,  oact.sa_handler);
  if (ohlr == CAST_FROM_FN_PTR(void*, SIG_IGN)) {
    return true;
  } else {
    return false;
  }
}

void os::Linux::signal_sets_init() {
  // Should also have an assertion stating we are still single-threaded.
  assert(!signal_sets_initialized, "Already initialized");
  // Fill in signals that are necessarily unblocked for all threads in
  // the VM. Currently, we unblock the following signals:
  // SHUTDOWN{1,2,3}_SIGNAL: for shutdown hooks support (unless over-ridden
  //                         by -Xrs (=ReduceSignalUsage));
  // BREAK_SIGNAL which is unblocked only by the VM thread and blocked by all
  // other threads. The "ReduceSignalUsage" boolean tells us not to alter
  // the dispositions or masks wrt these signals.
  // Programs embedding the VM that want to use the above signals for their
  // own purposes must, at this time, use the "-Xrs" option to prevent
  // interference with shutdown hooks and BREAK_SIGNAL thread dumping.
  // (See bug 4345157, and other related bugs).
  // In reality, though, unblocking these signals is really a nop, since
  // these signals are not blocked by default.
  sigemptyset(&unblocked_sigs);
  sigaddset(&unblocked_sigs, SIGILL);
  sigaddset(&unblocked_sigs, SIGSEGV);
  sigaddset(&unblocked_sigs, SIGBUS);
  sigaddset(&unblocked_sigs, SIGFPE);
#if defined(PPC64)
  sigaddset(&unblocked_sigs, SIGTRAP);
#endif
  sigaddset(&unblocked_sigs, SR_signum);

  if (!ReduceSignalUsage) {
    if (!os::Linux::is_sig_ignored(SHUTDOWN1_SIGNAL)) {
      sigaddset(&unblocked_sigs, SHUTDOWN1_SIGNAL);
    }
    if (!os::Linux::is_sig_ignored(SHUTDOWN2_SIGNAL)) {
      sigaddset(&unblocked_sigs, SHUTDOWN2_SIGNAL);
    }
    if (!os::Linux::is_sig_ignored(SHUTDOWN3_SIGNAL)) {
      sigaddset(&unblocked_sigs, SHUTDOWN3_SIGNAL);
    }
  }
  // Fill in signals that are blocked by all but the VM thread.
  sigemptyset(&vm_sigs);
  if (!ReduceSignalUsage) {
    sigaddset(&vm_sigs, BREAK_SIGNAL);
  }
  debug_only(signal_sets_initialized = true);

}

// These are signals that are unblocked while a thread is running Java.
// (For some reason, they get blocked by default.)
sigset_t* os::Linux::unblocked_signals() {
  assert(signal_sets_initialized, "Not initialized");
  return &unblocked_sigs;
}

// These are the signals that are blocked while a (non-VM) thread is
// running Java. Only the VM thread handles these signals.
sigset_t* os::Linux::vm_signals() {
  assert(signal_sets_initialized, "Not initialized");
  return &vm_sigs;
}

void os::Linux::hotspot_sigmask(Thread* thread) {

  //Save caller's signal mask before setting VM signal mask
  sigset_t caller_sigmask;
  pthread_sigmask(SIG_BLOCK, NULL, &caller_sigmask);

  OSThread* osthread = thread->osthread();
  osthread->set_caller_sigmask(caller_sigmask);

  pthread_sigmask(SIG_UNBLOCK, os::Linux::unblocked_signals(), NULL);

  if (!ReduceSignalUsage) {
    if (thread->is_VM_thread()) {
      // Only the VM thread handles BREAK_SIGNAL ...
      pthread_sigmask(SIG_UNBLOCK, vm_signals(), NULL);
    } else {
      // ... all other threads block BREAK_SIGNAL
      pthread_sigmask(SIG_BLOCK, vm_signals(), NULL);
    }
  }
}

//////////////////////////////////////////////////////////////////////////////
// detecting pthread library

void os::Linux::libpthread_init() {
  // Save glibc and pthread version strings.
#if !defined(_CS_GNU_LIBC_VERSION) || \
    !defined(_CS_GNU_LIBPTHREAD_VERSION)
  #error "glibc too old (< 2.3.2)"
#endif

  size_t n = confstr(_CS_GNU_LIBC_VERSION, NULL, 0);
  assert(n > 0, "cannot retrieve glibc version");
  char *str = (char *)malloc(n, mtInternal);
  confstr(_CS_GNU_LIBC_VERSION, str, n);
  os::Linux::set_glibc_version(str);

  n = confstr(_CS_GNU_LIBPTHREAD_VERSION, NULL, 0);
  assert(n > 0, "cannot retrieve pthread version");
  str = (char *)malloc(n, mtInternal);
  confstr(_CS_GNU_LIBPTHREAD_VERSION, str, n);
  os::Linux::set_libpthread_version(str);
}

/////////////////////////////////////////////////////////////////////////////
// thread stack expansion

// os::Linux::manually_expand_stack() takes care of expanding the thread
// stack. Note that this is normally not needed: pthread stacks allocate
// thread stack using mmap() without MAP_NORESERVE, so the stack is already
// committed. Therefore it is not necessary to expand the stack manually.
//
// Manually expanding the stack was historically needed on LinuxThreads
// thread stacks, which were allocated with mmap(MAP_GROWSDOWN). Nowadays
// it is kept to deal with very rare corner cases:
//
// For one, user may run the VM on an own implementation of threads
// whose stacks are - like the old LinuxThreads - implemented using
// mmap(MAP_GROWSDOWN).
//
// Also, this coding may be needed if the VM is running on the primordial
// thread. Normally we avoid running on the primordial thread; however,
// user may still invoke the VM on the primordial thread.
//
// The following historical comment describes the details about running
// on a thread stack allocated with mmap(MAP_GROWSDOWN):


// Force Linux kernel to expand current thread stack. If "bottom" is close
// to the stack guard, caller should block all signals.
//
// MAP_GROWSDOWN:
//   A special mmap() flag that is used to implement thread stacks. It tells
//   kernel that the memory region should extend downwards when needed. This
//   allows early versions of LinuxThreads to only mmap the first few pages
//   when creating a new thread. Linux kernel will automatically expand thread
//   stack as needed (on page faults).
//
//   However, because the memory region of a MAP_GROWSDOWN stack can grow on
//   demand, if a page fault happens outside an already mapped MAP_GROWSDOWN
//   region, it's hard to tell if the fault is due to a legitimate stack
//   access or because of reading/writing non-exist memory (e.g. buffer
//   overrun). As a rule, if the fault happens below current stack pointer,
//   Linux kernel does not expand stack, instead a SIGSEGV is sent to the
//   application (see Linux kernel fault.c).
//
//   This Linux feature can cause SIGSEGV when VM bangs thread stack for
//   stack overflow detection.
//
//   Newer version of LinuxThreads (since glibc-2.2, or, RH-7.x) and NPTL do
//   not use MAP_GROWSDOWN.
//
// To get around the problem and allow stack banging on Linux, we need to
// manually expand thread stack after receiving the SIGSEGV.
//
// There are two ways to expand thread stack to address "bottom", we used
// both of them in JVM before 1.5:
//   1. adjust stack pointer first so that it is below "bottom", and then
//      touch "bottom"
//   2. mmap() the page in question
//
// Now alternate signal stack is gone, it's harder to use 2. For instance,
// if current sp is already near the lower end of page 101, and we need to
// call mmap() to map page 100, it is possible that part of the mmap() frame
// will be placed in page 100. When page 100 is mapped, it is zero-filled.
// That will destroy the mmap() frame and cause VM to crash.
//
// The following code works by adjusting sp first, then accessing the "bottom"
// page to force a page fault. Linux kernel will then automatically expand the
// stack mapping.
//
// _expand_stack_to() assumes its frame size is less than page size, which
// should always be true if the function is not inlined.

static void NOINLINE _expand_stack_to(address bottom) {
  address sp;
  size_t size;
  volatile char *p;

  // Adjust bottom to point to the largest address within the same page, it
  // gives us a one-page buffer if alloca() allocates slightly more memory.
  bottom = (address)align_down((uintptr_t)bottom, os::Linux::page_size());
  bottom += os::Linux::page_size() - 1;

  // sp might be slightly above current stack pointer; if that's the case, we
  // will alloca() a little more space than necessary, which is OK. Don't use
  // os::current_stack_pointer(), as its result can be slightly below current
  // stack pointer, causing us to not alloca enough to reach "bottom".
  sp = (address)&sp;

  if (sp > bottom) {
    size = sp - bottom;
    p = (volatile char *)alloca(size);
    assert(p != NULL && p <= (volatile char *)bottom, "alloca problem?");
    p[0] = '\0';
  }
}

bool os::Linux::manually_expand_stack(JavaThread * t, address addr) {
  assert(t!=NULL, "just checking");
  assert(t->osthread()->expanding_stack(), "expand should be set");
  assert(t->stack_base() != NULL, "stack_base was not initialized");

  if (addr <  t->stack_base() && addr >= t->stack_reserved_zone_base()) {
    sigset_t mask_all, old_sigset;
    sigfillset(&mask_all);
    pthread_sigmask(SIG_SETMASK, &mask_all, &old_sigset);
    _expand_stack_to(addr);
    pthread_sigmask(SIG_SETMASK, &old_sigset, NULL);
    return true;
  }
  return false;
}

//////////////////////////////////////////////////////////////////////////////
// create new thread

// Thread start routine for all newly created threads
static void *thread_native_entry(Thread *thread) {
  // Try to randomize the cache line index of hot stack frames.
  // This helps when threads of the same stack traces evict each other's
  // cache lines. The threads can be either from the same JVM instance, or
  // from different JVM instances. The benefit is especially true for
  // processors with hyperthreading technology.
  static int counter = 0;
  int pid = os::current_process_id();
  alloca(((pid ^ counter++) & 7) * 128);

  thread->initialize_thread_current();

  OSThread* osthread = thread->osthread();
  Monitor* sync = osthread->startThread_lock();

  osthread->set_thread_id(os::current_thread_id());

  log_info(os, thread)("Thread is alive (tid: " UINTX_FORMAT ", pthread id: " UINTX_FORMAT ").",
    os::current_thread_id(), (uintx) pthread_self());

  if (UseNUMA) {
    int lgrp_id = os::numa_get_group_id();
    if (lgrp_id != -1) {
      thread->set_lgrp_id(lgrp_id);
    }
  }
  // initialize signal mask for this thread
  os::Linux::hotspot_sigmask(thread);

  // initialize floating point control register
  os::Linux::init_thread_fpu_state();

  // handshaking with parent thread
  {
    MutexLockerEx ml(sync, Mutex::_no_safepoint_check_flag);

    // notify parent thread
    osthread->set_state(INITIALIZED);
    sync->notify_all();

    // wait until os::start_thread()
    while (osthread->get_state() == INITIALIZED) {
      sync->wait(Mutex::_no_safepoint_check_flag);
    }
  }

  // call one more level start routine
  thread->run();

  log_info(os, thread)("Thread finished (tid: " UINTX_FORMAT ", pthread id: " UINTX_FORMAT ").",
    os::current_thread_id(), (uintx) pthread_self());

  // If a thread has not deleted itself ("delete this") as part of its
  // termination sequence, we have to ensure thread-local-storage is
  // cleared before we actually terminate. No threads should ever be
  // deleted asynchronously with respect to their termination.
  if (Thread::current_or_null_safe() != NULL) {
    assert(Thread::current_or_null_safe() == thread, "current thread is wrong");
    thread->clear_thread_current();
  }

  return 0;
}

bool os::create_thread(Thread* thread, ThreadType thr_type,
                       size_t req_stack_size) {
  assert(thread->osthread() == NULL, "caller responsible");

  // Allocate the OSThread object
  OSThread* osthread = new OSThread(NULL, NULL);
  if (osthread == NULL) {
    return false;
  }

  // set the correct thread state
  osthread->set_thread_type(thr_type);

  // Initial state is ALLOCATED but not INITIALIZED
  osthread->set_state(ALLOCATED);

  thread->set_osthread(osthread);

  // init thread attributes
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  // Calculate stack size if it's not specified by caller.
  size_t stack_size = os::Posix::get_initial_stack_size(thr_type, req_stack_size);
  // In the Linux NPTL pthread implementation the guard size mechanism
  // is not implemented properly. The posix standard requires adding
  // the size of the guard pages to the stack size, instead Linux
  // takes the space out of 'stacksize'. Thus we adapt the requested
  // stack_size by the size of the guard pages to mimick proper
  // behaviour. However, be careful not to end up with a size
  // of zero due to overflow. Don't add the guard page in that case.
  size_t guard_size = os::Linux::default_guard_size(thr_type);
  if (stack_size <= SIZE_MAX - guard_size) {
    stack_size += guard_size;
  }
  assert(is_aligned(stack_size, os::vm_page_size()), "stack_size not aligned");

  int status = pthread_attr_setstacksize(&attr, stack_size);
  assert_status(status == 0, status, "pthread_attr_setstacksize");

  // Configure glibc guard page.
  pthread_attr_setguardsize(&attr, os::Linux::default_guard_size(thr_type));

  ThreadState state;

  {
    pthread_t tid;
    int ret = pthread_create(&tid, &attr, (void* (*)(void*)) thread_native_entry, thread);

    char buf[64];
    if (ret == 0) {
      log_info(os, thread)("Thread started (pthread id: " UINTX_FORMAT ", attributes: %s). ",
        (uintx) tid, os::Posix::describe_pthread_attr(buf, sizeof(buf), &attr));
    } else {
      log_warning(os, thread)("Failed to start thread - pthread_create failed (%s) for attributes: %s.",
        os::errno_name(ret), os::Posix::describe_pthread_attr(buf, sizeof(buf), &attr));
    }

    pthread_attr_destroy(&attr);

    if (ret != 0) {
      // Need to clean up stuff we've allocated so far
      thread->set_osthread(NULL);
      delete osthread;
      return false;
    }

    // Store pthread info into the OSThread
    osthread->set_pthread_id(tid);

    // Wait until child thread is either initialized or aborted
    {
      Monitor* sync_with_child = osthread->startThread_lock();
      MutexLockerEx ml(sync_with_child, Mutex::_no_safepoint_check_flag);
      while ((state = osthread->get_state()) == ALLOCATED) {
        sync_with_child->wait(Mutex::_no_safepoint_check_flag);
      }
    }
  }

  // Aborted due to thread limit being reached
  if (state == ZOMBIE) {
    thread->set_osthread(NULL);
    delete osthread;
    return false;
  }

  // The thread is returned suspended (in state INITIALIZED),
  // and is started higher up in the call chain
  assert(state == INITIALIZED, "race condition");
  return true;
}

/////////////////////////////////////////////////////////////////////////////
// attach existing thread

// bootstrap the main thread
bool os::create_main_thread(JavaThread* thread) {
  assert(os::Linux::_main_thread == pthread_self(), "should be called inside main thread");
  return create_attached_thread(thread);
}

bool os::create_attached_thread(JavaThread* thread) {
#ifdef ASSERT
  thread->verify_not_published();
#endif

  // Allocate the OSThread object
  OSThread* osthread = new OSThread(NULL, NULL);

  if (osthread == NULL) {
    return false;
  }

  // Store pthread info into the OSThread
  osthread->set_thread_id(os::Linux::gettid());
  osthread->set_pthread_id(::pthread_self());

  // initialize floating point control register
  os::Linux::init_thread_fpu_state();

  // Initial thread state is RUNNABLE
  osthread->set_state(RUNNABLE);

  thread->set_osthread(osthread);

  if (UseNUMA) {
    int lgrp_id = os::numa_get_group_id();
    if (lgrp_id != -1) {
      thread->set_lgrp_id(lgrp_id);
    }
  }

  if (os::is_primordial_thread()) {
    // If current thread is primordial thread, its stack is mapped on demand,
    // see notes about MAP_GROWSDOWN. Here we try to force kernel to map
    // the entire stack region to avoid SEGV in stack banging.
    // It is also useful to get around the heap-stack-gap problem on SuSE
    // kernel (see 4821821 for details). We first expand stack to the top
    // of yellow zone, then enable stack yellow zone (order is significant,
    // enabling yellow zone first will crash JVM on SuSE Linux), so there
    // is no gap between the last two virtual memory regions.

    JavaThread *jt = (JavaThread *)thread;
    address addr = jt->stack_reserved_zone_base();
    assert(addr != NULL, "initialization problem?");
    assert(jt->stack_available(addr) > 0, "stack guard should not be enabled");

    osthread->set_expanding_stack();
    os::Linux::manually_expand_stack(jt, addr);
    osthread->clear_expanding_stack();
  }

  // initialize signal mask for this thread
  // and save the caller's signal mask
  os::Linux::hotspot_sigmask(thread);

  log_info(os, thread)("Thread attached (tid: " UINTX_FORMAT ", pthread id: " UINTX_FORMAT ").",
    os::current_thread_id(), (uintx) pthread_self());

  return true;
}

void os::pd_start_thread(Thread* thread) {
  OSThread * osthread = thread->osthread();
  assert(osthread->get_state() != INITIALIZED, "just checking");
  Monitor* sync_with_child = osthread->startThread_lock();
  MutexLockerEx ml(sync_with_child, Mutex::_no_safepoint_check_flag);
  sync_with_child->notify();
}

// Free Linux resources related to the OSThread
void os::free_thread(OSThread* osthread) {
  assert(osthread != NULL, "osthread not set");

  // We are told to free resources of the argument thread,
  // but we can only really operate on the current thread.
  assert(Thread::current()->osthread() == osthread,
         "os::free_thread but not current thread");

#ifdef ASSERT
  sigset_t current;
  sigemptyset(&current);
  pthread_sigmask(SIG_SETMASK, NULL, &current);
  assert(!sigismember(&current, SR_signum), "SR signal should not be blocked!");
#endif

  // Restore caller's signal mask
  sigset_t sigmask = osthread->caller_sigmask();
  pthread_sigmask(SIG_SETMASK, &sigmask, NULL);

  delete osthread;
}

//////////////////////////////////////////////////////////////////////////////
// primordial thread

// Check if current thread is the primordial thread, similar to Solaris thr_main.
bool os::is_primordial_thread(void) {
  char dummy;
  // If called before init complete, thread stack bottom will be null.
  // Can be called if fatal error occurs before initialization.
  if (os::Linux::initial_thread_stack_bottom() == NULL) return false;
  assert(os::Linux::initial_thread_stack_bottom() != NULL &&
         os::Linux::initial_thread_stack_size()   != 0,
         "os::init did not locate primordial thread's stack region");
  if ((address)&dummy >= os::Linux::initial_thread_stack_bottom() &&
      (address)&dummy < os::Linux::initial_thread_stack_bottom() +
                        os::Linux::initial_thread_stack_size()) {
    return true;
  } else {
    return false;
  }
}

// Find the virtual memory area that contains addr
static bool find_vma(address addr, address* vma_low, address* vma_high) {
  FILE *fp = fopen("/proc/self/maps", "r");
  if (fp) {
    address low, high;
    while (!feof(fp)) {
      if (fscanf(fp, "%p-%p", &low, &high) == 2) {
        if (low <= addr && addr < high) {
          if (vma_low)  *vma_low  = low;
          if (vma_high) *vma_high = high;
          fclose(fp);
          return true;
        }
      }
      for (;;) {
        int ch = fgetc(fp);
        if (ch == EOF || ch == (int)'\n') break;
      }
    }
    fclose(fp);
  }
  return false;
}

// Locate primordial thread stack. This special handling of primordial thread stack
// is needed because pthread_getattr_np() on most (all?) Linux distros returns
// bogus value for the primordial process thread. While the launcher has created
// the VM in a new thread since JDK 6, we still have to allow for the use of the
// JNI invocation API from a primordial thread.
void os::Linux::capture_initial_stack(size_t max_size) {

  // max_size is either 0 (which means accept OS default for thread stacks) or
  // a user-specified value known to be at least the minimum needed. If we
  // are actually on the primordial thread we can make it appear that we have a
  // smaller max_size stack by inserting the guard pages at that location. But we
  // cannot do anything to emulate a larger stack than what has been provided by
  // the OS or threading library. In fact if we try to use a stack greater than
  // what is set by rlimit then we will crash the hosting process.

  // Maximum stack size is the easy part, get it from RLIMIT_STACK.
  // If this is "unlimited" then it will be a huge value.
  struct rlimit rlim;
  getrlimit(RLIMIT_STACK, &rlim);
  size_t stack_size = rlim.rlim_cur;

  // 6308388: a bug in ld.so will relocate its own .data section to the
  //   lower end of primordial stack; reduce ulimit -s value a little bit
  //   so we won't install guard page on ld.so's data section.
  //   But ensure we don't underflow the stack size - allow 1 page spare
  if (stack_size >= (size_t)(3 * page_size())) {
    stack_size -= 2 * page_size();
  }

  // Try to figure out where the stack base (top) is. This is harder.
  //
  // When an application is started, glibc saves the initial stack pointer in
  // a global variable "__libc_stack_end", which is then used by system
  // libraries. __libc_stack_end should be pretty close to stack top. The
  // variable is available since the very early days. However, because it is
  // a private interface, it could disappear in the future.
  //
  // Linux kernel saves start_stack information in /proc/<pid>/stat. Similar
  // to __libc_stack_end, it is very close to stack top, but isn't the real
  // stack top. Note that /proc may not exist if VM is running as a chroot
  // program, so reading /proc/<pid>/stat could fail. Also the contents of
  // /proc/<pid>/stat could change in the future (though unlikely).
  //
  // We try __libc_stack_end first. If that doesn't work, look for
  // /proc/<pid>/stat. If neither of them works, we use current stack pointer
  // as a hint, which should work well in most cases.

  uintptr_t stack_start;

  // try __libc_stack_end first
  uintptr_t *p = (uintptr_t *)dlsym(RTLD_DEFAULT, "__libc_stack_end");
  if (p && *p) {
    stack_start = *p;
  } else {
    // see if we can get the start_stack field from /proc/self/stat
    FILE *fp;
    int pid;
    char state;
    int ppid;
    int pgrp;
    int session;
    int nr;
    int tpgrp;
    unsigned long flags;
    unsigned long minflt;
    unsigned long cminflt;
    unsigned long majflt;
    unsigned long cmajflt;
    unsigned long utime;
    unsigned long stime;
    long cutime;
    long cstime;
    long prio;
    long nice;
    long junk;
    long it_real;
    uintptr_t start;
    uintptr_t vsize;
    intptr_t rss;
    uintptr_t rsslim;
    uintptr_t scodes;
    uintptr_t ecode;
    int i;

    // Figure what the primordial thread stack base is. Code is inspired
    // by email from Hans Boehm. /proc/self/stat begins with current pid,
    // followed by command name surrounded by parentheses, state, etc.
    char stat[2048];
    int statlen;

    fp = fopen("/proc/self/stat", "r");
    if (fp) {
      statlen = fread(stat, 1, 2047, fp);
      stat[statlen] = '\0';
      fclose(fp);

      // Skip pid and the command string. Note that we could be dealing with
      // weird command names, e.g. user could decide to rename java launcher
      // to "java 1.4.2 :)", then the stat file would look like
      //                1234 (java 1.4.2 :)) R ... ...
      // We don't really need to know the command string, just find the last
      // occurrence of ")" and then start parsing from there. See bug 4726580.
      char * s = strrchr(stat, ')');

      i = 0;
      if (s) {
        // Skip blank chars
        do { s++; } while (s && isspace(*s));

#define _UFM UINTX_FORMAT
#define _DFM INTX_FORMAT

        //                                     1   1   1   1   1   1   1   1   1   1   2   2    2    2    2    2    2    2    2
        //              3  4  5  6  7  8   9   0   1   2   3   4   5   6   7   8   9   0   1    2    3    4    5    6    7    8
        i = sscanf(s, "%c %d %d %d %d %d %lu %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld " _UFM _UFM _DFM _UFM _UFM _UFM _UFM,
                   &state,          // 3  %c
                   &ppid,           // 4  %d
                   &pgrp,           // 5  %d
                   &session,        // 6  %d
                   &nr,             // 7  %d
                   &tpgrp,          // 8  %d
                   &flags,          // 9  %lu
                   &minflt,         // 10 %lu
                   &cminflt,        // 11 %lu
                   &majflt,         // 12 %lu
                   &cmajflt,        // 13 %lu
                   &utime,          // 14 %lu
                   &stime,          // 15 %lu
                   &cutime,         // 16 %ld
                   &cstime,         // 17 %ld
                   &prio,           // 18 %ld
                   &nice,           // 19 %ld
                   &junk,           // 20 %ld
                   &it_real,        // 21 %ld
                   &start,          // 22 UINTX_FORMAT
                   &vsize,          // 23 UINTX_FORMAT
                   &rss,            // 24 INTX_FORMAT
                   &rsslim,         // 25 UINTX_FORMAT
                   &scodes,         // 26 UINTX_FORMAT
                   &ecode,          // 27 UINTX_FORMAT
                   &stack_start);   // 28 UINTX_FORMAT
      }

#undef _UFM
#undef _DFM

      if (i != 28 - 2) {
        assert(false, "Bad conversion from /proc/self/stat");
        // product mode - assume we are the primordial thread, good luck in the
        // embedded case.
        warning("Can't detect primordial thread stack location - bad conversion");
        stack_start = (uintptr_t) &rlim;
      }
    } else {
      // For some reason we can't open /proc/self/stat (for example, running on
      // FreeBSD with a Linux emulator, or inside chroot), this should work for
      // most cases, so don't abort:
      warning("Can't detect primordial thread stack location - no /proc/self/stat");
      stack_start = (uintptr_t) &rlim;
    }
  }

  // Now we have a pointer (stack_start) very close to the stack top, the
  // next thing to do is to figure out the exact location of stack top. We
  // can find out the virtual memory area that contains stack_start by
  // reading /proc/self/maps, it should be the last vma in /proc/self/maps,
  // and its upper limit is the real stack top. (again, this would fail if
  // running inside chroot, because /proc may not exist.)

  uintptr_t stack_top;
  address low, high;
  if (find_vma((address)stack_start, &low, &high)) {
    // success, "high" is the true stack top. (ignore "low", because initial
    // thread stack grows on demand, its real bottom is high - RLIMIT_STACK.)
    stack_top = (uintptr_t)high;
  } else {
    // failed, likely because /proc/self/maps does not exist
    warning("Can't detect primordial thread stack location - find_vma failed");
    // best effort: stack_start is normally within a few pages below the real
    // stack top, use it as stack top, and reduce stack size so we won't put
    // guard page outside stack.
    stack_top = stack_start;
    stack_size -= 16 * page_size();
  }

  // stack_top could be partially down the page so align it
  stack_top = align_up(stack_top, page_size());

  // Allowed stack value is minimum of max_size and what we derived from rlimit
  if (max_size > 0) {
    _initial_thread_stack_size = MIN2(max_size, stack_size);
  } else {
    // Accept the rlimit max, but if stack is unlimited then it will be huge, so
    // clamp it at 8MB as we do on Solaris
    _initial_thread_stack_size = MIN2(stack_size, 8*M);
  }
  _initial_thread_stack_size = align_down(_initial_thread_stack_size, page_size());
  _initial_thread_stack_bottom = (address)stack_top - _initial_thread_stack_size;

  assert(_initial_thread_stack_bottom < (address)stack_top, "overflow!");

  if (log_is_enabled(Info, os, thread)) {
    // See if we seem to be on primordial process thread
    bool primordial = uintptr_t(&rlim) > uintptr_t(_initial_thread_stack_bottom) &&
                      uintptr_t(&rlim) < stack_top;

    log_info(os, thread)("Capturing initial stack in %s thread: req. size: " SIZE_FORMAT "K, actual size: "
                         SIZE_FORMAT "K, top=" INTPTR_FORMAT ", bottom=" INTPTR_FORMAT,
                         primordial ? "primordial" : "user", max_size / K,  _initial_thread_stack_size / K,
                         stack_top, intptr_t(_initial_thread_stack_bottom));
  }
}

////////////////////////////////////////////////////////////////////////////////
// time support

// Time since start-up in seconds to a fine granularity.
// Used by VMSelfDestructTimer and the MemProfiler.
double os::elapsedTime() {

  return ((double)os::elapsed_counter()) / os::elapsed_frequency(); // nanosecond resolution
}

jlong os::elapsed_counter() {
  return javaTimeNanos() - initial_time_count;
}

jlong os::elapsed_frequency() {
  return NANOSECS_PER_SEC; // nanosecond resolution
}

bool os::supports_vtime() { return true; }
bool os::enable_vtime()   { return false; }
bool os::vtime_enabled()  { return false; }

double os::elapsedVTime() {
  struct rusage usage;
  int retval = getrusage(RUSAGE_THREAD, &usage);
  if (retval == 0) {
    return (double) (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + (double) (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / (1000 * 1000);
  } else {
    // better than nothing, but not much
    return elapsedTime();
  }
}

jlong os::javaTimeMillis() {
  timeval time;
  int status = gettimeofday(&time, NULL);
  assert(status != -1, "linux error");
  return jlong(time.tv_sec) * 1000  +  jlong(time.tv_usec / 1000);
}

void os::javaTimeSystemUTC(jlong &seconds, jlong &nanos) {
  timeval time;
  int status = gettimeofday(&time, NULL);
  assert(status != -1, "linux error");
  seconds = jlong(time.tv_sec);
  nanos = jlong(time.tv_usec) * 1000;
}


#ifndef CLOCK_MONOTONIC
  #define CLOCK_MONOTONIC (1)
#endif

void os::Linux::clock_init() {
  // we do dlopen's in this particular order due to bug in linux
  // dynamical loader (see 6348968) leading to crash on exit
  void* handle = dlopen("librt.so.1", RTLD_LAZY);
  if (handle == NULL) {
    handle = dlopen("librt.so", RTLD_LAZY);
  }

  if (handle) {
    int (*clock_getres_func)(clockid_t, struct timespec*) =
           (int(*)(clockid_t, struct timespec*))dlsym(handle, "clock_getres");
    int (*clock_gettime_func)(clockid_t, struct timespec*) =
           (int(*)(clockid_t, struct timespec*))dlsym(handle, "clock_gettime");
    if (clock_getres_func && clock_gettime_func) {
      // See if monotonic clock is supported by the kernel. Note that some
      // early implementations simply return kernel jiffies (updated every
      // 1/100 or 1/1000 second). It would be bad to use such a low res clock
      // for nano time (though the monotonic property is still nice to have).
      // It's fixed in newer kernels, however clock_getres() still returns
      // 1/HZ. We check if clock_getres() works, but will ignore its reported
      // resolution for now. Hopefully as people move to new kernels, this
      // won't be a problem.
      struct timespec res;
      struct timespec tp;
      if (clock_getres_func (CLOCK_MONOTONIC, &res) == 0 &&
          clock_gettime_func(CLOCK_MONOTONIC, &tp)  == 0) {
        // yes, monotonic clock is supported
        _clock_gettime = clock_gettime_func;
        return;
      } else {
        // close librt if there is no monotonic clock
        dlclose(handle);
      }
    }
  }
  warning("No monotonic clock was available - timed services may " \
          "be adversely affected if the time-of-day clock changes");
}

#ifndef SYS_clock_getres
  #if defined(X86) || defined(PPC64) || defined(S390)
    #define SYS_clock_getres AMD64_ONLY(229) IA32_ONLY(266) PPC64_ONLY(247) S390_ONLY(261)
    #define sys_clock_getres(x,y)  ::syscall(SYS_clock_getres, x, y)
  #else
    #warning "SYS_clock_getres not defined for this platform, disabling fast_thread_cpu_time"
    #define sys_clock_getres(x,y)  -1
  #endif
#else
  #define sys_clock_getres(x,y)  ::syscall(SYS_clock_getres, x, y)
#endif

void os::Linux::fast_thread_clock_init() {
  if (!UseLinuxPosixThreadCPUClocks) {
    return;
  }
  clockid_t clockid;
  struct timespec tp;
  int (*pthread_getcpuclockid_func)(pthread_t, clockid_t *) =
      (int(*)(pthread_t, clockid_t *)) dlsym(RTLD_DEFAULT, "pthread_getcpuclockid");

  // Switch to using fast clocks for thread cpu time if
  // the sys_clock_getres() returns 0 error code.
  // Note, that some kernels may support the current thread
  // clock (CLOCK_THREAD_CPUTIME_ID) but not the clocks
  // returned by the pthread_getcpuclockid().
  // If the fast Posix clocks are supported then the sys_clock_getres()
  // must return at least tp.tv_sec == 0 which means a resolution
  // better than 1 sec. This is extra check for reliability.

  if (pthread_getcpuclockid_func &&
      pthread_getcpuclockid_func(_main_thread, &clockid) == 0 &&
      sys_clock_getres(clockid, &tp) == 0 && tp.tv_sec == 0) {
    _supports_fast_thread_cpu_time = true;
    _pthread_getcpuclockid = pthread_getcpuclockid_func;
  }
}

jlong os::javaTimeNanos() {
  if (os::supports_monotonic_clock()) {
    struct timespec tp;
    int status = Linux::clock_gettime(CLOCK_MONOTONIC, &tp);
    assert(status == 0, "gettime error");
    jlong result = jlong(tp.tv_sec) * (1000 * 1000 * 1000) + jlong(tp.tv_nsec);
    return result;
  } else {
    timeval time;
    int status = gettimeofday(&time, NULL);
    assert(status != -1, "linux error");
    jlong usecs = jlong(time.tv_sec) * (1000 * 1000) + jlong(time.tv_usec);
    return 1000 * usecs;
  }
}

void os::javaTimeNanos_info(jvmtiTimerInfo *info_ptr) {
  if (os::supports_monotonic_clock()) {
    info_ptr->max_value = ALL_64_BITS;

    // CLOCK_MONOTONIC - amount of time since some arbitrary point in the past
    info_ptr->may_skip_backward = false;      // not subject to resetting or drifting
    info_ptr->may_skip_forward = false;       // not subject to resetting or drifting
  } else {
    // gettimeofday - based on time in seconds since the Epoch thus does not wrap
    info_ptr->max_value = ALL_64_BITS;

    // gettimeofday is a real time clock so it skips
    info_ptr->may_skip_backward = true;
    info_ptr->may_skip_forward = true;
  }

  info_ptr->kind = JVMTI_TIMER_ELAPSED;                // elapsed not CPU time
}

// Return the real, user, and system times in seconds from an
// arbitrary fixed point in the past.
bool os::getTimesSecs(double* process_real_time,
                      double* process_user_time,
                      double* process_system_time) {
  struct tms ticks;
  clock_t real_ticks = times(&ticks);

  if (real_ticks == (clock_t) (-1)) {
    return false;
  } else {
    double ticks_per_second = (double) clock_tics_per_sec;
    *process_user_time = ((double) ticks.tms_utime) / ticks_per_second;
    *process_system_time = ((double) ticks.tms_stime) / ticks_per_second;
    *process_real_time = ((double) real_ticks) / ticks_per_second;

    return true;
  }
}


char * os::local_time_string(char *buf, size_t buflen) {
  struct tm t;
  time_t long_time;
  time(&long_time);
  localtime_r(&long_time, &t);
  jio_snprintf(buf, buflen, "%d-%02d-%02d %02d:%02d:%02d",
               t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
               t.tm_hour, t.tm_min, t.tm_sec);
  return buf;
}

struct tm* os::localtime_pd(const time_t* clock, struct tm*  res) {
  return localtime_r(clock, res);
}

////////////////////////////////////////////////////////////////////////////////
// runtime exit support

// Note: os::shutdown() might be called very early during initialization, or
// called from signal handler. Before adding something to os::shutdown(), make
// sure it is async-safe and can handle partially initialized VM.
void os::shutdown() {

  // allow PerfMemory to attempt cleanup of any persistent resources
  perfMemory_exit();

  // needs to remove object in file system
  AttachListener::abort();

  // flush buffered output, finish log files
  ostream_abort();

  // Check for abort hook
  abort_hook_t abort_hook = Arguments::abort_hook();
  if (abort_hook != NULL) {
    abort_hook();
  }

}

// Note: os::abort() might be called very early during initialization, or
// called from signal handler. Before adding something to os::abort(), make
// sure it is async-safe and can handle partially initialized VM.
void os::abort(bool dump_core, void* siginfo, const void* context) {
  os::shutdown();
  if (dump_core) {
#ifndef PRODUCT
    fdStream out(defaultStream::output_fd());
    out.print_raw("Current thread is ");
    char buf[16];
    jio_snprintf(buf, sizeof(buf), UINTX_FORMAT, os::current_thread_id());
    out.print_raw_cr(buf);
    out.print_raw_cr("Dumping core ...");
#endif
    ::abort(); // dump core
  }

  ::exit(1);
}

// Die immediately, no exit hook, no abort hook, no cleanup.
void os::die() {
  ::abort();
}


// This method is a copy of JDK's sysGetLastErrorString
// from src/solaris/hpi/src/system_md.c

size_t os::lasterror(char *buf, size_t len) {
  if (errno == 0)  return 0;

  const char *s = os::strerror(errno);
  size_t n = ::strlen(s);
  if (n >= len) {
    n = len - 1;
  }
  ::strncpy(buf, s, n);
  buf[n] = '\0';
  return n;
}

// thread_id is kernel thread id (similar to Solaris LWP id)
intx os::current_thread_id() { return os::Linux::gettid(); }
int os::current_process_id() {
  return ::getpid();
}

// DLL functions

const char* os::dll_file_extension() { return ".so"; }

// This must be hard coded because it's the system's temporary
// directory not the java application's temp directory, ala java.io.tmpdir.
const char* os::get_temp_directory() { return "/tmp"; }

static bool file_exists(const char* filename) {
  struct stat statbuf;
  if (filename == NULL || strlen(filename) == 0) {
    return false;
  }
  return os::stat(filename, &statbuf) == 0;
}

// check if addr is inside libjvm.so
bool os::address_is_in_vm(address addr) {
  static address libjvm_base_addr;
  Dl_info dlinfo;

  if (libjvm_base_addr == NULL) {
    if (dladdr(CAST_FROM_FN_PTR(void *, os::address_is_in_vm), &dlinfo) != 0) {
      libjvm_base_addr = (address)dlinfo.dli_fbase;
    }
    assert(libjvm_base_addr !=NULL, "Cannot obtain base address for libjvm");
  }

  if (dladdr((void *)addr, &dlinfo) != 0) {
    if (libjvm_base_addr == (address)dlinfo.dli_fbase) return true;
  }

  return false;
}

bool os::dll_address_to_function_name(address addr, char *buf,
                                      int buflen, int *offset,
                                      bool demangle) {
  // buf is not optional, but offset is optional
  assert(buf != NULL, "sanity check");

  Dl_info dlinfo;

  if (dladdr((void*)addr, &dlinfo) != 0) {
    // see if we have a matching symbol
    if (dlinfo.dli_saddr != NULL && dlinfo.dli_sname != NULL) {
      if (!(demangle && Decoder::demangle(dlinfo.dli_sname, buf, buflen))) {
        jio_snprintf(buf, buflen, "%s", dlinfo.dli_sname);
      }
      if (offset != NULL) *offset = addr - (address)dlinfo.dli_saddr;
      return true;
    }
    // no matching symbol so try for just file info
    if (dlinfo.dli_fname != NULL && dlinfo.dli_fbase != NULL) {
      if (Decoder::decode((address)(addr - (address)dlinfo.dli_fbase),
                          buf, buflen, offset, dlinfo.dli_fname, demangle)) {
        return true;
      }
    }
  }

  buf[0] = '\0';
  if (offset != NULL) *offset = -1;
  return false;
}

struct _address_to_library_name {
  address addr;          // input : memory address
  size_t  buflen;        //         size of fname
  char*   fname;         // output: library name
  address base;          //         library base addr
};

static int address_to_library_name_callback(struct dl_phdr_info *info,
                                            size_t size, void *data) {
  int i;
  bool found = false;
  address libbase = NULL;
  struct _address_to_library_name * d = (struct _address_to_library_name *)data;

  // iterate through all loadable segments
  for (i = 0; i < info->dlpi_phnum; i++) {
    address segbase = (address)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    if (info->dlpi_phdr[i].p_type == PT_LOAD) {
      // base address of a library is the lowest address of its loaded
      // segments.
      if (libbase == NULL || libbase > segbase) {
        libbase = segbase;
      }
      // see if 'addr' is within current segment
      if (segbase <= d->addr &&
          d->addr < segbase + info->dlpi_phdr[i].p_memsz) {
        found = true;
      }
    }
  }

  // dlpi_name is NULL or empty if the ELF file is executable, return 0
  // so dll_address_to_library_name() can fall through to use dladdr() which
  // can figure out executable name from argv[0].
  if (found && info->dlpi_name && info->dlpi_name[0]) {
    d->base = libbase;
    if (d->fname) {
      jio_snprintf(d->fname, d->buflen, "%s", info->dlpi_name);
    }
    return 1;
  }
  return 0;
}

bool os::dll_address_to_library_name(address addr, char* buf,
                                     int buflen, int* offset) {
  // buf is not optional, but offset is optional
  assert(buf != NULL, "sanity check");

  Dl_info dlinfo;
  struct _address_to_library_name data;

  // There is a bug in old glibc dladdr() implementation that it could resolve
  // to wrong library name if the .so file has a base address != NULL. Here
  // we iterate through the program headers of all loaded libraries to find
  // out which library 'addr' really belongs to. This workaround can be
  // removed once the minimum requirement for glibc is moved to 2.3.x.
  data.addr = addr;
  data.fname = buf;
  data.buflen = buflen;
  data.base = NULL;
  int rslt = dl_iterate_phdr(address_to_library_name_callback, (void *)&data);

  if (rslt) {
    // buf already contains library name
    if (offset) *offset = addr - data.base;
    return true;
  }
  if (dladdr((void*)addr, &dlinfo) != 0) {
    if (dlinfo.dli_fname != NULL) {
      jio_snprintf(buf, buflen, "%s", dlinfo.dli_fname);
    }
    if (dlinfo.dli_fbase != NULL && offset != NULL) {
      *offset = addr - (address)dlinfo.dli_fbase;
    }
    return true;
  }

  buf[0] = '\0';
  if (offset) *offset = -1;
  return false;
}

// Loads .dll/.so and
// in case of error it checks if .dll/.so was built for the
// same architecture as Hotspot is running on


// Remember the stack's state. The Linux dynamic linker will change
// the stack to 'executable' at most once, so we must safepoint only once.
bool os::Linux::_stack_is_executable = false;

// VM operation that loads a library.  This is necessary if stack protection
// of the Java stacks can be lost during loading the library.  If we
// do not stop the Java threads, they can stack overflow before the stacks
// are protected again.
class VM_LinuxDllLoad: public VM_Operation {
 private:
  const char *_filename;
  char *_ebuf;
  int _ebuflen;
  void *_lib;
 public:
  VM_LinuxDllLoad(const char *fn, char *ebuf, int ebuflen) :
    _filename(fn), _ebuf(ebuf), _ebuflen(ebuflen), _lib(NULL) {}
  VMOp_Type type() const { return VMOp_LinuxDllLoad; }
  void doit() {
    _lib = os::Linux::dll_load_in_vmthread(_filename, _ebuf, _ebuflen);
    os::Linux::_stack_is_executable = true;
  }
  void* loaded_library() { return _lib; }
};

void * os::dll_load(const char *filename, char *ebuf, int ebuflen) {
  void * result = NULL;
  bool load_attempted = false;

  // Check whether the library to load might change execution rights
  // of the stack. If they are changed, the protection of the stack
  // guard pages will be lost. We need a safepoint to fix this.
  //
  // See Linux man page execstack(8) for more info.
  if (os::uses_stack_guard_pages() && !os::Linux::_stack_is_executable) {
    if (!ElfFile::specifies_noexecstack(filename)) {
      if (!is_init_completed()) {
        os::Linux::_stack_is_executable = true;
        // This is OK - No Java threads have been created yet, and hence no
        // stack guard pages to fix.
        //
        // This should happen only when you are building JDK7 using a very
        // old version of JDK6 (e.g., with JPRT) and running test_gamma.
        //
        // Dynamic loader will make all stacks executable after
        // this function returns, and will not do that again.
        assert(Threads::first() == NULL, "no Java threads should exist yet.");
      } else {
        warning("You have loaded library %s which might have disabled stack guard. "
                "The VM will try to fix the stack guard now.\n"
                "It's highly recommended that you fix the library with "
                "'execstack -c <libfile>', or link it with '-z noexecstack'.",
                filename);

        assert(Thread::current()->is_Java_thread(), "must be Java thread");
        JavaThread *jt = JavaThread::current();
        if (jt->thread_state() != _thread_in_native) {
          // This happens when a compiler thread tries to load a hsdis-<arch>.so file
          // that requires ExecStack. Cannot enter safe point. Let's give up.
          warning("Unable to fix stack guard. Giving up.");
        } else {
          if (!LoadExecStackDllInVMThread) {
            // This is for the case where the DLL has an static
            // constructor function that executes JNI code. We cannot
            // load such DLLs in the VMThread.
            result = os::Linux::dlopen_helper(filename, ebuf, ebuflen);
          }

          ThreadInVMfromNative tiv(jt);
          debug_only(VMNativeEntryWrapper vew;)

          VM_LinuxDllLoad op(filename, ebuf, ebuflen);
          VMThread::execute(&op);
          if (LoadExecStackDllInVMThread) {
            result = op.loaded_library();
          }
          load_attempted = true;
        }
      }
    }
  }

  if (!load_attempted) {
    result = os::Linux::dlopen_helper(filename, ebuf, ebuflen);
  }

  if (result != NULL) {
    // Successful loading
    return result;
  }

  Elf32_Ehdr elf_head;
  int diag_msg_max_length=ebuflen-strlen(ebuf);
  char* diag_msg_buf=ebuf+strlen(ebuf);

  if (diag_msg_max_length==0) {
    // No more space in ebuf for additional diagnostics message
    return NULL;
  }


  int file_descriptor= ::open(filename, O_RDONLY | O_NONBLOCK);

  if (file_descriptor < 0) {
    // Can't open library, report dlerror() message
    return NULL;
  }

  bool failed_to_read_elf_head=
    (sizeof(elf_head)!=
     (::read(file_descriptor, &elf_head,sizeof(elf_head))));

  ::close(file_descriptor);
  if (failed_to_read_elf_head) {
    // file i/o error - report dlerror() msg
    return NULL;
  }

  typedef struct {
    Elf32_Half    code;         // Actual value as defined in elf.h
    Elf32_Half    compat_class; // Compatibility of archs at VM's sense
    unsigned char elf_class;    // 32 or 64 bit
    unsigned char endianess;    // MSB or LSB
    char*         name;         // String representation
  } arch_t;

#ifndef EM_486
  #define EM_486          6               /* Intel 80486 */
#endif
#ifndef EM_AARCH64
  #define EM_AARCH64    183               /* ARM AARCH64 */
#endif

  static const arch_t arch_array[]={
    {EM_386,         EM_386,     ELFCLASS32, ELFDATA2LSB, (char*)"IA 32"},
    {EM_486,         EM_386,     ELFCLASS32, ELFDATA2LSB, (char*)"IA 32"},
    {EM_IA_64,       EM_IA_64,   ELFCLASS64, ELFDATA2LSB, (char*)"IA 64"},
    {EM_X86_64,      EM_X86_64,  ELFCLASS64, ELFDATA2LSB, (char*)"AMD 64"},
    {EM_SPARC,       EM_SPARC,   ELFCLASS32, ELFDATA2MSB, (char*)"Sparc 32"},
    {EM_SPARC32PLUS, EM_SPARC,   ELFCLASS32, ELFDATA2MSB, (char*)"Sparc 32"},
    {EM_SPARCV9,     EM_SPARCV9, ELFCLASS64, ELFDATA2MSB, (char*)"Sparc v9 64"},
    {EM_PPC,         EM_PPC,     ELFCLASS32, ELFDATA2MSB, (char*)"Power PC 32"},
#if defined(VM_LITTLE_ENDIAN)
    {EM_PPC64,       EM_PPC64,   ELFCLASS64, ELFDATA2LSB, (char*)"Power PC 64 LE"},
    {EM_SH,          EM_SH,      ELFCLASS32, ELFDATA2LSB, (char*)"SuperH"},
#else
    {EM_PPC64,       EM_PPC64,   ELFCLASS64, ELFDATA2MSB, (char*)"Power PC 64"},
    {EM_SH,          EM_SH,      ELFCLASS32, ELFDATA2MSB, (char*)"SuperH BE"},
#endif
    {EM_ARM,         EM_ARM,     ELFCLASS32,   ELFDATA2LSB, (char*)"ARM"},
    {EM_S390,        EM_S390,    ELFCLASSNONE, ELFDATA2MSB, (char*)"IBM System/390"},
    {EM_ALPHA,       EM_ALPHA,   ELFCLASS64, ELFDATA2LSB, (char*)"Alpha"},
    {EM_MIPS_RS3_LE, EM_MIPS_RS3_LE, ELFCLASS32, ELFDATA2LSB, (char*)"MIPSel"},
    {EM_MIPS,        EM_MIPS,    ELFCLASS32, ELFDATA2MSB, (char*)"MIPS"},
    {EM_PARISC,      EM_PARISC,  ELFCLASS32, ELFDATA2MSB, (char*)"PARISC"},
    {EM_68K,         EM_68K,     ELFCLASS32, ELFDATA2MSB, (char*)"M68k"},
    {EM_AARCH64,     EM_AARCH64, ELFCLASS64, ELFDATA2LSB, (char*)"AARCH64"},
  };

#if  (defined IA32)
  static  Elf32_Half running_arch_code=EM_386;
#elif   (defined AMD64)
  static  Elf32_Half running_arch_code=EM_X86_64;
#elif  (defined IA64)
  static  Elf32_Half running_arch_code=EM_IA_64;
#elif  (defined __sparc) && (defined _LP64)
  static  Elf32_Half running_arch_code=EM_SPARCV9;
#elif  (defined __sparc) && (!defined _LP64)
  static  Elf32_Half running_arch_code=EM_SPARC;
#elif  (defined __powerpc64__)
  static  Elf32_Half running_arch_code=EM_PPC64;
#elif  (defined __powerpc__)
  static  Elf32_Half running_arch_code=EM_PPC;
#elif  (defined AARCH64)
  static  Elf32_Half running_arch_code=EM_AARCH64;
#elif  (defined ARM)
  static  Elf32_Half running_arch_code=EM_ARM;
#elif  (defined S390)
  static  Elf32_Half running_arch_code=EM_S390;
#elif  (defined ALPHA)
  static  Elf32_Half running_arch_code=EM_ALPHA;
#elif  (defined MIPSEL)
  static  Elf32_Half running_arch_code=EM_MIPS_RS3_LE;
#elif  (defined PARISC)
  static  Elf32_Half running_arch_code=EM_PARISC;
#elif  (defined MIPS)
  static  Elf32_Half running_arch_code=EM_MIPS;
#elif  (defined M68K)
  static  Elf32_Half running_arch_code=EM_68K;
#elif  (defined SH)
  static  Elf32_Half running_arch_code=EM_SH;
#else
    #error Method os::dll_load requires that one of following is defined:\
        AARCH64, ALPHA, ARM, AMD64, IA32, IA64, M68K, MIPS, MIPSEL, PARISC, __powerpc__, __powerpc64__, S390, SH, __sparc
#endif

  // Identify compatability class for VM's architecture and library's architecture
  // Obtain string descriptions for architectures

  arch_t lib_arch={elf_head.e_machine,0,elf_head.e_ident[EI_CLASS], elf_head.e_ident[EI_DATA], NULL};
  int running_arch_index=-1;

  for (unsigned int i=0; i < ARRAY_SIZE(arch_array); i++) {
    if (running_arch_code == arch_array[i].code) {
      running_arch_index    = i;
    }
    if (lib_arch.code == arch_array[i].code) {
      lib_arch.compat_class = arch_array[i].compat_class;
      lib_arch.name         = arch_array[i].name;
    }
  }

  assert(running_arch_index != -1,
         "Didn't find running architecture code (running_arch_code) in arch_array");
  if (running_arch_index == -1) {
    // Even though running architecture detection failed
    // we may still continue with reporting dlerror() message
    return NULL;
  }

  if (lib_arch.endianess != arch_array[running_arch_index].endianess) {
    ::snprintf(diag_msg_buf, diag_msg_max_length-1," (Possible cause: endianness mismatch)");
    return NULL;
  }

#ifndef S390
  if (lib_arch.elf_class != arch_array[running_arch_index].elf_class) {
    ::snprintf(diag_msg_buf, diag_msg_max_length-1," (Possible cause: architecture word width mismatch)");
    return NULL;
  }
#endif // !S390

  if (lib_arch.compat_class != arch_array[running_arch_index].compat_class) {
    if (lib_arch.name!=NULL) {
      ::snprintf(diag_msg_buf, diag_msg_max_length-1,
                 " (Possible cause: can't load %s-bit .so on a %s-bit platform)",
                 lib_arch.name, arch_array[running_arch_index].name);
    } else {
      ::snprintf(diag_msg_buf, diag_msg_max_length-1,
                 " (Possible cause: can't load this .so (machine code=0x%x) on a %s-bit platform)",
                 lib_arch.code,
                 arch_array[running_arch_index].name);
    }
  }

  return NULL;
}

void * os::Linux::dlopen_helper(const char *filename, char *ebuf,
                                int ebuflen) {
  void * result = ::dlopen(filename, RTLD_LAZY);
  if (result == NULL) {
    ::strncpy(ebuf, ::dlerror(), ebuflen - 1);
    ebuf[ebuflen-1] = '\0';
  }
  return result;
}

void * os::Linux::dll_load_in_vmthread(const char *filename, char *ebuf,
                                       int ebuflen) {
  void * result = NULL;
  if (LoadExecStackDllInVMThread) {
    result = dlopen_helper(filename, ebuf, ebuflen);
  }

  // Since 7019808, libjvm.so is linked with -noexecstack. If the VM loads a
  // library that requires an executable stack, or which does not have this
  // stack attribute set, dlopen changes the stack attribute to executable. The
  // read protection of the guard pages gets lost.
  //
  // Need to check _stack_is_executable again as multiple VM_LinuxDllLoad
  // may have been queued at the same time.

  if (!_stack_is_executable) {
    JavaThread *jt = Threads::first();

    while (jt) {
      if (!jt->stack_guard_zone_unused() &&     // Stack not yet fully initialized
          jt->stack_guards_enabled()) {         // No pending stack overflow exceptions
        if (!os::guard_memory((char *)jt->stack_end(), jt->stack_guard_zone_size())) {
          warning("Attempt to reguard stack yellow zone failed.");
        }
      }
      jt = jt->next();
    }
  }

  return result;
}

void* os::dll_lookup(void* handle, const char* name) {
  void* res = dlsym(handle, name);
  return res;
}

void* os::get_default_process_handle() {
  return (void*)::dlopen(NULL, RTLD_LAZY);
}

static bool _print_ascii_file(const char* filename, outputStream* st) {
  int fd = ::open(filename, O_RDONLY);
  if (fd == -1) {
    return false;
  }

  char buf[33];
  int bytes;
  buf[32] = '\0';
  while ((bytes = ::read(fd, buf, sizeof(buf)-1)) > 0) {
    st->print_raw(buf, bytes);
  }

  ::close(fd);

  return true;
}

void os::print_dll_info(outputStream *st) {
  st->print_cr("Dynamic libraries:");

  char fname[32];
  pid_t pid = os::Linux::gettid();

  jio_snprintf(fname, sizeof(fname), "/proc/%d/maps", pid);

  if (!_print_ascii_file(fname, st)) {
    st->print("Can not get library information for pid = %d\n", pid);
  }
}

int os::get_loaded_modules_info(os::LoadedModulesCallbackFunc callback, void *param) {
  FILE *procmapsFile = NULL;

  // Open the procfs maps file for the current process
  if ((procmapsFile = fopen("/proc/self/maps", "r")) != NULL) {
    // Allocate PATH_MAX for file name plus a reasonable size for other fields.
    char line[PATH_MAX + 100];

    // Read line by line from 'file'
    while (fgets(line, sizeof(line), procmapsFile) != NULL) {
      u8 base, top, offset, inode;
      char permissions[5];
      char device[6];
      char name[PATH_MAX + 1];

      // Parse fields from line
      sscanf(line, UINT64_FORMAT_X "-" UINT64_FORMAT_X " %4s " UINT64_FORMAT_X " %5s " INT64_FORMAT " %s",
             &base, &top, permissions, &offset, device, &inode, name);

      // Filter by device id '00:00' so that we only get file system mapped files.
      if (strcmp(device, "00:00") != 0) {

        // Call callback with the fields of interest
        if(callback(name, (address)base, (address)top, param)) {
          // Oops abort, callback aborted
          fclose(procmapsFile);
          return 1;
        }
      }
    }
    fclose(procmapsFile);
  }
  return 0;
}

void os::print_os_info_brief(outputStream* st) {
  os::Linux::print_distro_info(st);

  os::Posix::print_uname_info(st);

  os::Linux::print_libversion_info(st);

}

void os::print_os_info(outputStream* st) {
  st->print("OS:");

  os::Linux::print_distro_info(st);

  os::Posix::print_uname_info(st);

  // Print warning if unsafe chroot environment detected
  if (unsafe_chroot_detected) {
    st->print("WARNING!! ");
    st->print_cr("%s", unstable_chroot_error);
  }

  os::Linux::print_libversion_info(st);

  os::Posix::print_rlimit_info(st);

  os::Posix::print_load_average(st);

  os::Linux::print_full_memory_info(st);

  os::Linux::print_container_info(st);
}

// Try to identify popular distros.
// Most Linux distributions have a /etc/XXX-release file, which contains
// the OS version string. Newer Linux distributions have a /etc/lsb-release
// file that also contains the OS version string. Some have more than one
// /etc/XXX-release file (e.g. Mandrake has both /etc/mandrake-release and
// /etc/redhat-release.), so the order is important.
// Any Linux that is based on Redhat (i.e. Oracle, Mandrake, Sun JDS...) have
// their own specific XXX-release file as well as a redhat-release file.
// Because of this the XXX-release file needs to be searched for before the
// redhat-release file.
// Since Red Hat and SuSE have an lsb-release file that is not very descriptive the
// search for redhat-release / SuSE-release needs to be before lsb-release.
// Since the lsb-release file is the new standard it needs to be searched
// before the older style release files.
// Searching system-release (Red Hat) and os-release (other Linuxes) are a
// next to last resort.  The os-release file is a new standard that contains
// distribution information and the system-release file seems to be an old
// standard that has been replaced by the lsb-release and os-release files.
// Searching for the debian_version file is the last resort.  It contains
// an informative string like "6.0.6" or "wheezy/sid". Because of this
// "Debian " is printed before the contents of the debian_version file.

const char* distro_files[] = {
  "/etc/oracle-release",
  "/etc/mandriva-release",
  "/etc/mandrake-release",
  "/etc/sun-release",
  "/etc/redhat-release",
  "/etc/SuSE-release",
  "/etc/lsb-release",
  "/etc/turbolinux-release",
  "/etc/gentoo-release",
  "/etc/ltib-release",
  "/etc/angstrom-version",
  "/etc/system-release",
  "/etc/os-release",
  NULL };

void os::Linux::print_distro_info(outputStream* st) {
  for (int i = 0;; i++) {
    const char* file = distro_files[i];
    if (file == NULL) {
      break;  // done
    }
    // If file prints, we found it.
    if (_print_ascii_file(file, st)) {
      return;
    }
  }

  if (file_exists("/etc/debian_version")) {
    st->print("Debian ");
    _print_ascii_file("/etc/debian_version", st);
  } else {
    st->print("Linux");
  }
  st->cr();
}

static void parse_os_info_helper(FILE* fp, char* distro, size_t length, bool get_first_line) {
  char buf[256];
  while (fgets(buf, sizeof(buf), fp)) {
    // Edit out extra stuff in expected format
    if (strstr(buf, "DISTRIB_DESCRIPTION=") != NULL || strstr(buf, "PRETTY_NAME=") != NULL) {
      char* ptr = strstr(buf, "\"");  // the name is in quotes
      if (ptr != NULL) {
        ptr++; // go beyond first quote
        char* nl = strchr(ptr, '\"');
        if (nl != NULL) *nl = '\0';
        strncpy(distro, ptr, length);
      } else {
        ptr = strstr(buf, "=");
        ptr++; // go beyond equals then
        char* nl = strchr(ptr, '\n');
        if (nl != NULL) *nl = '\0';
        strncpy(distro, ptr, length);
      }
      return;
    } else if (get_first_line) {
      char* nl = strchr(buf, '\n');
      if (nl != NULL) *nl = '\0';
      strncpy(distro, buf, length);
      return;
    }
  }
  // print last line and close
  char* nl = strchr(buf, '\n');
  if (nl != NULL) *nl = '\0';
  strncpy(distro, buf, length);
}

static void parse_os_info(char* distro, size_t length, const char* file) {
  FILE* fp = fopen(file, "r");
  if (fp != NULL) {
    // if suse format, print out first line
    bool get_first_line = (strcmp(file, "/etc/SuSE-release") == 0);
    parse_os_info_helper(fp, distro, length, get_first_line);
    fclose(fp);
  }
}

void os::get_summary_os_info(char* buf, size_t buflen) {
  for (int i = 0;; i++) {
    const char* file = distro_files[i];
    if (file == NULL) {
      break; // ran out of distro_files
    }
    if (file_exists(file)) {
      parse_os_info(buf, buflen, file);
      return;
    }
  }
  // special case for debian
  if (file_exists("/etc/debian_version")) {
    strncpy(buf, "Debian ", buflen);
    parse_os_info(&buf[7], buflen-7, "/etc/debian_version");
  } else {
    strncpy(buf, "Linux", buflen);
  }
}

void os::Linux::print_libversion_info(outputStream* st) {
  // libc, pthread
  st->print("libc:");
  st->print("%s ", os::Linux::glibc_version());
  st->print("%s ", os::Linux::libpthread_version());
  st->cr();
}

void os::Linux::print_full_memory_info(outputStream* st) {
  st->print("\n/proc/meminfo:\n");
  _print_ascii_file("/proc/meminfo", st);
  st->cr();
}

void os::Linux::print_container_info(outputStream* st) {
  if (OSContainer::is_containerized()) {
    st->print("container (cgroup) information:\n");

    char *p = OSContainer::container_type();
    if (p == NULL)
      st->print("container_type() failed\n");
    else {
      st->print("container_type: %s\n", p);
    }

    p = OSContainer::cpu_cpuset_cpus();
    if (p == NULL)
      st->print("cpu_cpuset_cpus() failed\n");
    else {
      st->print("cpu_cpuset_cpus: %s\n", p);
      free(p);
    }

    p = OSContainer::cpu_cpuset_memory_nodes();
    if (p < 0)
      st->print("cpu_memory_nodes() failed\n");
    else {
      st->print("cpu_memory_nodes: %s\n", p);
      free(p);
    }

    int i = OSContainer::active_processor_count();
    if (i < 0)
      st->print("active_processor_count() failed\n");
    else
      st->print("active_processor_count: %d\n", i);

    i = OSContainer::cpu_quota();
    st->print("cpu_quota: %d\n", i);

    i = OSContainer::cpu_period();
    st->print("cpu_period: %d\n", i);

    i = OSContainer::cpu_shares();
    st->print("cpu_shares: %d\n", i);

    jlong j = OSContainer::memory_limit_in_bytes();
    st->print("memory_limit_in_bytes: " JLONG_FORMAT "\n", j);

    j = OSContainer::memory_and_swap_limit_in_bytes();
    st->print("memory_and_swap_limit_in_bytes: " JLONG_FORMAT "\n", j);

    j = OSContainer::memory_soft_limit_in_bytes();
    st->print("memory_soft_limit_in_bytes: " JLONG_FORMAT "\n", j);

    j = OSContainer::OSContainer::memory_usage_in_bytes();
    st->print("memory_usage_in_bytes: " JLONG_FORMAT "\n", j);

    j = OSContainer::OSContainer::memory_max_usage_in_bytes();
    st->print("memory_max_usage_in_bytes: " JLONG_FORMAT "\n", j);
    st->cr();
  }
}

void os::print_memory_info(outputStream* st) {

  st->print("Memory:");
  st->print(" %dk page", os::vm_page_size()>>10);

  // values in struct sysinfo are "unsigned long"
  struct sysinfo si;
  sysinfo(&si);

  st->print(", physical " UINT64_FORMAT "k",
            os::physical_memory() >> 10);
  st->print("(" UINT64_FORMAT "k free)",
            os::available_memory() >> 10);
  st->print(", swap " UINT64_FORMAT "k",
            ((jlong)si.totalswap * si.mem_unit) >> 10);
  st->print("(" UINT64_FORMAT "k free)",
            ((jlong)si.freeswap * si.mem_unit) >> 10);
  st->cr();
}

// Print the first "model name" line and the first "flags" line
// that we find and nothing more. We assume "model name" comes
// before "flags" so if we find a second "model name", then the
// "flags" field is considered missing.
static bool print_model_name_and_flags(outputStream* st, char* buf, size_t buflen) {
#if defined(IA32) || defined(AMD64)
  // Other platforms have less repetitive cpuinfo files
  FILE *fp = fopen("/proc/cpuinfo", "r");
  if (fp) {
    while (!feof(fp)) {
      if (fgets(buf, buflen, fp)) {
        // Assume model name comes before flags
        bool model_name_printed = false;
        if (strstr(buf, "model name") != NULL) {
          if (!model_name_printed) {
            st->print_raw("CPU Model and flags from /proc/cpuinfo:\n");
            st->print_raw(buf);
            model_name_printed = true;
          } else {
            // model name printed but not flags?  Odd, just return
            fclose(fp);
            return true;
          }
        }
        // print the flags line too
        if (strstr(buf, "flags") != NULL) {
          st->print_raw(buf);
          fclose(fp);
          return true;
        }
      }
    }
    fclose(fp);
  }
#endif // x86 platforms
  return false;
}

void os::pd_print_cpu_info(outputStream* st, char* buf, size_t buflen) {
  // Only print the model name if the platform provides this as a summary
  if (!print_model_name_and_flags(st, buf, buflen)) {
    st->print("\n/proc/cpuinfo:\n");
    if (!_print_ascii_file("/proc/cpuinfo", st)) {
      st->print_cr("  <Not Available>");
    }
  }
}

#if defined(AMD64) || defined(IA32) || defined(X32)
const char* search_string = "model name";
#elif defined(M68K)
const char* search_string = "CPU";
#elif defined(PPC64)
const char* search_string = "cpu";
#elif defined(S390)
const char* search_string = "processor";
#elif defined(SPARC)
const char* search_string = "cpu";
#else
const char* search_string = "Processor";
#endif

// Parses the cpuinfo file for string representing the model name.
void os::get_summary_cpu_info(char* cpuinfo, size_t length) {
  FILE* fp = fopen("/proc/cpuinfo", "r");
  if (fp != NULL) {
    while (!feof(fp)) {
      char buf[256];
      if (fgets(buf, sizeof(buf), fp)) {
        char* start = strstr(buf, search_string);
        if (start != NULL) {
          char *ptr = start + strlen(search_string);
          char *end = buf + strlen(buf);
          while (ptr != end) {
             // skip whitespace and colon for the rest of the name.
             if (*ptr != ' ' && *ptr != '\t' && *ptr != ':') {
               break;
             }
             ptr++;
          }
          if (ptr != end) {
            // reasonable string, get rid of newline and keep the rest
            char* nl = strchr(buf, '\n');
            if (nl != NULL) *nl = '\0';
            strncpy(cpuinfo, ptr, length);
            fclose(fp);
            return;
          }
        }
      }
    }
    fclose(fp);
  }
  // cpuinfo not found or parsing failed, just print generic string.  The entire
  // /proc/cpuinfo file will be printed later in the file (or enough of it for x86)
#if   defined(AARCH64)
  strncpy(cpuinfo, "AArch64", length);
#elif defined(AMD64)
  strncpy(cpuinfo, "x86_64", length);
#elif defined(ARM)  // Order wrt. AARCH64 is relevant!
  strncpy(cpuinfo, "ARM", length);
#elif defined(IA32)
  strncpy(cpuinfo, "x86_32", length);
#elif defined(IA64)
  strncpy(cpuinfo, "IA64", length);
#elif defined(PPC)
  strncpy(cpuinfo, "PPC64", length);
#elif defined(S390)
  strncpy(cpuinfo, "S390", length);
#elif defined(SPARC)
  strncpy(cpuinfo, "sparcv9", length);
#elif defined(ZERO_LIBARCH)
  strncpy(cpuinfo, ZERO_LIBARCH, length);
#else
  strncpy(cpuinfo, "unknown", length);
#endif
}

static void print_signal_handler(outputStream* st, int sig,
                                 char* buf, size_t buflen);

void os::print_signal_handlers(outputStream* st, char* buf, size_t buflen) {
  st->print_cr("Signal Handlers:");
  print_signal_handler(st, SIGSEGV, buf, buflen);
  print_signal_handler(st, SIGBUS , buf, buflen);
  print_signal_handler(st, SIGFPE , buf, buflen);
  print_signal_handler(st, SIGPIPE, buf, buflen);
  print_signal_handler(st, SIGXFSZ, buf, buflen);
  print_signal_handler(st, SIGILL , buf, buflen);
  print_signal_handler(st, SR_signum, buf, buflen);
  print_signal_handler(st, SHUTDOWN1_SIGNAL, buf, buflen);
  print_signal_handler(st, SHUTDOWN2_SIGNAL , buf, buflen);
  print_signal_handler(st, SHUTDOWN3_SIGNAL , buf, buflen);
  print_signal_handler(st, BREAK_SIGNAL, buf, buflen);
#if defined(PPC64)
  print_signal_handler(st, SIGTRAP, buf, buflen);
#endif
}

static char saved_jvm_path[MAXPATHLEN] = {0};

// Find the full path to the current module, libjvm.so
void os::jvm_path(char *buf, jint buflen) {
  // Error checking.
  if (buflen < MAXPATHLEN) {
    assert(false, "must use a large-enough buffer");
    buf[0] = '\0';
    return;
  }
  // Lazy resolve the path to current module.
  if (saved_jvm_path[0] != 0) {
    strcpy(buf, saved_jvm_path);
    return;
  }

  char dli_fname[MAXPATHLEN];
  bool ret = dll_address_to_library_name(
                                         CAST_FROM_FN_PTR(address, os::jvm_path),
                                         dli_fname, sizeof(dli_fname), NULL);
  assert(ret, "cannot locate libjvm");
  char *rp = NULL;
  if (ret && dli_fname[0] != '\0') {
    rp = os::Posix::realpath(dli_fname, buf, buflen);
  }
  if (rp == NULL) {
    return;
  }

  if (Arguments::sun_java_launcher_is_altjvm()) {
    // Support for the java launcher's '-XXaltjvm=<path>' option. Typical
    // value for buf is "<JAVA_HOME>/jre/lib/<vmtype>/libjvm.so".
    // If "/jre/lib/" appears at the right place in the string, then
    // assume we are installed in a JDK and we're done. Otherwise, check
    // for a JAVA_HOME environment variable and fix up the path so it
    // looks like libjvm.so is installed there (append a fake suffix
    // hotspot/libjvm.so).
    const char *p = buf + strlen(buf) - 1;
    for (int count = 0; p > buf && count < 5; ++count) {
      for (--p; p > buf && *p != '/'; --p)
        /* empty */ ;
    }

    if (strncmp(p, "/jre/lib/", 9) != 0) {
      // Look for JAVA_HOME in the environment.
      char* java_home_var = ::getenv("JAVA_HOME");
      if (java_home_var != NULL && java_home_var[0] != 0) {
        char* jrelib_p;
        int len;

        // Check the current module name "libjvm.so".
        p = strrchr(buf, '/');
        if (p == NULL) {
          return;
        }
        assert(strstr(p, "/libjvm") == p, "invalid library name");

        rp = os::Posix::realpath(java_home_var, buf, buflen);
        if (rp == NULL) {
          return;
        }

        // determine if this is a legacy image or modules image
        // modules image doesn't have "jre" subdirectory
        len = strlen(buf);
        assert(len < buflen, "Ran out of buffer room");
        jrelib_p = buf + len;
        snprintf(jrelib_p, buflen-len, "/jre/lib");
        if (0 != access(buf, F_OK)) {
          snprintf(jrelib_p, buflen-len, "/lib");
        }

        if (0 == access(buf, F_OK)) {
          // Use current module name "libjvm.so"
          len = strlen(buf);
          snprintf(buf + len, buflen-len, "/hotspot/libjvm.so");
        } else {
          // Go back to path of .so
          rp = os::Posix::realpath(dli_fname, buf, buflen);
          if (rp == NULL) {
            return;
          }
        }
      }
    }
  }

  strncpy(saved_jvm_path, buf, MAXPATHLEN);
  saved_jvm_path[MAXPATHLEN - 1] = '\0';
}

void os::print_jni_name_prefix_on(outputStream* st, int args_size) {
  // no prefix required, not even "_"
}

void os::print_jni_name_suffix_on(outputStream* st, int args_size) {
  // no suffix required
}

////////////////////////////////////////////////////////////////////////////////
// sun.misc.Signal support

static volatile jint sigint_count = 0;

static void UserHandler(int sig, void *siginfo, void *context) {
  // 4511530 - sem_post is serialized and handled by the manager thread. When
  // the program is interrupted by Ctrl-C, SIGINT is sent to every thread. We
  // don't want to flood the manager thread with sem_post requests.
  if (sig == SIGINT && Atomic::add(1, &sigint_count) > 1) {
    return;
  }

  // Ctrl-C is pressed during error reporting, likely because the error
  // handler fails to abort. Let VM die immediately.
  if (sig == SIGINT && VMError::is_error_reported()) {
    os::die();
  }

  os::signal_notify(sig);
}

void* os::user_handler() {
  return CAST_FROM_FN_PTR(void*, UserHandler);
}

struct timespec PosixSemaphore::create_timespec(unsigned int sec, int nsec) {
  struct timespec ts;
  // Semaphore's are always associated with CLOCK_REALTIME
  os::Linux::clock_gettime(CLOCK_REALTIME, &ts);
  // see unpackTime for discussion on overflow checking
  if (sec >= MAX_SECS) {
    ts.tv_sec += MAX_SECS;
    ts.tv_nsec = 0;
  } else {
    ts.tv_sec += sec;
    ts.tv_nsec += nsec;
    if (ts.tv_nsec >= NANOSECS_PER_SEC) {
      ts.tv_nsec -= NANOSECS_PER_SEC;
      ++ts.tv_sec; // note: this must be <= max_secs
    }
  }

  return ts;
}

extern "C" {
  typedef void (*sa_handler_t)(int);
  typedef void (*sa_sigaction_t)(int, siginfo_t *, void *);
}

void* os::signal(int signal_number, void* handler) {
  struct sigaction sigAct, oldSigAct;

  sigfillset(&(sigAct.sa_mask));
  sigAct.sa_flags   = SA_RESTART|SA_SIGINFO;
  sigAct.sa_handler = CAST_TO_FN_PTR(sa_handler_t, handler);

  if (sigaction(signal_number, &sigAct, &oldSigAct)) {
    // -1 means registration failed
    return (void *)-1;
  }

  return CAST_FROM_FN_PTR(void*, oldSigAct.sa_handler);
}

void os::signal_raise(int signal_number) {
  ::raise(signal_number);
}

// The following code is moved from os.cpp for making this
// code platform specific, which it is by its very nature.

// Will be modified when max signal is changed to be dynamic
int os::sigexitnum_pd() {
  return NSIG;
}

// a counter for each possible signal value
static volatile jint pending_signals[NSIG+1] = { 0 };

// Linux(POSIX) specific hand shaking semaphore.
static sem_t sig_sem;
static PosixSemaphore sr_semaphore;

void os::signal_init_pd() {
  // Initialize signal structures
  ::memset((void*)pending_signals, 0, sizeof(pending_signals));

  // Initialize signal semaphore
  ::sem_init(&sig_sem, 0, 0);
}

void os::signal_notify(int sig) {
  Atomic::inc(&pending_signals[sig]);
  ::sem_post(&sig_sem);
}

static int check_pending_signals(bool wait) {
  Atomic::store(0, &sigint_count);
  for (;;) {
    for (int i = 0; i < NSIG + 1; i++) {
      jint n = pending_signals[i];
      if (n > 0 && n == Atomic::cmpxchg(n - 1, &pending_signals[i], n)) {
        return i;
      }
    }
    if (!wait) {
      return -1;
    }
    JavaThread *thread = JavaThread::current();
    ThreadBlockInVM tbivm(thread);

    bool threadIsSuspended;
    do {
      thread->set_suspend_equivalent();
      // cleared by handle_special_suspend_equivalent_condition() or java_suspend_self()
      ::sem_wait(&sig_sem);

      // were we externally suspended while we were waiting?
      threadIsSuspended = thread->handle_special_suspend_equivalent_condition();
      if (threadIsSuspended) {
        // The semaphore has been incremented, but while we were waiting
        // another thread suspended us. We don't want to continue running
        // while suspended because that would surprise the thread that
        // suspended us.
        ::sem_post(&sig_sem);

        thread->java_suspend_self();
      }
    } while (threadIsSuspended);
  }
}

int os::signal_lookup() {
  return check_pending_signals(false);
}

int os::signal_wait() {
  return check_pending_signals(true);
}

////////////////////////////////////////////////////////////////////////////////
// Virtual Memory

int os::vm_page_size() {
  // Seems redundant as all get out
  assert(os::Linux::page_size() != -1, "must call os::init");
  return os::Linux::page_size();
}

// Solaris allocates memory by pages.
int os::vm_allocation_granularity() {
  assert(os::Linux::page_size() != -1, "must call os::init");
  return os::Linux::page_size();
}

// Rationale behind this function:
//  current (Mon Apr 25 20:12:18 MSD 2005) oprofile drops samples without executable
//  mapping for address (see lookup_dcookie() in the kernel module), thus we cannot get
//  samples for JITted code. Here we create private executable mapping over the code cache
//  and then we can use standard (well, almost, as mapping can change) way to provide
//  info for the reporting script by storing timestamp and location of symbol
void linux_wrap_code(char* base, size_t size) {
  static volatile jint cnt = 0;

  if (!UseOprofile) {
    return;
  }

  char buf[PATH_MAX+1];
  int num = Atomic::add(1, &cnt);

  snprintf(buf, sizeof(buf), "%s/hs-vm-%d-%d",
           os::get_temp_directory(), os::current_process_id(), num);
  unlink(buf);

  int fd = ::open(buf, O_CREAT | O_RDWR, S_IRWXU);

  if (fd != -1) {
    off_t rv = ::lseek(fd, size-2, SEEK_SET);
    if (rv != (off_t)-1) {
      if (::write(fd, "", 1) == 1) {
        mmap(base, size,
             PROT_READ|PROT_WRITE|PROT_EXEC,
             MAP_PRIVATE|MAP_FIXED|MAP_NORESERVE, fd, 0);
      }
    }
    ::close(fd);
    unlink(buf);
  }
}

static bool recoverable_mmap_error(int err) {
  // See if the error is one we can let the caller handle. This
  // list of errno values comes from JBS-6843484. I can't find a
  // Linux man page that documents this specific set of errno
  // values so while this list currently matches Solaris, it may
  // change as we gain experience with this failure mode.
  switch (err) {
  case EBADF:
  case EINVAL:
  case ENOTSUP:
    // let the caller deal with these errors
    return true;

  default:
    // Any remaining errors on this OS can cause our reserved mapping
    // to be lost. That can cause confusion where different data
    // structures think they have the same memory mapped. The worst
    // scenario is if both the VM and a library think they have the
    // same memory mapped.
    return false;
  }
}

static void warn_fail_commit_memory(char* addr, size_t size, bool exec,
                                    int err) {
  warning("INFO: os::commit_memory(" PTR_FORMAT ", " SIZE_FORMAT
          ", %d) failed; error='%s' (errno=%d)", p2i(addr), size, exec,
          os::strerror(err), err);
}

static void warn_fail_commit_memory(char* addr, size_t size,
                                    size_t alignment_hint, bool exec,
                                    int err) {
  warning("INFO: os::commit_memory(" PTR_FORMAT ", " SIZE_FORMAT
          ", " SIZE_FORMAT ", %d) failed; error='%s' (errno=%d)", p2i(addr), size,
          alignment_hint, exec, os::strerror(err), err);
}

// NOTE: Linux kernel does not really reserve the pages for us.
//       All it does is to check if there are enough free pages
//       left at the time of mmap(). This could be a potential
//       problem.
int os::Linux::commit_memory_impl(char* addr, size_t size, bool exec) {
  int prot = exec ? PROT_READ|PROT_WRITE|PROT_EXEC : PROT_READ|PROT_WRITE;
  uintptr_t res = (uintptr_t) ::mmap(addr, size, prot,
                                     MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0);
  if (res != (uintptr_t) MAP_FAILED) {
    if (UseNUMAInterleaving) {
      numa_make_global(addr, size);
    }
    return 0;
  }

  int err = errno;  // save errno from mmap() call above

  if (!recoverable_mmap_error(err)) {
    warn_fail_commit_memory(addr, size, exec, err);
    vm_exit_out_of_memory(size, OOM_MMAP_ERROR, "committing reserved memory.");
  }

  return err;
}

bool os::pd_commit_memory(char* addr, size_t size, bool exec) {
  return os::Linux::commit_memory_impl(addr, size, exec) == 0;
}

void os::pd_commit_memory_or_exit(char* addr, size_t size, bool exec,
                                  const char* mesg) {
  assert(mesg != NULL, "mesg must be specified");
  int err = os::Linux::commit_memory_impl(addr, size, exec);
  if (err != 0) {
    // the caller wants all commit errors to exit with the specified mesg:
    warn_fail_commit_memory(addr, size, exec, err);
    vm_exit_out_of_memory(size, OOM_MMAP_ERROR, "%s", mesg);
  }
}

// Define MAP_HUGETLB here so we can build HotSpot on old systems.
#ifndef MAP_HUGETLB
  #define MAP_HUGETLB 0x40000
#endif

// Define MADV_HUGEPAGE here so we can build HotSpot on old systems.
#ifndef MADV_HUGEPAGE
  #define MADV_HUGEPAGE 14
#endif

int os::Linux::commit_memory_impl(char* addr, size_t size,
                                  size_t alignment_hint, bool exec) {
  int err = os::Linux::commit_memory_impl(addr, size, exec);
  if (err == 0) {
    realign_memory(addr, size, alignment_hint);
  }
  return err;
}

bool os::pd_commit_memory(char* addr, size_t size, size_t alignment_hint,
                          bool exec) {
  return os::Linux::commit_memory_impl(addr, size, alignment_hint, exec) == 0;
}

void os::pd_commit_memory_or_exit(char* addr, size_t size,
                                  size_t alignment_hint, bool exec,
                                  const char* mesg) {
  assert(mesg != NULL, "mesg must be specified");
  int err = os::Linux::commit_memory_impl(addr, size, alignment_hint, exec);
  if (err != 0) {
    // the caller wants all commit errors to exit with the specified mesg:
    warn_fail_commit_memory(addr, size, alignment_hint, exec, err);
    vm_exit_out_of_memory(size, OOM_MMAP_ERROR, "%s", mesg);
  }
}

void os::pd_realign_memory(char *addr, size_t bytes, size_t alignment_hint) {
  if (UseTransparentHugePages && alignment_hint > (size_t)vm_page_size()) {
    // We don't check the return value: madvise(MADV_HUGEPAGE) may not
    // be supported or the memory may already be backed by huge pages.
    ::madvise(addr, bytes, MADV_HUGEPAGE);
  }
}

void os::pd_free_memory(char *addr, size_t bytes, size_t alignment_hint) {
  // This method works by doing an mmap over an existing mmaping and effectively discarding
  // the existing pages. However it won't work for SHM-based large pages that cannot be
  // uncommitted at all. We don't do anything in this case to avoid creating a segment with
  // small pages on top of the SHM segment. This method always works for small pages, so we
  // allow that in any case.
  if (alignment_hint <= (size_t)os::vm_page_size() || can_commit_large_page_memory()) {
    commit_memory(addr, bytes, alignment_hint, !ExecMem);
  }
}

void os::numa_make_global(char *addr, size_t bytes) {
  Linux::numa_interleave_memory(addr, bytes);
}

// Define for numa_set_bind_policy(int). Setting the argument to 0 will set the
// bind policy to MPOL_PREFERRED for the current thread.
#define USE_MPOL_PREFERRED 0

void os::numa_make_local(char *addr, size_t bytes, int lgrp_hint) {
  // To make NUMA and large pages more robust when both enabled, we need to ease
  // the requirements on where the memory should be allocated. MPOL_BIND is the
  // default policy and it will force memory to be allocated on the specified
  // node. Changing this to MPOL_PREFERRED will prefer to allocate the memory on
  // the specified node, but will not force it. Using this policy will prevent
  // getting SIGBUS when trying to allocate large pages on NUMA nodes with no
  // free large pages.
  Linux::numa_set_bind_policy(USE_MPOL_PREFERRED);
  Linux::numa_tonode_memory(addr, bytes, lgrp_hint);
}

bool os::numa_topology_changed() { return false; }

size_t os::numa_get_groups_num() {
  // Return just the number of nodes in which it's possible to allocate memory
  // (in numa terminology, configured nodes).
  return Linux::numa_num_configured_nodes();
}

int os::numa_get_group_id() {
  int cpu_id = Linux::sched_getcpu();
  if (cpu_id != -1) {
    int lgrp_id = Linux::get_node_by_cpu(cpu_id);
    if (lgrp_id != -1) {
      return lgrp_id;
    }
  }
  return 0;
}

int os::Linux::get_existing_num_nodes() {
  size_t node;
  size_t highest_node_number = Linux::numa_max_node();
  int num_nodes = 0;

  // Get the total number of nodes in the system including nodes without memory.
  for (node = 0; node <= highest_node_number; node++) {
    if (isnode_in_existing_nodes(node)) {
      num_nodes++;
    }
  }
  return num_nodes;
}

size_t os::numa_get_leaf_groups(int *ids, size_t size) {
  size_t highest_node_number = Linux::numa_max_node();
  size_t i = 0;

  // Map all node ids in which is possible to allocate memory. Also nodes are
  // not always consecutively available, i.e. available from 0 to the highest
  // node number.
  for (size_t node = 0; node <= highest_node_number; node++) {
    if (Linux::isnode_in_configured_nodes(node)) {
      ids[i++] = node;
    }
  }
  return i;
}

bool os::get_page_info(char *start, page_info* info) {
  return false;
}

char *os::scan_pages(char *start, char* end, page_info* page_expected,
                     page_info* page_found) {
  return end;
}


int os::Linux::sched_getcpu_syscall(void) {
  unsigned int cpu = 0;
  int retval = -1;

#if defined(IA32)
  #ifndef SYS_getcpu
    #define SYS_getcpu 318
  #endif
  retval = syscall(SYS_getcpu, &cpu, NULL, NULL);
#elif defined(AMD64)
// Unfortunately we have to bring all these macros here from vsyscall.h
// to be able to compile on old linuxes.
  #define __NR_vgetcpu 2
  #define VSYSCALL_START (-10UL << 20)
  #define VSYSCALL_SIZE 1024
  #define VSYSCALL_ADDR(vsyscall_nr) (VSYSCALL_START+VSYSCALL_SIZE*(vsyscall_nr))
  typedef long (*vgetcpu_t)(unsigned int *cpu, unsigned int *node, unsigned long *tcache);
  vgetcpu_t vgetcpu = (vgetcpu_t)VSYSCALL_ADDR(__NR_vgetcpu);
  retval = vgetcpu(&cpu, NULL, NULL);
#endif

  return (retval == -1) ? retval : cpu;
}

void os::Linux::sched_getcpu_init() {
  // sched_getcpu() should be in libc.
  set_sched_getcpu(CAST_TO_FN_PTR(sched_getcpu_func_t,
                                  dlsym(RTLD_DEFAULT, "sched_getcpu")));

  // If it's not, try a direct syscall.
  if (sched_getcpu() == -1) {
    set_sched_getcpu(CAST_TO_FN_PTR(sched_getcpu_func_t,
                                    (void*)&sched_getcpu_syscall));
  }
}

// Something to do with the numa-aware allocator needs these symbols
extern "C" JNIEXPORT void numa_warn(int number, char *where, ...) { }
extern "C" JNIEXPORT void numa_error(char *where) { }

// Handle request to load libnuma symbol version 1.1 (API v1). If it fails
// load symbol from base version instead.
void* os::Linux::libnuma_dlsym(void* handle, const char *name) {
  void *f = dlvsym(handle, name, "libnuma_1.1");
  if (f == NULL) {
    f = dlsym(handle, name);
  }
  return f;
}

// Handle request to load libnuma symbol version 1.2 (API v2) only.
// Return NULL if the symbol is not defined in this particular version.
void* os::Linux::libnuma_v2_dlsym(void* handle, const char* name) {
  return dlvsym(handle, name, "libnuma_1.2");
}

bool os::Linux::libnuma_init() {
  if (sched_getcpu() != -1) { // Requires sched_getcpu() support
    void *handle = dlopen("libnuma.so.1", RTLD_LAZY);
    if (handle != NULL) {
      set_numa_node_to_cpus(CAST_TO_FN_PTR(numa_node_to_cpus_func_t,
                                           libnuma_dlsym(handle, "numa_node_to_cpus")));
      set_numa_max_node(CAST_TO_FN_PTR(numa_max_node_func_t,
                                       libnuma_dlsym(handle, "numa_max_node")));
      set_numa_num_configured_nodes(CAST_TO_FN_PTR(numa_num_configured_nodes_func_t,
                                                   libnuma_dlsym(handle, "numa_num_configured_nodes")));
      set_numa_available(CAST_TO_FN_PTR(numa_available_func_t,
                                        libnuma_dlsym(handle, "numa_available")));
      set_numa_tonode_memory(CAST_TO_FN_PTR(numa_tonode_memory_func_t,
                                            libnuma_dlsym(handle, "numa_tonode_memory")));
      set_numa_interleave_memory(CAST_TO_FN_PTR(numa_interleave_memory_func_t,
                                                libnuma_dlsym(handle, "numa_interleave_memory")));
      set_numa_interleave_memory_v2(CAST_TO_FN_PTR(numa_interleave_memory_v2_func_t,
                                                libnuma_v2_dlsym(handle, "numa_interleave_memory")));
      set_numa_set_bind_policy(CAST_TO_FN_PTR(numa_set_bind_policy_func_t,
                                              libnuma_dlsym(handle, "numa_set_bind_policy")));
      set_numa_bitmask_isbitset(CAST_TO_FN_PTR(numa_bitmask_isbitset_func_t,
                                               libnuma_dlsym(handle, "numa_bitmask_isbitset")));
      set_numa_distance(CAST_TO_FN_PTR(numa_distance_func_t,
                                       libnuma_dlsym(handle, "numa_distance")));

      if (numa_available() != -1) {
        set_numa_all_nodes((unsigned long*)libnuma_dlsym(handle, "numa_all_nodes"));
        set_numa_all_nodes_ptr((struct bitmask **)libnuma_dlsym(handle, "numa_all_nodes_ptr"));
        set_numa_nodes_ptr((struct bitmask **)libnuma_dlsym(handle, "numa_nodes_ptr"));
        // Create an index -> node mapping, since nodes are not always consecutive
        _nindex_to_node = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<int>(0, true);
        rebuild_nindex_to_node_map();
        // Create a cpu -> node mapping
        _cpu_to_node = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<int>(0, true);
        rebuild_cpu_to_node_map();
        return true;
      }
    }
  }
  return false;
}

size_t os::Linux::default_guard_size(os::ThreadType thr_type) {
  // Creating guard page is very expensive. Java thread has HotSpot
  // guard pages, only enable glibc guard page for non-Java threads.
  // (Remember: compiler thread is a Java thread, too!)
  return ((thr_type == java_thread || thr_type == compiler_thread) ? 0 : page_size());
}

void os::Linux::rebuild_nindex_to_node_map() {
  int highest_node_number = Linux::numa_max_node();

  nindex_to_node()->clear();
  for (int node = 0; node <= highest_node_number; node++) {
    if (Linux::isnode_in_existing_nodes(node)) {
      nindex_to_node()->append(node);
    }
  }
}

// rebuild_cpu_to_node_map() constructs a table mapping cpud id to node id.
// The table is later used in get_node_by_cpu().
void os::Linux::rebuild_cpu_to_node_map() {
  const size_t NCPUS = 32768; // Since the buffer size computation is very obscure
                              // in libnuma (possible values are starting from 16,
                              // and continuing up with every other power of 2, but less
                              // than the maximum number of CPUs supported by kernel), and
                              // is a subject to change (in libnuma version 2 the requirements
                              // are more reasonable) we'll just hardcode the number they use
                              // in the library.
  const size_t BitsPerCLong = sizeof(long) * CHAR_BIT;

  size_t cpu_num = processor_count();
  size_t cpu_map_size = NCPUS / BitsPerCLong;
  size_t cpu_map_valid_size =
    MIN2((cpu_num + BitsPerCLong - 1) / BitsPerCLong, cpu_map_size);

  cpu_to_node()->clear();
  cpu_to_node()->at_grow(cpu_num - 1);

  size_t node_num = get_existing_num_nodes();

  int distance = 0;
  int closest_distance = INT_MAX;
  int closest_node = 0;
  unsigned long *cpu_map = NEW_C_HEAP_ARRAY(unsigned long, cpu_map_size, mtInternal);
  for (size_t i = 0; i < node_num; i++) {
    // Check if node is configured (not a memory-less node). If it is not, find
    // the closest configured node.
    if (!isnode_in_configured_nodes(nindex_to_node()->at(i))) {
      closest_distance = INT_MAX;
      // Check distance from all remaining nodes in the system. Ignore distance
      // from itself and from another non-configured node.
      for (size_t m = 0; m < node_num; m++) {
        if (m != i && isnode_in_configured_nodes(nindex_to_node()->at(m))) {
          distance = numa_distance(nindex_to_node()->at(i), nindex_to_node()->at(m));
          // If a closest node is found, update. There is always at least one
          // configured node in the system so there is always at least one node
          // close.
          if (distance != 0 && distance < closest_distance) {
            closest_distance = distance;
            closest_node = nindex_to_node()->at(m);
          }
        }
      }
     } else {
       // Current node is already a configured node.
       closest_node = nindex_to_node()->at(i);
     }

    // Get cpus from the original node and map them to the closest node. If node
    // is a configured node (not a memory-less node), then original node and
    // closest node are the same.
    if (numa_node_to_cpus(nindex_to_node()->at(i), cpu_map, cpu_map_size * sizeof(unsigned long)) != -1) {
      for (size_t j = 0; j < cpu_map_valid_size; j++) {
        if (cpu_map[j] != 0) {
          for (size_t k = 0; k < BitsPerCLong; k++) {
            if (cpu_map[j] & (1UL << k)) {
              cpu_to_node()->at_put(j * BitsPerCLong + k, closest_node);
            }
          }
        }
      }
    }
  }
  FREE_C_HEAP_ARRAY(unsigned long, cpu_map);
}

int os::Linux::get_node_by_cpu(int cpu_id) {
  if (cpu_to_node() != NULL && cpu_id >= 0 && cpu_id < cpu_to_node()->length()) {
    return cpu_to_node()->at(cpu_id);
  }
  return -1;
}

GrowableArray<int>* os::Linux::_cpu_to_node;
GrowableArray<int>* os::Linux::_nindex_to_node;
os::Linux::sched_getcpu_func_t os::Linux::_sched_getcpu;
os::Linux::numa_node_to_cpus_func_t os::Linux::_numa_node_to_cpus;
os::Linux::numa_max_node_func_t os::Linux::_numa_max_node;
os::Linux::numa_num_configured_nodes_func_t os::Linux::_numa_num_configured_nodes;
os::Linux::numa_available_func_t os::Linux::_numa_available;
os::Linux::numa_tonode_memory_func_t os::Linux::_numa_tonode_memory;
os::Linux::numa_interleave_memory_func_t os::Linux::_numa_interleave_memory;
os::Linux::numa_interleave_memory_v2_func_t os::Linux::_numa_interleave_memory_v2;
os::Linux::numa_set_bind_policy_func_t os::Linux::_numa_set_bind_policy;
os::Linux::numa_bitmask_isbitset_func_t os::Linux::_numa_bitmask_isbitset;
os::Linux::numa_distance_func_t os::Linux::_numa_distance;
unsigned long* os::Linux::_numa_all_nodes;
struct bitmask* os::Linux::_numa_all_nodes_ptr;
struct bitmask* os::Linux::_numa_nodes_ptr;

bool os::pd_uncommit_memory(char* addr, size_t size) {
  uintptr_t res = (uintptr_t) ::mmap(addr, size, PROT_NONE,
                                     MAP_PRIVATE|MAP_FIXED|MAP_NORESERVE|MAP_ANONYMOUS, -1, 0);
  return res  != (uintptr_t) MAP_FAILED;
}

static address get_stack_commited_bottom(address bottom, size_t size) {
  address nbot = bottom;
  address ntop = bottom + size;

  size_t page_sz = os::vm_page_size();
  unsigned pages = size / page_sz;

  unsigned char vec[1];
  unsigned imin = 1, imax = pages + 1, imid;
  int mincore_return_value = 0;

  assert(imin <= imax, "Unexpected page size");

  while (imin < imax) {
    imid = (imax + imin) / 2;
    nbot = ntop - (imid * page_sz);

    // Use a trick with mincore to check whether the page is mapped or not.
    // mincore sets vec to 1 if page resides in memory and to 0 if page
    // is swapped output but if page we are asking for is unmapped
    // it returns -1,ENOMEM
    mincore_return_value = mincore(nbot, page_sz, vec);

    if (mincore_return_value == -1) {
      // Page is not mapped go up
      // to find first mapped page
      if (errno != EAGAIN) {
        assert(errno == ENOMEM, "Unexpected mincore errno");
        imax = imid;
      }
    } else {
      // Page is mapped go down
      // to find first not mapped page
      imin = imid + 1;
    }
  }

  nbot = nbot + page_sz;

  // Adjust stack bottom one page up if last checked page is not mapped
  if (mincore_return_value == -1) {
    nbot = nbot + page_sz;
  }

  return nbot;
}


// Linux uses a growable mapping for the stack, and if the mapping for
// the stack guard pages is not removed when we detach a thread the
// stack cannot grow beyond the pages where the stack guard was
// mapped.  If at some point later in the process the stack expands to
// that point, the Linux kernel cannot expand the stack any further
// because the guard pages are in the way, and a segfault occurs.
//
// However, it's essential not to split the stack region by unmapping
// a region (leaving a hole) that's already part of the stack mapping,
// so if the stack mapping has already grown beyond the guard pages at
// the time we create them, we have to truncate the stack mapping.
// So, we need to know the extent of the stack mapping when
// create_stack_guard_pages() is called.

// We only need this for stacks that are growable: at the time of
// writing thread stacks don't use growable mappings (i.e. those
// creeated with MAP_GROWSDOWN), and aren't marked "[stack]", so this
// only applies to the main thread.

// If the (growable) stack mapping already extends beyond the point
// where we're going to put our guard pages, truncate the mapping at
// that point by munmap()ping it.  This ensures that when we later
// munmap() the guard pages we don't leave a hole in the stack
// mapping. This only affects the main/primordial thread

bool os::pd_create_stack_guard_pages(char* addr, size_t size) {
  if (os::is_primordial_thread()) {
    // As we manually grow stack up to bottom inside create_attached_thread(),
    // it's likely that os::Linux::initial_thread_stack_bottom is mapped and
    // we don't need to do anything special.
    // Check it first, before calling heavy function.
    uintptr_t stack_extent = (uintptr_t) os::Linux::initial_thread_stack_bottom();
    unsigned char vec[1];

    if (mincore((address)stack_extent, os::vm_page_size(), vec) == -1) {
      // Fallback to slow path on all errors, including EAGAIN
      stack_extent = (uintptr_t) get_stack_commited_bottom(
                                                           os::Linux::initial_thread_stack_bottom(),
                                                           (size_t)addr - stack_extent);
    }

    if (stack_extent < (uintptr_t)addr) {
      ::munmap((void*)stack_extent, (uintptr_t)(addr - stack_extent));
    }
  }

  return os::commit_memory(addr, size, !ExecMem);
}

// If this is a growable mapping, remove the guard pages entirely by
// munmap()ping them.  If not, just call uncommit_memory(). This only
// affects the main/primordial thread, but guard against future OS changes.
// It's safe to always unmap guard pages for primordial thread because we
// always place it right after end of the mapped region.

bool os::remove_stack_guard_pages(char* addr, size_t size) {
  uintptr_t stack_extent, stack_base;

  if (os::is_primordial_thread()) {
    return ::munmap(addr, size) == 0;
  }

  return os::uncommit_memory(addr, size);
}

// If 'fixed' is true, anon_mmap() will attempt to reserve anonymous memory
// at 'requested_addr'. If there are existing memory mappings at the same
// location, however, they will be overwritten. If 'fixed' is false,
// 'requested_addr' is only treated as a hint, the return value may or
// may not start from the requested address. Unlike Linux mmap(), this
// function returns NULL to indicate failure.
static char* anon_mmap(char* requested_addr, size_t bytes, bool fixed) {
  char * addr;
  int flags;

  flags = MAP_PRIVATE | MAP_NORESERVE | MAP_ANONYMOUS;
  if (fixed) {
    assert((uintptr_t)requested_addr % os::Linux::page_size() == 0, "unaligned address");
    flags |= MAP_FIXED;
  }

  // Map reserved/uncommitted pages PROT_NONE so we fail early if we
  // touch an uncommitted page. Otherwise, the read/write might
  // succeed if we have enough swap space to back the physical page.
  addr = (char*)::mmap(requested_addr, bytes, PROT_NONE,
                       flags, -1, 0);

  return addr == MAP_FAILED ? NULL : addr;
}

// Allocate (using mmap, NO_RESERVE, with small pages) at either a given request address
//   (req_addr != NULL) or with a given alignment.
//  - bytes shall be a multiple of alignment.
//  - req_addr can be NULL. If not NULL, it must be a multiple of alignment.
//  - alignment sets the alignment at which memory shall be allocated.
//     It must be a multiple of allocation granularity.
// Returns address of memory or NULL. If req_addr was not NULL, will only return
//  req_addr or NULL.
static char* anon_mmap_aligned(size_t bytes, size_t alignment, char* req_addr) {

  size_t extra_size = bytes;
  if (req_addr == NULL && alignment > 0) {
    extra_size += alignment;
  }

  char* start = (char*) ::mmap(req_addr, extra_size, PROT_NONE,
    MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,
    -1, 0);
  if (start == MAP_FAILED) {
    start = NULL;
  } else {
    if (req_addr != NULL) {
      if (start != req_addr) {
        ::munmap(start, extra_size);
        start = NULL;
      }
    } else {
      char* const start_aligned = align_up(start, alignment);
      char* const end_aligned = start_aligned + bytes;
      char* const end = start + extra_size;
      if (start_aligned > start) {
        ::munmap(start, start_aligned - start);
      }
      if (end_aligned < end) {
        ::munmap(end_aligned, end - end_aligned);
      }
      start = start_aligned;
    }
  }
  return start;
}

static int anon_munmap(char * addr, size_t size) {
  return ::munmap(addr, size) == 0;
}

char* os::pd_reserve_memory(size_t bytes, char* requested_addr,
                            size_t alignment_hint) {
  return anon_mmap(requested_addr, bytes, (requested_addr != NULL));
}

bool os::pd_release_memory(char* addr, size_t size) {
  return anon_munmap(addr, size);
}

static bool linux_mprotect(char* addr, size_t size, int prot) {
  // Linux wants the mprotect address argument to be page aligned.
  char* bottom = (char*)align_down((intptr_t)addr, os::Linux::page_size());

  // According to SUSv3, mprotect() should only be used with mappings
  // established by mmap(), and mmap() always maps whole pages. Unaligned
  // 'addr' likely indicates problem in the VM (e.g. trying to change
  // protection of malloc'ed or statically allocated memory). Check the
  // caller if you hit this assert.
  assert(addr == bottom, "sanity check");

  size = align_up(pointer_delta(addr, bottom, 1) + size, os::Linux::page_size());
  return ::mprotect(bottom, size, prot) == 0;
}

// Set protections specified
bool os::protect_memory(char* addr, size_t bytes, ProtType prot,
                        bool is_committed) {
  unsigned int p = 0;
  switch (prot) {
  case MEM_PROT_NONE: p = PROT_NONE; break;
  case MEM_PROT_READ: p = PROT_READ; break;
  case MEM_PROT_RW:   p = PROT_READ|PROT_WRITE; break;
  case MEM_PROT_RWX:  p = PROT_READ|PROT_WRITE|PROT_EXEC; break;
  default:
    ShouldNotReachHere();
  }
  // is_committed is unused.
  return linux_mprotect(addr, bytes, p);
}

bool os::guard_memory(char* addr, size_t size) {
  return linux_mprotect(addr, size, PROT_NONE);
}

bool os::unguard_memory(char* addr, size_t size) {
  return linux_mprotect(addr, size, PROT_READ|PROT_WRITE);
}

bool os::Linux::transparent_huge_pages_sanity_check(bool warn,
                                                    size_t page_size) {
  bool result = false;
  void *p = mmap(NULL, page_size * 2, PROT_READ|PROT_WRITE,
                 MAP_ANONYMOUS|MAP_PRIVATE,
                 -1, 0);
  if (p != MAP_FAILED) {
    void *aligned_p = align_up(p, page_size);

    result = madvise(aligned_p, page_size, MADV_HUGEPAGE) == 0;

    munmap(p, page_size * 2);
  }

  if (warn && !result) {
    warning("TransparentHugePages is not supported by the operating system.");
  }

  return result;
}

bool os::Linux::hugetlbfs_sanity_check(bool warn, size_t page_size) {
  bool result = false;
  void *p = mmap(NULL, page_size, PROT_READ|PROT_WRITE,
                 MAP_ANONYMOUS|MAP_PRIVATE|MAP_HUGETLB,
                 -1, 0);

  if (p != MAP_FAILED) {
    // We don't know if this really is a huge page or not.
    FILE *fp = fopen("/proc/self/maps", "r");
    if (fp) {
      while (!feof(fp)) {
        char chars[257];
        long x = 0;
        if (fgets(chars, sizeof(chars), fp)) {
          if (sscanf(chars, "%lx-%*x", &x) == 1
              && x == (long)p) {
            if (strstr (chars, "hugepage")) {
              result = true;
              break;
            }
          }
        }
      }
      fclose(fp);
    }
    munmap(p, page_size);
  }

  if (warn && !result) {
    warning("HugeTLBFS is not supported by the operating system.");
  }

  return result;
}

// Set the coredump_filter bits to include largepages in core dump (bit 6)
//
// From the coredump_filter documentation:
//
// - (bit 0) anonymous private memory
// - (bit 1) anonymous shared memory
// - (bit 2) file-backed private memory
// - (bit 3) file-backed shared memory
// - (bit 4) ELF header pages in file-backed private memory areas (it is
//           effective only if the bit 2 is cleared)
// - (bit 5) hugetlb private memory
// - (bit 6) hugetlb shared memory
//
static void set_coredump_filter(void) {
  FILE *f;
  long cdm;

  if ((f = fopen("/proc/self/coredump_filter", "r+")) == NULL) {
    return;
  }

  if (fscanf(f, "%lx", &cdm) != 1) {
    fclose(f);
    return;
  }

  rewind(f);

  if ((cdm & LARGEPAGES_BIT) == 0) {
    cdm |= LARGEPAGES_BIT;
    fprintf(f, "%#lx", cdm);
  }

  fclose(f);
}

// Large page support

static size_t _large_page_size = 0;

size_t os::Linux::find_large_page_size() {
  size_t large_page_size = 0;

  // large_page_size on Linux is used to round up heap size. x86 uses either
  // 2M or 4M page, depending on whether PAE (Physical Address Extensions)
  // mode is enabled. AMD64/EM64T uses 2M page in 64bit mode. IA64 can use
  // page as large as 256M.
  //
  // Here we try to figure out page size by parsing /proc/meminfo and looking
  // for a line with the following format:
  //    Hugepagesize:     2048 kB
  //
  // If we can't determine the value (e.g. /proc is not mounted, or the text
  // format has been changed), we'll use the largest page size supported by
  // the processor.

#ifndef ZERO
  large_page_size =
    AARCH64_ONLY(2 * M)
    AMD64_ONLY(2 * M)
    ARM32_ONLY(2 * M)
    IA32_ONLY(4 * M)
    IA64_ONLY(256 * M)
    PPC_ONLY(4 * M)
    S390_ONLY(1 * M)
    SPARC_ONLY(4 * M);
#endif // ZERO

  FILE *fp = fopen("/proc/meminfo", "r");
  if (fp) {
    while (!feof(fp)) {
      int x = 0;
      char buf[16];
      if (fscanf(fp, "Hugepagesize: %d", &x) == 1) {
        if (x && fgets(buf, sizeof(buf), fp) && strcmp(buf, " kB\n") == 0) {
          large_page_size = x * K;
          break;
        }
      } else {
        // skip to next line
        for (;;) {
          int ch = fgetc(fp);
          if (ch == EOF || ch == (int)'\n') break;
        }
      }
    }
    fclose(fp);
  }

  if (!FLAG_IS_DEFAULT(LargePageSizeInBytes) && LargePageSizeInBytes != large_page_size) {
    warning("Setting LargePageSizeInBytes has no effect on this OS. Large page size is "
            SIZE_FORMAT "%s.", byte_size_in_proper_unit(large_page_size),
            proper_unit_for_byte_size(large_page_size));
  }

  return large_page_size;
}

size_t os::Linux::setup_large_page_size() {
  _large_page_size = Linux::find_large_page_size();
  const size_t default_page_size = (size_t)Linux::page_size();
  if (_large_page_size > default_page_size) {
    _page_sizes[0] = _large_page_size;
    _page_sizes[1] = default_page_size;
    _page_sizes[2] = 0;
  }

  return _large_page_size;
}

bool os::Linux::setup_large_page_type(size_t page_size) {
  if (FLAG_IS_DEFAULT(UseHugeTLBFS) &&
      FLAG_IS_DEFAULT(UseSHM) &&
      FLAG_IS_DEFAULT(UseTransparentHugePages)) {

    // The type of large pages has not been specified by the user.

    // Try UseHugeTLBFS and then UseSHM.
    UseHugeTLBFS = UseSHM = true;

    // Don't try UseTransparentHugePages since there are known
    // performance issues with it turned on. This might change in the future.
    UseTransparentHugePages = false;
  }

  if (UseTransparentHugePages) {
    bool warn_on_failure = !FLAG_IS_DEFAULT(UseTransparentHugePages);
    if (transparent_huge_pages_sanity_check(warn_on_failure, page_size)) {
      UseHugeTLBFS = false;
      UseSHM = false;
      return true;
    }
    UseTransparentHugePages = false;
  }

  if (UseHugeTLBFS) {
    bool warn_on_failure = !FLAG_IS_DEFAULT(UseHugeTLBFS);
    if (hugetlbfs_sanity_check(warn_on_failure, page_size)) {
      UseSHM = false;
      return true;
    }
    UseHugeTLBFS = false;
  }

  return UseSHM;
}

void os::large_page_init() {
  if (!UseLargePages &&
      !UseTransparentHugePages &&
      !UseHugeTLBFS &&
      !UseSHM) {
    // Not using large pages.
    return;
  }

  if (!FLAG_IS_DEFAULT(UseLargePages) && !UseLargePages) {
    // The user explicitly turned off large pages.
    // Ignore the rest of the large pages flags.
    UseTransparentHugePages = false;
    UseHugeTLBFS = false;
    UseSHM = false;
    return;
  }

  size_t large_page_size = Linux::setup_large_page_size();
  UseLargePages          = Linux::setup_large_page_type(large_page_size);

  set_coredump_filter();
}

#ifndef SHM_HUGETLB
  #define SHM_HUGETLB 04000
#endif

#define shm_warning_format(format, ...)              \
  do {                                               \
    if (UseLargePages &&                             \
        (!FLAG_IS_DEFAULT(UseLargePages) ||          \
         !FLAG_IS_DEFAULT(UseSHM) ||                 \
         !FLAG_IS_DEFAULT(LargePageSizeInBytes))) {  \
      warning(format, __VA_ARGS__);                  \
    }                                                \
  } while (0)

#define shm_warning(str) shm_warning_format("%s", str)

#define shm_warning_with_errno(str)                \
  do {                                             \
    int err = errno;                               \
    shm_warning_format(str " (error = %d)", err);  \
  } while (0)

static char* shmat_with_alignment(int shmid, size_t bytes, size_t alignment) {
  assert(is_aligned(bytes, alignment), "Must be divisible by the alignment");

  if (!is_aligned(alignment, SHMLBA)) {
    assert(false, "Code below assumes that alignment is at least SHMLBA aligned");
    return NULL;
  }

  // To ensure that we get 'alignment' aligned memory from shmat,
  // we pre-reserve aligned virtual memory and then attach to that.

  char* pre_reserved_addr = anon_mmap_aligned(bytes, alignment, NULL);
  if (pre_reserved_addr == NULL) {
    // Couldn't pre-reserve aligned memory.
    shm_warning("Failed to pre-reserve aligned memory for shmat.");
    return NULL;
  }

  // SHM_REMAP is needed to allow shmat to map over an existing mapping.
  char* addr = (char*)shmat(shmid, pre_reserved_addr, SHM_REMAP);

  if ((intptr_t)addr == -1) {
    int err = errno;
    shm_warning_with_errno("Failed to attach shared memory.");

    assert(err != EACCES, "Unexpected error");
    assert(err != EIDRM,  "Unexpected error");
    assert(err != EINVAL, "Unexpected error");

    // Since we don't know if the kernel unmapped the pre-reserved memory area
    // we can't unmap it, since that would potentially unmap memory that was
    // mapped from other threads.
    return NULL;
  }

  return addr;
}

static char* shmat_at_address(int shmid, char* req_addr) {
  if (!is_aligned(req_addr, SHMLBA)) {
    assert(false, "Requested address needs to be SHMLBA aligned");
    return NULL;
  }

  char* addr = (char*)shmat(shmid, req_addr, 0);

  if ((intptr_t)addr == -1) {
    shm_warning_with_errno("Failed to attach shared memory.");
    return NULL;
  }

  return addr;
}

static char* shmat_large_pages(int shmid, size_t bytes, size_t alignment, char* req_addr) {
  // If a req_addr has been provided, we assume that the caller has already aligned the address.
  if (req_addr != NULL) {
    assert(is_aligned(req_addr, os::large_page_size()), "Must be divisible by the large page size");
    assert(is_aligned(req_addr, alignment), "Must be divisible by given alignment");
    return shmat_at_address(shmid, req_addr);
  }

  // Since shmid has been setup with SHM_HUGETLB, shmat will automatically
  // return large page size aligned memory addresses when req_addr == NULL.
  // However, if the alignment is larger than the large page size, we have
  // to manually ensure that the memory returned is 'alignment' aligned.
  if (alignment > os::large_page_size()) {
    assert(is_aligned(alignment, os::large_page_size()), "Must be divisible by the large page size");
    return shmat_with_alignment(shmid, bytes, alignment);
  } else {
    return shmat_at_address(shmid, NULL);
  }
}

char* os::Linux::reserve_memory_special_shm(size_t bytes, size_t alignment,
                                            char* req_addr, bool exec) {
  // "exec" is passed in but not used.  Creating the shared image for
  // the code cache doesn't have an SHM_X executable permission to check.
  assert(UseLargePages && UseSHM, "only for SHM large pages");
  assert(is_aligned(req_addr, os::large_page_size()), "Unaligned address");
  assert(is_aligned(req_addr, alignment), "Unaligned address");

  if (!is_aligned(bytes, os::large_page_size())) {
    return NULL; // Fallback to small pages.
  }

  // Create a large shared memory region to attach to based on size.
  // Currently, size is the total size of the heap.
  int shmid = shmget(IPC_PRIVATE, bytes, SHM_HUGETLB|IPC_CREAT|SHM_R|SHM_W);
  if (shmid == -1) {
    // Possible reasons for shmget failure:
    // 1. shmmax is too small for Java heap.
    //    > check shmmax value: cat /proc/sys/kernel/shmmax
    //    > increase shmmax value: echo "0xffffffff" > /proc/sys/kernel/shmmax
    // 2. not enough large page memory.
    //    > check available large pages: cat /proc/meminfo
    //    > increase amount of large pages:
    //          echo new_value > /proc/sys/vm/nr_hugepages
    //      Note 1: different Linux may use different name for this property,
    //            e.g. on Redhat AS-3 it is "hugetlb_pool".
    //      Note 2: it's possible there's enough physical memory available but
    //            they are so fragmented after a long run that they can't
    //            coalesce into large pages. Try to reserve large pages when
    //            the system is still "fresh".
    shm_warning_with_errno("Failed to reserve shared memory.");
    return NULL;
  }

  // Attach to the region.
  char* addr = shmat_large_pages(shmid, bytes, alignment, req_addr);

  // Remove shmid. If shmat() is successful, the actual shared memory segment
  // will be deleted when it's detached by shmdt() or when the process
  // terminates. If shmat() is not successful this will remove the shared
  // segment immediately.
  shmctl(shmid, IPC_RMID, NULL);

  return addr;
}

static void warn_on_large_pages_failure(char* req_addr, size_t bytes,
                                        int error) {
  assert(error == ENOMEM, "Only expect to fail if no memory is available");

  bool warn_on_failure = UseLargePages &&
      (!FLAG_IS_DEFAULT(UseLargePages) ||
       !FLAG_IS_DEFAULT(UseHugeTLBFS) ||
       !FLAG_IS_DEFAULT(LargePageSizeInBytes));

  if (warn_on_failure) {
    char msg[128];
    jio_snprintf(msg, sizeof(msg), "Failed to reserve large pages memory req_addr: "
                 PTR_FORMAT " bytes: " SIZE_FORMAT " (errno = %d).", req_addr, bytes, error);
    warning("%s", msg);
  }
}

char* os::Linux::reserve_memory_special_huge_tlbfs_only(size_t bytes,
                                                        char* req_addr,
                                                        bool exec) {
  assert(UseLargePages && UseHugeTLBFS, "only for Huge TLBFS large pages");
  assert(is_aligned(bytes, os::large_page_size()), "Unaligned size");
  assert(is_aligned(req_addr, os::large_page_size()), "Unaligned address");

  int prot = exec ? PROT_READ|PROT_WRITE|PROT_EXEC : PROT_READ|PROT_WRITE;
  char* addr = (char*)::mmap(req_addr, bytes, prot,
                             MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB,
                             -1, 0);

  if (addr == MAP_FAILED) {
    warn_on_large_pages_failure(req_addr, bytes, errno);
    return NULL;
  }

  assert(is_aligned(addr, os::large_page_size()), "Must be");

  return addr;
}

// Reserve memory using mmap(MAP_HUGETLB).
//  - bytes shall be a multiple of alignment.
//  - req_addr can be NULL. If not NULL, it must be a multiple of alignment.
//  - alignment sets the alignment at which memory shall be allocated.
//     It must be a multiple of allocation granularity.
// Returns address of memory or NULL. If req_addr was not NULL, will only return
//  req_addr or NULL.
char* os::Linux::reserve_memory_special_huge_tlbfs_mixed(size_t bytes,
                                                         size_t alignment,
                                                         char* req_addr,
                                                         bool exec) {
  size_t large_page_size = os::large_page_size();
  assert(bytes >= large_page_size, "Shouldn't allocate large pages for small sizes");

  assert(is_aligned(req_addr, alignment), "Must be");
  assert(is_aligned(bytes, alignment), "Must be");

  // First reserve - but not commit - the address range in small pages.
  char* const start = anon_mmap_aligned(bytes, alignment, req_addr);

  if (start == NULL) {
    return NULL;
  }

  assert(is_aligned(start, alignment), "Must be");

  char* end = start + bytes;

  // Find the regions of the allocated chunk that can be promoted to large pages.
  char* lp_start = align_up(start, large_page_size);
  char* lp_end   = align_down(end, large_page_size);

  size_t lp_bytes = lp_end - lp_start;

  assert(is_aligned(lp_bytes, large_page_size), "Must be");

  if (lp_bytes == 0) {
    // The mapped region doesn't even span the start and the end of a large page.
    // Fall back to allocate a non-special area.
    ::munmap(start, end - start);
    return NULL;
  }

  int prot = exec ? PROT_READ|PROT_WRITE|PROT_EXEC : PROT_READ|PROT_WRITE;

  void* result;

  // Commit small-paged leading area.
  if (start != lp_start) {
    result = ::mmap(start, lp_start - start, prot,
                    MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,
                    -1, 0);
    if (result == MAP_FAILED) {
      ::munmap(lp_start, end - lp_start);
      return NULL;
    }
  }

  // Commit large-paged area.
  result = ::mmap(lp_start, lp_bytes, prot,
                  MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_HUGETLB,
                  -1, 0);
  if (result == MAP_FAILED) {
    warn_on_large_pages_failure(lp_start, lp_bytes, errno);
    // If the mmap above fails, the large pages region will be unmapped and we
    // have regions before and after with small pages. Release these regions.
    //
    // |  mapped  |  unmapped  |  mapped  |
    // ^          ^            ^          ^
    // start      lp_start     lp_end     end
    //
    ::munmap(start, lp_start - start);
    ::munmap(lp_end, end - lp_end);
    return NULL;
  }

  // Commit small-paged trailing area.
  if (lp_end != end) {
    result = ::mmap(lp_end, end - lp_end, prot,
                    MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,
                    -1, 0);
    if (result == MAP_FAILED) {
      ::munmap(start, lp_end - start);
      return NULL;
    }
  }

  return start;
}

char* os::Linux::reserve_memory_special_huge_tlbfs(size_t bytes,
                                                   size_t alignment,
                                                   char* req_addr,
                                                   bool exec) {
  assert(UseLargePages && UseHugeTLBFS, "only for Huge TLBFS large pages");
  assert(is_aligned(req_addr, alignment), "Must be");
  assert(is_aligned(alignment, os::vm_allocation_granularity()), "Must be");
  assert(is_power_of_2(os::large_page_size()), "Must be");
  assert(bytes >= os::large_page_size(), "Shouldn't allocate large pages for small sizes");

  if (is_aligned(bytes, os::large_page_size()) && alignment <= os::large_page_size()) {
    return reserve_memory_special_huge_tlbfs_only(bytes, req_addr, exec);
  } else {
    return reserve_memory_special_huge_tlbfs_mixed(bytes, alignment, req_addr, exec);
  }
}

char* os::reserve_memory_special(size_t bytes, size_t alignment,
                                 char* req_addr, bool exec) {
  assert(UseLargePages, "only for large pages");

  char* addr;
  if (UseSHM) {
    addr = os::Linux::reserve_memory_special_shm(bytes, alignment, req_addr, exec);
  } else {
    assert(UseHugeTLBFS, "must be");
    addr = os::Linux::reserve_memory_special_huge_tlbfs(bytes, alignment, req_addr, exec);
  }

  if (addr != NULL) {
    if (UseNUMAInterleaving) {
      numa_make_global(addr, bytes);
    }

    // The memory is committed
    MemTracker::record_virtual_memory_reserve_and_commit((address)addr, bytes, CALLER_PC);
  }

  return addr;
}

bool os::Linux::release_memory_special_shm(char* base, size_t bytes) {
  // detaching the SHM segment will also delete it, see reserve_memory_special_shm()
  return shmdt(base) == 0;
}

bool os::Linux::release_memory_special_huge_tlbfs(char* base, size_t bytes) {
  return pd_release_memory(base, bytes);
}

bool os::release_memory_special(char* base, size_t bytes) {
  bool res;
  if (MemTracker::tracking_level() > NMT_minimal) {
    Tracker tkr = MemTracker::get_virtual_memory_release_tracker();
    res = os::Linux::release_memory_special_impl(base, bytes);
    if (res) {
      tkr.record((address)base, bytes);
    }

  } else {
    res = os::Linux::release_memory_special_impl(base, bytes);
  }
  return res;
}

bool os::Linux::release_memory_special_impl(char* base, size_t bytes) {
  assert(UseLargePages, "only for large pages");
  bool res;

  if (UseSHM) {
    res = os::Linux::release_memory_special_shm(base, bytes);
  } else {
    assert(UseHugeTLBFS, "must be");
    res = os::Linux::release_memory_special_huge_tlbfs(base, bytes);
  }
  return res;
}

size_t os::large_page_size() {
  return _large_page_size;
}

// With SysV SHM the entire memory region must be allocated as shared
// memory.
// HugeTLBFS allows application to commit large page memory on demand.
// However, when committing memory with HugeTLBFS fails, the region
// that was supposed to be committed will lose the old reservation
// and allow other threads to steal that memory region. Because of this
// behavior we can't commit HugeTLBFS memory.
bool os::can_commit_large_page_memory() {
  return UseTransparentHugePages;
}

bool os::can_execute_large_page_memory() {
  return UseTransparentHugePages || UseHugeTLBFS;
}

// Reserve memory at an arbitrary address, only if that area is
// available (and not reserved for something else).

char* os::pd_attempt_reserve_memory_at(size_t bytes, char* requested_addr) {
  const int max_tries = 10;
  char* base[max_tries];
  size_t size[max_tries];
  const size_t gap = 0x000000;

  // Assert only that the size is a multiple of the page size, since
  // that's all that mmap requires, and since that's all we really know
  // about at this low abstraction level.  If we need higher alignment,
  // we can either pass an alignment to this method or verify alignment
  // in one of the methods further up the call chain.  See bug 5044738.
  assert(bytes % os::vm_page_size() == 0, "reserving unexpected size block");

  // Repeatedly allocate blocks until the block is allocated at the
  // right spot.

  // Linux mmap allows caller to pass an address as hint; give it a try first,
  // if kernel honors the hint then we can return immediately.
  char * addr = anon_mmap(requested_addr, bytes, false);
  if (addr == requested_addr) {
    return requested_addr;
  }

  if (addr != NULL) {
    // mmap() is successful but it fails to reserve at the requested address
    anon_munmap(addr, bytes);
  }

  int i;
  for (i = 0; i < max_tries; ++i) {
    base[i] = reserve_memory(bytes);

    if (base[i] != NULL) {
      // Is this the block we wanted?
      if (base[i] == requested_addr) {
        size[i] = bytes;
        break;
      }

      // Does this overlap the block we wanted? Give back the overlapped
      // parts and try again.

      ptrdiff_t top_overlap = requested_addr + (bytes + gap) - base[i];
      if (top_overlap >= 0 && (size_t)top_overlap < bytes) {
        unmap_memory(base[i], top_overlap);
        base[i] += top_overlap;
        size[i] = bytes - top_overlap;
      } else {
        ptrdiff_t bottom_overlap = base[i] + bytes - requested_addr;
        if (bottom_overlap >= 0 && (size_t)bottom_overlap < bytes) {
          unmap_memory(requested_addr, bottom_overlap);
          size[i] = bytes - bottom_overlap;
        } else {
          size[i] = bytes;
        }
      }
    }
  }

  // Give back the unused reserved pieces.

  for (int j = 0; j < i; ++j) {
    if (base[j] != NULL) {
      unmap_memory(base[j], size[j]);
    }
  }

  if (i < max_tries) {
    return requested_addr;
  } else {
    return NULL;
  }
}

size_t os::read(int fd, void *buf, unsigned int nBytes) {
  return ::read(fd, buf, nBytes);
}

size_t os::read_at(int fd, void *buf, unsigned int nBytes, jlong offset) {
  return ::pread(fd, buf, nBytes, offset);
}

// Short sleep, direct OS call.
//
// Note: certain versions of Linux CFS scheduler (since 2.6.23) do not guarantee
// sched_yield(2) will actually give up the CPU:
//
//   * Alone on this pariticular CPU, keeps running.
//   * Before the introduction of "skip_buddy" with "compat_yield" disabled
//     (pre 2.6.39).
//
// So calling this with 0 is an alternative.
//
void os::naked_short_sleep(jlong ms) {
  struct timespec req;

  assert(ms < 1000, "Un-interruptable sleep, short time use only");
  req.tv_sec = 0;
  if (ms > 0) {
    req.tv_nsec = (ms % 1000) * 1000000;
  } else {
    req.tv_nsec = 1;
  }

  nanosleep(&req, NULL);

  return;
}

// Sleep forever; naked call to OS-specific sleep; use with CAUTION
void os::infinite_sleep() {
  while (true) {    // sleep forever ...
    ::sleep(100);   // ... 100 seconds at a time
  }
}

// Used to convert frequent JVM_Yield() to nops
bool os::dont_yield() {
  return DontYieldALot;
}

void os::naked_yield() {
  sched_yield();
}

////////////////////////////////////////////////////////////////////////////////
// thread priority support

// Note: Normal Linux applications are run with SCHED_OTHER policy. SCHED_OTHER
// only supports dynamic priority, static priority must be zero. For real-time
// applications, Linux supports SCHED_RR which allows static priority (1-99).
// However, for large multi-threaded applications, SCHED_RR is not only slower
// than SCHED_OTHER, but also very unstable (my volano tests hang hard 4 out
// of 5 runs - Sep 2005).
//
// The following code actually changes the niceness of kernel-thread/LWP. It
// has an assumption that setpriority() only modifies one kernel-thread/LWP,
// not the entire user process, and user level threads are 1:1 mapped to kernel
// threads. It has always been the case, but could change in the future. For
// this reason, the code should not be used as default (ThreadPriorityPolicy=0).
// It is only used when ThreadPriorityPolicy=1 and requires root privilege.

int os::java_to_os_priority[CriticalPriority + 1] = {
  19,              // 0 Entry should never be used

   4,              // 1 MinPriority
   3,              // 2
   2,              // 3

   1,              // 4
   0,              // 5 NormPriority
  -1,              // 6

  -2,              // 7
  -3,              // 8
  -4,              // 9 NearMaxPriority

  -5,              // 10 MaxPriority

  -5               // 11 CriticalPriority
};

static int prio_init() {
  if (ThreadPriorityPolicy == 1) {
    // Only root can raise thread priority. Don't allow ThreadPriorityPolicy=1
    // if effective uid is not root. Perhaps, a more elegant way of doing
    // this is to test CAP_SYS_NICE capability, but that will require libcap.so
    if (geteuid() != 0) {
      if (!FLAG_IS_DEFAULT(ThreadPriorityPolicy)) {
        warning("-XX:ThreadPriorityPolicy requires root privilege on Linux");
      }
      ThreadPriorityPolicy = 0;
    }
  }
  if (UseCriticalJavaThreadPriority) {
    os::java_to_os_priority[MaxPriority] = os::java_to_os_priority[CriticalPriority];
  }
  return 0;
}

OSReturn os::set_native_priority(Thread* thread, int newpri) {
  if (!UseThreadPriorities || ThreadPriorityPolicy == 0) return OS_OK;

  int ret = setpriority(PRIO_PROCESS, thread->osthread()->thread_id(), newpri);
  return (ret == 0) ? OS_OK : OS_ERR;
}

OSReturn os::get_native_priority(const Thread* const thread,
                                 int *priority_ptr) {
  if (!UseThreadPriorities || ThreadPriorityPolicy == 0) {
    *priority_ptr = java_to_os_priority[NormPriority];
    return OS_OK;
  }

  errno = 0;
  *priority_ptr = getpriority(PRIO_PROCESS, thread->osthread()->thread_id());
  return (*priority_ptr != -1 || errno == 0 ? OS_OK : OS_ERR);
}

// Hint to the underlying OS that a task switch would not be good.
// Void return because it's a hint and can fail.
void os::hint_no_preempt() {}

////////////////////////////////////////////////////////////////////////////////
// suspend/resume support

//  The low-level signal-based suspend/resume support is a remnant from the
//  old VM-suspension that used to be for java-suspension, safepoints etc,
//  within hotspot. Currently used by JFR's OSThreadSampler
//
//  The remaining code is greatly simplified from the more general suspension
//  code that used to be used.
//
//  The protocol is quite simple:
//  - suspend:
//      - sends a signal to the target thread
//      - polls the suspend state of the osthread using a yield loop
//      - target thread signal handler (SR_handler) sets suspend state
//        and blocks in sigsuspend until continued
//  - resume:
//      - sets target osthread state to continue
//      - sends signal to end the sigsuspend loop in the SR_handler
//
//  Note that the SR_lock plays no role in this suspend/resume protocol,
//  but is checked for NULL in SR_handler as a thread termination indicator.
//  The SR_lock is, however, used by JavaThread::java_suspend()/java_resume() APIs.
//
//  Note that resume_clear_context() and suspend_save_context() are needed
//  by SR_handler(), so that fetch_frame_from_ucontext() works,
//  which in part is used by:
//    - Forte Analyzer: AsyncGetCallTrace()
//    - StackBanging: get_frame_at_stack_banging_point()

static void resume_clear_context(OSThread *osthread) {
  osthread->set_ucontext(NULL);
  osthread->set_siginfo(NULL);
}

static void suspend_save_context(OSThread *osthread, siginfo_t* siginfo,
                                 ucontext_t* context) {
  osthread->set_ucontext(context);
  osthread->set_siginfo(siginfo);
}

// Handler function invoked when a thread's execution is suspended or
// resumed. We have to be careful that only async-safe functions are
// called here (Note: most pthread functions are not async safe and
// should be avoided.)
//
// Note: sigwait() is a more natural fit than sigsuspend() from an
// interface point of view, but sigwait() prevents the signal hander
// from being run. libpthread would get very confused by not having
// its signal handlers run and prevents sigwait()'s use with the
// mutex granting granting signal.
//
// Currently only ever called on the VMThread and JavaThreads (PC sampling)
//
static void SR_handler(int sig, siginfo_t* siginfo, ucontext_t* context) {
  // Save and restore errno to avoid confusing native code with EINTR
  // after sigsuspend.
  int old_errno = errno;

  Thread* thread = Thread::current_or_null_safe();
  assert(thread != NULL, "Missing current thread in SR_handler");

  // On some systems we have seen signal delivery get "stuck" until the signal
  // mask is changed as part of thread termination. Check that the current thread
  // has not already terminated (via SR_lock()) - else the following assertion
  // will fail because the thread is no longer a JavaThread as the ~JavaThread
  // destructor has completed.

  if (thread->SR_lock() == NULL) {
    return;
  }

  assert(thread->is_VM_thread() || thread->is_Java_thread(), "Must be VMThread or JavaThread");

  OSThread* osthread = thread->osthread();

  os::SuspendResume::State current = osthread->sr.state();
  if (current == os::SuspendResume::SR_SUSPEND_REQUEST) {
    suspend_save_context(osthread, siginfo, context);

    // attempt to switch the state, we assume we had a SUSPEND_REQUEST
    os::SuspendResume::State state = osthread->sr.suspended();
    if (state == os::SuspendResume::SR_SUSPENDED) {
      sigset_t suspend_set;  // signals for sigsuspend()
      sigemptyset(&suspend_set);
      // get current set of blocked signals and unblock resume signal
      pthread_sigmask(SIG_BLOCK, NULL, &suspend_set);
      sigdelset(&suspend_set, SR_signum);

      sr_semaphore.signal();
      // wait here until we are resumed
      while (1) {
        sigsuspend(&suspend_set);

        os::SuspendResume::State result = osthread->sr.running();
        if (result == os::SuspendResume::SR_RUNNING) {
          sr_semaphore.signal();
          break;
        }
      }

    } else if (state == os::SuspendResume::SR_RUNNING) {
      // request was cancelled, continue
    } else {
      ShouldNotReachHere();
    }

    resume_clear_context(osthread);
  } else if (current == os::SuspendResume::SR_RUNNING) {
    // request was cancelled, continue
  } else if (current == os::SuspendResume::SR_WAKEUP_REQUEST) {
    // ignore
  } else {
    // ignore
  }

  errno = old_errno;
}

static int SR_initialize() {
  struct sigaction act;
  char *s;

  // Get signal number to use for suspend/resume
  if ((s = ::getenv("_JAVA_SR_SIGNUM")) != 0) {
    int sig = ::strtol(s, 0, 10);
    if (sig > MAX2(SIGSEGV, SIGBUS) &&  // See 4355769.
        sig < NSIG) {                   // Must be legal signal and fit into sigflags[].
      SR_signum = sig;
    } else {
      warning("You set _JAVA_SR_SIGNUM=%d. It must be in range [%d, %d]. Using %d instead.",
              sig, MAX2(SIGSEGV, SIGBUS)+1, NSIG-1, SR_signum);
    }
  }

  assert(SR_signum > SIGSEGV && SR_signum > SIGBUS,
         "SR_signum must be greater than max(SIGSEGV, SIGBUS), see 4355769");

  sigemptyset(&SR_sigset);
  sigaddset(&SR_sigset, SR_signum);

  // Set up signal handler for suspend/resume
  act.sa_flags = SA_RESTART|SA_SIGINFO;
  act.sa_handler = (void (*)(int)) SR_handler;

  // SR_signum is blocked by default.
  // 4528190 - We also need to block pthread restart signal (32 on all
  // supported Linux platforms). Note that LinuxThreads need to block
  // this signal for all threads to work properly. So we don't have
  // to use hard-coded signal number when setting up the mask.
  pthread_sigmask(SIG_BLOCK, NULL, &act.sa_mask);

  if (sigaction(SR_signum, &act, 0) == -1) {
    return -1;
  }

  // Save signal flag
  os::Linux::set_our_sigflags(SR_signum, act.sa_flags);
  return 0;
}

static int sr_notify(OSThread* osthread) {
  int status = pthread_kill(osthread->pthread_id(), SR_signum);
  assert_status(status == 0, status, "pthread_kill");
  return status;
}

// "Randomly" selected value for how long we want to spin
// before bailing out on suspending a thread, also how often
// we send a signal to a thread we want to resume
static const int RANDOMLY_LARGE_INTEGER = 1000000;
static const int RANDOMLY_LARGE_INTEGER2 = 100;

// returns true on success and false on error - really an error is fatal
// but this seems the normal response to library errors
static bool do_suspend(OSThread* osthread) {
  assert(osthread->sr.is_running(), "thread should be running");
  assert(!sr_semaphore.trywait(), "semaphore has invalid state");

  // mark as suspended and send signal
  if (osthread->sr.request_suspend() != os::SuspendResume::SR_SUSPEND_REQUEST) {
    // failed to switch, state wasn't running?
    ShouldNotReachHere();
    return false;
  }

  if (sr_notify(osthread) != 0) {
    ShouldNotReachHere();
  }

  // managed to send the signal and switch to SUSPEND_REQUEST, now wait for SUSPENDED
  while (true) {
    if (sr_semaphore.timedwait(0, 2 * NANOSECS_PER_MILLISEC)) {
      break;
    } else {
      // timeout
      os::SuspendResume::State cancelled = osthread->sr.cancel_suspend();
      if (cancelled == os::SuspendResume::SR_RUNNING) {
        return false;
      } else if (cancelled == os::SuspendResume::SR_SUSPENDED) {
        // make sure that we consume the signal on the semaphore as well
        sr_semaphore.wait();
        break;
      } else {
        ShouldNotReachHere();
        return false;
      }
    }
  }

  guarantee(osthread->sr.is_suspended(), "Must be suspended");
  return true;
}

static void do_resume(OSThread* osthread) {
  assert(osthread->sr.is_suspended(), "thread should be suspended");
  assert(!sr_semaphore.trywait(), "invalid semaphore state");

  if (osthread->sr.request_wakeup() != os::SuspendResume::SR_WAKEUP_REQUEST) {
    // failed to switch to WAKEUP_REQUEST
    ShouldNotReachHere();
    return;
  }

  while (true) {
    if (sr_notify(osthread) == 0) {
      if (sr_semaphore.timedwait(0, 2 * NANOSECS_PER_MILLISEC)) {
        if (osthread->sr.is_running()) {
          return;
        }
      }
    } else {
      ShouldNotReachHere();
    }
  }

  guarantee(osthread->sr.is_running(), "Must be running!");
}

///////////////////////////////////////////////////////////////////////////////////
// signal handling (except suspend/resume)

// This routine may be used by user applications as a "hook" to catch signals.
// The user-defined signal handler must pass unrecognized signals to this
// routine, and if it returns true (non-zero), then the signal handler must
// return immediately.  If the flag "abort_if_unrecognized" is true, then this
// routine will never retun false (zero), but instead will execute a VM panic
// routine kill the process.
//
// If this routine returns false, it is OK to call it again.  This allows
// the user-defined signal handler to perform checks either before or after
// the VM performs its own checks.  Naturally, the user code would be making
// a serious error if it tried to handle an exception (such as a null check
// or breakpoint) that the VM was generating for its own correct operation.
//
// This routine may recognize any of the following kinds of signals:
//    SIGBUS, SIGSEGV, SIGILL, SIGFPE, SIGQUIT, SIGPIPE, SIGXFSZ, SIGUSR1.
// It should be consulted by handlers for any of those signals.
//
// The caller of this routine must pass in the three arguments supplied
// to the function referred to in the "sa_sigaction" (not the "sa_handler")
// field of the structure passed to sigaction().  This routine assumes that
// the sa_flags field passed to sigaction() includes SA_SIGINFO and SA_RESTART.
//
// Note that the VM will print warnings if it detects conflicting signal
// handlers, unless invoked with the option "-XX:+AllowUserSignalHandlers".
//
extern "C" JNIEXPORT int JVM_handle_linux_signal(int signo,
                                                 siginfo_t* siginfo,
                                                 void* ucontext,
                                                 int abort_if_unrecognized);

void signalHandler(int sig, siginfo_t* info, void* uc) {
  assert(info != NULL && uc != NULL, "it must be old kernel");
  int orig_errno = errno;  // Preserve errno value over signal handler.
  JVM_handle_linux_signal(sig, info, uc, true);
  errno = orig_errno;
}


// This boolean allows users to forward their own non-matching signals
// to JVM_handle_linux_signal, harmlessly.
bool os::Linux::signal_handlers_are_installed = false;

// For signal-chaining
struct sigaction sigact[NSIG];
uint64_t sigs = 0;
#if (64 < NSIG-1)
#error "Not all signals can be encoded in sigs. Adapt its type!"
#endif
bool os::Linux::libjsig_is_loaded = false;
typedef struct sigaction *(*get_signal_t)(int);
get_signal_t os::Linux::get_signal_action = NULL;

struct sigaction* os::Linux::get_chained_signal_action(int sig) {
  struct sigaction *actp = NULL;

  if (libjsig_is_loaded) {
    // Retrieve the old signal handler from libjsig
    actp = (*get_signal_action)(sig);
  }
  if (actp == NULL) {
    // Retrieve the preinstalled signal handler from jvm
    actp = get_preinstalled_handler(sig);
  }

  return actp;
}

static bool call_chained_handler(struct sigaction *actp, int sig,
                                 siginfo_t *siginfo, void *context) {
  // Call the old signal handler
  if (actp->sa_handler == SIG_DFL) {
    // It's more reasonable to let jvm treat it as an unexpected exception
    // instead of taking the default action.
    return false;
  } else if (actp->sa_handler != SIG_IGN) {
    if ((actp->sa_flags & SA_NODEFER) == 0) {
      // automaticlly block the signal
      sigaddset(&(actp->sa_mask), sig);
    }

    sa_handler_t hand = NULL;
    sa_sigaction_t sa = NULL;
    bool siginfo_flag_set = (actp->sa_flags & SA_SIGINFO) != 0;
    // retrieve the chained handler
    if (siginfo_flag_set) {
      sa = actp->sa_sigaction;
    } else {
      hand = actp->sa_handler;
    }

    if ((actp->sa_flags & SA_RESETHAND) != 0) {
      actp->sa_handler = SIG_DFL;
    }

    // try to honor the signal mask
    sigset_t oset;
    sigemptyset(&oset);
    pthread_sigmask(SIG_SETMASK, &(actp->sa_mask), &oset);

    // call into the chained handler
    if (siginfo_flag_set) {
      (*sa)(sig, siginfo, context);
    } else {
      (*hand)(sig);
    }

    // restore the signal mask
    pthread_sigmask(SIG_SETMASK, &oset, NULL);
  }
  // Tell jvm's signal handler the signal is taken care of.
  return true;
}

bool os::Linux::chained_handler(int sig, siginfo_t* siginfo, void* context) {
  bool chained = false;
  // signal-chaining
  if (UseSignalChaining) {
    struct sigaction *actp = get_chained_signal_action(sig);
    if (actp != NULL) {
      chained = call_chained_handler(actp, sig, siginfo, context);
    }
  }
  return chained;
}

struct sigaction* os::Linux::get_preinstalled_handler(int sig) {
  if ((((uint64_t)1 << (sig-1)) & sigs) != 0) {
    return &sigact[sig];
  }
  return NULL;
}

void os::Linux::save_preinstalled_handler(int sig, struct sigaction& oldAct) {
  assert(sig > 0 && sig < NSIG, "vm signal out of expected range");
  sigact[sig] = oldAct;
  sigs |= (uint64_t)1 << (sig-1);
}

// for diagnostic
int sigflags[NSIG];

int os::Linux::get_our_sigflags(int sig) {
  assert(sig > 0 && sig < NSIG, "vm signal out of expected range");
  return sigflags[sig];
}

void os::Linux::set_our_sigflags(int sig, int flags) {
  assert(sig > 0 && sig < NSIG, "vm signal out of expected range");
  if (sig > 0 && sig < NSIG) {
    sigflags[sig] = flags;
  }
}

void os::Linux::set_signal_handler(int sig, bool set_installed) {
  // Check for overwrite.
  struct sigaction oldAct;
  sigaction(sig, (struct sigaction*)NULL, &oldAct);

  void* oldhand = oldAct.sa_sigaction
                ? CAST_FROM_FN_PTR(void*,  oldAct.sa_sigaction)
                : CAST_FROM_FN_PTR(void*,  oldAct.sa_handler);
  if (oldhand != CAST_FROM_FN_PTR(void*, SIG_DFL) &&
      oldhand != CAST_FROM_FN_PTR(void*, SIG_IGN) &&
      oldhand != CAST_FROM_FN_PTR(void*, (sa_sigaction_t)signalHandler)) {
    if (AllowUserSignalHandlers || !set_installed) {
      // Do not overwrite; user takes responsibility to forward to us.
      return;
    } else if (UseSignalChaining) {
      // save the old handler in jvm
      save_preinstalled_handler(sig, oldAct);
      // libjsig also interposes the sigaction() call below and saves the
      // old sigaction on it own.
    } else {
      fatal("Encountered unexpected pre-existing sigaction handler "
            "%#lx for signal %d.", (long)oldhand, sig);
    }
  }

  struct sigaction sigAct;
  sigfillset(&(sigAct.sa_mask));
  sigAct.sa_handler = SIG_DFL;
  if (!set_installed) {
    sigAct.sa_flags = SA_SIGINFO|SA_RESTART;
  } else {
    sigAct.sa_sigaction = signalHandler;
    sigAct.sa_flags = SA_SIGINFO|SA_RESTART;
  }
  // Save flags, which are set by ours
  assert(sig > 0 && sig < NSIG, "vm signal out of expected range");
  sigflags[sig] = sigAct.sa_flags;

  int ret = sigaction(sig, &sigAct, &oldAct);
  assert(ret == 0, "check");

  void* oldhand2  = oldAct.sa_sigaction
                  ? CAST_FROM_FN_PTR(void*, oldAct.sa_sigaction)
                  : CAST_FROM_FN_PTR(void*, oldAct.sa_handler);
  assert(oldhand2 == oldhand, "no concurrent signal handler installation");
}

// install signal handlers for signals that HotSpot needs to
// handle in order to support Java-level exception handling.

void os::Linux::install_signal_handlers() {
  if (!signal_handlers_are_installed) {
    signal_handlers_are_installed = true;

    // signal-chaining
    typedef void (*signal_setting_t)();
    signal_setting_t begin_signal_setting = NULL;
    signal_setting_t end_signal_setting = NULL;
    begin_signal_setting = CAST_TO_FN_PTR(signal_setting_t,
                                          dlsym(RTLD_DEFAULT, "JVM_begin_signal_setting"));
    if (begin_signal_setting != NULL) {
      end_signal_setting = CAST_TO_FN_PTR(signal_setting_t,
                                          dlsym(RTLD_DEFAULT, "JVM_end_signal_setting"));
      get_signal_action = CAST_TO_FN_PTR(get_signal_t,
                                         dlsym(RTLD_DEFAULT, "JVM_get_signal_action"));
      libjsig_is_loaded = true;
      assert(UseSignalChaining, "should enable signal-chaining");
    }
    if (libjsig_is_loaded) {
      // Tell libjsig jvm is setting signal handlers
      (*begin_signal_setting)();
    }

    set_signal_handler(SIGSEGV, true);
    set_signal_handler(SIGPIPE, true);
    set_signal_handler(SIGBUS, true);
    set_signal_handler(SIGILL, true);
    set_signal_handler(SIGFPE, true);
#if defined(PPC64)
    set_signal_handler(SIGTRAP, true);
#endif
    set_signal_handler(SIGXFSZ, true);

    if (libjsig_is_loaded) {
      // Tell libjsig jvm finishes setting signal handlers
      (*end_signal_setting)();
    }

    // We don't activate signal checker if libjsig is in place, we trust ourselves
    // and if UserSignalHandler is installed all bets are off.
    // Log that signal checking is off only if -verbose:jni is specified.
    if (CheckJNICalls) {
      if (libjsig_is_loaded) {
        if (PrintJNIResolving) {
          tty->print_cr("Info: libjsig is activated, all active signal checking is disabled");
        }
        check_signals = false;
      }
      if (AllowUserSignalHandlers) {
        if (PrintJNIResolving) {
          tty->print_cr("Info: AllowUserSignalHandlers is activated, all active signal checking is disabled");
        }
        check_signals = false;
      }
    }
  }
}

// This is the fastest way to get thread cpu time on Linux.
// Returns cpu time (user+sys) for any thread, not only for current.
// POSIX compliant clocks are implemented in the kernels 2.6.16+.
// It might work on 2.6.10+ with a special kernel/glibc patch.
// For reference, please, see IEEE Std 1003.1-2004:
//   http://www.unix.org/single_unix_specification

jlong os::Linux::fast_thread_cpu_time(clockid_t clockid) {
  struct timespec tp;
  int rc = os::Linux::clock_gettime(clockid, &tp);
  assert(rc == 0, "clock_gettime is expected to return 0 code");

  return (tp.tv_sec * NANOSECS_PER_SEC) + tp.tv_nsec;
}

void os::Linux::initialize_os_info() {
  assert(_os_version == 0, "OS info already initialized");

  struct utsname _uname;

  uint32_t major;
  uint32_t minor;
  uint32_t fix;

  int rc;

  // Kernel version is unknown if
  // verification below fails.
  _os_version = 0x01000000;

  rc = uname(&_uname);
  if (rc != -1) {

    rc = sscanf(_uname.release,"%d.%d.%d", &major, &minor, &fix);
    if (rc == 3) {

      if (major < 256 && minor < 256 && fix < 256) {
        // Kernel version format is as expected,
        // set it overriding unknown state.
        _os_version = (major << 16) |
                      (minor << 8 ) |
                      (fix   << 0 ) ;
      }
    }
  }
}

uint32_t os::Linux::os_version() {
  assert(_os_version != 0, "not initialized");
  return _os_version & 0x00FFFFFF;
}

bool os::Linux::os_version_is_known() {
  assert(_os_version != 0, "not initialized");
  return _os_version & 0x01000000 ? false : true;
}

/////
// glibc on Linux platform uses non-documented flag
// to indicate, that some special sort of signal
// trampoline is used.
// We will never set this flag, and we should
// ignore this flag in our diagnostic
#ifdef SIGNIFICANT_SIGNAL_MASK
  #undef SIGNIFICANT_SIGNAL_MASK
#endif
#define SIGNIFICANT_SIGNAL_MASK (~0x04000000)

static const char* get_signal_handler_name(address handler,
                                           char* buf, int buflen) {
  int offset = 0;
  bool found = os::dll_address_to_library_name(handler, buf, buflen, &offset);
  if (found) {
    // skip directory names
    const char *p1, *p2;
    p1 = buf;
    size_t len = strlen(os::file_separator());
    while ((p2 = strstr(p1, os::file_separator())) != NULL) p1 = p2 + len;
    jio_snprintf(buf, buflen, "%s+0x%x", p1, offset);
  } else {
    jio_snprintf(buf, buflen, PTR_FORMAT, handler);
  }
  return buf;
}

static void print_signal_handler(outputStream* st, int sig,
                                 char* buf, size_t buflen) {
  struct sigaction sa;

  sigaction(sig, NULL, &sa);

  // See comment for SIGNIFICANT_SIGNAL_MASK define
  sa.sa_flags &= SIGNIFICANT_SIGNAL_MASK;

  st->print("%s: ", os::exception_name(sig, buf, buflen));

  address handler = (sa.sa_flags & SA_SIGINFO)
    ? CAST_FROM_FN_PTR(address, sa.sa_sigaction)
    : CAST_FROM_FN_PTR(address, sa.sa_handler);

  if (handler == CAST_FROM_FN_PTR(address, SIG_DFL)) {
    st->print("SIG_DFL");
  } else if (handler == CAST_FROM_FN_PTR(address, SIG_IGN)) {
    st->print("SIG_IGN");
  } else {
    st->print("[%s]", get_signal_handler_name(handler, buf, buflen));
  }

  st->print(", sa_mask[0]=");
  os::Posix::print_signal_set_short(st, &sa.sa_mask);

  address rh = VMError::get_resetted_sighandler(sig);
  // May be, handler was resetted by VMError?
  if (rh != NULL) {
    handler = rh;
    sa.sa_flags = VMError::get_resetted_sigflags(sig) & SIGNIFICANT_SIGNAL_MASK;
  }

  st->print(", sa_flags=");
  os::Posix::print_sa_flags(st, sa.sa_flags);

  // Check: is it our handler?
  if (handler == CAST_FROM_FN_PTR(address, (sa_sigaction_t)signalHandler) ||
      handler == CAST_FROM_FN_PTR(address, (sa_sigaction_t)SR_handler)) {
    // It is our signal handler
    // check for flags, reset system-used one!
    if ((int)sa.sa_flags != os::Linux::get_our_sigflags(sig)) {
      st->print(
                ", flags was changed from " PTR32_FORMAT ", consider using jsig library",
                os::Linux::get_our_sigflags(sig));
    }
  }
  st->cr();
}


#define DO_SIGNAL_CHECK(sig)                      \
  do {                                            \
    if (!sigismember(&check_signal_done, sig)) {  \
      os::Linux::check_signal_handler(sig);       \
    }                                             \
  } while (0)

// This method is a periodic task to check for misbehaving JNI applications
// under CheckJNI, we can add any periodic checks here

void os::run_periodic_checks() {
  if (check_signals == false) return;

  // SEGV and BUS if overridden could potentially prevent
  // generation of hs*.log in the event of a crash, debugging
  // such a case can be very challenging, so we absolutely
  // check the following for a good measure:
  DO_SIGNAL_CHECK(SIGSEGV);
  DO_SIGNAL_CHECK(SIGILL);
  DO_SIGNAL_CHECK(SIGFPE);
  DO_SIGNAL_CHECK(SIGBUS);
  DO_SIGNAL_CHECK(SIGPIPE);
  DO_SIGNAL_CHECK(SIGXFSZ);
#if defined(PPC64)
  DO_SIGNAL_CHECK(SIGTRAP);
#endif

  // ReduceSignalUsage allows the user to override these handlers
  // see comments at the very top and jvm_md.h
  if (!ReduceSignalUsage) {
    DO_SIGNAL_CHECK(SHUTDOWN1_SIGNAL);
    DO_SIGNAL_CHECK(SHUTDOWN2_SIGNAL);
    DO_SIGNAL_CHECK(SHUTDOWN3_SIGNAL);
    DO_SIGNAL_CHECK(BREAK_SIGNAL);
  }

  DO_SIGNAL_CHECK(SR_signum);
}

typedef int (*os_sigaction_t)(int, const struct sigaction *, struct sigaction *);

static os_sigaction_t os_sigaction = NULL;

void os::Linux::check_signal_handler(int sig) {
  char buf[O_BUFLEN];
  address jvmHandler = NULL;


  struct sigaction act;
  if (os_sigaction == NULL) {
    // only trust the default sigaction, in case it has been interposed
    os_sigaction = (os_sigaction_t)dlsym(RTLD_DEFAULT, "sigaction");
    if (os_sigaction == NULL) return;
  }

  os_sigaction(sig, (struct sigaction*)NULL, &act);


  act.sa_flags &= SIGNIFICANT_SIGNAL_MASK;

  address thisHandler = (act.sa_flags & SA_SIGINFO)
    ? CAST_FROM_FN_PTR(address, act.sa_sigaction)
    : CAST_FROM_FN_PTR(address, act.sa_handler);


  switch (sig) {
  case SIGSEGV:
  case SIGBUS:
  case SIGFPE:
  case SIGPIPE:
  case SIGILL:
  case SIGXFSZ:
    jvmHandler = CAST_FROM_FN_PTR(address, (sa_sigaction_t)signalHandler);
    break;

  case SHUTDOWN1_SIGNAL:
  case SHUTDOWN2_SIGNAL:
  case SHUTDOWN3_SIGNAL:
  case BREAK_SIGNAL:
    jvmHandler = (address)user_handler();
    break;

  default:
    if (sig == SR_signum) {
      jvmHandler = CAST_FROM_FN_PTR(address, (sa_sigaction_t)SR_handler);
    } else {
      return;
    }
    break;
  }

  if (thisHandler != jvmHandler) {
    tty->print("Warning: %s handler ", exception_name(sig, buf, O_BUFLEN));
    tty->print("expected:%s", get_signal_handler_name(jvmHandler, buf, O_BUFLEN));
    tty->print_cr("  found:%s", get_signal_handler_name(thisHandler, buf, O_BUFLEN));
    // No need to check this sig any longer
    sigaddset(&check_signal_done, sig);
    // Running under non-interactive shell, SHUTDOWN2_SIGNAL will be reassigned SIG_IGN
    if (sig == SHUTDOWN2_SIGNAL && !isatty(fileno(stdin))) {
      tty->print_cr("Running in non-interactive shell, %s handler is replaced by shell",
                    exception_name(sig, buf, O_BUFLEN));
    }
  } else if(os::Linux::get_our_sigflags(sig) != 0 && (int)act.sa_flags != os::Linux::get_our_sigflags(sig)) {
    tty->print("Warning: %s handler flags ", exception_name(sig, buf, O_BUFLEN));
    tty->print("expected:");
    os::Posix::print_sa_flags(tty, os::Linux::get_our_sigflags(sig));
    tty->cr();
    tty->print("  found:");
    os::Posix::print_sa_flags(tty, act.sa_flags);
    tty->cr();
    // No need to check this sig any longer
    sigaddset(&check_signal_done, sig);
  }

  // Dump all the signal
  if (sigismember(&check_signal_done, sig)) {
    print_signal_handlers(tty, buf, O_BUFLEN);
  }
}

extern void report_error(char* file_name, int line_no, char* title,
                         char* format, ...);

// this is called _before_ most of the global arguments have been parsed
void os::init(void) {
  char dummy;   // used to get a guess on initial stack address

  clock_tics_per_sec = sysconf(_SC_CLK_TCK);

  init_random(1234567);

  Linux::set_page_size(sysconf(_SC_PAGESIZE));
  if (Linux::page_size() == -1) {
    fatal("os_linux.cpp: os::init: sysconf failed (%s)",
          os::strerror(errno));
  }
  init_page_sizes((size_t) Linux::page_size());

  Linux::initialize_system_info();

  Linux::initialize_os_info();

  // _main_thread points to the thread that created/loaded the JVM.
  Linux::_main_thread = pthread_self();

  Linux::clock_init();
  initial_time_count = javaTimeNanos();

  // retrieve entry point for pthread_setname_np
  Linux::_pthread_setname_np =
    (int(*)(pthread_t, const char*))dlsym(RTLD_DEFAULT, "pthread_setname_np");

  os::Posix::init();
}

// To install functions for atexit system call
extern "C" {
  static void perfMemory_exit_helper() {
    perfMemory_exit();
  }
}

void os::pd_init_container_support() {
  OSContainer::init();
}

// this is called _after_ the global arguments have been parsed
jint os::init_2(void) {

  os::Posix::init_2();

  Linux::fast_thread_clock_init();

  // initialize suspend/resume support - must do this before signal_sets_init()
  if (SR_initialize() != 0) {
    perror("SR_initialize failed");
    return JNI_ERR;
  }

  Linux::signal_sets_init();
  Linux::install_signal_handlers();

  // Check and sets minimum stack sizes against command line options
  if (Posix::set_minimum_stack_sizes() == JNI_ERR) {
    return JNI_ERR;
  }
  Linux::capture_initial_stack(JavaThread::stack_size_at_create());

#if defined(IA32)
  workaround_expand_exec_shield_cs_limit();
#endif

  Linux::libpthread_init();
  Linux::sched_getcpu_init();
  log_info(os)("HotSpot is running with %s, %s",
               Linux::glibc_version(), Linux::libpthread_version());

  if (UseNUMA) {
    if (!Linux::libnuma_init()) {
      UseNUMA = false;
    } else {
      if ((Linux::numa_max_node() < 1)) {
        // There's only one node(they start from 0), disable NUMA.
        UseNUMA = false;
      }
    }
    // With SHM and HugeTLBFS large pages we cannot uncommit a page, so there's no way
    // we can make the adaptive lgrp chunk resizing work. If the user specified
    // both UseNUMA and UseLargePages (or UseSHM/UseHugeTLBFS) on the command line - warn and
    // disable adaptive resizing.
    if (UseNUMA && UseLargePages && !can_commit_large_page_memory()) {
      if (FLAG_IS_DEFAULT(UseNUMA)) {
        UseNUMA = false;
      } else {
        if (FLAG_IS_DEFAULT(UseLargePages) &&
            FLAG_IS_DEFAULT(UseSHM) &&
            FLAG_IS_DEFAULT(UseHugeTLBFS)) {
          UseLargePages = false;
        } else if (UseAdaptiveSizePolicy || UseAdaptiveNUMAChunkSizing) {
          warning("UseNUMA is not fully compatible with SHM/HugeTLBFS large pages, disabling adaptive resizing (-XX:-UseAdaptiveSizePolicy -XX:-UseAdaptiveNUMAChunkSizing)");
          UseAdaptiveSizePolicy = false;
          UseAdaptiveNUMAChunkSizing = false;
        }
      }
    }
    if (!UseNUMA && ForceNUMA) {
      UseNUMA = true;
    }
  }

  if (MaxFDLimit) {
    // set the number of file descriptors to max. print out error
    // if getrlimit/setrlimit fails but continue regardless.
    struct rlimit nbr_files;
    int status = getrlimit(RLIMIT_NOFILE, &nbr_files);
    if (status != 0) {
      log_info(os)("os::init_2 getrlimit failed: %s", os::strerror(errno));
    } else {
      nbr_files.rlim_cur = nbr_files.rlim_max;
      status = setrlimit(RLIMIT_NOFILE, &nbr_files);
      if (status != 0) {
        log_info(os)("os::init_2 setrlimit failed: %s", os::strerror(errno));
      }
    }
  }

  // Initialize lock used to serialize thread creation (see os::create_thread)
  Linux::set_createThread_lock(new Mutex(Mutex::leaf, "createThread_lock", false));

  // at-exit methods are called in the reverse order of their registration.
  // atexit functions are called on return from main or as a result of a
  // call to exit(3C). There can be only 32 of these functions registered
  // and atexit() does not set errno.

  if (PerfAllowAtExitRegistration) {
    // only register atexit functions if PerfAllowAtExitRegistration is set.
    // atexit functions can be delayed until process exit time, which
    // can be problematic for embedded VM situations. Embedded VMs should
    // call DestroyJavaVM() to assure that VM resources are released.

    // note: perfMemory_exit_helper atexit function may be removed in
    // the future if the appropriate cleanup code can be added to the
    // VM_Exit VMOperation's doit method.
    if (atexit(perfMemory_exit_helper) != 0) {
      warning("os::init_2 atexit(perfMemory_exit_helper) failed");
    }
  }

  // initialize thread priority policy
  prio_init();

  return JNI_OK;
}

// Mark the polling page as unreadable
void os::make_polling_page_unreadable(void) {
  if (!guard_memory((char*)_polling_page, Linux::page_size())) {
    fatal("Could not disable polling page");
  }
}

// Mark the polling page as readable
void os::make_polling_page_readable(void) {
  if (!linux_mprotect((char *)_polling_page, Linux::page_size(), PROT_READ)) {
    fatal("Could not enable polling page");
  }
}

// older glibc versions don't have this macro (which expands to
// an optimized bit-counting function) so we have to roll our own
#ifndef CPU_COUNT

static int _cpu_count(const cpu_set_t* cpus) {
  int count = 0;
  // only look up to the number of configured processors
  for (int i = 0; i < os::processor_count(); i++) {
    if (CPU_ISSET(i, cpus)) {
      count++;
    }
  }
  return count;
}

#define CPU_COUNT(cpus) _cpu_count(cpus)

#endif // CPU_COUNT

// Get the current number of available processors for this process.
// This value can change at any time during a process's lifetime.
// sched_getaffinity gives an accurate answer as it accounts for cpusets.
// If it appears there may be more than 1024 processors then we do a
// dynamic check - see 6515172 for details.
// If anything goes wrong we fallback to returning the number of online
// processors - which can be greater than the number available to the process.
int os::Linux::active_processor_count() {
  cpu_set_t cpus;  // can represent at most 1024 (CPU_SETSIZE) processors
  cpu_set_t* cpus_p = &cpus;
  int cpus_size = sizeof(cpu_set_t);

  int configured_cpus = os::processor_count();  // upper bound on available cpus
  int cpu_count = 0;

// old build platforms may not support dynamic cpu sets
#ifdef CPU_ALLOC

  // To enable easy testing of the dynamic path on different platforms we
  // introduce a diagnostic flag: UseCpuAllocPath
  if (configured_cpus >= CPU_SETSIZE || UseCpuAllocPath) {
    // kernel may use a mask bigger than cpu_set_t
    log_trace(os)("active_processor_count: using dynamic path %s"
                  "- configured processors: %d",
                  UseCpuAllocPath ? "(forced) " : "",
                  configured_cpus);
    cpus_p = CPU_ALLOC(configured_cpus);
    if (cpus_p != NULL) {
      cpus_size = CPU_ALLOC_SIZE(configured_cpus);
      // zero it just to be safe
      CPU_ZERO_S(cpus_size, cpus_p);
    }
    else {
       // failed to allocate so fallback to online cpus
       int online_cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
       log_trace(os)("active_processor_count: "
                     "CPU_ALLOC failed (%s) - using "
                     "online processor count: %d",
                     os::strerror(errno), online_cpus);
       return online_cpus;
    }
  }
  else {
    log_trace(os)("active_processor_count: using static path - configured processors: %d",
                  configured_cpus);
  }
#else // CPU_ALLOC
// these stubs won't be executed
#define CPU_COUNT_S(size, cpus) -1
#define CPU_FREE(cpus)

  log_trace(os)("active_processor_count: only static path available - configured processors: %d",
                configured_cpus);
#endif // CPU_ALLOC

  // pid 0 means the current thread - which we have to assume represents the process
  if (sched_getaffinity(0, cpus_size, cpus_p) == 0) {
    if (cpus_p != &cpus) { // can only be true when CPU_ALLOC used
      cpu_count = CPU_COUNT_S(cpus_size, cpus_p);
    }
    else {
      cpu_count = CPU_COUNT(cpus_p);
    }
    log_trace(os)("active_processor_count: sched_getaffinity processor count: %d", cpu_count);
  }
  else {
    cpu_count = ::sysconf(_SC_NPROCESSORS_ONLN);
    warning("sched_getaffinity failed (%s)- using online processor count (%d) "
            "which may exceed available processors", os::strerror(errno), cpu_count);
  }

  if (cpus_p != &cpus) { // can only be true when CPU_ALLOC used
    CPU_FREE(cpus_p);
  }

  assert(cpu_count > 0 && cpu_count <= os::processor_count(), "sanity check");
  return cpu_count;
}

// Determine the active processor count from one of
// three different sources:
//
// 1. User option -XX:ActiveProcessorCount
// 2. kernel os calls (sched_getaffinity or sysconf(_SC_NPROCESSORS_ONLN)
// 3. extracted from cgroup cpu subsystem (shares and quotas)
//
// Option 1, if specified, will always override.
// If the cgroup subsystem is active and configured, we
// will return the min of the cgroup and option 2 results.
// This is required since tools, such as numactl, that
// alter cpu affinity do not update cgroup subsystem
// cpuset configuration files.
int os::active_processor_count() {
  // User has overridden the number of active processors
  if (ActiveProcessorCount > 0) {
    log_trace(os)("active_processor_count: "
                  "active processor count set by user : %d",
                  ActiveProcessorCount);
    return ActiveProcessorCount;
  }

  int active_cpus;
  if (OSContainer::is_containerized()) {
    active_cpus = OSContainer::active_processor_count();
    log_trace(os)("active_processor_count: determined by OSContainer: %d",
                   active_cpus);
  } else {
    active_cpus = os::Linux::active_processor_count();
  }

  return active_cpus;
}

void os::set_native_thread_name(const char *name) {
  if (Linux::_pthread_setname_np) {
    char buf [16]; // according to glibc manpage, 16 chars incl. '/0'
    snprintf(buf, sizeof(buf), "%s", name);
    buf[sizeof(buf) - 1] = '\0';
    const int rc = Linux::_pthread_setname_np(pthread_self(), buf);
    // ERANGE should not happen; all other errors should just be ignored.
    assert(rc != ERANGE, "pthread_setname_np failed");
  }
}

bool os::distribute_processes(uint length, uint* distribution) {
  // Not yet implemented.
  return false;
}

bool os::bind_to_processor(uint processor_id) {
  // Not yet implemented.
  return false;
}

///

void os::SuspendedThreadTask::internal_do_task() {
  if (do_suspend(_thread->osthread())) {
    SuspendedThreadTaskContext context(_thread, _thread->osthread()->ucontext());
    do_task(context);
    do_resume(_thread->osthread());
  }
}

////////////////////////////////////////////////////////////////////////////////
// debug support

bool os::find(address addr, outputStream* st) {
  Dl_info dlinfo;
  memset(&dlinfo, 0, sizeof(dlinfo));
  if (dladdr(addr, &dlinfo) != 0) {
    st->print(PTR_FORMAT ": ", p2i(addr));
    if (dlinfo.dli_sname != NULL && dlinfo.dli_saddr != NULL) {
      st->print("%s+" PTR_FORMAT, dlinfo.dli_sname,
                p2i(addr) - p2i(dlinfo.dli_saddr));
    } else if (dlinfo.dli_fbase != NULL) {
      st->print("<offset " PTR_FORMAT ">", p2i(addr) - p2i(dlinfo.dli_fbase));
    } else {
      st->print("<absolute address>");
    }
    if (dlinfo.dli_fname != NULL) {
      st->print(" in %s", dlinfo.dli_fname);
    }
    if (dlinfo.dli_fbase != NULL) {
      st->print(" at " PTR_FORMAT, p2i(dlinfo.dli_fbase));
    }
    st->cr();

    if (Verbose) {
      // decode some bytes around the PC
      address begin = clamp_address_in_page(addr-40, addr, os::vm_page_size());
      address end   = clamp_address_in_page(addr+40, addr, os::vm_page_size());
      address       lowest = (address) dlinfo.dli_sname;
      if (!lowest)  lowest = (address) dlinfo.dli_fbase;
      if (begin < lowest)  begin = lowest;
      Dl_info dlinfo2;
      if (dladdr(end, &dlinfo2) != 0 && dlinfo2.dli_saddr != dlinfo.dli_saddr
          && end > dlinfo2.dli_saddr && dlinfo2.dli_saddr > begin) {
        end = (address) dlinfo2.dli_saddr;
      }
      Disassembler::decode(begin, end, st);
    }
    return true;
  }
  return false;
}

////////////////////////////////////////////////////////////////////////////////
// misc

// This does not do anything on Linux. This is basically a hook for being
// able to use structured exception handling (thread-local exception filters)
// on, e.g., Win32.
void
os::os_exception_wrapper(java_call_t f, JavaValue* value, const methodHandle& method,
                         JavaCallArguments* args, Thread* thread) {
  f(value, method, args, thread);
}

void os::print_statistics() {
}

bool os::message_box(const char* title, const char* message) {
  int i;
  fdStream err(defaultStream::error_fd());
  for (i = 0; i < 78; i++) err.print_raw("=");
  err.cr();
  err.print_raw_cr(title);
  for (i = 0; i < 78; i++) err.print_raw("-");
  err.cr();
  err.print_raw_cr(message);
  for (i = 0; i < 78; i++) err.print_raw("=");
  err.cr();

  char buf[16];
  // Prevent process from exiting upon "read error" without consuming all CPU
  while (::read(0, buf, sizeof(buf)) <= 0) { ::sleep(100); }

  return buf[0] == 'y' || buf[0] == 'Y';
}

int os::stat(const char *path, struct stat *sbuf) {
  char pathbuf[MAX_PATH];
  if (strlen(path) > MAX_PATH - 1) {
    errno = ENAMETOOLONG;
    return -1;
  }
  os::native_path(strcpy(pathbuf, path));
  return ::stat(pathbuf, sbuf);
}

// Is a (classpath) directory empty?
bool os::dir_is_empty(const char* path) {
  DIR *dir = NULL;
  struct dirent *ptr;

  dir = opendir(path);
  if (dir == NULL) return true;

  // Scan the directory
  bool result = true;
  char buf[sizeof(struct dirent) + MAX_PATH];
  while (result && (ptr = ::readdir(dir)) != NULL) {
    if (strcmp(ptr->d_name, ".") != 0 && strcmp(ptr->d_name, "..") != 0) {
      result = false;
    }
  }
  closedir(dir);
  return result;
}

// This code originates from JDK's sysOpen and open64_w
// from src/solaris/hpi/src/system_md.c

int os::open(const char *path, int oflag, int mode) {
  if (strlen(path) > MAX_PATH - 1) {
    errno = ENAMETOOLONG;
    return -1;
  }

  // All file descriptors that are opened in the Java process and not
  // specifically destined for a subprocess should have the close-on-exec
  // flag set.  If we don't set it, then careless 3rd party native code
  // might fork and exec without closing all appropriate file descriptors
  // (e.g. as we do in closeDescriptors in UNIXProcess.c), and this in
  // turn might:
  //
  // - cause end-of-file to fail to be detected on some file
  //   descriptors, resulting in mysterious hangs, or
  //
  // - might cause an fopen in the subprocess to fail on a system
  //   suffering from bug 1085341.
  //
  // (Yes, the default setting of the close-on-exec flag is a Unix
  // design flaw)
  //
  // See:
  // 1085341: 32-bit stdio routines should support file descriptors >255
  // 4843136: (process) pipe file descriptor from Runtime.exec not being closed
  // 6339493: (process) Runtime.exec does not close all file descriptors on Solaris 9
  //
  // Modern Linux kernels (after 2.6.23 2007) support O_CLOEXEC with open().
  // O_CLOEXEC is preferable to using FD_CLOEXEC on an open file descriptor
  // because it saves a system call and removes a small window where the flag
  // is unset.  On ancient Linux kernels the O_CLOEXEC flag will be ignored
  // and we fall back to using FD_CLOEXEC (see below).
#ifdef O_CLOEXEC
  oflag |= O_CLOEXEC;
#endif

  int fd = ::open64(path, oflag, mode);
  if (fd == -1) return -1;

  //If the open succeeded, the file might still be a directory
  {
    struct stat64 buf64;
    int ret = ::fstat64(fd, &buf64);
    int st_mode = buf64.st_mode;

    if (ret != -1) {
      if ((st_mode & S_IFMT) == S_IFDIR) {
        errno = EISDIR;
        ::close(fd);
        return -1;
      }
    } else {
      ::close(fd);
      return -1;
    }
  }

#ifdef FD_CLOEXEC
  // Validate that the use of the O_CLOEXEC flag on open above worked.
  // With recent kernels, we will perform this check exactly once.
  static sig_atomic_t O_CLOEXEC_is_known_to_work = 0;
  if (!O_CLOEXEC_is_known_to_work) {
    int flags = ::fcntl(fd, F_GETFD);
    if (flags != -1) {
      if ((flags & FD_CLOEXEC) != 0)
        O_CLOEXEC_is_known_to_work = 1;
      else
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
  }
#endif

  return fd;
}


// create binary file, rewriting existing file if required
int os::create_binary_file(const char* path, bool rewrite_existing) {
  int oflags = O_WRONLY | O_CREAT;
  if (!rewrite_existing) {
    oflags |= O_EXCL;
  }
  return ::open64(path, oflags, S_IREAD | S_IWRITE);
}

// return current position of file pointer
jlong os::current_file_offset(int fd) {
  return (jlong)::lseek64(fd, (off64_t)0, SEEK_CUR);
}

// move file pointer to the specified offset
jlong os::seek_to_file_offset(int fd, jlong offset) {
  return (jlong)::lseek64(fd, (off64_t)offset, SEEK_SET);
}

// This code originates from JDK's sysAvailable
// from src/solaris/hpi/src/native_threads/src/sys_api_td.c

int os::available(int fd, jlong *bytes) {
  jlong cur, end;
  int mode;
  struct stat64 buf64;

  if (::fstat64(fd, &buf64) >= 0) {
    mode = buf64.st_mode;
    if (S_ISCHR(mode) || S_ISFIFO(mode) || S_ISSOCK(mode)) {
      int n;
      if (::ioctl(fd, FIONREAD, &n) >= 0) {
        *bytes = n;
        return 1;
      }
    }
  }
  if ((cur = ::lseek64(fd, 0L, SEEK_CUR)) == -1) {
    return 0;
  } else if ((end = ::lseek64(fd, 0L, SEEK_END)) == -1) {
    return 0;
  } else if (::lseek64(fd, cur, SEEK_SET) == -1) {
    return 0;
  }
  *bytes = end - cur;
  return 1;
}

// Map a block of memory.
char* os::pd_map_memory(int fd, const char* file_name, size_t file_offset,
                        char *addr, size_t bytes, bool read_only,
                        bool allow_exec) {
  int prot;
  int flags = MAP_PRIVATE;

  if (read_only) {
    prot = PROT_READ;
  } else {
    prot = PROT_READ | PROT_WRITE;
  }

  if (allow_exec) {
    prot |= PROT_EXEC;
  }

  if (addr != NULL) {
    flags |= MAP_FIXED;
  }

  char* mapped_address = (char*)mmap(addr, (size_t)bytes, prot, flags,
                                     fd, file_offset);
  if (mapped_address == MAP_FAILED) {
    return NULL;
  }
  return mapped_address;
}


// Remap a block of memory.
char* os::pd_remap_memory(int fd, const char* file_name, size_t file_offset,
                          char *addr, size_t bytes, bool read_only,
                          bool allow_exec) {
  // same as map_memory() on this OS
  return os::map_memory(fd, file_name, file_offset, addr, bytes, read_only,
                        allow_exec);
}


// Unmap a block of memory.
bool os::pd_unmap_memory(char* addr, size_t bytes) {
  return munmap(addr, bytes) == 0;
}

static jlong slow_thread_cpu_time(Thread *thread, bool user_sys_cpu_time);

static clockid_t thread_cpu_clockid(Thread* thread) {
  pthread_t tid = thread->osthread()->pthread_id();
  clockid_t clockid;

  // Get thread clockid
  int rc = os::Linux::pthread_getcpuclockid(tid, &clockid);
  assert(rc == 0, "pthread_getcpuclockid is expected to return 0 code");
  return clockid;
}

// current_thread_cpu_time(bool) and thread_cpu_time(Thread*, bool)
// are used by JVM M&M and JVMTI to get user+sys or user CPU time
// of a thread.
//
// current_thread_cpu_time() and thread_cpu_time(Thread*) returns
// the fast estimate available on the platform.

jlong os::current_thread_cpu_time() {
  if (os::Linux::supports_fast_thread_cpu_time()) {
    return os::Linux::fast_thread_cpu_time(CLOCK_THREAD_CPUTIME_ID);
  } else {
    // return user + sys since the cost is the same
    return slow_thread_cpu_time(Thread::current(), true /* user + sys */);
  }
}

jlong os::thread_cpu_time(Thread* thread) {
  // consistent with what current_thread_cpu_time() returns
  if (os::Linux::supports_fast_thread_cpu_time()) {
    return os::Linux::fast_thread_cpu_time(thread_cpu_clockid(thread));
  } else {
    return slow_thread_cpu_time(thread, true /* user + sys */);
  }
}

jlong os::current_thread_cpu_time(bool user_sys_cpu_time) {
  if (user_sys_cpu_time && os::Linux::supports_fast_thread_cpu_time()) {
    return os::Linux::fast_thread_cpu_time(CLOCK_THREAD_CPUTIME_ID);
  } else {
    return slow_thread_cpu_time(Thread::current(), user_sys_cpu_time);
  }
}

jlong os::thread_cpu_time(Thread *thread, bool user_sys_cpu_time) {
  if (user_sys_cpu_time && os::Linux::supports_fast_thread_cpu_time()) {
    return os::Linux::fast_thread_cpu_time(thread_cpu_clockid(thread));
  } else {
    return slow_thread_cpu_time(thread, user_sys_cpu_time);
  }
}

//  -1 on error.
static jlong slow_thread_cpu_time(Thread *thread, bool user_sys_cpu_time) {
  pid_t  tid = thread->osthread()->thread_id();
  char *s;
  char stat[2048];
  int statlen;
  char proc_name[64];
  int count;
  long sys_time, user_time;
  char cdummy;
  int idummy;
  long ldummy;
  FILE *fp;

  snprintf(proc_name, 64, "/proc/self/task/%d/stat", tid);
  fp = fopen(proc_name, "r");
  if (fp == NULL) return -1;
  statlen = fread(stat, 1, 2047, fp);
  stat[statlen] = '\0';
  fclose(fp);

  // Skip pid and the command string. Note that we could be dealing with
  // weird command names, e.g. user could decide to rename java launcher
  // to "java 1.4.2 :)", then the stat file would look like
  //                1234 (java 1.4.2 :)) R ... ...
  // We don't really need to know the command string, just find the last
  // occurrence of ")" and then start parsing from there. See bug 4726580.
  s = strrchr(stat, ')');
  if (s == NULL) return -1;

  // Skip blank chars
  do { s++; } while (s && isspace(*s));

  count = sscanf(s,"%c %d %d %d %d %d %lu %lu %lu %lu %lu %lu %lu",
                 &cdummy, &idummy, &idummy, &idummy, &idummy, &idummy,
                 &ldummy, &ldummy, &ldummy, &ldummy, &ldummy,
                 &user_time, &sys_time);
  if (count != 13) return -1;
  if (user_sys_cpu_time) {
    return ((jlong)sys_time + (jlong)user_time) * (1000000000 / clock_tics_per_sec);
  } else {
    return (jlong)user_time * (1000000000 / clock_tics_per_sec);
  }
}

void os::current_thread_cpu_time_info(jvmtiTimerInfo *info_ptr) {
  info_ptr->max_value = ALL_64_BITS;       // will not wrap in less than 64 bits
  info_ptr->may_skip_backward = false;     // elapsed time not wall time
  info_ptr->may_skip_forward = false;      // elapsed time not wall time
  info_ptr->kind = JVMTI_TIMER_TOTAL_CPU;  // user+system time is returned
}

void os::thread_cpu_time_info(jvmtiTimerInfo *info_ptr) {
  info_ptr->max_value = ALL_64_BITS;       // will not wrap in less than 64 bits
  info_ptr->may_skip_backward = false;     // elapsed time not wall time
  info_ptr->may_skip_forward = false;      // elapsed time not wall time
  info_ptr->kind = JVMTI_TIMER_TOTAL_CPU;  // user+system time is returned
}

bool os::is_thread_cpu_time_supported() {
  return true;
}

// System loadavg support.  Returns -1 if load average cannot be obtained.
// Linux doesn't yet have a (official) notion of processor sets,
// so just return the system wide load average.
int os::loadavg(double loadavg[], int nelem) {
  return ::getloadavg(loadavg, nelem);
}

void os::pause() {
  char filename[MAX_PATH];
  if (PauseAtStartupFile && PauseAtStartupFile[0]) {
    jio_snprintf(filename, MAX_PATH, "%s", PauseAtStartupFile);
  } else {
    jio_snprintf(filename, MAX_PATH, "./vm.paused.%d", current_process_id());
  }

  int fd = ::open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd != -1) {
    struct stat buf;
    ::close(fd);
    while (::stat(filename, &buf) == 0) {
      (void)::poll(NULL, 0, 100);
    }
  } else {
    jio_fprintf(stderr,
                "Could not open pause file '%s', continuing immediately.\n", filename);
  }
}

extern char** environ;

// Run the specified command in a separate process. Return its exit value,
// or -1 on failure (e.g. can't fork a new process).
// Unlike system(), this function can be called from signal handler. It
// doesn't block SIGINT et al.
int os::fork_and_exec(char* cmd) {
  const char * argv[4] = {"sh", "-c", cmd, NULL};

  pid_t pid = fork();

  if (pid < 0) {
    // fork failed
    return -1;

  } else if (pid == 0) {
    // child process

    execve("/bin/sh", (char* const*)argv, environ);

    // execve failed
    _exit(-1);

  } else  {
    // copied from J2SE ..._waitForProcessExit() in UNIXProcess_md.c; we don't
    // care about the actual exit code, for now.

    int status;

    // Wait for the child process to exit.  This returns immediately if
    // the child has already exited. */
    while (waitpid(pid, &status, 0) < 0) {
      switch (errno) {
      case ECHILD: return 0;
      case EINTR: break;
      default: return -1;
      }
    }

    if (WIFEXITED(status)) {
      // The child exited normally; get its exit code.
      return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      // The child exited because of a signal
      // The best value to return is 0x80 + signal number,
      // because that is what all Unix shells do, and because
      // it allows callers to distinguish between process exit and
      // process death by signal.
      return 0x80 + WTERMSIG(status);
    } else {
      // Unknown exit code; pass it through
      return status;
    }
  }
}

// is_headless_jre()
//
// Test for the existence of xawt/libmawt.so or libawt_xawt.so
// in order to report if we are running in a headless jre
//
// Since JDK8 xawt/libmawt.so was moved into the same directory
// as libawt.so, and renamed libawt_xawt.so
//
bool os::is_headless_jre() {
  struct stat statbuf;
  char buf[MAXPATHLEN];
  char libmawtpath[MAXPATHLEN];
  const char *xawtstr  = "/xawt/libmawt.so";
  const char *new_xawtstr = "/libawt_xawt.so";
  char *p;

  // Get path to libjvm.so
  os::jvm_path(buf, sizeof(buf));

  // Get rid of libjvm.so
  p = strrchr(buf, '/');
  if (p == NULL) {
    return false;
  } else {
    *p = '\0';
  }

  // Get rid of client or server
  p = strrchr(buf, '/');
  if (p == NULL) {
    return false;
  } else {
    *p = '\0';
  }

  // check xawt/libmawt.so
  strcpy(libmawtpath, buf);
  strcat(libmawtpath, xawtstr);
  if (::stat(libmawtpath, &statbuf) == 0) return false;

  // check libawt_xawt.so
  strcpy(libmawtpath, buf);
  strcat(libmawtpath, new_xawtstr);
  if (::stat(libmawtpath, &statbuf) == 0) return false;

  return true;
}

// Get the default path to the core file
// Returns the length of the string
int os::get_core_path(char* buffer, size_t bufferSize) {
  /*
   * Max length of /proc/sys/kernel/core_pattern is 128 characters.
   * See https://www.kernel.org/doc/Documentation/sysctl/kernel.txt
   */
  const int core_pattern_len = 129;
  char core_pattern[core_pattern_len] = {0};

  int core_pattern_file = ::open("/proc/sys/kernel/core_pattern", O_RDONLY);
  if (core_pattern_file == -1) {
    return -1;
  }

  ssize_t ret = ::read(core_pattern_file, core_pattern, core_pattern_len);
  ::close(core_pattern_file);
  if (ret <= 0 || ret >= core_pattern_len || core_pattern[0] == '\n') {
    return -1;
  }
  if (core_pattern[ret-1] == '\n') {
    core_pattern[ret-1] = '\0';
  } else {
    core_pattern[ret] = '\0';
  }

  char *pid_pos = strstr(core_pattern, "%p");
  int written;

  if (core_pattern[0] == '/') {
    written = jio_snprintf(buffer, bufferSize, "%s", core_pattern);
  } else {
    char cwd[PATH_MAX];

    const char* p = get_current_directory(cwd, PATH_MAX);
    if (p == NULL) {
      return -1;
    }

    if (core_pattern[0] == '|') {
      written = jio_snprintf(buffer, bufferSize,
                             "\"%s\" (or dumping to %s/core.%d)",
                             &core_pattern[1], p, current_process_id());
    } else {
      written = jio_snprintf(buffer, bufferSize, "%s/%s", p, core_pattern);
    }
  }

  if (written < 0) {
    return -1;
  }

  if (((size_t)written < bufferSize) && (pid_pos == NULL) && (core_pattern[0] != '|')) {
    int core_uses_pid_file = ::open("/proc/sys/kernel/core_uses_pid", O_RDONLY);

    if (core_uses_pid_file != -1) {
      char core_uses_pid = 0;
      ssize_t ret = ::read(core_uses_pid_file, &core_uses_pid, 1);
      ::close(core_uses_pid_file);

      if (core_uses_pid == '1') {
        jio_snprintf(buffer + written, bufferSize - written,
                                          ".%d", current_process_id());
      }
    }
  }

  return strlen(buffer);
}

bool os::start_debugging(char *buf, int buflen) {
  int len = (int)strlen(buf);
  char *p = &buf[len];

  jio_snprintf(p, buflen-len,
               "\n\n"
               "Do you want to debug the problem?\n\n"
               "To debug, run 'gdb /proc/%d/exe %d'; then switch to thread " UINTX_FORMAT " (" INTPTR_FORMAT ")\n"
               "Enter 'yes' to launch gdb automatically (PATH must include gdb)\n"
               "Otherwise, press RETURN to abort...",
               os::current_process_id(), os::current_process_id(),
               os::current_thread_id(), os::current_thread_id());

  bool yes = os::message_box("Unexpected Error", buf);

  if (yes) {
    // yes, user asked VM to launch debugger
    jio_snprintf(buf, sizeof(char)*buflen, "gdb /proc/%d/exe %d",
                 os::current_process_id(), os::current_process_id());

    os::fork_and_exec(buf);
    yes = false;
  }
  return yes;
}


// Java/Compiler thread:
//
//   Low memory addresses
// P0 +------------------------+
//    |                        |\  Java thread created by VM does not have glibc
//    |    glibc guard page    | - guard page, attached Java thread usually has
//    |                        |/  1 glibc guard page.
// P1 +------------------------+ Thread::stack_base() - Thread::stack_size()
//    |                        |\
//    |  HotSpot Guard Pages   | - red, yellow and reserved pages
//    |                        |/
//    +------------------------+ JavaThread::stack_reserved_zone_base()
//    |                        |\
//    |      Normal Stack      | -
//    |                        |/
// P2 +------------------------+ Thread::stack_base()
//
// Non-Java thread:
//
//   Low memory addresses
// P0 +------------------------+
//    |                        |\
//    |  glibc guard page      | - usually 1 page
//    |                        |/
// P1 +------------------------+ Thread::stack_base() - Thread::stack_size()
//    |                        |\
//    |      Normal Stack      | -
//    |                        |/
// P2 +------------------------+ Thread::stack_base()
//
// ** P1 (aka bottom) and size (P2 = P1 - size) are the address and stack size
//    returned from pthread_attr_getstack().
// ** Due to NPTL implementation error, linux takes the glibc guard page out
//    of the stack size given in pthread_attr. We work around this for
//    threads created by the VM. (We adapt bottom to be P1 and size accordingly.)
//
#ifndef ZERO
static void current_stack_region(address * bottom, size_t * size) {
  if (os::is_primordial_thread()) {
    // primordial thread needs special handling because pthread_getattr_np()
    // may return bogus value.
    *bottom = os::Linux::initial_thread_stack_bottom();
    *size   = os::Linux::initial_thread_stack_size();
  } else {
    pthread_attr_t attr;

    int rslt = pthread_getattr_np(pthread_self(), &attr);

    // JVM needs to know exact stack location, abort if it fails
    if (rslt != 0) {
      if (rslt == ENOMEM) {
        vm_exit_out_of_memory(0, OOM_MMAP_ERROR, "pthread_getattr_np");
      } else {
        fatal("pthread_getattr_np failed with error = %d", rslt);
      }
    }

    if (pthread_attr_getstack(&attr, (void **)bottom, size) != 0) {
      fatal("Cannot locate current stack attributes!");
    }

    // Work around NPTL stack guard error.
    size_t guard_size = 0;
    rslt = pthread_attr_getguardsize(&attr, &guard_size);
    if (rslt != 0) {
      fatal("pthread_attr_getguardsize failed with error = %d", rslt);
    }
    *bottom += guard_size;
    *size   -= guard_size;

    pthread_attr_destroy(&attr);

  }
  assert(os::current_stack_pointer() >= *bottom &&
         os::current_stack_pointer() < *bottom + *size, "just checking");
}

address os::current_stack_base() {
  address bottom;
  size_t size;
  current_stack_region(&bottom, &size);
  return (bottom + size);
}

size_t os::current_stack_size() {
  // This stack size includes the usable stack and HotSpot guard pages
  // (for the threads that have Hotspot guard pages).
  address bottom;
  size_t size;
  current_stack_region(&bottom, &size);
  return size;
}
#endif

static inline struct timespec get_mtime(const char* filename) {
  struct stat st;
  int ret = os::stat(filename, &st);
  assert(ret == 0, "failed to stat() file '%s': %s", filename, strerror(errno));
  return st.st_mtim;
}

int os::compare_file_modified_times(const char* file1, const char* file2) {
  struct timespec filetime1 = get_mtime(file1);
  struct timespec filetime2 = get_mtime(file2);
  int diff = filetime1.tv_sec - filetime2.tv_sec;
  if (diff == 0) {
    return filetime1.tv_nsec - filetime2.tv_nsec;
  }
  return diff;
}

/////////////// Unit tests ///////////////

#ifndef PRODUCT

#define test_log(...)              \
  do {                             \
    if (VerboseInternalVMTests) {  \
      tty->print_cr(__VA_ARGS__);  \
      tty->flush();                \
    }                              \
  } while (false)

class TestReserveMemorySpecial : AllStatic {
 public:
  static void small_page_write(void* addr, size_t size) {
    size_t page_size = os::vm_page_size();

    char* end = (char*)addr + size;
    for (char* p = (char*)addr; p < end; p += page_size) {
      *p = 1;
    }
  }

  static void test_reserve_memory_special_huge_tlbfs_only(size_t size) {
    if (!UseHugeTLBFS) {
      return;
    }

    test_log("test_reserve_memory_special_huge_tlbfs_only(" SIZE_FORMAT ")", size);

    char* addr = os::Linux::reserve_memory_special_huge_tlbfs_only(size, NULL, false);

    if (addr != NULL) {
      small_page_write(addr, size);

      os::Linux::release_memory_special_huge_tlbfs(addr, size);
    }
  }

  static void test_reserve_memory_special_huge_tlbfs_only() {
    if (!UseHugeTLBFS) {
      return;
    }

    size_t lp = os::large_page_size();

    for (size_t size = lp; size <= lp * 10; size += lp) {
      test_reserve_memory_special_huge_tlbfs_only(size);
    }
  }

  static void test_reserve_memory_special_huge_tlbfs_mixed() {
    size_t lp = os::large_page_size();
    size_t ag = os::vm_allocation_granularity();

    // sizes to test
    const size_t sizes[] = {
      lp, lp + ag, lp + lp / 2, lp * 2,
      lp * 2 + ag, lp * 2 - ag, lp * 2 + lp / 2,
      lp * 10, lp * 10 + lp / 2
    };
    const int num_sizes = sizeof(sizes) / sizeof(size_t);

    // For each size/alignment combination, we test three scenarios:
    // 1) with req_addr == NULL
    // 2) with a non-null req_addr at which we expect to successfully allocate
    // 3) with a non-null req_addr which contains a pre-existing mapping, at which we
    //    expect the allocation to either fail or to ignore req_addr

    // Pre-allocate two areas; they shall be as large as the largest allocation
    //  and aligned to the largest alignment we will be testing.
    const size_t mapping_size = sizes[num_sizes - 1] * 2;
    char* const mapping1 = (char*) ::mmap(NULL, mapping_size,
      PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,
      -1, 0);
    assert(mapping1 != MAP_FAILED, "should work");

    char* const mapping2 = (char*) ::mmap(NULL, mapping_size,
      PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,
      -1, 0);
    assert(mapping2 != MAP_FAILED, "should work");

    // Unmap the first mapping, but leave the second mapping intact: the first
    // mapping will serve as a value for a "good" req_addr (case 2). The second
    // mapping, still intact, as "bad" req_addr (case 3).
    ::munmap(mapping1, mapping_size);

    // Case 1
    test_log("%s, req_addr NULL:", __FUNCTION__);
    test_log("size            align           result");

    for (int i = 0; i < num_sizes; i++) {
      const size_t size = sizes[i];
      for (size_t alignment = ag; is_aligned(size, alignment); alignment *= 2) {
        char* p = os::Linux::reserve_memory_special_huge_tlbfs_mixed(size, alignment, NULL, false);
        test_log(SIZE_FORMAT_HEX " " SIZE_FORMAT_HEX " ->  " PTR_FORMAT " %s",
                 size, alignment, p2i(p), (p != NULL ? "" : "(failed)"));
        if (p != NULL) {
          assert(is_aligned(p, alignment), "must be");
          small_page_write(p, size);
          os::Linux::release_memory_special_huge_tlbfs(p, size);
        }
      }
    }

    // Case 2
    test_log("%s, req_addr non-NULL:", __FUNCTION__);
    test_log("size            align           req_addr         result");

    for (int i = 0; i < num_sizes; i++) {
      const size_t size = sizes[i];
      for (size_t alignment = ag; is_aligned(size, alignment); alignment *= 2) {
        char* const req_addr = align_up(mapping1, alignment);
        char* p = os::Linux::reserve_memory_special_huge_tlbfs_mixed(size, alignment, req_addr, false);
        test_log(SIZE_FORMAT_HEX " " SIZE_FORMAT_HEX " " PTR_FORMAT " ->  " PTR_FORMAT " %s",
                 size, alignment, p2i(req_addr), p2i(p),
                 ((p != NULL ? (p == req_addr ? "(exact match)" : "") : "(failed)")));
        if (p != NULL) {
          assert(p == req_addr, "must be");
          small_page_write(p, size);
          os::Linux::release_memory_special_huge_tlbfs(p, size);
        }
      }
    }

    // Case 3
    test_log("%s, req_addr non-NULL with preexisting mapping:", __FUNCTION__);
    test_log("size            align           req_addr         result");

    for (int i = 0; i < num_sizes; i++) {
      const size_t size = sizes[i];
      for (size_t alignment = ag; is_aligned(size, alignment); alignment *= 2) {
        char* const req_addr = align_up(mapping2, alignment);
        char* p = os::Linux::reserve_memory_special_huge_tlbfs_mixed(size, alignment, req_addr, false);
        test_log(SIZE_FORMAT_HEX " " SIZE_FORMAT_HEX " " PTR_FORMAT " ->  " PTR_FORMAT " %s",
                 size, alignment, p2i(req_addr), p2i(p), ((p != NULL ? "" : "(failed)")));
        // as the area around req_addr contains already existing mappings, the API should always
        // return NULL (as per contract, it cannot return another address)
        assert(p == NULL, "must be");
      }
    }

    ::munmap(mapping2, mapping_size);

  }

  static void test_reserve_memory_special_huge_tlbfs() {
    if (!UseHugeTLBFS) {
      return;
    }

    test_reserve_memory_special_huge_tlbfs_only();
    test_reserve_memory_special_huge_tlbfs_mixed();
  }

  static void test_reserve_memory_special_shm(size_t size, size_t alignment) {
    if (!UseSHM) {
      return;
    }

    test_log("test_reserve_memory_special_shm(" SIZE_FORMAT ", " SIZE_FORMAT ")", size, alignment);

    char* addr = os::Linux::reserve_memory_special_shm(size, alignment, NULL, false);

    if (addr != NULL) {
      assert(is_aligned(addr, alignment), "Check");
      assert(is_aligned(addr, os::large_page_size()), "Check");

      small_page_write(addr, size);

      os::Linux::release_memory_special_shm(addr, size);
    }
  }

  static void test_reserve_memory_special_shm() {
    size_t lp = os::large_page_size();
    size_t ag = os::vm_allocation_granularity();

    for (size_t size = ag; size < lp * 3; size += ag) {
      for (size_t alignment = ag; is_aligned(size, alignment); alignment *= 2) {
        test_reserve_memory_special_shm(size, alignment);
      }
    }
  }

  static void test() {
    test_reserve_memory_special_huge_tlbfs();
    test_reserve_memory_special_shm();
  }
};

void TestReserveMemorySpecial_test() {
  TestReserveMemorySpecial::test();
}

#endif
