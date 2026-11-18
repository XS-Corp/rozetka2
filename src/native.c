#include "internal.h"

#include <ctype.h>
#include <complex.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ffi/ffi.h>
#include <fenv.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <math.h>
#include <pthread.h>
#include <poll.h>
#include <netdb.h>
#include <libproc.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>
#include <zlib.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>


static double r2_host_exp10(double value)
{
    return pow(10.0, value);
}

#define NATIVE_CALL_BASE UINT64_C(0x7f000000)
#define NATIVE_MAX_IMPORTS 4096u
#define NATIVE_MAX_CODE_RANGES (NATIVE_MAX_IMPORTS + 1024u)
#define NATIVE_HASH_SIZE 262144u
#define NATIVE_MAX_ARGS 32u
#define NATIVE_MAX_MAPPINGS 1024u

typedef struct {
    const char *name;
    const char *arguments;
    char result;
    int format; /* -1: fixed, otherwise printf format argument index */
} native_definition;
typedef struct { native_definition definition; uint32_t callbacks, chains; } r2_graphics_abi;
#include "graphics_abi.inc"
enum { GRAPHICS_SDL3 = 1, GRAPHICS_VULKAN = 2, GRAPHICS_OPENGL = 3, GRAPHICS_MODULE_COUNT = 3 };
typedef struct { void *library; char *path; unsigned references; bool global; int kind; } graphics_module;

/* Only declared ABI signatures may cross this boundary.
 * Guest function pointers are never cast to executable host functions. */
static const native_definition definitions[] = {
#define D(name, args, result) {name, args, result, -1}
#define V(name, args, index) {name, args, 'i', index}
#include "platform_abi.inc"
    D("adler32", "zpi", 'z'), D("crc32", "zpi", 'z'),
    D("inflateInit2_", "pipi", 'i'), D("inflate", "pi", 'i'),
    D("inflateEnd", "p", 'i'), D("inflateReset", "p", 'i'), D("inflateSetDictionary", "ppi", 'i'),
    D("deflateInit2_", "piiiiipi", 'i'), D("deflate", "pi", 'i'),
    D("deflateEnd", "p", 'i'), D("deflateReset", "p", 'i'), D("deflateParams", "pii", 'i'),
    D("deflateBound", "pz", 'z'), D("deflateSetDictionary", "ppi", 'i'), D("deflateSetHeader", "pp", 'i'),
    D("reallocf", "pz", 'p'),
    D("posix_memalign", "pzz", 'i'), D("mkstemp", "p", 'i'), D("mkostemp", "pi", 'i'),
    D("acos", "d", 'd'), D("asin", "d", 'd'), D("exp2", "d", 'd'),
    D("fseeko", "pli", 'i'), D("ftello", "p", 'l'), D("setbuf", "pp", 'v'),
    D("wmemchr", "piz", 'p'), D("ungetc", "ip", 'i'),
    D("posix_spawn", "pppppp", 'i'),
    D("host_page_size", "ip", 'i'),
    D("getfsstat$INODE64", "pii", 'i'), D("getfsstat", "pii", 'i'),
    D("mach_task_self", "", 'i'),
    D("IOMasterPort", "ip", 'i'), D("IOMainPort", "ip", 'i'),
    D("IOServiceMatching", "p", 'p'), D("IOServiceNameMatching", "p", 'p'),
    D("IOServiceGetMatchingService", "ip", 'i'), D("IOServiceGetMatchingServices", "ipp", 'i'),
    D("IOIteratorNext", "i", 'i'), D("IOObjectRelease", "i", 'i'), D("IOObjectRetain", "i", 'i'),
    D("IOObjectConformsTo", "ip", 'i'), D("IORegistryEntryFromPath", "ip", 'i'),
    D("IORegistryEntryGetChildIterator", "ipp", 'i'), D("IORegistryEntryGetParentIterator", "ipp", 'i'),
    D("IORegistryEntryGetChildEntry", "ipp", 'i'), D("IORegistryEntryGetParentEntry", "ipp", 'i'),
    D("IORegistryEntryCreateCFProperty", "ippi", 'p'), D("IORegistryEntrySearchCFProperty", "ipppi", 'p'),
    D("IORegistryEntryCreateCFProperties", "ippi", 'i'), D("IORegistryEntryGetRegistryEntryID", "ip", 'i'),
    D("IORegistryEntryGetName", "ip", 'i'), D("IORegistryEntryGetPath", "ipp", 'i'),
    D("CFGetTypeID", "p", 'z'), D("CFDataGetTypeID", "", 'z'), D("CFStringGetTypeID", "", 'z'),
    D("CFNumberGetTypeID", "", 'z'), D("CFArrayGetTypeID", "", 'z'), D("CFBooleanGetTypeID", "", 'z'),
    D("CFDateGetTypeID", "", 'z'), D("CFDictionaryGetTypeID", "", 'z'),
    D("CFAllocatorGetDefault", "", 'p'),
    D("CFNumberCreate", "plp", 'p'), D("CFArrayCreate", "pplp", 'p'),
    D("CFCopyDescription", "p", 'p'), D("CFGetRetainCount", "p", 'l'),
    D("CFDictionaryGetValueIfPresent", "ppp", 'b'), D("CFDictionarySetValue", "ppp", 'v'),
    D("CFNumberGetType", "p", 'l'), D("CFEqual", "pp", 'b'),
    D("host_statistics64", "iipp", 'i'),
    D("CFStringCreateWithCharacters", "ppl", 'p'),
    D("CFStringGetCharactersPtr", "p", 'p'),
    D("CFBundleGetMainBundle", "", 'p'), D("CFBundleCopyResourceURL", "pppp", 'p'),
    D("CFURLGetFileSystemRepresentation", "pbpl", 'b'),
    D("acosf", "f", 'f'), D("asinf", "f", 'f'), D("atan2f", "ff", 'f'),
    D("cbrtf", "f", 'f'), D("exp2f", "f", 'f'), D("logf", "f", 'f'), D("sinhf", "f", 'f'),
    D("__exp10f", "f", 'f'), D("__exp10", "d", 'd'), D("lround", "d", 'l'), D("__swbuf", "ip", 'i'),
    D("sched_get_priority_max", "i", 'i'), D("sched_get_priority_min", "i", 'i'),
    D("AudioComponentFindNext", "pp", 'p'), D("AudioComponentInstanceNew", "pp", 'i'),
    D("AudioComponentInstanceDispose", "p", 'i'),
    D("AudioObjectAddPropertyListener", "ippp", 'i'), D("AudioObjectRemovePropertyListener", "ippp", 'i'),
    D("AudioUnitSetProperty", "piiipi", 'i'),
    D("__ulock_wait", "ipzi", 'i'), D("__ulock_wake", "ipz", 'i'),
    D("_tlv_atexit", "pp", 'v'),
    D("pthread_kill", "pi", 'i'), D("sigprocmask", "ipp", 'i'), D("sigpending", "p", 'i'),
    D("sigsetjmp", "pi", 'i'), D("siglongjmp", "pi", 'v'), D("sigsuspend", "p", 'i'),
    D("__r2_suspend_complete", "", 'i'),
    D("__divti3", "zzzz", 'z'), D("__udivti3", "zzzz", 'z'), D("__muldc3", "dddd", 'd'),
    D("NSSetUncaughtExceptionHandler", "p", 'v'), D("NSGetUncaughtExceptionHandler", "", 'p'),
    D("NSStringFromSelector", "p", 'p'), D("NSSelectorFromString", "p", 'p'),
    D("NSStringFromClass", "p", 'p'), D("NSClassFromString", "p", 'p'),
    D("NSStringFromProtocol", "p", 'p'), D("NSProtocolFromString", "p", 'p'),
    D("objc_alloc", "p", 'p'), D("objc_msgSend_fpret", "pp", 'd'),
    D("method_getImplementation", "p", 'p'), D("class_getMethodImplementation", "pp", 'p'), D("objc_opt_class", "p", 'p'), D("objc_msgSendSuper2", "pp", 'p'), D("NSLog", "p", 'v'),
    D("objc_opt_respondsToSelector", "pp", 'b'),
    D("objc_retain", "p", 'p'), D("objc_release", "p", 'v'), D("objc_autorelease", "p", 'p'),
    D("objc_retainAutorelease", "p", 'p'), D("objc_autoreleaseReturnValue", "p", 'p'),
    D("objc_retainAutoreleasedReturnValue", "p", 'p'), D("objc_retainAutoreleaseReturnValue", "p", 'p'),
    D("objc_storeStrong", "pp", 'v'), D("objc_loadWeakRetained", "p", 'p'), D("objc_initWeak", "pp", 'p'), D("objc_destroyWeak", "p", 'v'),

    D("__cxa_demangle", "pppp", 'p'), D("_exit", "i", 'v'),
    /* These aggregate ABIs are converted explicitly in font_call below. */
    D("CGAffineTransformScale", "pdd", 'p'), D("CTFontGetBoundingRectsForGlyphs", "ppippl", 'p'),
    D("CTRunGetStringRange", "p", 'l'), D("CGPathApply", "ppp", 'v'), D("CGDataProviderCreateWithData", "ppzp", 'p'),
    D("AudioObjectGetPropertyData", "ipippp", 'i'), D("AudioObjectGetPropertyDataSize", "ipipp", 'i'),
    D("AudioOutputUnitStart", "p", 'i'), D("AudioOutputUnitStop", "p", 'i'),
    D("AudioUnitGetProperty", "piiipp", 'i'), D("AudioUnitGetPropertyInfo", "piiipp", 'i'),
    D("AudioUnitInitialize", "p", 'i'), D("AudioUnitUninitialize", "p", 'i'),
    D("AudioUnitRender", "pppiip", 'i'),
    D("UTTypeCreatePreferredIdentifierForTag", "ppp", 'p'),
    D("UTTypeCopyPreferredTagWithClass", "pp", 'p'),
    /* CFRange's two integer words occupy RSI/RDX (x86) and x1/x2 (ARM).
     * The range fits in registers in this signature on both Darwin ABIs. */
    D("CFStringGetCharacters", "pllp", 'v'),
    D("CFBooleanGetValue", "p", 'b'), D("CFDataGetLength", "p", 'l'), D("CFDataGetBytePtr", "p", 'p'),
    D("CFDictionaryGetCount", "p", 'l'), D("CFDictionaryGetKeysAndValues", "ppp", 'v'),
    D("___tolower_l", "ip", 'i'), D("___toupper_l", "ip", 'i'), D("__maskrune_l", "izp", 'i'),
    D("arc4random", "", 'i'), D("btowc_l", "ip", 'i'), D("wctob_l", "ip", 'i'),
    D("catopen", "pi", 'p'), D("catgets", "piip", 'p'), D("catclose", "p", 'i'),
    D("fputwc", "ip", 'i'), D("getwc", "p", 'i'), D("ungetwc", "ip", 'i'),
    D("localeconv_l", "p", 'p'), D("mbrlen_l", "pzpp", 'z'), D("mbrtowc_l", "ppzpp", 'z'),
    D("mbsnrtowcs_l", "ppzzpp", 'z'), D("mbsrtowcs_l", "ppzpp", 'z'), D("mbtowc_l", "ppzp", 'i'),
    D("wcrtomb_l", "pipp", 'z'), D("wcsnrtombs_l", "ppzzpp", 'z'),
    D("strcoll_l", "ppp", 'i'), D("wcscoll_l", "ppp", 'i'),
    D("strxfrm_l", "ppzp", 'z'), D("wcsxfrm_l", "ppzp", 'z'), D("strftime_l", "pzppp", 'z'),
    D("strtof", "pp", 'f'), D("strtof_l", "ppp", 'f'), D("strtod_l", "ppp", 'd'),
    D("strtoll_l", "ppip", 'l'), D("strtoull_l", "ppip", 'z'),
    D("wcstod", "pp", 'd'), D("wcstof", "pp", 'f'), D("wcstol", "ppi", 'l'),
    D("wcstoul", "ppi", 'z'), D("wcstoll", "ppi", 'l'), D("wcstoull", "ppi", 'z'),
    D("wmemcmp", "ppz", 'i'), D("statvfs", "pp", 'i'), D("truncate", "pl", 'i'),
    V("asprintf_l", "ppp", 2), V("snprintf_l", "pzpp", 3), V("sscanf_l", "ppp", 2),
    D("malloc", "z", 'p'), D("calloc", "zz", 'p'), D("realloc", "pz", 'p'), D("free", "p", 'v'),
    D("memcpy", "ppz", 'p'), D("memmove", "ppz", 'p'), D("memset", "piz", 'p'),
    D("memchr", "piz", 'p'), D("memcmp", "ppz", 'i'), D("__bzero", "pz", 'v'),
    D("strlen", "p", 'z'), D("strcmp", "pp", 'i'), D("strncmp", "ppz", 'i'),
    D("strnlen", "pz", 'z'), D("wcslen", "p", 'z'), D("wcwidth", "i", 'i'), D("wctomb", "pi", 'i'),
    D("strcasecmp", "pp", 'i'), D("strncasecmp", "ppz", 'i'), D("strcoll", "pp", 'i'),
    D("strcpy", "pp", 'p'), D("strncpy", "ppz", 'p'), D("strcat", "pp", 'p'),
    D("strchr", "pi", 'p'), D("strrchr", "pi", 'p'), D("strpbrk", "pp", 'p'),
    D("strstr", "pp", 'p'), D("strcasestr", "pp", 'p'), D("strdup", "p", 'p'),
    D("strtol", "ppi", 'l'), D("atoi", "p", 'i'), D("strerror", "i", 'p'),
    D("getenv", "p", 'p'), D("putenv", "p", 'i'), D("setlocale", "ip", 'p'),
    D("confstr", "ipz", 'z'), D("_NSGetEnviron", "", 'p'),
    D("uselocale", "p", 'p'), D("localeconv", "", 'p'), D("___mb_cur_max_l", "p", 'i'),
    D("atanf", "f", 'f'), D("ceil", "d", 'd'), D("ceilf", "f", 'f'),
    D("floor", "d", 'd'), D("floorf", "f", 'f'), D("round", "d", 'd'), D("roundf", "f", 'f'),
    D("hypotf", "ff", 'f'), D("log2", "d", 'd'), D("log2f", "f", 'f'),
    D("log10f", "f", 'f'), D("expf", "f", 'f'), D("expm1f", "f", 'f'),
    D("log10", "d", 'd'), D("fmod", "dd", 'd'), D("fmodf", "ff", 'f'),
    D("log1pf", "f", 'f'), D("powf", "ff", 'f'), D("modf", "dp", 'd'), D("tanf", "f", 'f'),
    D("gmtime", "p", 'p'), D("newlocale", "ipp", 'p'), D("freelocale", "p", 'v'), D("strtoul", "ppi", 'z'),
    D("__sincosf_stret", "f", 'v'),
    D("frexp", "dp", 'd'), D("ldexp", "di", 'd'),
    D("nl_langinfo", "i", 'p'), D("getpid", "", 'i'), D("geteuid", "", 'i'),
    D("getpagesize", "", 'i'), D("sysconf", "i", 'l'),
    D("pathconf", "pi", 'l'), D("fpathconf", "ii", 'l'),
    D("clock_gettime_nsec_np", "i", 'z'), D("creat", "pi", 'i'), D("issetugid", "", 'i'),
    D("madvise", "pzi", 'i'), D("pthread_atfork", "ppp", 'i'), D("pthread_equal", "pp", 'i'),
    D("pthread_getname_np", "ppz", 'i'), D("strerror_r", "ipz", 'i'), D("syscall", "i", 'l'),
    D("clock_gettime", "ip", 'i'), D("mach_absolute_time", "", 'z'), D("mach_timebase_info", "p", 'i'),
    D("getrlimit", "ip", 'i'), D("getuid", "", 'i'), D("getgid", "", 'i'), D("getegid", "", 'i'),
    D("sysctl", "pipppz", 'i'), D("sysctlbyname", "ppppz", 'i'),
    D("getpwent", "", 'p'), D("getpwuid", "i", 'p'), D("endpwent", "", 'v'),
    D("getpwuid_r", "ippzp", 'i'), D("getloadavg", "pi", 'i'),
    D("getgrgid", "i", 'p'), D("getgrgid_r", "ippzp", 'i'), D("getgrnam", "p", 'p'),
    D("chown", "pii", 'i'),
    D("chdir", "p", 'i'),
    D("chmod", "pi", 'i'), D("unlink", "p", 'i'), D("ftruncate", "il", 'i'),
    D("renameat", "ipip", 'i'), D("mkdirat", "ipi", 'i'), D("readlinkat", "ippz", 'l'),
    D("unlinkat", "ipi", 'i'), D("openat", "ipii", 'i'),
    D("fstatat$INODE64", "ippi", 'i'), D("fstatfs$INODE64", "ip", 'i'),
    D("statfs$INODE64", "pp", 'i'), D("fstatvfs", "ip", 'i'),
    D("fdopendir$INODE64", "i", 'p'), D("rmdir", "p", 'i'),
    D("link", "pp", 'i'), D("symlink", "pp", 'i'),
    D("fchmod", "ii", 'i'), D("fchmodat", "ipii", 'i'),
    D("fchown", "iii", 'i'), D("lchown", "pii", 'i'),
    D("dup", "i", 'i'), D("dup2", "ii", 'i'), D("pipe", "p", 'i'),
    D("pwrite", "ipzl", 'l'), D("readv", "ipi", 'l'), D("writev", "ipi", 'l'),
    D("getgrgid_r", "ippzp", 'i'), D("getgrnam_r", "pppzp", 'i'), D("getpwnam_r", "pppzp", 'i'),
    D("getsockopt", "iiipp", 'i'), D("getpeername", "ipp", 'i'),
    D("poll", "pii", 'i'), D("kqueue", "", 'i'), D("kevent", "ipipip", 'i'),
    D("recv", "ipzi", 'l'), D("send", "ipzi", 'l'),
    D("recvmsg", "ipi", 'l'), D("sendmsg", "ipi", 'l'),
    D("lseek", "ili", 'l'), D("dirfd", "p", 'i'),
    D("fchdir", "i", 'i'),
    D("kill", "ii", 'i'),
    D("mach_thread_self", "", 'i'), D("mach_host_self", "", 'i'), D("mach_port_deallocate", "ii", 'i'),
    D("pid_for_task", "ip", 'i'), D("task_info", "iipp", 'i'), D("host_statistics", "iipp", 'i'),
    D("proc_pidinfo", "iizpi", 'i'), D("getrusage", "ip", 'i'),
    D("proc_pidpath", "ipi", 'i'), D("proc_name", "ipi", 'i'),
    D("proc_pid_rusage", "iip", 'i'),
    D("thread_info", "iipp", 'i'),
    D("dirname", "p", 'p'), D("realpath$DARWIN_EXTSN", "pp", 'p'),
    D("access", "pi", 'i'), D("close", "i", 'i'), D("open", "pii", 'i'),
    D("faccessat", "ipii", 'i'), D("fstat$INODE64", "ip", 'i'), D("lstat$INODE64", "pp", 'i'),
    D("getcwd", "pz", 'p'), D("readlink", "ppz", 'l'),
    D("fcntl", "iil", 'i'), D("mkstemps", "pi", 'i'), D("stat$INODE64", "pp", 'i'),
    D("fopen$DARWIN_EXTSN", "pp", 'p'),
    D("fopen", "pp", 'p'), D("fdopen$DARWIN_EXTSN", "ip", 'p'), D("fclose", "p", 'i'),
    D("fflush", "p", 'i'), D("ferror", "p", 'i'), D("flockfile", "p", 'v'), D("funlockfile", "p", 'v'),
    D("fread", "pzzp", 'z'), D("fwrite", "pzzp", 'z'), D("fputc", "ip", 'i'), D("putc", "ip", 'i'),
    D("fputs", "pp", 'i'),
    D("puts", "p", 'i'), D("putchar", "i", 'i'), D("__srget", "p", 'i'), D("time", "p", 'l'),
    D("isatty", "i", 'i'), D("tcgetattr", "ip", 'i'), D("tcsetattr", "iip", 'i'),
    D("sigfillset", "p", 'i'), D("sigaction", "ipp", 'i'),
    D("pthread_mutex_init", "pp", 'i'), D("pthread_mutex_destroy", "p", 'i'),
    D("pthread_mutex_lock", "p", 'i'), D("pthread_mutex_unlock", "p", 'i'),
    D("pthread_key_create", "pp", 'i'), D("pthread_key_delete", "z", 'i'),
    D("pthread_getspecific", "z", 'p'), D("pthread_setspecific", "zp", 'i'),
    D("pthread_create", "pppp", 'i'), D("pthread_join", "pp", 'i'),
    D("pthread_once", "pp", 'i'), D("__r2_once_complete", "", 'i'),
    D("pthread_detach", "p", 'i'), D("pthread_exit", "p", 'v'),
    D("sched_yield", "", 'i'),
    D("pthread_setname_np", "p", 'i'), D("pthread_mach_thread_np", "p", 'i'),
    D("pthread_self", "", 'p'), D("pthread_main_np", "", 'i'),
    D("pthread_sigmask", "ipp", 'i'), D("pthread_attr_init", "p", 'i'), D("pthread_attr_destroy", "p", 'i'),
    D("pthread_attr_getdetachstate", "pp", 'i'), D("pthread_attr_getguardsize", "pp", 'i'),
    D("pthread_attr_getstacksize", "pp", 'i'), D("pthread_attr_setdetachstate", "pi", 'i'),
    D("pthread_attr_setstacksize", "pz", 'i'),
    D("pthread_condattr_init", "p", 'i'), D("pthread_condattr_destroy", "p", 'i'),
    D("pthread_cond_init", "pp", 'i'), D("pthread_cond_destroy", "p", 'i'),
    D("pthread_cond_signal", "p", 'i'), D("pthread_cond_broadcast", "p", 'i'),
    D("pthread_cond_wait", "pp", 'i'), D("pthread_cond_timedwait", "ppp", 'i'),
    D("pthread_mutexattr_init", "p", 'i'), D("pthread_mutexattr_destroy", "p", 'i'),
    D("pthread_mutexattr_settype", "pi", 'i'), D("pthread_mutex_trylock", "p", 'i'),
    D("pthread_get_stackaddr_np", "p", 'p'), D("pthread_get_stacksize_np", "p", 'z'),
    D("mbrtowc", "ppzp", 'z'), D("wcrtomb", "pip", 'z'), D("mbsinit", "p", 'i'),
    D("__error", "", 'p'), D("__maskrune", "il", 'i'), D("__tolower", "i", 'i'),
    D("__toupper", "i", 'i'), D("___mb_cur_max", "", 'i'),
    D("__strcpy_chk", "ppz", 'p'), D("___chkstk_darwin", "", 'z'),
    D("__memcpy_chk", "ppzz", 'p'),
    D("memset_pattern16", "ppz", 'v'), D("atof", "p", 'd'),
    D("fseek", "pli", 'i'), D("ftell", "p", 'l'), D("perror", "p", 'v'),
    D("mkdir", "pi", 'i'), D("remove", "p", 'i'), D("rename", "pp", 'i'), D("system", "p", 'i'),
    D("exit", "i", 'v'), D("abort", "", 'v'),
    V("printf", "p", 0), V("fprintf", "pp", 1), V("sprintf", "pp", 1),
    V("snprintf", "pzp", 2), V("__sprintf_chk", "pizp", 3),
    V("sscanf", "pp", 1),
    D("vsscanf", "ppp", 'i'),
    D("vfprintf", "ppp", 'i'), D("vsnprintf", "pzpp", 'i'),
    D("initscr", "", 'p'), D("newwin", "iiii", 'p'), D("delwin", "p", 'i'),
    D("endwin", "", 'i'), D("isendwin", "", 'i'), D("doupdate", "", 'i'),
    D("beep", "", 'i'), D("raw", "", 'i'), D("noecho", "", 'i'), D("nonl", "", 'i'),
    D("curs_set", "i", 'i'), D("napms", "i", 'i'), D("typeahead", "i", 'i'),
    D("keypad", "pi", 'i'), D("nodelay", "pi", 'i'), D("scrollok", "pi", 'i'),
    D("key_defined", "p", 'i'), D("set_escdelay", "i", 'i'), D("ungetch", "i", 'i'),
    D("waddch", "pi", 'i'), D("waddnstr", "ppi", 'i'), D("wattr_off", "pip", 'i'),
    D("wattr_on", "pip", 'i'), D("wclrtoeol", "p", 'i'), D("wgetch", "p", 'i'),
    D("wmove", "pii", 'i'), D("wnoutrefresh", "p", 'i'), D("wrefresh", "p", 'i'),
    D("wredrawln", "pii", 'i'), D("wscrl", "pi", 'i'), D("tgetstr", "pp", 'p'), D("tigetstr", "p", 'p'),
    V("mvwprintw", "piip", 3),
    D("SDL_SetMainReady", "", 'v'), D("SDL_Init", "i", 'i'), D("SDL_GetError", "", 'p'),
    D("SDL_CreateWindow", "piiiii", 'p'), D("SDL_CreateRenderer", "pii", 'p'),
    D("SDL_CreateTexture", "piiii", 'p'), D("SDL_UpdateTexture", "pppi", 'i'),
    D("SDL_RenderClear", "p", 'i'), D("SDL_RenderCopy", "pppp", 'i'), D("SDL_RenderPresent", "p", 'v'),
    D("SDL_PollEvent", "p", 'i'), D("SDL_PushEvent", "p", 'i'), D("SDL_Delay", "i", 'v'),
    D("SDL_GetTicks", "", 'i'), D("SDL_SetWindowTitle", "pp", 'v'), D("SDL_DestroyTexture", "p", 'v'),
    D("SDL_DestroyRenderer", "p", 'v'), D("SDL_DestroyWindow", "p", 'v'), D("SDL_Quit", "", 'v'),
    D("SDL_InitSubSystem", "i", 'i'), D("SDL_GetCurrentAudioDriver", "", 'p'),
    D("SDL_SetRelativeMouseMode", "i", 'i'),
    D("SDL_OpenAudioDevice", "pippi", 'i'),
    D("SDL_CloseAudioDevice", "i", 'v'), D("SDL_PauseAudioDevice", "ii", 'v'),
    D("SDL_QueueAudio", "ipi", 'i'), D("SDL_GetQueuedAudioSize", "i", 'i'),
    D("SDL_ClearQueuedAudio", "i", 'v'), D("SDL_GetAudioDeviceStatus", "i", 'i'),
    D("_dyld_get_image_header", "i", 'p'), D("_dyld_get_image_name", "i", 'p'), D("_dyld_image_count", "", 'i'),
    D("CFBundleCreate", "pp", 'p'), D("CFBundleGetBundleWithIdentifier", "p", 'p'),
    D("CFBundleGetFunctionPointerForName", "pp", 'p'), D("CFRetain", "p", 'p'),
    D("CFStringCreateWithCStringNoCopy", "ppip", 'p'), D("CFURLCreateWithFileSystemPath", "pplb", 'p'),
    D("CFDataCreate", "ppl", 'p'), D("CFRelease", "p", 'v'),
    D("CFArrayGetCount", "p", 'l'), D("CFArrayGetValueAtIndex", "pl", 'p'),
    D("CFCalendarCopyCurrent", "", 'p'), D("CFCalendarGetFirstWeekday", "p", 'l'),
    D("CFCalendarGetMinimumDaysInFirstWeek", "p", 'l'),
    D("CFDateFormatterCopyProperty", "pp", 'p'), D("CFDateFormatterCreate", "ppll", 'p'),
    D("CFDateFormatterGetFormat", "p", 'p'), D("CFDictionaryGetValue", "pp", 'p'),
    D("CFLocaleCopyCurrent", "", 'p'), D("CFLocaleCopyDisplayNameForPropertyValue", "ppp", 'p'),
    D("CFLocaleCopyPreferredLanguages", "", 'p'), D("CFLocaleCreate", "pp", 'p'),
    D("CFLocaleGetIdentifier", "p", 'p'), D("CFLocaleGetValue", "pp", 'p'),
    D("CFNumberFormatterCopyProperty", "pp", 'p'), D("CFNumberFormatterCreate", "ppl", 'p'),
    D("CFNumberFormatterCreateStringWithValue", "pppz", 'p'), D("CFNumberFormatterGetFormat", "p", 'p'),
    D("CFNumberGetValue", "plp", 'b'), D("CFStringAppendCString", "ppi", 'v'),
    D("CFStringCreateMutable", "pl", 'p'), D("CFStringCreateWithCString", "ppi", 'p'),
    D("CFStringGetCString", "ppli", 'b'), D("CFStringGetCharacterAtIndex", "pl", 'h'),
    D("CFStringGetLength", "p", 'l'), D("CFStringGetMaximumSizeForEncoding", "li", 'l'),
    D("CFStringGetSystemEncoding", "", 'i'), D("CFStringNormalize", "pl", 'v'),
    D("CFTimeZoneCopyLocalizedName", "plp", 'p'), D("CFTimeZoneCreateWithName", "ppb", 'p'),
    D("objc_msgSend", "pp", 'p'), D("objc_msgSend_stret", "ppp", 'v'),
    D("NSHomeDirectory", "", 'p'), D("SCDynamicStoreCopyProxies", "p", 'p'), D("objc_alloc_init", "p", 'p'),
    D("NewMusicPlayer", "p", 'i'), D("DisposeMusicPlayer", "p", 'i'),
    D("NewMusicSequence", "p", 'i'), D("DisposeMusicSequence", "p", 'i'),
    D("MusicSequenceFileLoadData", "ppii", 'i'), D("MusicPlayerSetSequence", "pp", 'i'),
    D("MusicPlayerPreroll", "p", 'i'), D("MusicPlayerStart", "p", 'i'), D("MusicPlayerStop", "p", 'i'),
    D("MusicPlayerSetTime", "pd", 'i'), D("MusicPlayerGetTime", "pp", 'i'),
    D("MusicPlayerIsPlaying", "pp", 'i'), D("MusicSequenceGetTrackCount", "pp", 'i'),
    D("MusicSequenceGetIndTrack", "pip", 'i'), D("MusicTrackGetProperty", "pipp", 'i'),
    D("MusicTrackSetProperty", "pipi", 'i'), D("MusicSequenceGetAUGraph", "pp", 'i'),
    D("AUGraphGetNodeCount", "pp", 'i'), D("AUGraphGetIndNode", "pip", 'i'),
    D("AUGraphNodeInfo", "pipp", 'i'), D("AUGraphIsRunning", "pp", 'i'),
    D("AudioUnitSetParameter", "piiifi", 'i'),
    D("__darwin_check_fd_set_overflow", "ipi", 'i'),
    D("__memmove_chk", "ppzz", 'p'), D("__memset_chk", "pizz", 'p'),
    D("__strcat_chk", "ppz", 'p'), D("__strncat_chk", "ppzz", 'p'),
    D("atan", "d", 'd'), D("atan2", "dd", 'd'), D("exp", "d", 'd'),
    D("log", "d", 'd'), D("pow", "dd", 'd'), D("sin", "d", 'd'), D("tan", "d", 'd'),
    D("cos", "d", 'd'), D("sqrt", "d", 'd'), D("sinf", "f", 'f'), D("cosf", "f", 'f'),
    D("__sincos_stret", "d", 'v'),
    D("feof", "p", 'i'), D("fgetc", "p", 'i'), D("getc", "p", 'i'), V("fscanf", "pp", 1),
    D("opendir$INODE64", "p", 'p'), D("readdir$INODE64", "p", 'p'), D("closedir", "p", 'i'),
    D("tzset", "", 'v'), D("uname", "p", 'i'),
    D("gettimeofday", "pp", 'i'), D("gmtime_r", "pp", 'p'), D("strftime", "pzpp", 'z'),
    D("localtime_r", "pp", 'p'), D("strtoll", "ppi", 'l'), D("strtoull", "ppi", 'z'),
    D("strtod", "pp", 'd'), D("strncat", "ppz", 'p'), D("strcspn", "pp", 'z'), D("strtok_r", "ppp", 'p'),
    D("read", "ipz", 'l'), D("write", "ipz", 'l'), D("usleep", "i", 'i'), D("nanosleep", "pp", 'i'),
    D("socket", "iii", 'i'), D("bind", "ipi", 'i'), D("connect", "ipi", 'i'),
    D("socketpair", "iiip", 'i'),
    D("accept", "ipp", 'i'),
    D("shutdown", "ii", 'i'),
    D("listen", "ii", 'i'),
    D("getsockname", "ipp", 'i'), D("setsockopt", "iiipi", 'i'), D("ioctl", "izp", 'i'),
    D("recvfrom", "ipzipp", 'l'), D("sendto", "ipzipi", 'l'), D("select$1050", "ipppp", 'i'),
    D("getaddrinfo", "pppp", 'i'), D("freeaddrinfo", "p", 'v'), D("gai_strerror", "i", 'p'),
    D("getnameinfo", "pipipii", 'i'), D("if_nametoindex", "p", 'i'),
    D("inet_pton", "ipp", 'i'), D("gethostname", "pz", 'i'),
    D("gethostbyaddr", "pii", 'p'), D("gethostbyname", "p", 'p'), D("inet_addr", "p", 'i'),
    D("getifaddrs", "p", 'i'), D("freeifaddrs", "p", 'v'), D("rand", "", 'i'),
    D("setjmp", "p", 'i'), D("_setjmp", "p", 'i'), D("longjmp", "pi", 'v'), D("_longjmp", "pi", 'v'),
    D("raise", "i", 'i'),
    D("signal", "ip", 'p'), D("qsort", "pzzp", 'v'), D("qsort_r", "pzzpp", 'v'),
    D("__cxa_atexit", "ppp", 'i'), D("atexit", "p", 'i'),
    D("__cxa_guard_acquire", "p", 'i'), D("__cxa_guard_release", "p", 'v'),
    D("__stack_chk_fail", "", 'v'),
    D("__cxa_pure_virtual", "", 'v'), D("__assert_rtn", "ppip", 'v'),
    D("semaphore_create", "ipii", 'i'), D("semaphore_destroy", "ii", 'i'),
    D("semaphore_signal", "i", 'i'), D("semaphore_wait", "i", 'i'),
    D("semaphore_timedwait", "iz", 'i'),
    D("dlsym", "pp", 'p'), D("dlerror", "", 'p'), D("dladdr", "pp", 'i'),
    D("dlopen", "pi", 'p'), D("dlclose", "p", 'i'),
    D("pthread_condattr_setclock", "pi", 'i'),
    D("fegetenv", "p", 'i'), D("fesetenv", "p", 'i'),
    D("pread", "ipzl", 'l'), D("mmap", "pziiil", 'p'), D("munmap", "pz", 'i'),
    D("task_set_exception_ports", "iiiii", 'i'),
    D("setrlimit", "ip", 'i'),
    D("mprotect", "pzi", 'i'),
    D("_tlv_bootstrap", "p", 'p'),
#undef D
#undef V
};

static const native_definition jvm_probe_definitions[] = {
#include "jvm_probe_imports.inc"
};

typedef struct { uint64_t start, end; bool writable; } native_mapping;
typedef struct { uint64_t start, end; int protection; } native_vm_mapping;
typedef struct { const native_definition *definition; void *function; int graphics_kind; uint8_t stub[16]; } native_import;
typedef struct {
    uint64_t key, regs[16], return_address;
    uint8_t xmm[16][16];
    uint32_t mask;
    size_t signal_frames,suspend_depth;
    r2_block *block;
    bool save_mask;
} native_jump;
typedef struct native_thread_state {
    r2_native_runtime *runtime;
    r2_native_execution execution;
    size_t *hash;
    size_t hash_capacity;
    r2_block *hash_block;
    r2_block *callback_block;
    native_jump jumps[128];
    r2_x87_state x87;
    uint32_t mxcsr;
    uint64_t callback_stack;
    pthread_t host;
    uint32_t signal_mask;
    struct {uint32_t mask;uint64_t stack;} suspends[32];
    size_t suspend_depth;
    unsigned engine_depth, quantum;
    struct { uint64_t control, stack; } once[64];
    size_t once_depth;
    struct { pthread_mutex_t *mutex; unsigned count; } mutexes[128];
    bool thread_returning;
    struct { uint64_t function,argument; } tlv_exits[256];
    size_t tlv_exit_count;
    void *key_values[128],*tls_values[32];
    _Atomic uint32_t pending;
} native_thread_state;
typedef struct {
    r2_native_runtime *runtime;
    native_thread_state *state;
    pthread_t host;
    r2_interp_env env;
    uint64_t entry, argument, result;
    size_t stack_size;
    bool detached, joined, joining;
    _Atomic bool done;
} native_guest_thread;
typedef struct native_callback {
    r2_native_runtime *runtime;
    r2_interp_env env;
    uint64_t function, argument;
    _Atomic bool disabled;
    _Atomic unsigned inflight;
    struct native_callback *next;
} native_callback;
typedef void (*native_sdl_log)(void *,int,int,const char *);
static _Thread_local native_thread_state *native_thread;
static _Atomic(native_callback *) pinned_callbacks;
static _Atomic unsigned pinned_callback_count;
struct r2_native_runtime {
    void *curses;
    void *sdl;
    void *audio_toolbox;
    void *core_audio;
    void *core_foundation;
    void *core_graphics, *core_text;
    void *foundation, *system_configuration, *objc, *core_services, *cf_network;
    void *cpp;
    void *zlib;
    struct addrinfo *resolver_results[64];
    struct { uint64_t guest; z_stream stream; bool deflating; } zstreams[256];
    bool jvm_probe;
    void *music_players[32], *music_sequences[32];
    native_import imports[NATIVE_MAX_IMPORTS];
    size_t import_count;
    native_mapping code[NATIVE_MAX_CODE_RANGES];
    size_t code_count;
    native_mapping memory[256];
    const r2_memory_region *image_regions;
    const size_t *image_region_count;
    native_thread_state *root_thread, *previous_thread;
    pthread_mutex_t execution_mutex;
    bool execution_mutex_initialized;
    _Atomic bool stopping;
    r2_status stop_status;
    char stop_error[R2_ERROR_CAP];
    size_t thread_count;
    native_guest_thread *threads[256];
    native_thread_state *callback_threads[64];
    unsigned active_callbacks;
    native_callback *sdl_log_callback;
    void *sdl_original_log, *sdl_original_log_argument;
    bool sdl_log_saved;
    native_callback *objc_exception_callback,*previous_objc_exception;
    void *original_objc_exception;
    bool objc_exception_saved;
    struct {uint64_t guest;void *native;} objc_classes[64];
    size_t objc_class_count;
    struct {native_definition definition;char name[64],arguments[NATIVE_MAX_ARGS+1];} objc_imp_definitions[128];
    size_t objc_imp_definition_count;
    struct { void *instance; uint64_t handle; native_callback *callback; bool report; } vk_callbacks[128];
    AudioUnit audio_units[64];
    struct { AudioObjectID object; AudioObjectPropertyAddress address; native_callback *callback; } audio_listeners[64];
    struct { AudioUnit unit; AudioUnitPropertyID property; AudioUnitScope scope; AudioUnitElement element; native_callback *callback; } audio_renders[128];
    bool has_terminal;
    struct termios terminal;
    struct sigaction original_signals[NSIG];
    bool installed_signals[NSIG];
    uint64_t signal_handlers[NSIG];
    uint32_t signal_masks[NSIG], signal_flags[NSIG];
    _Atomic uint32_t pending;
    struct { pthread_key_t key; uint64_t destructor; } keys[128];
    size_t key_count;
    struct { uint64_t function, argument, dso; } exits[4096];
    size_t exit_count;
    struct { uint64_t prepare, parent, child; } forks[64];
    size_t fork_count;
    semaphore_t semaphores[128];
    size_t semaphore_count;
    void *loader_context;
    r2_symbol_lookup lookup;
    r2_address_lookup address_lookup;
    r2_image_open open_image;
    r2_image_close close_image;
    r2_image_count image_count;
    r2_image_name image_name;
    r2_image_header image_header;
    const char *override_name;
    void *override_function;
    const native_definition *override_definition;
    int override_graphics_kind;
    graphics_module graphics[GRAPHICS_MODULE_COUNT];
    size_t process_opens;
    void *system_library;
    size_t system_opens;
    void *iokit;
    size_t iokit_opens, core_foundation_opens,objc_opens;
    unsigned framework_global;
    char loader_error[256];
    bool loader_error_pending;
    fenv_t host_fenv;
    bool fenv_saved;
    native_vm_mapping vm_mappings[NATIVE_MAX_MAPPINGS];
    size_t vm_mapping_count;
    native_mapping executable_cache;
    sigset_t host_signal_mask;
    bool signal_mask_saved;
    bool nofile_saved;
    struct rlimit host_nofile;
    int host_cwd;
    struct {
        r2_tls_range ranges[32]; size_t range_count;
        void *data; size_t size, alignment;
        pthread_key_t key;
    } tls[32];
    size_t tls_count;
    struct { mach_port_t port; unsigned references; } ports[1024];
};

static r2_native_runtime *signal_runtime;

static void abandon_mutexes(void);
static void graphics_destroy(r2_native_runtime *runtime);
static void callbacks_quiesce(r2_native_runtime *runtime);
static void audio_quiesce(r2_native_runtime *runtime);
static void objc_callbacks_quiesce(r2_native_runtime *runtime);
static void vulkan_callbacks_quiesce(r2_native_runtime *runtime);
static r2_status vulkan_special(r2_native_runtime *runtime,native_import *import,uint64_t regs[16],bool *handled,char *error,size_t error_len);
static native_callback *callback_create(r2_native_runtime *runtime, uint64_t function, uint64_t argument,
                                          char *error, size_t error_len);
static r2_status callback_invoke(native_callback *callback, const uint64_t *arguments, size_t count,
                                  uint64_t *result, char *error, size_t error_len);
static void sdl_log_proxy(void *opaque,int category,int priority,const char *message);

static native_thread_state *allocate_thread_state(r2_native_runtime *runtime)
{
    native_thread_state *state = calloc(1, sizeof(*state));
    if (state == NULL) return NULL;
    state->runtime = runtime; state->x87.control = 0x037f; state->mxcsr = 0x1f80;
    state->host=pthread_self();
    state->hash_capacity = NATIVE_HASH_SIZE;
    state->hash = calloc(NATIVE_HASH_SIZE, sizeof(size_t));
    if (state->hash == NULL) { free(state); return NULL; }
    return state;
}

static void destroy_thread_state(native_thread_state *state)
{
    if(!state)return;
    for(size_t i=0;i<32;i++)free(state->tls_values[i]);
    if(state->callback_block) {
        if(state->callback_block->insns!=state->callback_block->storage)free(state->callback_block->insns);
        free(state->callback_block);
    }
    free(state->hash);free(state);
}

static void engine_enter(r2_native_runtime *runtime)
{
    pthread_mutex_lock(&runtime->execution_mutex);
    native_thread->engine_depth++;
}
static void engine_leave(r2_native_runtime *runtime)
{
    native_thread->engine_depth--;
    pthread_mutex_unlock(&runtime->execution_mutex);
}
static unsigned engine_suspend(r2_native_runtime *runtime)
{
    unsigned depth = native_thread->engine_depth;
    while (native_thread->engine_depth) engine_leave(runtime);
    return depth;
}
static void engine_resume(r2_native_runtime *runtime, unsigned depth)
{
    while (depth--) engine_enter(runtime);
}

void r2_native_execution_enter(r2_native_runtime *runtime, r2_block *block,
                               const r2_interp_env *env,uint32_t *signal_mask,uint32_t *pending_signals,size_t *signal_frames,r2_native_execution *previous)
{
    engine_enter(runtime);
    *previous = native_thread->execution;
    *signal_mask=previous->signal_mask?*previous->signal_mask:native_thread->signal_mask;
    native_thread->execution.signal_mask=signal_mask;native_thread->execution.pending_signals=pending_signals;native_thread->execution.signal_frames=signal_frames;
    native_thread->execution.block = block; native_thread->execution.env = env;
}
void r2_native_execution_leave(r2_native_runtime *runtime, const r2_native_execution *previous)
{
    if(native_thread->execution.signal_mask)native_thread->signal_mask=*native_thread->execution.signal_mask;
    native_thread->execution = *previous;
    if(previous->signal_mask)*previous->signal_mask=native_thread->signal_mask;
    engine_leave(runtime);
}
bool r2_native_stopping(r2_native_runtime *runtime)
{
    return atomic_load_explicit(&runtime->stopping, memory_order_acquire);
}
void r2_native_request_stop(r2_native_runtime *runtime, r2_status status, const char *error)
{
    if (status != R2_OK && runtime->thread_count == 0 && runtime->active_callbacks == 0) return;
    pthread_mutex_lock(&runtime->execution_mutex);
    if (!r2_native_stopping(runtime)) {
        runtime->stop_status = status == R2_OK ? R2_ERR_UNSUPPORTED : status;
        snprintf(runtime->stop_error, sizeof(runtime->stop_error), "%s", error ? error : "guest runtime stopped");
        atomic_store_explicit(&runtime->stopping, true, memory_order_release);
    }
    pthread_mutex_unlock(&runtime->execution_mutex);
}
r2_status r2_native_execution_poll(r2_native_runtime *runtime, char *error, size_t error_len)
{
    if (!r2_native_stopping(runtime) && ++native_thread->quantum >= 1024) {
        native_thread->quantum = 0;
        unsigned depth = engine_suspend(runtime);
        sched_yield(); engine_resume(runtime, depth);
    }
    if (!r2_native_stopping(runtime)) return R2_OK;
    r2_set_error(error, error_len, "%s", runtime->stop_error);
    return runtime->stop_status;
}
void r2_native_fast_state_get(r2_native_runtime *runtime,r2_native_fast_state *state)
{
    *state=(r2_native_fast_state){&runtime->stopping,&native_thread->pending,&runtime->pending,&native_thread->quantum};
}
bool r2_native_thread_returning(r2_native_runtime *runtime)
{
    return native_thread != NULL && native_thread->runtime == runtime && native_thread->thread_returning;
}
bool r2_native_current_execution(r2_native_runtime *runtime, r2_native_execution *execution)
{
    if (native_thread == NULL || native_thread->runtime != runtime) return false;
    *execution = native_thread->execution;
    return execution->block != NULL && execution->env != NULL;
}
void r2_native_stop_and_join(r2_native_runtime *runtime)
{
    if (runtime == NULL || !runtime->execution_mutex_initialized) return;
    r2_native_request_stop(runtime, R2_OK, "guest runtime stopped");
    if (native_thread != NULL && native_thread->runtime == runtime) abandon_mutexes();
    unsigned depth = native_thread != NULL && native_thread->runtime == runtime ? engine_suspend(runtime) : 0;
    callbacks_quiesce(runtime);
    for (size_t i = 0; i < runtime->thread_count; i++) {
        native_guest_thread *thread = runtime->threads[i];
        if (thread == NULL) continue;
        while (true) {
            engine_enter(runtime);
            if (thread->joined) { engine_leave(runtime); break; }
            if (thread->joining) {
                engine_leave(runtime);
                struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL); continue;
            }
            thread->joining = true; engine_leave(runtime);
            pthread_join(thread->host, NULL);
            engine_enter(runtime); thread->joined = true; thread->joining = false; engine_leave(runtime);
            break;
        }
    }
    engine_resume(runtime, depth);
}

r2_status r2_native_add_tls(r2_native_runtime *runtime, const r2_tls_range *ranges, size_t count,
                            const void *data, size_t size, size_t alignment, char *error, size_t error_len)
{
    if (runtime->tls_count == 32 || count > 32 || count == 0 || size == 0 ||
        alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0) {
        r2_set_error(error, error_len, "invalid or excessive Mach-O TLS template"); return R2_ERR_UNSUPPORTED;
    }
    for (size_t i = 0; i < count; i++) {
        if (ranges[i].count > SIZE_MAX / 24 || ranges[i].address > UINT64_MAX - ranges[i].count * 24) return R2_ERR_DECODE;
        const uint8_t *descriptors = r2_native_pointer(runtime, ranges[i].address, (size_t)ranges[i].count * 24, true, error, error_len);
        if (descriptors == NULL) return R2_ERR_DECODE;
        for (uint64_t j = 0; j < ranges[i].count; j++) {
            uint64_t offset; memcpy(&offset, descriptors + j * 24 + 16, 8);
            if (offset >= size) { r2_set_error(error, error_len, "TLS descriptor offset is outside template"); return R2_ERR_DECODE; }
        }
    }
    void *copy = malloc(size);
    if (copy == NULL) return R2_ERR_NO_MEMORY;
    memcpy(copy, data, size);
    pthread_key_t key;
    if (pthread_key_create(&key, free) != 0) { free(copy); return R2_ERR_NO_MEMORY; }
    size_t index = runtime->tls_count++;
    memcpy(runtime->tls[index].ranges, ranges, count * sizeof(*ranges));
    runtime->tls[index].range_count = count; runtime->tls[index].data = copy;
    runtime->tls[index].size = size; runtime->tls[index].alignment = alignment; runtime->tls[index].key = key;
    for (size_t i = 0; i < count; i++)
        for (uint64_t j = 0; j < ranges[i].count; j++) {
            uint64_t encoded = (uint64_t)key;
            memcpy((void *)(uintptr_t)(ranges[i].address + j * 24 + 8), &encoded, 8);
        }
    return R2_OK;
}

static void forget_mapping_permissions(r2_native_runtime *runtime)
{
    memset(runtime->memory, 0, sizeof(runtime->memory));
    memset(&runtime->executable_cache, 0, sizeof(runtime->executable_cache));
}

static bool owns_vm_range(const r2_native_runtime *runtime, uint64_t start, uint64_t end)
{
    uint64_t cursor = start;
    while (cursor < end) {
        bool covered = false;
        for (size_t i = 0; i < runtime->vm_mapping_count; i++) {
            const native_vm_mapping *m = &runtime->vm_mappings[i];
            if (cursor >= m->start && cursor < m->end) {
                cursor = m->end < end ? m->end : end;
                covered = true; break;
            }
        }
        if (!covered) return false;
    }
    return true;
}

static bool vm_range_fits(const r2_native_runtime *runtime, uint64_t start, uint64_t end, int protection)
{
    size_t count = runtime->vm_mapping_count + (protection >= 0);
    for (size_t i = 0; i < runtime->vm_mapping_count; i++) {
        const native_vm_mapping *m = &runtime->vm_mappings[i];
        if (m->start >= end || m->end <= start) continue;
        count--; count += m->start < start; count += m->end > end;
    }
    return count <= NATIVE_MAX_MAPPINGS;
}

static void replace_vm_range(r2_native_runtime *runtime, uint64_t start, uint64_t end, int protection)
{
    size_t i = 0;
    while (i < runtime->vm_mapping_count) {
        native_vm_mapping m = runtime->vm_mappings[i];
        if (m.start >= end || m.end <= start) { i++; continue; }
        runtime->vm_mappings[i] = runtime->vm_mappings[--runtime->vm_mapping_count];
        if (m.start < start) runtime->vm_mappings[runtime->vm_mapping_count++] = (native_vm_mapping){m.start, start, m.protection};
        if (m.end > end) runtime->vm_mappings[runtime->vm_mapping_count++] = (native_vm_mapping){end, m.end, m.protection};
    }
    if (protection >= 0) {
        /* Keep adjacent commits together rather than consuming one tracking
         * entry per HotSpot page. Splits retain their individual protections. */
        for (size_t j = 0; j < runtime->vm_mapping_count;) {
            native_vm_mapping m = runtime->vm_mappings[j];
            if (m.protection == protection && (m.end == start || m.start == end)) {
                if (m.end == start) start = m.start; else end = m.end;
                runtime->vm_mappings[j] = runtime->vm_mappings[--runtime->vm_mapping_count]; j = 0;
            } else j++;
        }
        runtime->vm_mappings[runtime->vm_mapping_count++] = (native_vm_mapping){start, end, protection};
    }
    forget_mapping_permissions(runtime);
}

static int host_mapping_protection(int guest)
{
    /* Guest executable pages contain x86, and never execute as ARM code. */
    return (guest & ~PROT_EXEC) | ((guest & PROT_EXEC) ? PROT_READ : 0);
}

void *r2_native_stack_allocate(r2_native_runtime *runtime, size_t size)
{
    if (runtime->vm_mapping_count == NATIVE_MAX_MAPPINGS) return NULL;
    void *address = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (address == MAP_FAILED) return NULL;
    replace_vm_range(runtime, (uintptr_t)address, (uintptr_t)address + size, PROT_READ | PROT_WRITE);
    return address;
}

void r2_native_stack_release(r2_native_runtime *runtime, void *address, size_t size)
{
    if (address == NULL) return;
    uint64_t start = (uintptr_t)address;
    if (owns_vm_range(runtime, start, start + size) && munmap(address, size) == 0)
        replace_vm_range(runtime, start, start + size, -1);
}

void r2_native_set_images(r2_native_runtime *runtime, r2_image_count count, r2_image_name name,r2_image_header header)
{
    runtime->image_count = count; runtime->image_name = name;runtime->image_header=header;
}

void r2_native_set_lookup(r2_native_runtime *runtime, void *context,
                          r2_symbol_lookup lookup, r2_address_lookup address_lookup,
                          r2_image_open open_image, r2_image_close close_image)
{
    if (runtime != NULL) {
        runtime->loader_context = context; runtime->lookup = lookup;
        runtime->address_lookup = address_lookup;
        runtime->open_image = open_image; runtime->close_image = close_image;
    }
}

void r2_native_set_regions(r2_native_runtime *runtime, const r2_memory_region *regions, const size_t *count)
{
    runtime->image_regions = regions; runtime->image_region_count = count;
    forget_mapping_permissions(runtime);
}

void r2_native_set_stack(r2_native_runtime *runtime, uint64_t base, uint64_t size)
{
    if (runtime != NULL && native_thread != NULL && native_thread->runtime == runtime) {
        native_thread->execution.stack_base = base; native_thread->execution.stack_size = size;
    }
}

bool r2_native_pop_exit(r2_native_runtime *runtime, uint64_t *function, uint64_t *argument)
{
    if(runtime&&native_thread&&native_thread->runtime==runtime&&native_thread->tlv_exit_count) {
        size_t index=--native_thread->tlv_exit_count;
        *function=native_thread->tlv_exits[index].function;*argument=native_thread->tlv_exits[index].argument;return true;
    }
    if (runtime == NULL || runtime->exit_count == 0) return false;
    size_t index = --runtime->exit_count;
    *function = runtime->exits[index].function;
    *argument = runtime->exits[index].argument;
    return true;
}
static void signal_proxy(int signum)
{
    int saved_errno = errno;
    if (signal_runtime != NULL && signum > 0 && signum <= 32) {
        if (native_thread != NULL && native_thread->runtime == signal_runtime)
            atomic_fetch_or_explicit(&native_thread->pending, UINT32_C(1) << (signum - 1), memory_order_relaxed);
        else atomic_fetch_or_explicit(&signal_runtime->pending, UINT32_C(1) << (signum - 1), memory_order_relaxed);
    }
    errno = saved_errno;
}

int r2_native_fault_code(r2_native_runtime *runtime, uint64_t address)
{
    (void)runtime;
    mach_vm_address_t start = address; mach_vm_size_t length = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t status = mach_vm_region(mach_task_self(), &start, &length, VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    return status == KERN_SUCCESS && start <= address && length > address - start ? SEGV_ACCERR : SEGV_MAPERR;
}
void r2_native_reset_signal_action(r2_native_runtime *runtime, unsigned signum)
{
    if (signum >= NSIG) return;
    runtime->signal_handlers[signum] = 0; runtime->signal_masks[signum] = runtime->signal_flags[signum] = 0;
}

uint32_t r2_native_pending(r2_native_runtime *runtime)
{
    _Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal pending flags must be lock-free");
    if ((atomic_load_explicit(&native_thread->pending, memory_order_relaxed) |
         atomic_load_explicit(&runtime->pending, memory_order_relaxed)) == 0) return 0;
    return atomic_exchange_explicit(&native_thread->pending, 0, memory_order_relaxed) |
           atomic_exchange_explicit(&runtime->pending, 0, memory_order_relaxed);
}

void r2_native_signal_action(r2_native_runtime *runtime, unsigned signum,
                              uint64_t *handler, uint32_t *mask, uint32_t *flags)
{
    if (signum >= NSIG) { *handler = 0; *mask = 0; *flags = 0; return; }
    *handler = runtime->signal_handlers[signum];
    *mask = runtime->signal_masks[signum];
    *flags = runtime->signal_flags[signum];
}

r2_native_runtime *r2_native_create(char *error, size_t error_len)
{
    r2_native_runtime *runtime = calloc(1, sizeof(*runtime));
    if (runtime != NULL) {
        runtime->host_cwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        runtime->signal_mask_saved = pthread_sigmask(SIG_SETMASK, NULL, &runtime->host_signal_mask) == 0;
        runtime->fenv_saved = fegetenv(&runtime->host_fenv) == 0;
        runtime->previous_thread = native_thread;
        runtime->root_thread = allocate_thread_state(runtime);
        native_thread = runtime->root_thread;
        pthread_mutexattr_t mutex_attributes;
        if (pthread_mutexattr_init(&mutex_attributes) == 0) {
            pthread_mutexattr_settype(&mutex_attributes, PTHREAD_MUTEX_RECURSIVE);
            runtime->execution_mutex_initialized = pthread_mutex_init(&runtime->execution_mutex, &mutex_attributes) == 0;
            pthread_mutexattr_destroy(&mutex_attributes);
        }
        fesetenv(FE_DFL_ENV);
        runtime->jvm_probe = getenv("R2_JVM_PROBE") != NULL;
        const char *sdl3_path = getenv("R2_HOST_SDL3"), *vulkan_path = getenv("R2_HOST_VULKAN");
        runtime->graphics[0].kind = GRAPHICS_SDL3; runtime->graphics[1].kind = GRAPHICS_VULKAN;
        runtime->graphics[2].kind = GRAPHICS_OPENGL;
        runtime->graphics[2].path = strdup("/System/Library/Frameworks/OpenGL.framework/OpenGL");
        if (sdl3_path && sdl3_path[0] == '/') runtime->graphics[0].path = strdup(sdl3_path);
        if (vulkan_path && vulkan_path[0] == '/') runtime->graphics[1].path = strdup(vulkan_path);
        if (runtime->jvm_probe) runtime->cpp = dlopen("/usr/lib/libc++.1.dylib", RTLD_NOW | RTLD_LOCAL);
        runtime->curses = dlopen("/usr/lib/libncurses.5.4.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (runtime == NULL || runtime->root_thread == NULL || !runtime->execution_mutex_initialized || runtime->curses == NULL || runtime->host_cwd < 0) {
        r2_set_error(error, error_len, "cannot initialize host library bridge");
        r2_native_destroy(runtime);
        return NULL;
    }
    runtime->has_terminal = tcgetattr(STDIN_FILENO, &runtime->terminal) == 0;
    return runtime;
}

void r2_native_destroy(r2_native_runtime *runtime)
{
    if (runtime == NULL) return;
    r2_native_stop_and_join(runtime);
    for (size_t i = 0; i < 1024; i++)
        for (unsigned j = 0; j < runtime->ports[i].references; j++)
            mach_port_deallocate(mach_task_self(), runtime->ports[i].port);
    for (size_t i = 0; i < 64; i++) if (runtime->resolver_results[i]) freeaddrinfo(runtime->resolver_results[i]);
    for (size_t i = 0; i < runtime->tls_count; i++) {
                pthread_key_delete(runtime->tls[i].key); free(runtime->tls[i].data);
    }
    if (runtime->host_cwd >= 0) { fchdir(runtime->host_cwd); close(runtime->host_cwd); }
    for (size_t i = 0; i < runtime->vm_mapping_count; i++)
        munmap((void *)(uintptr_t)runtime->vm_mappings[i].start,
               (size_t)(runtime->vm_mappings[i].end - runtime->vm_mappings[i].start));
    for (size_t i = 0; i < runtime->semaphore_count; i++) semaphore_destroy(mach_task_self(), runtime->semaphores[i]);
    for (size_t i = 0; i < runtime->key_count; i++) pthread_key_delete(runtime->keys[i].key);
    for (unsigned i = 1; i < NSIG; i += 1)
        if (runtime->installed_signals[i]) sigaction((int)i, &runtime->original_signals[i], NULL);
    if (signal_runtime == runtime) signal_runtime = NULL;
    if (runtime->has_terminal) {
        int (*endwin_function)(void) = dlsym(runtime->curses, "endwin");
        int (*isendwin_function)(void) = dlsym(runtime->curses, "isendwin");
        if (endwin_function != NULL && isendwin_function != NULL && !isendwin_function())
            (void)endwin_function();
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &runtime->terminal);
    }
    graphics_destroy(runtime);
    if (runtime->system_library) dlclose(runtime->system_library);
    if (runtime->iokit) dlclose(runtime->iokit);
    if (runtime->curses != NULL) dlclose(runtime->curses);
    if (runtime->audio_toolbox != NULL) {
        int (*stop)(void *) = dlsym(runtime->audio_toolbox, "MusicPlayerStop");
        int (*dispose_player)(void *) = dlsym(runtime->audio_toolbox, "DisposeMusicPlayer");
        int (*dispose_sequence)(void *) = dlsym(runtime->audio_toolbox, "DisposeMusicSequence");
        for (size_t i = 0; i < 32; i++) {
            if (runtime->music_players[i] != NULL) {
                if (stop != NULL) stop(runtime->music_players[i]);
                if (dispose_player != NULL) dispose_player(runtime->music_players[i]);
            }
        }
        for (size_t i = 0; i < 32; i++)
            if (runtime->music_sequences[i] != NULL && dispose_sequence != NULL)
                dispose_sequence(runtime->music_sequences[i]);
        dlclose(runtime->audio_toolbox);
    }
    if (runtime->core_graphics != NULL) dlclose(runtime->core_graphics);
    if (runtime->core_audio != NULL) dlclose(runtime->core_audio);
    if (runtime->core_text != NULL) dlclose(runtime->core_text);
    if (runtime->core_foundation != NULL) dlclose(runtime->core_foundation);
    if (runtime->foundation != NULL) dlclose(runtime->foundation);
    if (runtime->system_configuration != NULL) dlclose(runtime->system_configuration);
    if (runtime->objc != NULL) dlclose(runtime->objc);
    if (runtime->core_services != NULL) dlclose(runtime->core_services);
    if (runtime->cf_network != NULL) dlclose(runtime->cf_network);
    if (runtime->cpp != NULL) dlclose(runtime->cpp);
    if (runtime->zlib != NULL) {
        for (size_t i = 0; i < 256; i++) if (runtime->zstreams[i].guest) {
            int (*end)(z_streamp) = dlsym(runtime->zlib, runtime->zstreams[i].deflating ? "deflateEnd" : "inflateEnd");
            if (end != NULL) end(&runtime->zstreams[i].stream);
        }
        dlclose(runtime->zlib);
    }
    if (runtime->sdl != NULL) {
        void (*quit)(void) = dlsym(runtime->sdl, "SDL_Quit");
        if (quit != NULL) quit();
        dlclose(runtime->sdl);
    }
    for (size_t i = 0; i < runtime->thread_count; i++) {
        native_guest_thread *thread = runtime->threads[i];
        if (thread == NULL) continue;
        destroy_thread_state(thread->state); free(thread);
    }
    if (runtime->root_thread != NULL) { destroy_thread_state(runtime->root_thread); }
    if (native_thread == runtime->root_thread) native_thread = runtime->previous_thread;
    if (runtime->execution_mutex_initialized) pthread_mutex_destroy(&runtime->execution_mutex);
    if (runtime->fenv_saved) fesetenv(&runtime->host_fenv);
    if (runtime->signal_mask_saved) pthread_sigmask(SIG_SETMASK, &runtime->host_signal_mask, NULL);
    if (runtime->nofile_saved) setrlimit(RLIMIT_NOFILE, &runtime->host_nofile);
    free(runtime);
}

static uint32_t x86_fp_exceptions(void)
{
    int host = fetestexcept(FE_ALL_EXCEPT);
    return ((host & FE_INVALID) ? 1 : 0) | ((host & FE_DIVBYZERO) ? 4 : 0) |
           ((host & FE_OVERFLOW) ? 8 : 0) | ((host & FE_UNDERFLOW) ? 16 : 0) |
           ((host & FE_INEXACT) ? 32 : 0);
}

static void restore_fp_exceptions(uint32_t x86)
{
    int host = ((x86 & 1) ? FE_INVALID : 0) | ((x86 & 4) ? FE_DIVBYZERO : 0) |
               ((x86 & 8) ? FE_OVERFLOW : 0) | ((x86 & 16) ? FE_UNDERFLOW : 0) |
               ((x86 & 32) ? FE_INEXACT : 0);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(host);
}

void r2_native_get_x87(r2_native_runtime *runtime, uint16_t *control, uint16_t *status)
{
    (void)runtime; *control = native_thread->x87.control; *status = native_thread->x87.status;
}
void r2_native_set_x87(r2_native_runtime *runtime, uint16_t control, uint16_t status)
{
    (void)runtime; native_thread->x87.control = control; native_thread->x87.status = status;
}
r2_x87_state *r2_native_x87_state(r2_native_runtime *runtime)
{
    (void)runtime; return &native_thread->x87;
}
uint32_t r2_native_get_mxcsr(r2_native_runtime *runtime)
{
    (void)runtime; return native_thread->mxcsr | x86_fp_exceptions();
}
void r2_native_set_mxcsr(r2_native_runtime *runtime, uint32_t value)
{
    (void)runtime; native_thread->mxcsr = value; restore_fp_exceptions(value);
}

bool r2_native_library(const char *path)
{
    return strcmp(path, "/usr/lib/libSystem.B.dylib") == 0 ||
           strcmp(path, "/usr/lib/libz.1.dylib") == 0 ||
           strcmp(path, "/usr/lib/libncurses.5.4.dylib") == 0 ||
           strcmp(path, "/opt/homebrew/opt/sdl2/lib/libSDL2.dylib") == 0 ||
           strcmp(path, "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox") == 0 ||
           strcmp(path, "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation") == 0 ||
           (getenv("R2_JVM_PROBE") != NULL &&
            (strcmp(path, "/usr/lib/libc++.1.dylib") == 0 ||
             strcmp(path, "/usr/lib/libobjc.A.dylib") == 0 ||
             strcmp(path, "/System/Library/Frameworks/AudioUnit.framework/Versions/A/AudioUnit") == 0 ||
             strcmp(path, "/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa") == 0 ||
             strcmp(path, "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit") == 0 ||
             strcmp(path, "/System/Library/Frameworks/CoreAudio.framework/Versions/A/CoreAudio") == 0 ||
             strcmp(path, "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices") == 0 ||
             strcmp(path, "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics") == 0 ||
             strcmp(path, "/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText") == 0 ||
             strcmp(path, "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices") == 0 ||
             strcmp(path, "/System/Library/Frameworks/CFNetwork.framework/Versions/A/CFNetwork") == 0 ||
             strcmp(path, "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation") == 0 ||
             strcmp(path, "/System/Library/Frameworks/SystemConfiguration.framework/Versions/A/SystemConfiguration") == 0));
}

void r2_native_add_code(r2_native_runtime *runtime, uint64_t address, uint64_t size)
{
    if (runtime->code_count < NATIVE_MAX_CODE_RANGES && size <= UINT64_MAX - address) {
        runtime->code[runtime->code_count++] = (native_mapping){address, address + size, false};
        forget_mapping_permissions(runtime);
    }
}

static void *cross_region_pointer(r2_native_runtime *runtime, uint64_t address, size_t size, bool write)
{
    uint64_t end = address + size, cursor = address; bool all_writable = true;
    size_t count = runtime->image_region_count ? *runtime->image_region_count : 0;
    for (size_t i = 0; i < count; i++) {
        const r2_memory_region *region = &runtime->image_regions[i];
        if (address < region->base + region->size && end > region->base && !region->writable) {
            if (write) return NULL;
            all_writable = false;
        }
    }
    while (cursor < end) {
        mach_vm_address_t start = cursor; mach_vm_size_t length = 0;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t status = mach_vm_region(mach_task_self(), &start, &length, VM_REGION_BASIC_INFO_64,
                                              (vm_region_info_t)&info, &info_count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (status != KERN_SUCCESS || start > cursor || length == 0 || length > UINT64_MAX - start ||
            !(info.protection & VM_PROT_READ) || (write && !(info.protection & VM_PROT_WRITE))) return NULL;
        all_writable &= (info.protection & VM_PROT_WRITE) != 0;
        cursor = start + length;
    }
    runtime->memory[(address >> 12) & 255] = (native_mapping){address, end, all_writable};
    return (void *)(uintptr_t)address;
}

void *r2_native_pointer(r2_native_runtime *runtime, uint64_t address, size_t size,
                        bool write, char *error, size_t error_len)
{
    if (size == 0 || address > UINT64_MAX - size) goto invalid;
    native_mapping *cached = &runtime->memory[(address >> 12) & 255u];
    if (address >= cached->start && address + size <= cached->end && (!write || cached->writable)) {
        return (void *)(uintptr_t)address;
    }
    size_t image_count = runtime->image_region_count ? *runtime->image_region_count : 0;
    for (size_t i = 0; i < image_count; i++) {
        const r2_memory_region *region = &runtime->image_regions[i];
        if (address < region->base || address - region->base >= region->size) continue;
        if (write && !region->writable) goto invalid;
        if (size > region->size - (address - region->base)) goto crossing;
        *cached = (native_mapping){region->base, region->base + region->size, region->writable};
        return (void *)(uintptr_t)address;
    }
    /* These protections are owned by the guest VM bridge and invalidated by
     * every reserve/commit/protect/unmap operation. A HotSpot heap access need
     * not issue a Mach IPC query to rediscover a mapping we just created. */
    for(size_t i=0;i<runtime->vm_mapping_count;i++) {
        const native_vm_mapping *mapping=&runtime->vm_mappings[i];
        if(address<mapping->start||address>=mapping->end)continue;
        if(!(mapping->protection&PROT_READ)||(write&&!(mapping->protection&PROT_WRITE)))goto invalid;
        if(address+size>mapping->end)goto crossing;
        *cached=(native_mapping){mapping->start,mapping->end,(mapping->protection&PROT_WRITE)!=0};
        return (void *)(uintptr_t)address;
    }
    mach_vm_address_t start = address;
    mach_vm_size_t length = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t status = mach_vm_region(mach_task_self(), &start, &length, VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (status != KERN_SUCCESS || start > address || length > UINT64_MAX - start ||
        !(info.protection & VM_PROT_READ) ||
        (write && !(info.protection & VM_PROT_WRITE))) goto invalid;
    if (address + size > start + length) goto crossing;
    uint64_t end = start + length;
    /* A native malloc region can also contain guest images. Do not let a
     * cached writable heap span cross a read-only guest segment. */
    for (size_t i = 0; i < image_count; i++) {
        const r2_memory_region *region = &runtime->image_regions[i];
        uint64_t limit = region->base + region->size;
        if (limit <= address && limit > start) start = limit;
        if (region->base > address && region->base < end) end = region->base;
    }
    if (address + size > end) goto crossing;
    *cached = (native_mapping){start, end, (info.protection & VM_PROT_WRITE) != 0};
    return (void *)(uintptr_t)address;
crossing: {
    void *pointer = cross_region_pointer(runtime, address, size, write);
    if (pointer != NULL) return pointer;
}
invalid:
    r2_set_error(error, error_len, "host-backed guest address 0x%llx (%zu bytes) is not %s",
                 (unsigned long long)address, size, write ? "writable" : "readable");
    return NULL;
}

static size_t executable_span(r2_native_runtime *runtime, uint64_t address, bool *dynamic)
{
    if (address >= runtime->executable_cache.start && address < runtime->executable_cache.end) {
        uint64_t remaining = runtime->executable_cache.end - address;
        *dynamic = true; return remaining < 15 ? (size_t)remaining : 15;
    }
    for (size_t i = 0; i < runtime->vm_mapping_count; i++) {
        const native_vm_mapping *m = &runtime->vm_mappings[i];
        if ((m->protection & PROT_EXEC) && address >= m->start && address < m->end) {
            runtime->executable_cache = (native_mapping){m->start, m->end, false};
            uint64_t remaining = m->end - address;
            *dynamic = true; return remaining < 15 ? (size_t)remaining : 15;
        }
    }
    for (size_t i = 0; i < runtime->code_count; i++) {
        if (address >= runtime->code[i].start && address < runtime->code[i].end) {
            uint64_t remaining = runtime->code[i].end - address;
            *dynamic = false; return remaining < 15 ? (size_t)remaining : 15;
        }
    }
    return 0;
}

static bool instruction_matches(const uint8_t *code, const r2_insn *instruction)
{
    /* Instructions are at most 15 bytes. Constant-size copies inline into
     * unaligned loads and never read beyond the validated instruction span. */
    size_t length = instruction->x86_len;
    const uint8_t *saved = instruction->encoding;
    if (length >= 8) {
        uint64_t first, last, saved_first, saved_last;
        memcpy(&first, code, 8); memcpy(&last, code + length - 8, 8);
        memcpy(&saved_first, saved, 8); memcpy(&saved_last, saved + length - 8, 8);
        return first == saved_first && last == saved_last;
    }
    if (length >= 4) {
        uint32_t first, last, saved_first, saved_last;
        memcpy(&first, code, 4); memcpy(&last, code + length - 4, 4);
        memcpy(&saved_first, saved, 4); memcpy(&saved_last, saved + length - 4, 4);
        return first == saved_first && last == saved_last;
    }
    if (length >= 2) {
        uint16_t first, last, saved_first, saved_last;
        memcpy(&first, code, 2); memcpy(&last, code + length - 2, 2);
        memcpy(&saved_first, saved, 2); memcpy(&saved_last, saved + length - 2, 2);
        return first == saved_first && last == saved_last;
    }
    return length == 1 && code[0] == saved[0];
}

r2_status r2_native_decode_next(r2_block *block,uint64_t address,size_t hint,size_t *pc,char *error,size_t error_len)
{
    if(hint<block->count) {
        const r2_insn *cached=&block->insns[hint];
        if(cached->x86_offset==address) {
            bool dynamic=false;
            size_t span=cached->dynamic_code?executable_span(block->runtime,address,&dynamic):15;
            /* Retain code-byte and current-execute-permission checks. Only
             * the hash lookup is skipped for a known sequential successor. */
            if(cached->x86_len<=span&&instruction_matches((const void *)(uintptr_t)address,cached)) {*pc=hint;return R2_OK;}
        }
    }
    return r2_native_decode(block,address,pc,error,error_len);
}

r2_status r2_native_decode(r2_block *block, uint64_t address, size_t *pc,
                           char *error, size_t error_len)
{
    r2_native_runtime *runtime = block->runtime;
    size_t capacity = native_thread->hash_capacity;
    while (block->count >= capacity / 2) {
        if(capacity>SIZE_MAX/(2*sizeof(size_t))) {
            r2_set_error(error,error_len,"instruction lookup table size overflows");return R2_ERR_NO_MEMORY;
        }
        capacity *= 2;
    }
    if (native_thread->hash_block != block || block->count == 0 || capacity != native_thread->hash_capacity) {
        if (capacity != native_thread->hash_capacity) {
            size_t *hash = calloc(capacity, sizeof(*hash));
            if (hash == NULL) return R2_ERR_NO_MEMORY;
            free(native_thread->hash); native_thread->hash = hash; native_thread->hash_capacity = capacity;
        } else memset(native_thread->hash, 0, capacity * sizeof(size_t));
        for (size_t i = 0; i < block->count; i++) {
            uint64_t key = block->insns[i].x86_offset;
            size_t slot = (size_t)((key ^ (key >> 19)) * UINT64_C(11400714819323198485)) & (capacity - 1);
            while (native_thread->hash[slot]) slot = (slot + 1) & (capacity - 1);
            native_thread->hash[slot] = i + 1;
        }
        native_thread->hash_block = block;
    }
    size_t slot = (size_t)((address ^ (address >> 19)) * UINT64_C(11400714819323198485)) & (capacity - 1);
    size_t existing = SIZE_MAX;
    while (native_thread->hash[slot] != 0) {
        size_t index = native_thread->hash[slot] - 1u;
        if (index >= block->count) {
            r2_set_error(error, error_len, "native instruction cache belongs to another block");
            return R2_ERR_UNSUPPORTED;
        }
        if (block->insns[index].x86_offset == address) { existing = index; break; }
        slot = (slot + 1u) & (capacity - 1u);
    }
    bool dynamic = false;
    size_t length;
    if (existing != SIZE_MAX && !block->insns[existing].dynamic_code) {
        /* Pinned image/import code cannot be unmapped by guest VM operations. */
        const r2_insn *cached = &block->insns[existing];
        if (instruction_matches((const void *)(uintptr_t)address, cached)) {
            *pc = existing; return R2_OK;
        }
    }
    length = executable_span(runtime, address, &dynamic);
    if (length == 0) {
        r2_set_error(error, error_len, "guest execution outside loaded code at 0x%llx", (unsigned long long)address);
        return R2_ERR_DECODE;
    }
    if (existing != SIZE_MAX) {
        const r2_insn *cached = &block->insns[existing];
        if (cached->x86_len <= length && instruction_matches((const void *)(uintptr_t)address, cached)) {
            block->insns[existing].dynamic_code = dynamic;
            *pc = existing; return R2_OK;
        }
    }
    r2_insn insn;
    r2_status status = r2_decode_insn_at((const uint8_t *)(uintptr_t)address, length, address,
                                       &insn, error, error_len);
    if (status != R2_OK) {
        char detail[R2_ERROR_CAP];
        snprintf(detail, sizeof(detail), "%s", error);
        char bytes[46] = {0};
        for (size_t i = 0; i < length; i++)
            snprintf(bytes + i * 3, sizeof(bytes) - i * 3, "%02x ", ((const uint8_t *)(uintptr_t)address)[i]);
        r2_set_error(error, error_len, "RIP=0x%llx: %s [%s]", (unsigned long long)address, detail, bytes);
        return status;
    }
    memcpy(insn.encoding, (const void *)(uintptr_t)address, insn.x86_len);
    insn.dynamic_code = dynamic;
    if (existing != SIZE_MAX) { block->insns[existing] = insn; *pc = existing; return R2_OK; }
    if (block->count == block->capacity) {
        if(block->capacity>SIZE_MAX/(2*sizeof(r2_insn))) {
            r2_set_error(error,error_len,"decoded instruction storage size overflows");return R2_ERR_NO_MEMORY;
        }
        size_t capacity = block->capacity * 2;
        r2_insn *data = malloc(capacity * sizeof(*data));
        if (data == NULL) return R2_ERR_NO_MEMORY;
        memcpy(data, block->insns, block->count * sizeof(*data));
        if (block->insns != block->storage) free(block->insns);
        block->insns = data; block->capacity = capacity;
    }
    *pc = block->count;
    block->insns[block->count++] = insn;
    native_thread->hash[slot] = *pc + 1u;
    return R2_OK;
}

r2_status r2_native_symbol(r2_native_runtime *runtime, const char *symbol,
                           uint64_t *address, char *error, size_t error_len)
{
    const char *name = symbol[0] == '_' ? symbol + 1 : symbol;
    /* All lazy slots are bound eagerly; the Mach-O helper is never entered. */
    if (strcmp(symbol, "dyld_stub_binder") == 0) { *address = 0; return R2_OK; }
    const char *data_symbols[] = {"COLS", "LINES", "curscr", "stdscr", "_DefaultRuneLocale",
                                  "__stderrp", "__stdoutp", "__stdinp", "__mb_cur_max", "optind", "optarg", "mach_task_self_", "__stack_chk_guard", NULL};
    for (size_t i = 0; data_symbols[i] != NULL; i += 1) {
        if (strcmp(name, data_symbols[i]) == 0) {
            void *data = dlsym(runtime->curses, name);
            if (data == NULL) data = dlsym(RTLD_DEFAULT, name);
            if (data != NULL) { *address = (uintptr_t)data; return R2_OK; }
        }
    }
    if (runtime->jvm_probe && strcmp(name, "_ZSt7nothrow") == 0) {
        void *data = dlsym(RTLD_DEFAULT, name);
        if (data == NULL && runtime->cpp != NULL) data = dlsym(runtime->cpp, name);
        if (data != NULL) { *address = (uintptr_t)data; return R2_OK; }
    }
    if (runtime->jvm_probe && (strcmp(name, "CGAffineTransformIdentity") == 0 || strncmp(name, "kCT", 3) == 0 || strncmp(name, "kUT", 3) == 0 || strncmp(name, "kCF", 3) == 0 || strncmp(name, "kSC", 3) == 0 ||
        strncmp(name, "OBJC_CLASS_$_", 13) == 0 || strncmp(name,"OBJC_METACLASS_$_",17)==0 || strncmp(name,"OBJC_EHTYPE_$_",14)==0 || strcmp(name,"_objc_empty_cache")==0 || strcmp(name, "__CFConstantStringClassReference") == 0)) {
        void **library;
        const char *path;
        if (strcmp(name, "CGAffineTransformIdentity") == 0) {
            library = &runtime->core_graphics; path = "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
        } else if (strncmp(name, "kCT", 3) == 0) {
            library = &runtime->core_text; path = "/System/Library/Frameworks/CoreText.framework/CoreText";
        } else if (strncmp(name, "kUT", 3) == 0) {
            library = &runtime->core_services; path = "/System/Library/Frameworks/CoreServices.framework/CoreServices";
        } else if (strncmp(name, "kCFProxy", 8) == 0) {
            library = &runtime->cf_network; path = "/System/Library/Frameworks/CFNetwork.framework/CFNetwork";
        } else if (strncmp(name, "kSC", 3) == 0) {
            library = &runtime->system_configuration;
            path = "/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration";
        } else if (strncmp(name, "OBJC_CLASS_$_", 13) == 0 || strncmp(name,"OBJC_METACLASS_$_",17)==0 || strncmp(name,"OBJC_EHTYPE_$_",14)==0) {
            library = &runtime->foundation; path = "/System/Library/Frameworks/Foundation.framework/Foundation";
        } else {
            library = &runtime->core_foundation; path = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
        }
        if (*library == NULL) *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        void *data = *library != NULL ? dlsym(*library, name) : NULL;
        if (data != NULL) { *address = (uintptr_t)data; return R2_OK; }
        r2_set_error(error, error_len, "cannot resolve framework data %s", name); return R2_ERR_UNSUPPORTED;
    }
    for (size_t i = 0; i < runtime->import_count; i += 1) {
        if (strcmp(name, runtime->imports[i].definition->name) == 0 &&
            (runtime->override_definition ? runtime->imports[i].definition == runtime->override_definition : runtime->imports[i].graphics_kind == 0) &&
            (!runtime->override_name || strcmp(name, runtime->override_name) != 0 || runtime->imports[i].function == runtime->override_function)) {
            *address = (uintptr_t)runtime->imports[i].stub;
            return R2_OK;
        }
    }
    const native_definition *definition = runtime->override_definition;
    for (size_t i = 0; definition == NULL && i < sizeof(definitions) / sizeof(definitions[0]); i += 1) {
        if (strcmp(name, definitions[i].name) == 0) { definition = &definitions[i]; break; }
    }
    if (definition == NULL && runtime->jvm_probe) {
        for (size_t i = 0; i < sizeof(jvm_probe_definitions) / sizeof(jvm_probe_definitions[0]); i++)
            if (strcmp(name, jvm_probe_definitions[i].name) == 0) { definition = &jvm_probe_definitions[i]; break; }
    }
    if (definition == NULL || runtime->import_count >= NATIVE_MAX_IMPORTS) {
        r2_set_error(error, error_len, "host bridge has no ABI declaration for %s", symbol);
        return R2_ERR_UNSUPPORTED;
    }
    void *function = runtime->override_name && strcmp(name, runtime->override_name) == 0 ? runtime->override_function : NULL;
    if (function != NULL) { /* Scoped native graphics/framework function. */ }
    else if (strncmp(name, "inflate", 7) == 0 || strncmp(name, "deflate", 7) == 0 || strcmp(name, "crc32") == 0 || strcmp(name, "adler32") == 0) {
        if (runtime->zlib == NULL) runtime->zlib = dlopen("/usr/lib/libz.1.dylib", RTLD_NOW | RTLD_LOCAL);
        if (runtime->zlib != NULL) function = dlsym(runtime->zlib, name);
    } else if (strncmp(name, "SDL_", 4) == 0) {
        if (runtime->sdl == NULL)
            runtime->sdl = dlopen("/opt/homebrew/opt/sdl2/lib/libSDL2.dylib", RTLD_NOW | RTLD_LOCAL);
        if (runtime->sdl != NULL) function = dlsym(runtime->sdl, name);
    } else if (strncmp(name, "IO", 2) == 0) {
        if (runtime->iokit == NULL)
            runtime->iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
        if (runtime->iokit != NULL) function = dlsym(runtime->iokit, name);
    } else if (strncmp(name, "CFNetwork", 9) == 0) {
        if (runtime->cf_network == NULL)
            runtime->cf_network = dlopen("/System/Library/Frameworks/CFNetwork.framework/CFNetwork", RTLD_NOW | RTLD_LOCAL);
        if (runtime->cf_network != NULL) function = dlsym(runtime->cf_network, name);
    } else if (strncmp(name, "CF", 2) == 0) {
        if (runtime->core_foundation == NULL)
            runtime->core_foundation = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW | RTLD_LOCAL);
        if (runtime->core_foundation != NULL) function = dlsym(runtime->core_foundation, name);
    } else if (strncmp(name, "UTType", 6) == 0) {
        if (runtime->core_services == NULL)
            runtime->core_services = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW | RTLD_LOCAL);
        if (runtime->core_services != NULL) function = dlsym(runtime->core_services, name);
    } else if (strncmp(name, "CG", 2) == 0 || strncmp(name, "CT", 2) == 0) {
        void **library = name[1] == 'G' ? &runtime->core_graphics : &runtime->core_text;
        if (*library == NULL) *library = dlopen(name[1] == 'G' ? "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics" : "/System/Library/Frameworks/CoreText.framework/CoreText", RTLD_NOW | RTLD_LOCAL);
        if (*library != NULL) function = dlsym(*library, name);
    } else if (strncmp(name, "NS", 2) == 0 || strncmp(name, "objc_", 5) == 0) {
        if (runtime->foundation == NULL)
            runtime->foundation = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW | RTLD_LOCAL);
        if (runtime->foundation != NULL) function = dlsym(runtime->foundation, name);
        if (function == NULL && strncmp(name, "objc_", 5) == 0) {
            if (runtime->objc == NULL) runtime->objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
            if (runtime->objc != NULL) function = dlsym(runtime->objc, name);
        }
    } else if (strncmp(name, "SC", 2) == 0) {
        if (runtime->system_configuration == NULL)
            runtime->system_configuration = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_NOW | RTLD_LOCAL);
        if (runtime->system_configuration != NULL) function = dlsym(runtime->system_configuration, name);
    } else if (strncmp(name, "AudioObject", 11) == 0) {
        if (runtime->core_audio == NULL)
            runtime->core_audio = dlopen("/System/Library/Frameworks/CoreAudio.framework/CoreAudio", RTLD_NOW | RTLD_LOCAL);
        if (runtime->core_audio != NULL) function = dlsym(runtime->core_audio, name);
    } else if (strstr(name, "Music") == name || strstr(name, "NewMusic") == name ||
               strstr(name, "DisposeMusic") == name || strncmp(name, "AUGraph", 7) == 0 ||
               strncmp(name, "Audio", 5) == 0) {
        if (runtime->audio_toolbox == NULL)
            runtime->audio_toolbox = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW | RTLD_LOCAL);
        if (runtime->audio_toolbox != NULL) function = dlsym(runtime->audio_toolbox, name);
    } else function = dlsym(runtime->curses, name);
    if (function == NULL) function = dlsym(RTLD_DEFAULT, name);
    if(function==NULL&&strcmp(name,"__cxa_demangle")==0&&!runtime->cpp)
        runtime->cpp=dlopen("/usr/lib/libc++.1.dylib",RTLD_NOW|RTLD_LOCAL);
    if (function == NULL && runtime->cpp != NULL) function = dlsym(runtime->cpp, name);
    if (function == NULL && strcmp(name, "__exp10") == 0) function = (void *)r2_host_exp10;
    if (function == NULL && strchr(name, '$') != NULL) {
        char base[128];
        snprintf(base, sizeof(base), "%.*s", (int)(strchr(name, '$') - name), name);
        function = dlsym(RTLD_DEFAULT, base);
    }
    bool guest_only = strcmp(name, "setjmp") == 0 || strcmp(name, "_setjmp") == 0 ||
                      strcmp(name, "longjmp") == 0 || strcmp(name, "_longjmp") == 0 ||
                      strcmp(name, "__cxa_pure_virtual") == 0 || strcmp(name, "objc_msgSend_stret") == 0 || strcmp(name,"objc_msgSend_fpret")==0 ||
                      strcmp(name,"__divti3")==0 || strcmp(name,"__udivti3")==0 || strcmp(name,"__muldc3")==0 ||
                      strcmp(name, "__r2_once_complete") == 0 || strcmp(name,"__r2_suspend_complete")==0 || strcmp(name,"sigsetjmp")==0 || strcmp(name,"siglongjmp")==0;
    if (runtime->override_name && strcmp(name, runtime->override_name) == 0) function = runtime->override_function;
    if (function == NULL && strcmp(name, "___chkstk_darwin") != 0 && !guest_only && definition->result != '!') {
        r2_set_error(error, error_len, "host bridge cannot resolve %s: %s", symbol, dlerror());
        return R2_ERR_UNSUPPORTED;
    }
    size_t index = runtime->import_count++;
    native_import *import = &runtime->imports[index];
    import->definition = definition;
    import->function = function; import->graphics_kind = runtime->override_graphics_kind;
    uint32_t number = (uint32_t)(NATIVE_CALL_BASE + index);
    unsigned offset = 0;
    if (strcmp(name, "___chkstk_darwin") == 0) {
        import->stub[0] = 0x49; import->stub[1] = 0x89; import->stub[2] = 0xc3;
        offset = 3; /* preserve the stack-probe input RAX in caller-saved R11 */
    }
    import->stub[offset] = 0xb8;
    memcpy(import->stub + offset + 1, &number, 4);
    import->stub[offset + 5] = 0x0f; import->stub[offset + 6] = 0x05; import->stub[offset + 7] = 0xc3;
    *address = (uintptr_t)import->stub;
    r2_native_add_code(runtime, *address, offset + 8u);
    return R2_OK;
}

bool r2_native_is_call(uint64_t number)
{
    return number >= NATIVE_CALL_BASE && number < NATIVE_CALL_BASE + NATIVE_MAX_IMPORTS;
}

#include "graphics.inc"
#include "frameworks.inc"

static ffi_type *argument_type(char type)
{
    switch (type) {
    case 'v': return &ffi_type_void;
    case 'p': return &ffi_type_pointer;
    case 'i': return &ffi_type_sint32;
    case 'l': return &ffi_type_sint64;
    case 'd': return &ffi_type_double;
    case 'f': return &ffi_type_float;
    case 'b': return &ffi_type_uint8;
    case 'c': return &ffi_type_sint8;
    case 's': return &ffi_type_sint16;
    case 'h': return &ffi_type_uint16;
    default: return &ffi_type_uint64;
    }
}

static uint64_t guest_argument(const uint64_t regs[16], size_t index)
{
    const unsigned registers[] = {R2_X86_RDI, R2_X86_RSI, R2_X86_RDX, R2_X86_RCX, R2_X86_R8, R2_X86_R9};
    if (index < 6) return regs[registers[index]];
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)(regs[R2_X86_RSP] + 8u + (index - 6u) * 8u), 8);
    return value;
}

static bool printf_types(const char *format, char *types, size_t *count)
{
    for (const char *p = format; *p != '\0'; p += 1) {
        if (*p != '%') continue;
        p += 1;
        if (*p == '%') continue;
        while (*p && strchr("#0- +'", *p)) p += 1;
        if (*p == '*') { if (*count >= NATIVE_MAX_ARGS) return false; types[(*count)++] = 'i'; p += 1; }
        else while (isdigit((unsigned char)*p)) p += 1;
        if (*p == '.') {
            p += 1;
            if (*p == '*') { if (*count >= NATIVE_MAX_ARGS) return false; types[(*count)++] = 'i'; p += 1; }
            else while (isdigit((unsigned char)*p)) p += 1;
        }
        char type = 'i';
        if (*p == 'l' || *p == 'z' || *p == 't' || *p == 'j') {
            type = 'l'; if (*p == 'l' && p[1] == 'l') p += 1; p += 1;
        } else if (*p == 'h') { p += 1; if (*p == 'h') p += 1; }
        if (*p && strchr("fFeEgGaA", *p) != NULL) type = 'd';
        else if (*p == 'c') type = 'i';
        if (*p == 's' || *p == 'p' || *p == 'n' || *p == '@') type = 'p';
        else if (!*p || strchr("diuoxXcfFeEgGaA", *p) == NULL) return false;
        if (*count >= NATIVE_MAX_ARGS) return false;
        types[(*count)++] = type;
    }
    return true;
}

static void remember_mutex(pthread_mutex_t *mutex)
{
    for (size_t i = 0; i < 128; i++)
        if (native_thread->mutexes[i].mutex == mutex) { native_thread->mutexes[i].count++; return; }
    for (size_t i = 0; i < 128; i++)
        if (native_thread->mutexes[i].mutex == NULL) {
            native_thread->mutexes[i].mutex = mutex; native_thread->mutexes[i].count = 1; return;
        }
}
static void forget_mutex(pthread_mutex_t *mutex)
{
    for (size_t i = 0; i < 128; i++)
        if (native_thread->mutexes[i].mutex == mutex) {
            if (--native_thread->mutexes[i].count == 0) native_thread->mutexes[i].mutex = NULL;
            return;
        }
}
static void abandon_mutexes(void)
{
    for (size_t i = 0; i < 128; i++) {
        while (native_thread->mutexes[i].count) {
            pthread_mutex_unlock(native_thread->mutexes[i].mutex);
            native_thread->mutexes[i].count--;
        }
        native_thread->mutexes[i].mutex = NULL;
    }
}
static void free_guest_block(r2_block *block)
{
    if (native_thread->hash_block == block) native_thread->hash_block = NULL;
    if (block->insns != block->storage) free(block->insns);
    free(block);
}
static r2_status run_thread_function(native_guest_thread *thread, uint64_t function, uint64_t argument,
                                      r2_result *result, char *error, size_t error_len)
{
    r2_block *block = calloc(1, sizeof(*block));
    if (block == NULL) return R2_ERR_NO_MEMORY;
    block->insns = block->storage; block->capacity = R2_MAX_INSNS; block->runtime = thread->runtime;
    r2_interp_env env = thread->env;
    env.has_initial_regs = true; memset(env.initial_regs, 0, sizeof(env.initial_regs));
    env.initial_regs[R2_X86_RDI] = argument;
    env.has_initial_rip = true; env.initial_rip = function; env.stack_size = thread->stack_size;
    if(env.borrowed_stack_base)env.initial_regs[R2_X86_RSP]=env.borrowed_stack_base+env.stack_size-8;
    r2_status status = r2_interp_block_with_env(block, &env, result, error, error_len);
    free_guest_block(block);
    return status;
}
static void *guest_thread_start(void *opaque)
{
    native_guest_thread *thread = opaque;
    r2_native_runtime *runtime = thread->runtime;
    native_thread = thread->state;
    native_thread->host=pthread_self();
    char error[R2_ERROR_CAP] = {0}; r2_result result = {0};
    engine_enter(runtime);
    void *stack=r2_native_stack_allocate(runtime,thread->stack_size);
    thread->env.borrowed_stack_base=(uintptr_t)stack;
    engine_leave(runtime);
    r2_status status = stack?run_thread_function(thread, thread->entry, thread->argument, &result, error, sizeof(error)):R2_ERR_NO_MEMORY;
    engine_enter(runtime);
    thread->result = result.rax;
    while(native_thread->tlv_exit_count&&status==R2_OK&&!r2_native_stopping(runtime)) {
        size_t index=--native_thread->tlv_exit_count;native_thread->thread_returning=false;
        status=run_thread_function(thread,native_thread->tlv_exits[index].function,native_thread->tlv_exits[index].argument,&result,error,sizeof(error));
    }
    /* POSIX destructors are guest functions and may reinstall a value. Never
     * hand their x86 address to the native pthread runtime. */
    for (unsigned pass = 0; pass < PTHREAD_DESTRUCTOR_ITERATIONS && status == R2_OK && !r2_native_stopping(runtime); pass++) {
        bool invoked = false;
        for (size_t i = 0; i < runtime->key_count; i++) {
            uint64_t function = runtime->keys[i].destructor;
            void *value = native_thread->key_values[i];
            if (value == NULL || function == 0) continue;
            native_thread->key_values[i]=NULL; invoked = true;
            native_thread->thread_returning = false;
            status = run_thread_function(thread, function, (uintptr_t)value, &result, error, sizeof(error));
            if (status != R2_OK || r2_native_stopping(runtime)) break;
        }
        if (!invoked) break;
    }
    if (status != R2_OK) r2_native_request_stop(runtime, status, error);
    if (r2_native_stopping(runtime)) abandon_mutexes();
    if(stack&&!r2_native_stopping(runtime))r2_native_stack_release(runtime,stack,thread->stack_size);
    atomic_store_explicit(&thread->done, true, memory_order_release);
    engine_leave(runtime);
    native_thread = NULL;
    return NULL;
}
#include "callbacks.inc"
#include "sort_callbacks.inc"
#include "audio_callbacks.inc"
#include "vulkan_callbacks.inc"
#include "font_callbacks.inc"
#include "signal_calls.inc"

static r2_status once_call(r2_native_runtime *runtime, const char *name, uint64_t regs[16],
                            char *error, size_t error_len)
{
    _Static_assert(sizeof(pthread_once_t) == 16, "Darwin 64-bit pthread_once layout");
    if (strcmp(name, "__r2_once_complete") == 0) {
        if (!native_thread->once_depth) goto invalid;
        size_t frame = native_thread->once_depth - 1;
        uint64_t stack = native_thread->once[frame].stack;
        if (regs[R2_X86_RSP] != stack - 8) goto invalid;
        void *state = r2_native_pointer(runtime, native_thread->once[frame].control + 8, 8, true, error, error_len);
        if (state == NULL) return R2_ERR_UNSUPPORTED;
        uint64_t done = 2; memcpy(state, &done, 8);
        native_thread->once_depth--; regs[R2_X86_RSP] = stack;
        regs[R2_X86_RAX] = 0; return R2_OK;
    }
    uint64_t control = regs[R2_X86_RDI];
    if (control & 7) goto invalid;
    for (;;) {
        const uint8_t *object = r2_native_pointer(runtime, control, 16, true, error, error_len);
        if (!object) return R2_ERR_UNSUPPORTED;
        uint64_t signature, state;
        memcpy(&signature, object, 8); memcpy(&state, object + 8, 8);
        /* The public initializer signature is identical on Darwin x86_64 and
         * arm64. The opaque state belongs to the virtual guest pthread API. */
        if (signature != UINT64_C(0x30b1bcba) || state > 2) goto invalid;
        if (state == 2) { regs[R2_X86_RAX] = 0; return R2_OK; }
        if (state == 0) break;
        for (size_t i = 0; i < native_thread->once_depth; i++)
            if (native_thread->once[i].control == control) {
                regs[R2_X86_RAX] = EDEADLK; return R2_OK;
            }
        bool owned = false;
        native_thread_state *owner = runtime->root_thread;
        for (size_t t = 0; t <= runtime->thread_count; t++) {
            if (t) owner = runtime->threads[t - 1] ? runtime->threads[t - 1]->state : NULL;
            if (owner) for (size_t i = 0; i < owner->once_depth; i++)
                if (owner->once[i].control == control) owned = true;
        }
        for(size_t t=0;t<64;t++)if(runtime->callback_threads[t])
            for(size_t i=0;i<runtime->callback_threads[t]->once_depth;i++)
                if(runtime->callback_threads[t]->once[i].control==control)owned=true;
        if (!owned) goto invalid;
        unsigned depth = engine_suspend(runtime);
        struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
        engine_resume(runtime, depth);
        if (r2_native_stopping(runtime)) return r2_native_execution_poll(runtime, error, error_len);
    }
    if (native_thread->once_depth == 64 || !native_thread->execution.block || regs[R2_X86_RSP] < 24) goto invalid;
    uint64_t complete; size_t pc;
    r2_status status = r2_native_decode(native_thread->execution.block, regs[R2_X86_RSI], &pc, error, error_len);
    if (status != R2_OK) return status;
    status = r2_native_symbol(runtime, "___r2_once_complete", &complete, error, error_len);
    if (status != R2_OK) return status;
    void *stack = r2_native_pointer(runtime, regs[R2_X86_RSP] - 24, 24, true, error, error_len);
    void *state = r2_native_pointer(runtime, control + 8, 8, true, error, error_len);
    if (!stack || !state) return R2_ERR_UNSUPPORTED;
    uint64_t continuation[3] = {regs[R2_X86_RSI], complete, 0};
    memcpy(stack, continuation, sizeof(continuation));
    size_t frame = native_thread->once_depth++;
    native_thread->once[frame].control = control; native_thread->once[frame].stack = regs[R2_X86_RSP];
    uint64_t running = 1; memcpy(state, &running, 8);
    /* The import stub's RET enters the guest initializer on this same stack.
     * Its return enters our completion stub, which restores the caller's RSP.
     * Three slots retain the SysV 16-byte call alignment at initializer entry. */
    regs[R2_X86_RSP] -= 24; regs[R2_X86_RAX] = 0; return R2_OK;
invalid:
    r2_set_error(error, error_len, "invalid guest pthread_once state or continuation"); return R2_ERR_UNSUPPORTED;
}

static r2_status thread_call(r2_native_runtime *runtime, const char *name, uint64_t regs[16],
                             bool *exited, char *error, size_t error_len)
{
    if (strcmp(name, "pthread_exit") == 0) {
        while (native_thread->once_depth) {
            uint64_t zero = 0, control = native_thread->once[--native_thread->once_depth].control;
            void *state = r2_native_pointer(runtime, control + 8, 8, true, error, error_len);
            if (state == NULL) return R2_ERR_UNSUPPORTED;
            memcpy(state, &zero, 8);
        }
        native_thread->thread_returning = true;
        regs[R2_X86_RAX] = regs[R2_X86_RDI]; *exited = true; return R2_OK;
    }
    if (strcmp(name, "pthread_create") == 0) {
        void *output = r2_native_pointer(runtime, regs[R2_X86_RDI], sizeof(pthread_t), true, error, error_len);
        if (output == NULL) return R2_ERR_UNSUPPORTED;
        if (native_thread->execution.env == NULL || r2_native_stopping(runtime)) {
            regs[R2_X86_RAX] = EAGAIN; return R2_OK;
        }
        size_t slot = runtime->thread_count;
        for (size_t i = 0; i < runtime->thread_count; i++) {
            native_guest_thread *old = runtime->threads[i];
            if (old == NULL) { slot = i; break; }
            if (!old->joining && (old->joined || (old->detached && atomic_load_explicit(&old->done, memory_order_acquire)))) {
                if (!old->joined) pthread_join(old->host, NULL);
                destroy_thread_state(old->state); free(old);
                runtime->threads[i] = NULL; slot = i; break;
            }
        }
        if (slot == 256) { regs[R2_X86_RAX] = EAGAIN; return R2_OK; }
        size_t stack_size = 512 * 1024; int detached = PTHREAD_CREATE_JOINABLE, result = 0;
        if (regs[R2_X86_RSI]) {
            pthread_attr_t *attr = r2_native_pointer(runtime, regs[R2_X86_RSI], sizeof(*attr), false, error, error_len);
            if (attr == NULL) return R2_ERR_UNSUPPORTED;
            result = pthread_attr_getstacksize(attr, &stack_size);
            if (result == 0) result = pthread_attr_getdetachstate(attr, &detached);
        }
        native_guest_thread *thread = result == 0 ? calloc(1, sizeof(*thread)) : NULL;
        if (thread == NULL) { regs[R2_X86_RAX] = result ? (uint32_t)result : EAGAIN; return R2_OK; }
        thread->runtime = runtime; thread->env = *native_thread->execution.env;
        thread->entry = regs[R2_X86_RDX]; thread->argument = regs[R2_X86_RCX];
        thread->stack_size = (stack_size + (size_t)getpagesize() - 1) & ~((size_t)getpagesize() - 1);
        thread->detached = detached == PTHREAD_CREATE_DETACHED;
        thread->state = allocate_thread_state(runtime);
        if (thread->state == NULL) { free(thread); regs[R2_X86_RAX] = EAGAIN; return R2_OK; }
        thread->state->signal_mask=native_thread->execution.signal_mask?*native_thread->execution.signal_mask:native_thread->signal_mask;
        pthread_attr_t host_attr; pthread_attr_init(&host_attr);
        /* The ARM interpreter has a separate, larger host stack. */
        pthread_attr_setstacksize(&host_attr, 8 * 1024 * 1024);
        result = pthread_create(&thread->host, &host_attr, guest_thread_start, thread);
        pthread_attr_destroy(&host_attr);
        if (result != 0) { destroy_thread_state(thread->state); free(thread); }
        else { runtime->threads[slot] = thread; if (slot == runtime->thread_count) runtime->thread_count++; memcpy(output, &thread->host, sizeof(thread->host)); }
        regs[R2_X86_RAX] = (uint32_t)result; return R2_OK;
    }
    native_guest_thread *thread = NULL;
    for (size_t i = 0; i < runtime->thread_count; i++)
        if (runtime->threads[i] != NULL && (uintptr_t)runtime->threads[i]->host == regs[R2_X86_RDI]) { thread = runtime->threads[i]; break; }
    if (thread == NULL) { regs[R2_X86_RAX] = ESRCH; return R2_OK; }
    if (thread->joined || thread->detached || thread->joining) { regs[R2_X86_RAX] = EINVAL; return R2_OK; }
    if (strcmp(name, "pthread_detach") == 0) { thread->detached = true; regs[R2_X86_RAX] = 0; return R2_OK; }
    if (pthread_equal(thread->host, pthread_self())) { regs[R2_X86_RAX] = EDEADLK; return R2_OK; }
    void *output = regs[R2_X86_RSI] ? r2_native_pointer(runtime, regs[R2_X86_RSI], 8, true, error, error_len) : NULL;
    if (regs[R2_X86_RSI] && output == NULL) return R2_ERR_UNSUPPORTED;
    thread->joining = true;
    unsigned depth = engine_suspend(runtime);
    while (!atomic_load_explicit(&thread->done, memory_order_acquire) && !r2_native_stopping(runtime)) {
        struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
    }
    bool reaped = atomic_load_explicit(&thread->done, memory_order_acquire);
    int join_result = reaped ? pthread_join(thread->host, NULL) : 0;
    engine_resume(runtime, depth);
    thread->joining = false;
    if (reaped && join_result == 0) thread->joined = true;
    if (r2_native_stopping(runtime)) return r2_native_execution_poll(runtime, error, error_len);
    if (join_result != 0) { regs[R2_X86_RAX] = (uint32_t)join_result; return R2_OK; }
    if (output != NULL) memcpy(output, &thread->result, 8);
    regs[R2_X86_RAX] = 0; return R2_OK;
}
static r2_status synchronization_call(r2_native_runtime *runtime, const char *name, uint64_t regs[16],
                                      char *error, size_t error_len)
{
    int result;
    if (strncmp(name, "pthread_mutex_", 14) == 0) {
        pthread_mutex_t *mutex = r2_native_pointer(runtime, regs[R2_X86_RDI], sizeof(*mutex), true, error, error_len);
        if (mutex == NULL) return R2_ERR_UNSUPPORTED;
        if (strcmp(name, "pthread_mutex_unlock") == 0) {
            result = pthread_mutex_unlock(mutex); if (result == 0) forget_mutex(mutex);
        } else if (strcmp(name, "pthread_mutex_trylock") == 0) {
            result = pthread_mutex_trylock(mutex); if (result == 0) remember_mutex(mutex);
        } else {
            unsigned depth = engine_suspend(runtime);
            while ((result = pthread_mutex_trylock(mutex)) == EBUSY && !r2_native_stopping(runtime)) {
                struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
            }
            engine_resume(runtime, depth);
            if (r2_native_stopping(runtime)) {
                if (result == 0) pthread_mutex_unlock(mutex);
                return r2_native_execution_poll(runtime, error, error_len);
            }
            if (result == 0) remember_mutex(mutex);
        }
    } else if (strcmp(name, "semaphore_wait") == 0 || strcmp(name, "semaphore_timedwait") == 0) {
        bool owned = false;
        for (size_t i = 0; i < runtime->semaphore_count; i++) if (runtime->semaphores[i] == (semaphore_t)regs[R2_X86_RDI]) owned = true;
        if (!owned) { regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK; }
        bool timed = strcmp(name, "semaphore_timedwait") == 0;
        uint32_t seconds = (uint32_t)regs[R2_X86_RSI], nanos = (uint32_t)(regs[R2_X86_RSI] >> 32);
        if (timed && nanos >= 1000000000) { regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t deadline = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec + (uint64_t)seconds * UINT64_C(1000000000) + nanos;
        unsigned depth = engine_suspend(runtime);
        do {
            uint64_t duration = 50000000;
            if (timed) {
                clock_gettime(CLOCK_MONOTONIC, &now);
                uint64_t current = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
                uint64_t remaining = current >= deadline ? 0 : deadline - current;
                if (remaining < duration) duration = remaining;
            }
            result = semaphore_timedwait((semaphore_t)regs[R2_X86_RDI], (mach_timespec_t){0, (unsigned)duration});
            if (result != KERN_OPERATION_TIMED_OUT || (timed && duration < 50000000)) break;
        } while (!r2_native_stopping(runtime));
        engine_resume(runtime, depth);
        if (r2_native_stopping(runtime)) return r2_native_execution_poll(runtime, error, error_len);
    } else {
        pthread_cond_t *cond = r2_native_pointer(runtime, regs[R2_X86_RDI], sizeof(*cond), true, error, error_len);
        pthread_mutex_t *mutex = r2_native_pointer(runtime, regs[R2_X86_RSI], sizeof(*mutex), true, error, error_len);
        if (cond == NULL || mutex == NULL) return R2_ERR_UNSUPPORTED;
        struct timespec deadline = {0}; bool timed = strcmp(name, "pthread_cond_timedwait") == 0;
        if (timed) {
            const void *input = r2_native_pointer(runtime, regs[R2_X86_RDX], sizeof(deadline), false, error, error_len);
            if (input == NULL) return R2_ERR_UNSUPPORTED;
            memcpy(&deadline, input, sizeof(deadline));
            if (deadline.tv_sec < 0 || deadline.tv_nsec < 0 || deadline.tv_nsec >= 1000000000) { regs[R2_X86_RAX] = EINVAL; return R2_OK; }
        }
        forget_mutex(mutex);
        unsigned depth = engine_suspend(runtime);
        do {
            struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 50000000; if (until.tv_nsec >= 1000000000) { until.tv_nsec -= 1000000000; until.tv_sec++; }
            if (timed && (deadline.tv_sec < until.tv_sec || (deadline.tv_sec == until.tv_sec && deadline.tv_nsec < until.tv_nsec))) until = deadline;
            result = pthread_cond_timedwait(cond, mutex, &until);
            if (result != ETIMEDOUT || r2_native_stopping(runtime)) break;
            if (timed && until.tv_sec == deadline.tv_sec && until.tv_nsec == deadline.tv_nsec) break;
        } while (true);
        /* Waiting reacquires the native mutex before the execution mutex. */
        if (r2_native_stopping(runtime)) pthread_mutex_unlock(mutex);
        engine_resume(runtime, depth);
        if (r2_native_stopping(runtime)) return r2_native_execution_poll(runtime, error, error_len);
        remember_mutex(mutex);
    }
    regs[R2_X86_RAX] = (uint32_t)result;
    return R2_OK;
}

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static r2_status event_wait_call(r2_native_runtime *runtime, const char *name, uint64_t regs[16],
                                 char *error, size_t error_len)
{
    bool polling = strcmp(name, "poll") == 0;
    uint64_t duration = UINT64_MAX; void *events = NULL; const struct kevent *changes = NULL;
    struct pollfd *descriptors = NULL; int change_count = 0, event_count = 0;
    if (polling) {
        unsigned count = (unsigned)regs[R2_X86_RSI];
        if (count > 65536) { errno = EINVAL; regs[R2_X86_RAX] = UINT32_MAX; return R2_OK; }
        if (count) descriptors = r2_native_pointer(runtime, regs[R2_X86_RDI], (size_t)count * sizeof(*descriptors), true, error, error_len);
        if (count && descriptors == NULL) return R2_ERR_UNSUPPORTED;
        int timeout = (int)regs[R2_X86_RDX];
        if (timeout >= 0) duration = (uint64_t)timeout * 1000000;
    } else {
        change_count = (int)regs[R2_X86_RDX]; event_count = (int)regs[R2_X86_R8];
        if (change_count < 0 || event_count < 0 || change_count > 65536 || event_count > 65536) {
            errno = EINVAL; regs[R2_X86_RAX] = UINT32_MAX; return R2_OK;
        }
        if (change_count) changes = r2_native_pointer(runtime, regs[R2_X86_RSI], (size_t)change_count * sizeof(struct kevent), false, error, error_len);
        if (event_count) events = r2_native_pointer(runtime, regs[R2_X86_RCX], (size_t)event_count * sizeof(struct kevent), true, error, error_len);
        if ((change_count && changes == NULL) || (event_count && events == NULL)) return R2_ERR_UNSUPPORTED;
        if (regs[R2_X86_R9]) {
            const struct timespec *timeout = r2_native_pointer(runtime, regs[R2_X86_R9], sizeof(*timeout), false, error, error_len);
            if (timeout == NULL) return R2_ERR_UNSUPPORTED;
            if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000 || (uint64_t)timeout->tv_sec > UINT64_MAX / 1000000000 - 1) {
                errno = EINVAL; regs[R2_X86_RAX] = UINT32_MAX; return R2_OK;
            }
            duration = (uint64_t)timeout->tv_sec * UINT64_C(1000000000) + (uint64_t)timeout->tv_nsec;
        }
        /* With no requested event output, kevent applies changes immediately. */
        if (event_count == 0) duration = 0;
    }
    uint64_t start = monotonic_nanoseconds(); int result, saved_errno;
    unsigned depth = engine_suspend(runtime);
    do {
        uint64_t elapsed = monotonic_nanoseconds() - start;
        uint64_t remaining = duration == UINT64_MAX ? UINT64_MAX : elapsed >= duration ? 0 : duration - elapsed;
        uint64_t quantum = remaining < 50000000 ? remaining : 50000000;
        if (polling) result = poll(descriptors, (nfds_t)regs[R2_X86_RSI], (int)((quantum + 999999) / 1000000));
        else {
            struct timespec timeout = {0, (long)quantum};
            result = kevent((int)regs[R2_X86_RDI], changes, change_count, events, event_count, &timeout);
            /* EV_ADD/EV_DELETE changes must only be applied once. */
            change_count = 0;
        }
        saved_errno = errno;
        if (result != 0 || (duration != UINT64_MAX && monotonic_nanoseconds() - start >= duration)) break;
    } while (!r2_native_stopping(runtime));
    engine_resume(runtime, depth);
    if (r2_native_stopping(runtime)) return r2_native_execution_poll(runtime, error, error_len);
    errno = saved_errno; regs[R2_X86_RAX] = (uint32_t)result;
    return R2_OK;
}

static char objc_scalar_type(const char *encoding)
{
    while(*encoding&&strchr("rnNoORV",*encoding))encoding++;
    switch(*encoding) {
    case 'v':return 'v';case 'c':return 'c';case 'C':case 'B':return 'b';case 's':return 's';case 'S':return 'h';
    case 'i':case 'I':case 'l':case 'L':return 'i';case 'q':return 'l';case 'Q':return 'z';case 'f':return 'f';case 'd':return 'd';
    case '@':return encoding[1]=='?'?0:'p';case '^':return encoding[1]=='?'?0:'p';case '#':case ':':case '*':return 'p';default:return 0;
    }
}
#include "objc_calls.inc"

static r2_status objc_call(r2_native_runtime *runtime, const char *name, uint64_t regs[16],uint8_t xmm[16][16],
                            char *error, size_t error_len)
{
    bool stret = strcmp(name, "objc_msgSend_stret") == 0;
    bool super_call=strcmp(name,"objc_msgSendSuper2")==0;
    struct {void *receiver;Class current;} native_super={0};Class lookup_class=Nil;

    size_t shift = stret ? 1 : 0;
    uint64_t receiver = guest_argument(regs, shift), selector_address = guest_argument(regs, shift + 1);
    if(super_call) {
        const uint64_t *super=r2_native_pointer(runtime,receiver,16,false,error,error_len);if(!super)return R2_ERR_UNSUPPORTED;
        receiver=super[0];Class current=NULL;bool mapped=false;
        for(size_t i=0;i<runtime->objc_class_count;i++) {
            uint64_t meta;memcpy(&meta,(void *)(uintptr_t)runtime->objc_classes[i].guest,8);
            if(meta==super[1]) {
                Class (*get)(id)=dlsym(runtime->objc,"object_getClass");current=get((id)runtime->objc_classes[i].native);mapped=true;break;
            }
        }
        if(!mapped){r2_status status=objc_bridge_class(runtime,super[1],&current,error,error_len);if(status!=R2_OK)return status;}
        Class (*parent)(Class)=dlsym(runtime->objc,"class_getSuperclass");lookup_class=parent(current);native_super.receiver=(void *)(uintptr_t)receiver;native_super.current=current;
    }
    char selector[256]; size_t length;
    for (length = 0; length < sizeof(selector); length++) {
        const char *byte = r2_native_pointer(runtime, selector_address + length, 1, false, error, error_len);
        if (byte == NULL) return R2_ERR_UNSUPPORTED;
        if ((selector[length] = *byte) == 0) break;
    }
    if (length == sizeof(selector)) { r2_set_error(error, error_len, "Objective-C selector too long"); return R2_ERR_UNSUPPORTED; }
    if(runtime->objc==NULL)runtime->objc=dlopen("/usr/lib/libobjc.A.dylib",RTLD_NOW|RTLD_LOCAL);
    if(!runtime->objc){r2_set_error(error,error_len,"cannot load native Objective-C runtime");return R2_ERR_UNSUPPORTED;}
    void *(*register_selector)(const char *)=dlsym(runtime->objc,"sel_registerName");
    if(!register_selector)return R2_ERR_UNSUPPORTED;
    void *canonical_selector=register_selector(selector);
    bool guest_class=false;uint64_t implementation=0;
    r2_status guest_status=super_call?R2_OK:objc_guest_method(runtime,receiver,selector,&guest_class,&implementation,error,error_len);
    if(guest_status!=R2_OK)return guest_status;
    if(guest_class) {
        if(!implementation) {
            Class mapped;r2_status status=objc_bridge_class(runtime,receiver,&mapped,error,error_len);if(status!=R2_OK)return status;
            receiver=(uintptr_t)mapped;guest_class=false;
        }
    }
    if(guest_class) {
        void *stack=r2_native_pointer(runtime,regs[R2_X86_RSP]-8,8,true,error,error_len);if(!stack)return R2_ERR_UNSUPPORTED;
        memcpy(stack,&implementation,8);regs[R2_X86_RSP]-=8;regs[stret?R2_X86_RDX:R2_X86_RSI]=(uintptr_t)canonical_selector;regs[R2_X86_RAX]=0;return R2_OK;
    }
    char inferred[NATIVE_MAX_ARGS+1];uint32_t selector_arguments=0,class_arguments=0;
    const char *signature = NULL; char result_type = 'p'; bool variadic = false;
    if (strcmp(selector, "processInfo") == 0 || strcmp(selector, "UTF8String") == 0 ||
        strcmp(selector, "alloc") == 0 || strcmp(selector, "init") == 0 || strcmp(selector, "new") == 0) signature = "pp";
    else if (strcmp(selector, "drain") == 0 || strcmp(selector, "release") == 0) { signature = "pp"; result_type = 'v'; }
    else if (strcmp(selector, "dictionaryWithContentsOfFile:") == 0 || strcmp(selector, "objectForKey:") == 0) signature = "ppp";
    else if (strcmp(selector, "stringWithFormat:") == 0) { signature = "ppp"; variadic = true; }
    else if (strcmp(selector, "operatingSystemVersion") == 0 && stret) signature = "pp";
    if(runtime->objc==NULL)runtime->objc=dlopen("/usr/lib/libobjc.A.dylib",RTLD_NOW|RTLD_LOCAL);
    if(!runtime->objc){r2_set_error(error,error_len,"cannot load native Objective-C runtime");return R2_ERR_UNSUPPORTED;}
    if(!signature&&!stret) {
        if(!receiver){regs[R2_X86_RAX]=0;memset(xmm[0],0,16);return R2_OK;}
        Class (*get_class)(id)=dlsym(runtime->objc,"object_getClass");
        Method (*get_method)(Class,SEL)=dlsym(runtime->objc,"class_getInstanceMethod");
        unsigned (*get_count)(Method)=dlsym(runtime->objc,"method_getNumberOfArguments");
        void (*get_return)(Method,char *,size_t)=dlsym(runtime->objc,"method_getReturnType");
        void (*get_argument)(Method,unsigned,char *,size_t)=dlsym(runtime->objc,"method_getArgumentType");
        if(!get_class||!get_method||!get_count||!get_return||!get_argument)return R2_ERR_UNSUPPORTED;
        Method method=get_method(super_call?lookup_class:get_class((id)(uintptr_t)receiver),(SEL)canonical_selector);
        if(method) {
            unsigned count=get_count(method);char encoding[256]={0};get_return(method,encoding,sizeof(encoding));
            result_type=objc_scalar_type(encoding);
            if(!result_type||count<2||count>NATIVE_MAX_ARGS){r2_set_error(error,error_len,"unsupported Objective-C method ABI for %s: %s",selector,encoding);return R2_ERR_UNSUPPORTED;}
            for(unsigned i=0;i<count;i++) {
                memset(encoding,0,sizeof(encoding));get_argument(method,i,encoding,sizeof(encoding));inferred[i]=objc_scalar_type(encoding);
                const char *kind=encoding;while(*kind&&strchr("rnNoORV",*kind))kind++;if(*kind==':')selector_arguments|=UINT32_C(1)<<i;if(*kind=='#')class_arguments|=UINT32_C(1)<<i;
                if(!inferred[i]||inferred[i]=='v'){r2_set_error(error,error_len,"unsupported Objective-C argument ABI for %s: %s",selector,encoding);return R2_ERR_UNSUPPORTED;}
            }
            inferred[count]=0;signature=inferred;
        }
    }
    if (signature == NULL || (stret && strcmp(selector, "operatingSystemVersion") != 0)) {
        r2_set_error(error, error_len, "unsupported Objective-C selector: %s", selector); return R2_ERR_UNSUPPORTED;
    }
    if (getenv("R2_TRACE_IMPORTS")||getenv("R2_TRACE_OBJC")) fprintf(stderr, "bridge Objective-C %s\n", selector);
    if (runtime->objc == NULL) runtime->objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
    void *function = dlsym(runtime->objc, super_call?"objc_msgSendSuper2":"objc_msgSend");
    if (register_selector == NULL || function == NULL) return R2_ERR_UNSUPPORTED;
    char types[NATIVE_MAX_ARGS]; size_t count = strlen(signature), fixed = count;
    memcpy(types, signature, count);
    uint64_t values[NATIVE_MAX_ARGS] = {super_call?(uintptr_t)&native_super:receiver, (uintptr_t)canonical_selector};
    size_t gp=2+shift,fp=0,stack=0;
    for (size_t i = 2; i < count; i++) {
        bool floating=types[i]=='d'||types[i]=='f';
        if(floating&&fp<8)memcpy(&values[i],xmm[fp++],types[i]=='f'?4:8);
        else if(!floating&&gp<6)values[i]=guest_argument(regs,gp++);
        else {memcpy(&values[i],(void *)(uintptr_t)(regs[R2_X86_RSP]+8+stack*8),8);stack++;}
        if((class_arguments&(UINT32_C(1)<<i))&&values[i]) {
            Class mapped;r2_status status=objc_bridge_class(runtime,values[i],&mapped,error,error_len);if(status!=R2_OK)return status;values[i]=(uintptr_t)mapped;
        }
        if((selector_arguments&(UINT32_C(1)<<i))&&values[i]) {
            char argument[256];if(!graphics_string(runtime,values[i],argument,error,error_len))return R2_ERR_UNSUPPORTED;
            values[i]=(uintptr_t)register_selector(argument);
        }
    }
    if (variadic) {
        if (runtime->core_foundation == NULL) runtime->core_foundation = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW | RTLD_LOCAL);
        unsigned char (*get_string)(const void *, char *, long, uint32_t) = dlsym(runtime->core_foundation, "CFStringGetCString");
        char format[256];
        if (get_string == NULL || !get_string((void *)(uintptr_t)values[2], format, sizeof(format), 0x08000100) || !printf_types(format, types, &count)) {
            r2_set_error(error, error_len, "unsupported Objective-C format"); return R2_ERR_UNSUPPORTED;
        }
        /* This whitelist currently admits integer/pointer NSString formats.
         * Floating variadic arguments require the guest XMM register bank. */
        for (size_t i = fixed; i < count; i++) {
            if(types[i]=='d'&&fp<8)memcpy(&values[i],xmm[fp++],8);
            else if(types[i]!='d'&&gp<6)values[i]=guest_argument(regs,gp++);
            else {const void *p=r2_native_pointer(runtime,regs[R2_X86_RSP]+8+8*stack++,8,false,error,error_len);if(!p)return R2_ERR_UNSUPPORTED;memcpy(&values[i],p,8);}
        }
    }
    ffi_type *elements[] = {&ffi_type_sint64, &ffi_type_sint64, &ffi_type_sint64, NULL};
    ffi_type version = {0, 0, FFI_TYPE_STRUCT, elements};
    ffi_type *arguments_types[NATIVE_MAX_ARGS]; void *arguments[NATIVE_MAX_ARGS];
    for (size_t i = 0; i < count; i++) { arguments_types[i] = argument_type(types[i]); arguments[i] = &values[i]; }
    ffi_cif cif; ffi_type *result_ffi = stret ? &version : argument_type(result_type);
    ffi_status status = variadic ? ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, (unsigned)fixed, (unsigned)count, result_ffi, arguments_types)
                                : ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)count, result_ffi, arguments_types);
    if (status != FFI_OK) return R2_ERR_UNSUPPORTED;
    uint64_t result[3] = {0};
    if (receiver != 0) {unsigned depth=engine_suspend(runtime);ffi_call(&cif, FFI_FN(function), result, arguments);engine_resume(runtime,depth);}
    if (stret) {
        void *output = r2_native_pointer(runtime, regs[R2_X86_RDI], sizeof(result), true, error, error_len);
        if (output == NULL) return R2_ERR_UNSUPPORTED;
        memcpy(output, result, sizeof(result)); regs[R2_X86_RAX] = regs[R2_X86_RDI];
    } else if(result_type=='f'||result_type=='d') {
        memset(xmm[0],0,16);memcpy(xmm[0],result,result_type=='f'?4:8);
    } else regs[R2_X86_RAX] = result_type=='i'?(uint32_t)result[0]:result_type=='b'?(uint8_t)result[0]:
        result_type=='c'?(uint32_t)(int32_t)(int8_t)result[0]:result_type=='h'?(uint16_t)result[0]:result_type=='s'?(uint32_t)(int32_t)(int16_t)result[0]:result[0];
    memset(runtime->memory, 0, sizeof(runtime->memory));
    return R2_OK;
}

static r2_status native_call_impl(r2_native_runtime *runtime, uint64_t regs[16],
                         uint8_t xmm[16][16],
                         bool *exited, char *error, size_t error_len)
{
    size_t index = (size_t)(regs[R2_X86_RAX] - NATIVE_CALL_BASE);
    if (index >= runtime->import_count) return R2_ERR_UNSUPPORTED;
    native_import *import = &runtime->imports[index];
    const native_definition *definition = import->definition;
    const char *name = definition->name;
    if(strcmp(name,"__divti3")==0||strcmp(name,"__udivti3")==0) {
        unsigned __int128 a=((unsigned __int128)regs[R2_X86_RSI]<<64)|regs[R2_X86_RDI];
        unsigned __int128 b=((unsigned __int128)regs[R2_X86_RCX]<<64)|regs[R2_X86_RDX];
        if(!b) {r2_set_error(error,error_len,"guest 128-bit integer division by zero");return R2_ERR_UNSUPPORTED;}
        bool signed_division=strcmp(name,"__divti3")==0,negative_a=signed_division&&(a>>127),negative_b=signed_division&&(b>>127);
        if(negative_a)a=0-a;if(negative_b)b=0-b;
        unsigned __int128 quotient=a/b;if(negative_a!=negative_b)quotient=0-quotient;
        regs[R2_X86_RAX]=(uint64_t)quotient;regs[R2_X86_RDX]=(uint64_t)(quotient>>64);return R2_OK;
    }
    if(strcmp(name,"__muldc3")==0) {
        double parts[4];for(size_t i=0;i<4;i++)memcpy(&parts[i],xmm[i],8);
        double _Complex product=__builtin_complex(parts[0],parts[1])*__builtin_complex(parts[2],parts[3]);
        double real=creal(product),imaginary=cimag(product);memset(xmm[0],0,16);memset(xmm[1],0,16);
        memcpy(xmm[0],&real,8);memcpy(xmm[1],&imaginary,8);return R2_OK;
    }
    if (strcmp(name, "posix_spawn") == 0) {
        /* An x86 JVM's helper cannot be submitted to the native ARM loader.
         * Surface the unsupported operation as a POSIX failure, leaving PID
         * and file actions untouched, so Java can raise its IOException. */
        regs[R2_X86_RAX] = ENOSYS; return R2_OK;
    }
    if(strcmp(name,"_tlv_atexit")==0) {
        bool dynamic;
        if(native_thread->tlv_exit_count==256||!executable_span(runtime,regs[R2_X86_RDI],&dynamic)) {
            r2_set_error(error,error_len,"invalid or excessive thread-local destructor registration");return R2_ERR_UNSUPPORTED;
        }
        size_t i=native_thread->tlv_exit_count++;
        native_thread->tlv_exits[i].function=regs[R2_X86_RDI];native_thread->tlv_exits[i].argument=regs[R2_X86_RSI];return R2_OK;
    }
    if(strcmp(name,"__ulock_wait")==0||strcmp(name,"__ulock_wake")==0) {
        /* XNU bsd/sys/ulock.h: timeout is relative microseconds; NO_ERRNO
         * returns a negative errno without modifying the calling thread's errno. */
        uint32_t operation=(uint32_t)regs[R2_X86_RDI],opcode=operation&255;bool wait=strcmp(name,"__ulock_wait")==0;
        if(opcode<1||opcode>6||(operation&~(wait?UINT32_C(0x010700ff):UINT32_C(0x010007ff)))) {
            r2_set_error(error,error_len,"unsupported ulock operation 0x%x",operation);return R2_ERR_UNSUPPORTED;
        }
        void *address=r2_native_pointer(runtime,regs[R2_X86_RSI],opcode>=4?8:4,false,error,error_len);if(!address)return R2_ERR_UNSUPPORTED;
        if(!wait) {
            int (*function)(uint32_t,void *,uint64_t)=import->function;
            regs[R2_X86_RAX]=(uint32_t)function(operation,address,regs[R2_X86_RDX]);return R2_OK;
        }
        int (*function)(uint32_t,void *,uint64_t,uint32_t)=import->function;
        uint32_t timeout=(uint32_t)regs[R2_X86_RCX];struct timespec start;clock_gettime(CLOCK_MONOTONIC,&start);
        uint64_t begin=(uint64_t)start.tv_sec*1000000+(uint64_t)start.tv_nsec/1000;
        int initial_errno=errno;
        for(;;) {
            struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
            uint64_t elapsed=(uint64_t)now.tv_sec*1000000+(uint64_t)now.tv_nsec/1000-begin;
            uint32_t slice=timeout?(elapsed>=timeout?1:(uint32_t)(timeout-elapsed)):20000;if(slice>20000)slice=20000;
            unsigned depth=engine_suspend(runtime);int result=function(operation,address,regs[R2_X86_RDX],slice),call_errno=errno;engine_resume(runtime,depth);
            errno=(operation&0x01000000)?initial_errno:call_errno;
            if(r2_native_stopping(runtime))return r2_native_execution_poll(runtime,error,error_len);
            bool expired=(operation&0x01000000)?result==-ETIMEDOUT:result==-1&&call_errno==ETIMEDOUT;
            clock_gettime(CLOCK_MONOTONIC,&now);elapsed=(uint64_t)now.tv_sec*1000000+(uint64_t)now.tv_nsec/1000-begin;
            if(!expired||(timeout&&elapsed>=timeout)) {regs[R2_X86_RAX]=(uint32_t)result;return R2_OK;}
        }
    }
    {bool handled=false;r2_status status=signal_call(runtime,import,regs,&handled,error,error_len);if(handled||status!=R2_OK)return status;}
    if(strcmp(name,"NSLog")==0)return objc_log(runtime,import,regs,xmm,error,error_len);
    {bool handled=false;r2_status status=objc_special(runtime,import,regs,&handled,error,error_len);if(handled||status!=R2_OK)return status;}
    if (strcmp(name, "pthread_once") == 0 || strcmp(name, "__r2_once_complete") == 0)
        return once_call(runtime, name, regs, error, error_len);
    if (getenv("R2_TRACE_IMPORTS") != NULL) fprintf(stderr, "bridge %s(%llx,%llx,%llx,%llx,%llx,%llx)\n", name,
        (unsigned long long)regs[R2_X86_RDI], (unsigned long long)regs[R2_X86_RSI], (unsigned long long)regs[R2_X86_RDX],
        (unsigned long long)regs[R2_X86_RCX], (unsigned long long)regs[R2_X86_R8], (unsigned long long)regs[R2_X86_R9]);
    if (strcmp(name, "objc_msgSend") == 0 || strcmp(name, "objc_msgSend_stret") == 0||strcmp(name,"objc_msgSendSuper2")==0||strcmp(name,"objc_msgSend_fpret")==0)
        return objc_call(runtime, name, regs,xmm,error,error_len);
    if (import->graphics_kind && definition->result != '!') {
        bool handled = false;
        r2_status status = graphics_special(runtime, import, regs, &handled, error, error_len);
        if (handled || status != R2_OK) return status;
    }
    if (strncmp(name,"Audio",5)==0) {
        bool handled=false;
        r2_status status=audio_special(runtime,import,regs,&handled,error,error_len);
        if(handled||status!=R2_OK)return status;
    }
    if(strncmp(name,"CG",2)==0||strncmp(name,"CT",2)==0) {
        bool handled=false;r2_status status=font_call(runtime,import,regs,xmm,&handled,error,error_len);
        if(handled||status!=R2_OK)return status;
    }
    if (definition->result == '!') {
        if (strcmp(name, "posix_spawn") == 0) {
            char path[256];
            if (!graphics_string(runtime, regs[R2_X86_RSI], path, error, error_len)) return R2_ERR_UNSUPPORTED;
            r2_set_error(error, error_len, "unsupported guest posix_spawn executable: %s", path); return R2_ERR_UNSUPPORTED;
        }
        r2_set_error(error, error_len, "JVM probe reached unsupported host function: %s", name);
        return R2_ERR_UNSUPPORTED;
    }
    if (strcmp(name, "CFArrayCreate") == 0 || strcmp(name, "CFArrayCreateMutable") == 0) {
        uint64_t callbacks=regs[strcmp(name,"CFArrayCreate")==0?R2_X86_RCX:R2_X86_RDX];
        void *type_callbacks = runtime->core_foundation ? dlsym(runtime->core_foundation, "kCFTypeArrayCallBacks") : NULL;
        void *string_callbacks = runtime->core_foundation ? dlsym(runtime->core_foundation, "kCFCopyStringArrayCallBacks") : NULL;
        if (callbacks && callbacks != (uintptr_t)type_callbacks && callbacks != (uintptr_t)string_callbacks) {
            r2_set_error(error, error_len, "guest CFArray callbacks are unsupported"); return R2_ERR_UNSUPPORTED;
        }
    }
    if (strcmp(name,"CFDictionaryCreate")==0 || strcmp(name,"CFDictionaryCreateMutable")==0) {
        bool mutable=strcmp(name,"CFDictionaryCreateMutable")==0;
        uint64_t keys=regs[mutable?R2_X86_RDX:R2_X86_R8],values=regs[mutable?R2_X86_RCX:R2_X86_R9];
        void *type_keys=runtime->core_foundation?dlsym(runtime->core_foundation,"kCFTypeDictionaryKeyCallBacks"):NULL;
        void *string_keys=runtime->core_foundation?dlsym(runtime->core_foundation,"kCFCopyStringDictionaryKeyCallBacks"):NULL;
        void *type_values=runtime->core_foundation?dlsym(runtime->core_foundation,"kCFTypeDictionaryValueCallBacks"):NULL;
        if ((keys&&keys!=(uintptr_t)type_keys&&keys!=(uintptr_t)string_keys)||(values&&values!=(uintptr_t)type_values)) {
            r2_set_error(error,error_len,"guest CFDictionary callbacks are unsupported");return R2_ERR_UNSUPPORTED;
        }
    }
    if (strcmp(name, "_dyld_image_count") == 0 || strcmp(name, "_dyld_get_image_name") == 0 || strcmp(name,"_dyld_get_image_header")==0) {
        if (runtime->image_count == NULL || runtime->image_name == NULL) { r2_set_error(error, error_len, "guest image enumeration is unavailable"); return R2_ERR_UNSUPPORTED; }
        regs[R2_X86_RAX] = strcmp(name, "_dyld_image_count") == 0 ? runtime->image_count(runtime->loader_context) :
            strcmp(name,"_dyld_get_image_header")==0?(runtime->image_header?(uintptr_t)runtime->image_header(runtime->loader_context,(uint32_t)regs[R2_X86_RDI]):0):
            (uintptr_t)runtime->image_name(runtime->loader_context, (uint32_t)regs[R2_X86_RDI]);
        return R2_OK;
    }
    if (strcmp(name, "CFBundleGetFunctionPointerForName") == 0) {
        if (runtime->core_foundation == NULL) return R2_ERR_UNSUPPORTED;
        unsigned char (*get_string)(const void *, char *, long, uint32_t) = dlsym(runtime->core_foundation, "CFStringGetCString");
        void *(*get_function)(void *, const void *) = import->function;
        char symbol[256] = {'_'};
        if (get_string == NULL || !get_string((void *)(uintptr_t)regs[R2_X86_RSI], symbol + 1, sizeof(symbol) - 1, 0x08000100)) {
            r2_set_error(error, error_len, "invalid framework function name"); return R2_ERR_UNSUPPORTED;
        }
        void *function = get_function((void *)(uintptr_t)regs[R2_X86_RDI], (void *)(uintptr_t)regs[R2_X86_RSI]);
        if (function == NULL) { regs[R2_X86_RAX] = 0; return R2_OK; }
        uint64_t address = 0; runtime->override_name = symbol + 1; runtime->override_function = function;
        r2_status status = r2_native_symbol(runtime, symbol, &address, error, error_len);
        runtime->override_name = NULL; runtime->override_function = NULL;
        if (status != R2_OK) return status;
        regs[R2_X86_RAX] = address; return R2_OK;
    }
    if (strcmp(name, "pthread_atfork") == 0) {
        /* Fork itself is unsupported. Preserve guest registrations without
         * ever installing their x86 addresses as native ARM callbacks. */
        if (runtime->fork_count == 64) { regs[R2_X86_RAX] = ENOMEM; return R2_OK; }
        size_t slot = runtime->fork_count++;
        runtime->forks[slot].prepare = regs[R2_X86_RDI]; runtime->forks[slot].parent = regs[R2_X86_RSI]; runtime->forks[slot].child = regs[R2_X86_RDX];
        regs[R2_X86_RAX] = 0; return R2_OK;
    }
    if (strcmp(name, "syscall") == 0) {
        unsigned number = (unsigned)regs[R2_X86_RDI];
        if (number == SYS_thread_selfid) {
            uint64_t tid = 0; int status = pthread_threadid_np(NULL, &tid);
            regs[R2_X86_RAX] = status == 0 ? tid : UINT64_MAX; return R2_OK;
        }
        if (number == SYS_write) {
            size_t length = (size_t)regs[R2_X86_RCX];
            if (length && r2_native_pointer(runtime, regs[R2_X86_RDX], length, false, error, error_len) == NULL) return R2_ERR_UNSUPPORTED;
            regs[R2_X86_RAX] = (uint64_t)write((int)regs[R2_X86_RSI], (void *)(uintptr_t)regs[R2_X86_RDX], length); return R2_OK;
        }
        r2_set_error(error, error_len, "unsupported guest libc syscall %u", number); return R2_ERR_UNSUPPORTED;
    }
    if (strcmp(name, "madvise") == 0) {
        uint64_t start = regs[R2_X86_RDI], size = regs[R2_X86_RSI];
        if (start > UINT64_MAX - size || !owns_vm_range(runtime, start, start + size)) {
            errno = EINVAL; regs[R2_X86_RAX] = UINT32_MAX; return R2_OK;
        }
    }
    if (strcmp(name, "raise") == 0) {
        unsigned signum = (unsigned)regs[R2_X86_RDI];
        if (signum == 0 || signum >= NSIG || signum == SIGKILL || signum == SIGSTOP) { errno = EINVAL; regs[R2_X86_RAX] = UINT32_MAX; return R2_OK; }
        atomic_fetch_or_explicit(&native_thread->pending, UINT32_C(1) << (signum - 1), memory_order_relaxed);
        regs[R2_X86_RAX] = 0; return R2_OK;
    }
    if (strcmp(name, "pid_for_task") == 0) {
        if ((task_t)regs[R2_X86_RDI] != mach_task_self()) { regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK; }
        void *output = r2_native_pointer(runtime, regs[R2_X86_RSI], sizeof(pid_t), true, error, error_len);
        if (output == NULL) return R2_ERR_UNSUPPORTED;
        pid_t pid = getpid(); memcpy(output, &pid, sizeof(pid)); regs[R2_X86_RAX] = KERN_SUCCESS; return R2_OK;
    }
    if (strcmp(name, "mach_task_self") == 0) { regs[R2_X86_RAX] = mach_task_self(); return R2_OK; }
    if (strcmp(name, "host_page_size") == 0) {
        bool owned = false;
        for (size_t i = 0; i < 1024; i++) if (runtime->ports[i].references && runtime->ports[i].port == (mach_port_t)regs[R2_X86_RDI]) owned = true;
        if (!owned) { regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK; }
        if (!r2_native_pointer(runtime, regs[R2_X86_RSI], sizeof(vm_size_t), true, error, error_len)) return R2_ERR_UNSUPPORTED;
    }
    if (strcmp(name, "getfsstat$INODE64") == 0 || strcmp(name, "getfsstat") == 0) {
        _Static_assert(sizeof(struct statfs) == 2168 && offsetof(struct statfs, f_blocks) == 8 &&
                       offsetof(struct statfs, f_mntonname) == 88 && offsetof(struct statfs, f_mntfromname) == 1112,
                       "Darwin x86 statfs64 ABI");
        int bytes = (int)regs[R2_X86_RSI];
        if (bytes < 0) { errno = EINVAL; regs[R2_X86_RAX] = UINT32_MAX; return R2_OK; }
        if (regs[R2_X86_RDI] && bytes && !r2_native_pointer(runtime, regs[R2_X86_RDI], (size_t)bytes, true, error, error_len)) return R2_ERR_UNSUPPORTED;
    }
    if (strcmp(name, "IOObjectRelease") == 0 || strcmp(name, "IOObjectRetain") == 0) {
        bool retain = strcmp(name, "IOObjectRetain") == 0;
        for (size_t i = 0; i < 1024; i++) if (runtime->ports[i].references && runtime->ports[i].port == (mach_port_t)regs[R2_X86_RDI]) {
            kern_return_t status = retain ? mach_port_mod_refs(mach_task_self(), runtime->ports[i].port, MACH_PORT_RIGHT_SEND, 1)
                                         : mach_port_deallocate(mach_task_self(), runtime->ports[i].port);
            if (status == KERN_SUCCESS) { if (retain) runtime->ports[i].references++; else runtime->ports[i].references--; }
            regs[R2_X86_RAX] = status; return R2_OK;
        }
        regs[R2_X86_RAX] = KERN_INVALID_NAME; return R2_OK;
    }
    if (strcmp(name, "task_info") == 0 || strcmp(name, "host_statistics") == 0 || strcmp(name, "host_statistics64") == 0) {
        _Static_assert(sizeof(task_basic_info_data_t) == 40 && offsetof(task_basic_info_data_t, virtual_size) == 4, "Darwin x86 task-info packing");
        _Static_assert(sizeof(vm_statistics_data_t) == 60, "Darwin x86 host VM-info packing");
        _Static_assert(sizeof(vm_statistics64_data_t) == 416 && offsetof(vm_statistics64_data_t, purgeable_count) == 88 &&
                       offsetof(vm_statistics64_data_t, compressor_page_count) == 128, "Darwin x86 host VM-info64 packing");
        bool task = name[0] == 't'; unsigned flavor = (unsigned)regs[R2_X86_RSI];
        if (task) {
            if ((task_t)regs[R2_X86_RDI] != mach_task_self()) { regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK; }
            /* TASK_BASIC_INFO has the same packed layout but different flavor
             * numbers on x86_64 (5) and ARM64 (18). Never pass that through. */
            if (flavor == 5) regs[R2_X86_RSI] = TASK_BASIC_INFO;
            else if (flavor != MACH_TASK_BASIC_INFO && flavor != TASK_THREAD_TIMES_INFO && flavor != TASK_VM_INFO) {
                r2_set_error(error, error_len, "unsupported guest task_info flavor %u", flavor); return R2_ERR_UNSUPPORTED;
            }
        } else {
            bool owned = false;
            for (size_t i = 0; i < 1024; i++) if (runtime->ports[i].references && runtime->ports[i].port == (mach_port_t)regs[R2_X86_RDI]) owned = true;
            if (!owned) { regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK; }
            bool wide = strcmp(name, "host_statistics64") == 0;
            if (wide ? flavor != HOST_VM_INFO64 : (flavor != HOST_VM_INFO && flavor != HOST_CPU_LOAD_INFO && flavor != HOST_LOAD_INFO)) {
                r2_set_error(error, error_len, "unsupported guest host_statistics flavor %u", flavor); return R2_ERR_UNSUPPORTED;
            }
        }
        mach_msg_type_number_t *count = r2_native_pointer(runtime, regs[R2_X86_RCX], sizeof(*count), true, error, error_len);
        if (count == NULL) return R2_ERR_UNSUPPORTED;
        if (*count == 0 || *count > 4096) { regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK; }
        if (r2_native_pointer(runtime, regs[R2_X86_RDX], (size_t)*count * sizeof(natural_t), true, error, error_len) == NULL) return R2_ERR_UNSUPPORTED;
    }
    if (strcmp(name, "proc_pidpath") == 0 || strcmp(name, "proc_name") == 0) {
        bool path_query = strcmp(name, "proc_pidpath") == 0;
        uint32_t size = (uint32_t)regs[R2_X86_RDX];
        size_t minimum = path_query ? PROC_PIDPATHINFO_SIZE : sizeof(((struct proc_bsdinfo *)0)->pbi_name);
        if (size < minimum) { errno = ENOMEM; regs[R2_X86_RAX] = 0; return R2_OK; }
        if (path_query && size > PROC_PIDPATHINFO_MAXSIZE) { errno = EOVERFLOW; regs[R2_X86_RAX] = 0; return R2_OK; }
        if ((pid_t)regs[R2_X86_RDI] != getpid()) { errno = EPERM; regs[R2_X86_RAX] = 0; return R2_OK; }
        const char *path = runtime->image_name ? runtime->image_name(runtime->loader_context, 0) : NULL;
        if (!path) { errno = ENOENT; regs[R2_X86_RAX] = 0; return R2_OK; }
        const char *value = path;
        if (!path_query) { const char *slash = strrchr(path, '/'); if (slash) value = slash + 1; }
        size_t length = strlen(value);
        if (!path_query && length >= minimum) length = minimum - 1;
        if (length >= size) { errno = ENAMETOOLONG; regs[R2_X86_RAX] = 0; return R2_OK; }
        char *output = r2_native_pointer(runtime, regs[R2_X86_RSI], length + 1, true, error, error_len);
        if (!output) return R2_ERR_UNSUPPORTED;
        memcpy(output, value, length); output[length] = 0; regs[R2_X86_RAX] = length; return R2_OK;
    }
    if (strcmp(name, "proc_pid_rusage") == 0) {
        _Static_assert(sizeof(struct rusage_info_v2) == 160 && offsetof(struct rusage_info_v2, ri_diskio_bytesread) == 144,
                       "Darwin x86 rusage-info-v2 ABI");
        const size_t sizes[] = {sizeof(struct rusage_info_v0), sizeof(struct rusage_info_v1), sizeof(struct rusage_info_v2)};
        unsigned flavor = (unsigned)regs[R2_X86_RSI];
        if ((pid_t)regs[R2_X86_RDI] != getpid() || flavor >= sizeof(sizes) / sizeof(sizes[0])) {
            errno = (pid_t)regs[R2_X86_RDI] != getpid() ? EPERM : EINVAL; regs[R2_X86_RAX] = UINT32_MAX; return R2_OK;
        }
        if (!r2_native_pointer(runtime, regs[R2_X86_RDX], sizes[flavor], true, error, error_len)) return R2_ERR_UNSUPPORTED;
    }
    if (strcmp(name, "proc_pidinfo") == 0) {
        _Static_assert(sizeof(struct proc_bsdinfo) == 136 && sizeof(struct proc_taskinfo) == 96 && sizeof(struct proc_fdinfo) == 8 &&
                       sizeof(struct proc_taskallinfo) == 232 && offsetof(struct proc_taskallinfo, ptinfo) == 136, "Darwin x86 process-info packing");
        _Static_assert(sizeof(struct proc_vnodepathinfo) == 2352 && offsetof(struct proc_vnodepathinfo, pvi_rdir) == 1176 &&
                       offsetof(struct vnode_info_path, vip_path) == 152, "Darwin x86 vnode-path-info ABI");
        int flavor = (int)regs[R2_X86_RSI];
        if ((pid_t)regs[R2_X86_RDI] != getpid() || (flavor != PROC_PIDTBSDINFO && flavor != PROC_PIDLISTFDS && flavor != PROC_PIDTASKINFO && flavor != PROC_PIDTASKALLINFO && flavor != PROC_PIDVNODEPATHINFO)) {
            r2_set_error(error, error_len, "unsupported guest proc_pidinfo pid %u flavor %d", (unsigned)regs[R2_X86_RDI], flavor); return R2_ERR_UNSUPPORTED;
        }
        int size = (int)regs[R2_X86_R8];
        if (size < 0) { errno = EINVAL; regs[R2_X86_RAX] = 0; return R2_OK; }
        if (size && r2_native_pointer(runtime, regs[R2_X86_RCX], (size_t)size, true, error, error_len) == NULL) return R2_ERR_UNSUPPORTED;
    }
    if (strcmp(name, "socketpair") == 0) {
        if (!r2_native_pointer(runtime, regs[R2_X86_RCX], sizeof(int) * 2, true, error, error_len)) return R2_ERR_UNSUPPORTED;
    }
    if (strcmp(name, "getaddrinfo") == 0) {
        size_t slot = 0;
        for (; slot < 64 && runtime->resolver_results[slot]; slot++) {}
        if (slot == 64) { regs[R2_X86_RAX] = (uint32_t)EAI_MEMORY; return R2_OK; }
        void *output = r2_native_pointer(runtime, regs[R2_X86_RCX], sizeof(void *), true, error, error_len);
        if (output == NULL) return R2_ERR_UNSUPPORTED;
        struct addrinfo hints, *result = NULL; const struct addrinfo *input = NULL;
        if (regs[R2_X86_RDX]) {
            input = r2_native_pointer(runtime, regs[R2_X86_RDX], sizeof(hints), false, error, error_len);
            if (input == NULL) return R2_ERR_UNSUPPORTED;
            memcpy(&hints, input, sizeof(hints)); input = &hints;
        }
        /* Reserve this ownership slot while other guest threads can run. */
        runtime->resolver_results[slot] = (struct addrinfo *)(uintptr_t)UINTPTR_MAX;
        unsigned depth = engine_suspend(runtime);
        int status = getaddrinfo((const char *)(uintptr_t)regs[R2_X86_RDI], (const char *)(uintptr_t)regs[R2_X86_RSI], input, &result);
        engine_resume(runtime, depth);
        runtime->resolver_results[slot] = result;
        memcpy(output, &result, sizeof(result));
        regs[R2_X86_RAX] = (uint32_t)status;
        memset(runtime->memory, 0, sizeof(runtime->memory)); return R2_OK;
    }
    if (strcmp(name, "freeaddrinfo") == 0) {
        struct addrinfo *pointer = (void *)(uintptr_t)regs[R2_X86_RDI];
        if (pointer == NULL) return R2_OK;
        for (size_t i = 0; i < 64; i++) if (runtime->resolver_results[i] == pointer && pointer != (void *)(uintptr_t)UINTPTR_MAX) {
            freeaddrinfo(pointer); runtime->resolver_results[i] = NULL;
            memset(runtime->memory, 0, sizeof(runtime->memory)); return R2_OK;
        }
        r2_set_error(error, error_len, "freeaddrinfo received an unowned resolver result"); return R2_ERR_UNSUPPORTED;
    }
    if (strcmp(name, "sched_yield") == 0) {
        unsigned depth = engine_suspend(runtime); int result = sched_yield(); engine_resume(runtime, depth);
        regs[R2_X86_RAX] = (uint32_t)result; return R2_OK;
    }
    if (strcmp(name, "poll") == 0 || strcmp(name, "kevent") == 0)
        return event_wait_call(runtime, name, regs, error, error_len);
    if (strcmp(name, "accept") == 0) {
        int fd = (int)regs[R2_X86_RDI], result, saved_errno = 0;
        struct sockaddr *address = NULL; socklen_t *length = NULL;
        if (regs[R2_X86_RSI]) {
            length = r2_native_pointer(runtime, regs[R2_X86_RDX], sizeof(*length), true, error, error_len);
            if (length == NULL) return R2_ERR_UNSUPPORTED;
            address = r2_native_pointer(runtime, regs[R2_X86_RSI], *length, true, error, error_len);
            if (address == NULL) return R2_ERR_UNSUPPORTED;
        }
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0) { regs[R2_X86_RAX] = UINT32_MAX; return R2_OK; }
        while (true) {
            if (!(flags & O_NONBLOCK) && fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                regs[R2_X86_RAX] = UINT32_MAX; return R2_OK;
            }
            result = accept(fd, address, length); saved_errno = errno;
            if (!(flags & O_NONBLOCK)) fcntl(fd, F_SETFL, flags);
            if (result >= 0 || (flags & O_NONBLOCK) || (saved_errno != EAGAIN && saved_errno != EWOULDBLOCK)) break;
            unsigned depth = engine_suspend(runtime);
            struct pollfd descriptor = {fd, POLLIN, 0}; poll(&descriptor, 1, 50);
            engine_resume(runtime, depth);
            if (r2_native_stopping(runtime)) return r2_native_execution_poll(runtime, error, error_len);
        }
        errno = saved_errno; regs[R2_X86_RAX] = (uint32_t)result; return R2_OK;
    }
    if (strcmp(name, "pthread_create") == 0 || strcmp(name, "pthread_join") == 0 ||
        strcmp(name, "pthread_detach") == 0 || strcmp(name, "pthread_exit") == 0)
        return thread_call(runtime, name, regs, exited, error, error_len);
    if (strcmp(name, "pthread_mutex_lock") == 0 || strcmp(name, "pthread_mutex_trylock") == 0 ||
        strcmp(name, "pthread_mutex_unlock") == 0 || strcmp(name, "pthread_cond_wait") == 0 ||
        strcmp(name, "pthread_cond_timedwait") == 0 || strcmp(name, "semaphore_wait") == 0 || strcmp(name, "semaphore_timedwait") == 0)
        return synchronization_call(runtime, name, regs, error, error_len);
    if (strcmp(name, "mach_thread_self") == 0 || strcmp(name, "mach_host_self") == 0) {
        mach_port_t port = strcmp(name, "mach_thread_self") == 0 ? mach_thread_self() : mach_host_self();
        size_t empty = SIZE_MAX;
        for (size_t i = 0; i < 1024; i++) {
            if (runtime->ports[i].references && runtime->ports[i].port == port) {
                runtime->ports[i].references++; regs[R2_X86_RAX] = port; return R2_OK;
            }
            if (!runtime->ports[i].references && empty == SIZE_MAX) empty = i;
        }
        if (empty == SIZE_MAX) {
            mach_port_deallocate(mach_task_self(), port);
            r2_set_error(error, error_len, "guest Mach port table exhausted"); return R2_ERR_UNSUPPORTED;
        }
        runtime->ports[empty].port = port; runtime->ports[empty].references = 1;
        regs[R2_X86_RAX] = port; return R2_OK;
    }
    if (strcmp(name, "thread_info") == 0) {
        bool owned = false;
        for (size_t i = 0; i < 1024; i++)
            if (runtime->ports[i].references && runtime->ports[i].port == (mach_port_t)regs[R2_X86_RDI]) owned = true;
        if (!owned) { regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK; }
        thread_flavor_t flavor = (thread_flavor_t)regs[R2_X86_RSI];
        if (flavor != THREAD_BASIC_INFO && flavor != THREAD_IDENTIFIER_INFO && flavor != THREAD_EXTENDED_INFO) {
            r2_set_error(error, error_len, "architecture-specific guest thread_info flavor is unsupported"); return R2_ERR_UNSUPPORTED;
        }
        uint32_t *count = r2_native_pointer(runtime, regs[R2_X86_RCX], 4, true, error, error_len);
        if (count == NULL) return R2_ERR_UNSUPPORTED;
        uint32_t capacity; memcpy(&capacity, count, 4);
        if (capacity == 0 || capacity > THREAD_INFO_MAX) { regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK; }
        if (r2_native_pointer(runtime, regs[R2_X86_RDX], capacity * sizeof(natural_t), true, error, error_len) == NULL) return R2_ERR_UNSUPPORTED;
    }
    if (strcmp(name, "mach_port_deallocate") == 0) {
        if ((task_t)regs[R2_X86_RDI] != mach_task_self()) { regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK; }
        for (size_t i = 0; i < 1024; i++) {
            if (runtime->ports[i].references && runtime->ports[i].port == (mach_port_t)regs[R2_X86_RSI]) {
                kern_return_t status = mach_port_deallocate(mach_task_self(), runtime->ports[i].port);
                if (status == KERN_SUCCESS) runtime->ports[i].references--;
                regs[R2_X86_RAX] = (uint32_t)status; return R2_OK;
            }
        }
        regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK;
    }
    if (strcmp(name, "kill") == 0) {
        if ((int)regs[R2_X86_RSI] != 0) {
            r2_set_error(error, error_len, "guest kill signal delivery is not supported yet"); return R2_ERR_UNSUPPORTED;
        }
        regs[R2_X86_RAX] = (uint32_t)kill((pid_t)regs[R2_X86_RDI], 0);
        return R2_OK;
    }
    if (strcmp(name, "_tlv_bootstrap") == 0) {
        uint64_t descriptor = regs[R2_X86_RDI];
        for (size_t i = 0; i < runtime->tls_count; i++) {
            for (size_t j = 0; j < runtime->tls[i].range_count; j++) {
                r2_tls_range range = runtime->tls[i].ranges[j];
                if (descriptor < range.address || descriptor - range.address >= range.count * 24 ||
                    (descriptor - range.address) % 24 != 0) continue;
                uint64_t offset;
                memcpy(&offset, (void *)(uintptr_t)(descriptor + 16), 8);
                if (offset >= runtime->tls[i].size) {
                    r2_set_error(error, error_len, "TLS descriptor offset is outside template"); return R2_ERR_DECODE;
                }
                void *storage = native_thread->tls_values[i];
                if (storage == NULL) {
                    if (posix_memalign(&storage, runtime->tls[i].alignment, runtime->tls[i].size) != 0) return R2_ERR_NO_MEMORY;
                    memcpy(storage, runtime->tls[i].data, runtime->tls[i].size);
                    native_thread->tls_values[i]=storage;
                    memset(runtime->memory, 0, sizeof(runtime->memory));
                }
                regs[R2_X86_RAX] = (uintptr_t)storage + offset;
                return R2_OK;
            }
        }
        r2_set_error(error, error_len, "unregistered Mach-O TLS descriptor"); return R2_ERR_UNSUPPORTED;
    }
    if (strcmp(name, "setrlimit") == 0) {
        if ((int)regs[R2_X86_RDI] != RLIMIT_NOFILE) {
            r2_set_error(error, error_len, "guest setrlimit resource is not supported yet"); return R2_ERR_UNSUPPORTED;
        }
        const void *pointer = r2_native_pointer(runtime, regs[R2_X86_RSI], sizeof(struct rlimit), false, error, error_len);
        if (pointer == NULL) return R2_ERR_UNSUPPORTED;
        struct rlimit requested, original;
        memcpy(&requested, pointer, sizeof(requested));
        if (getrlimit(RLIMIT_NOFILE, &original) != 0) { regs[R2_X86_RAX] = UINT32_MAX; return R2_OK; }
        if (requested.rlim_max != original.rlim_max) {
            r2_set_error(error, error_len, "changing guest hard resource limits is unsupported"); return R2_ERR_UNSUPPORTED;
        }
        if (!runtime->nofile_saved) { runtime->host_nofile = original; runtime->nofile_saved = true; }
        regs[R2_X86_RAX] = (uint32_t)setrlimit(RLIMIT_NOFILE, &requested);
        return R2_OK;
    }
    if (strcmp(name, "task_set_exception_ports") == 0) {
        unsigned behavior = (unsigned)regs[R2_X86_RCX] & ~MACH_EXCEPTION_CODES;
        if ((task_t)regs[R2_X86_RDI] != mach_task_self() ||
            ((exception_mask_t)regs[R2_X86_RSI] & ~EXC_MASK_ALL) != 0 ||
            behavior < EXCEPTION_DEFAULT || behavior > EXCEPTION_STATE_IDENTITY) {
            regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK;
        }
        if ((mach_port_t)regs[R2_X86_RDX] != MACH_PORT_NULL) {
            r2_set_error(error, error_len, "guest Mach exception port delivery is not supported yet");
            return R2_ERR_UNSUPPORTED;
        }
        /* The guest exception-port table starts empty. Clearing its entries
         * is valid, and must not clear the ARM process's real exception ports. */
        regs[R2_X86_RAX] = KERN_SUCCESS;
        return R2_OK;
    }
    if (strcmp(name, "mmap") == 0 || strcmp(name, "munmap") == 0 || strcmp(name, "mprotect") == 0) {
        bool allocating = strcmp(name, "mmap") == 0;
        bool removing = strcmp(name, "munmap") == 0;
        uint64_t length = regs[R2_X86_RSI], page = (uint64_t)getpagesize();
        uint64_t start = regs[R2_X86_RDI];
        if (length == 0 || length > SIZE_MAX - (page - 1) ||
            (!allocating && start % page != 0)) {
            errno = EINVAL;
            regs[R2_X86_RAX] = allocating ? UINT64_MAX : UINT32_MAX;
            return R2_OK;
        }
        size_t rounded = (size_t)((length + page - 1) & ~(page - 1));
        if (start > UINT64_MAX - rounded) {
            errno = EINVAL; regs[R2_X86_RAX] = allocating ? UINT64_MAX : UINT32_MAX; return R2_OK;
        }
        uint64_t end = start + rounded;
        int protection = removing ? -1 : (int)regs[R2_X86_RDX];
        int flags = allocating ? (int)regs[R2_X86_RCX] : 0;
        if (!removing && (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
            errno = EINVAL; regs[R2_X86_RAX] = allocating ? UINT64_MAX : UINT32_MAX; return R2_OK;
        }
        if ((!allocating || (flags & MAP_FIXED)) && !owns_vm_range(runtime, start, end)) {
            r2_set_error(error, error_len, "%s range is not owned by the guest", name);
            return R2_ERR_UNSUPPORTED;
        }
        if (!vm_range_fits(runtime, allocating && !(flags & MAP_FIXED) ? 0 : start,
                           allocating && !(flags & MAP_FIXED) ? 0 : end, protection)) {
            errno = ENOMEM; regs[R2_X86_RAX] = allocating ? UINT64_MAX : UINT32_MAX; return R2_OK;
        }
        if (allocating) {
#ifdef MAP_JIT
            flags &= ~MAP_JIT;
#endif
            void *mapping = mmap((void *)(uintptr_t)start, (size_t)length,
                                  host_mapping_protection(protection), flags,
                                  (int)regs[R2_X86_R8], (off_t)regs[R2_X86_R9]);
            if (mapping != MAP_FAILED)
                replace_vm_range(runtime, (uintptr_t)mapping, (uintptr_t)mapping + rounded, protection);
            regs[R2_X86_RAX] = (uintptr_t)mapping;
        } else {
            int status = removing ? munmap((void *)(uintptr_t)start, rounded)
                : mprotect((void *)(uintptr_t)start, rounded, host_mapping_protection(protection));
            if (status == 0) replace_vm_range(runtime, start, end, protection);
            regs[R2_X86_RAX] = (uint32_t)status;
        }
        return R2_OK;
    }
    if (strcmp(name, "fegetenv") == 0 || strcmp(name, "fesetenv") == 0) {
        bool save = strcmp(name, "fegetenv") == 0;
        uint8_t *pointer = r2_native_pointer(runtime, regs[R2_X86_RDI], 16, save, error, error_len);
        if (pointer == NULL) return R2_ERR_UNSUPPORTED;
        /* Darwin x86 fenv_t is { u16 control, u16 status, u32 mxcsr,
         * u8 reserved[8] }; the ARM type has a different layout. */
        if (save) {
            uint32_t mxcsr = native_thread->mxcsr | x86_fp_exceptions();
            memset(pointer, 0, 16);
            memcpy(pointer, &native_thread->x87.control, 2);
            memcpy(pointer + 2, &native_thread->x87.status, 2);
            memcpy(pointer + 4, &mxcsr, 4);
        } else {
            uint16_t control, status; uint32_t mxcsr;
            memcpy(&control, pointer, 2); memcpy(&status, pointer + 2, 2); memcpy(&mxcsr, pointer + 4, 4);
            if ((control & 0x0c3f) != 0x003f || !r2_mxcsr_supported(mxcsr)) {
                r2_set_error(error, error_len, "unsupported guest floating-point control mode");
                return R2_ERR_UNSUPPORTED;
            }
            native_thread->x87.control = control; native_thread->x87.status = status; native_thread->mxcsr = mxcsr;
            restore_fp_exceptions(mxcsr);
        }
        regs[R2_X86_RAX] = 0;
        return R2_OK;
    }
    if (strcmp(name, "dlopen") == 0 || strcmp(name, "dlclose") == 0) {
        uint64_t handle = 0;
        native_framework_scope scopes[4]; framework_scopes(runtime, scopes);
        if (strcmp(name, "dlclose") == 0) {
            for (size_t i = 0; i < 4; i++) if (regs[R2_X86_RDI] == (uintptr_t)scopes[i].references && *scopes[i].references) {
                (*scopes[i].references)--; regs[R2_X86_RAX] = 0; return R2_OK;
            }
            if (regs[R2_X86_RDI] == (uintptr_t)&runtime->process_opens && runtime->process_opens) {
                runtime->process_opens--; regs[R2_X86_RAX] = 0; return R2_OK;
            }
            graphics_module *module = graphics_handle(runtime, regs[R2_X86_RDI]);
            if (module) { module->references--; regs[R2_X86_RAX] = 0; return R2_OK; }
            bool closed = runtime->close_image != NULL && runtime->close_image(runtime->loader_context, regs[R2_X86_RDI]);
            if (!closed) {
                snprintf(runtime->loader_error, sizeof(runtime->loader_error), "invalid dlclose handle");
                runtime->loader_error_pending = true;
            }
            regs[R2_X86_RAX] = closed ? 0 : UINT32_MAX;
            return R2_OK;
        }
        if (runtime->open_image == NULL) {
            r2_set_error(error, error_len, "guest dynamic loader is unavailable"); return R2_ERR_UNSUPPORTED;
        }
        if (regs[R2_X86_RDI] == 0 && !(regs[R2_X86_RSI] & RTLD_FIRST)) {
            int supported = RTLD_LAZY | RTLD_NOW | RTLD_LOCAL | RTLD_GLOBAL | RTLD_NODELETE | RTLD_NOLOAD;
            if (regs[R2_X86_RSI] & ~(uint64_t)supported) {
                r2_set_error(error, error_len, "unsupported guest dlopen process flags"); return R2_ERR_UNSUPPORTED;
            }
            runtime->process_opens++; regs[R2_X86_RAX] = (uintptr_t)&runtime->process_opens; return R2_OK;
        }
        char path[4096];
        if (regs[R2_X86_RDI] == 0) path[0] = 0;
        else {
            size_t i;
            for (i = 0; i < sizeof(path); i++) {
                const char *byte = r2_native_pointer(runtime, regs[R2_X86_RDI] + i, 1, false, error, error_len);
                if (byte == NULL) return R2_ERR_UNSUPPORTED;
                if ((path[i] = *byte) == 0) break;
            }
            if (i == sizeof(path)) { r2_set_error(error, error_len, "dlopen path is too long"); return R2_ERR_UNSUPPORTED; }
        }
        if (getenv("R2_TRACE_IMPORTS") || getenv("R2_TRACE_LOADER")) fprintf(stderr, "bridge dlopen path=%s\n", path);
        if (regs[R2_X86_RDI] && !path[0]) {
            snprintf(runtime->loader_error, sizeof(runtime->loader_error), "empty guest dlopen path");
            runtime->loader_error_pending = true; regs[R2_X86_RAX] = 0; return R2_OK;
        }
        for (unsigned i = 0; i < 4; i++) if (framework_path(&scopes[i], path))
            return framework_open(runtime, &scopes[i], i, regs[R2_X86_RSI], &regs[R2_X86_RAX], error, error_len);
        graphics_module *module = graphics_path(runtime, path);
        uint64_t caller = 0;
        if (!module) {
            const void *return_address = r2_native_pointer(runtime, regs[R2_X86_RSP], 8, false, error, error_len);
            if (!return_address) return R2_ERR_UNSUPPORTED;
            memcpy(&caller, return_address, 8);
        }
        r2_status status = module ? graphics_open(module, (int)regs[R2_X86_RSI], &handle, error, error_len) :
            runtime->open_image(runtime->loader_context, path, (int)regs[R2_X86_RSI], caller, &handle, error, error_len);
        memset(runtime->memory, 0, sizeof(runtime->memory));
        if (status == R2_ERR_IO) {
            if (getenv("R2_TRACE_LOADER")) fprintf(stderr, "bridge dlopen failed: %s\n", error);
            snprintf(runtime->loader_error, sizeof(runtime->loader_error), "%s", error);
            runtime->loader_error_pending = true;
            handle = 0;
        } else if (status != R2_OK) return status;
        regs[R2_X86_RAX] = handle;
        return R2_OK;
    }
    if (strcmp(name, "dladdr") == 0) {
        void *output = r2_native_pointer(runtime, regs[R2_X86_RSI], sizeof(r2_dl_info), true, error, error_len);
        if (output == NULL) return R2_ERR_UNSUPPORTED;
        r2_dl_info info = {0};
        bool found = runtime->address_lookup != NULL &&
                     runtime->address_lookup(runtime->loader_context, regs[R2_X86_RDI], &info);
        if (found) memcpy(output, &info, sizeof(info));
        regs[R2_X86_RAX] = found;
        return R2_OK;
    }
    if (strcmp(name, "dlerror") == 0) {
        regs[R2_X86_RAX] = runtime->loader_error_pending ? (uintptr_t)runtime->loader_error : 0;
        runtime->loader_error_pending = false;
        return R2_OK;
    }
    if (strcmp(name, "dlsym") == 0) {
        uint64_t handle = regs[R2_X86_RDI];
        if (handle == (uintptr_t)&runtime->process_opens && runtime->process_opens) handle = (uintptr_t)RTLD_DEFAULT;
        char symbol[256] = {'_'};
        size_t i;
        for (i = 1; i < sizeof(symbol); i++) {
            const char *byte = r2_native_pointer(runtime, regs[R2_X86_RSI] + i - 1, 1, false, error, error_len);
            if (byte == NULL) return R2_ERR_UNSUPPORTED;
            if ((symbol[i] = *byte) == 0) break;
        }
        if (i == sizeof(symbol)) { r2_set_error(error, error_len, "dlsym symbol is too long"); return R2_ERR_UNSUPPORTED; }
        uint64_t address = 0; bool found = false;
        native_framework_scope scopes[4]; framework_scopes(runtime, scopes);
        for (size_t j = 0; j < 4; j++) if (handle == (uintptr_t)scopes[j].references && *scopes[j].references)
            return framework_symbol(runtime, &scopes[j], symbol, &regs[R2_X86_RAX], error, error_len);
        graphics_module *module = graphics_handle(runtime, handle);
        if (module) {
            r2_status status = graphics_lookup(runtime, module, symbol + 1, &address, error, error_len);
            if (status != R2_OK) return status;
            regs[R2_X86_RAX] = address; return R2_OK;
        }
        if (runtime->lookup != NULL) {
            r2_status status = runtime->lookup(runtime->loader_context, handle, symbol, &address, &found, error, error_len);
            if (status != R2_OK) return status;
        }
        if (!found && handle == (uintptr_t)RTLD_DEFAULT) {
            for (size_t j = 0; j < 4; j++) if ((runtime->framework_global & (1u << j)) && *scopes[j].library &&
                dlsym(*scopes[j].library, symbol + 1))
                return framework_symbol(runtime, &scopes[j], symbol, &regs[R2_X86_RAX], error, error_len);
            for (size_t j = 0; j < GRAPHICS_MODULE_COUNT; j++) if (runtime->graphics[j].library && runtime->graphics[j].global && runtime->graphics[j].references) {
                r2_status status = graphics_lookup(runtime, &runtime->graphics[j], symbol + 1, &address, error, error_len);
                if (status != R2_OK) return status;
                if (address) { found = true; break; }
            }
        }
        if (!found) {
            /* Missing optional symbols remain missing. Existing native symbols
             * may only be returned as ABI-declared guest stubs/data. */
            void *host = handle == (uintptr_t)RTLD_DEFAULT ? dlsym(RTLD_DEFAULT, symbol + 1) : NULL;
            if (host == NULL && runtime->cpp != NULL && handle == (uintptr_t)RTLD_DEFAULT)
                host = dlsym(runtime->cpp, symbol + 1);
            if (host == NULL) {
                snprintf(runtime->loader_error, sizeof(runtime->loader_error), "symbol not found: %.230s", symbol + 1);
                runtime->loader_error_pending = true;
            } else {
                r2_status status = r2_native_symbol(runtime, symbol, &address, error, error_len);
                if (status != R2_OK) return status;
            }
        }
        regs[R2_X86_RAX] = address;
        return R2_OK;
    }
    if (strcmp(name, "pthread_get_stackaddr_np") == 0 || strcmp(name, "pthread_get_stacksize_np") == 0) {
        if (regs[R2_X86_RDI] != (uintptr_t)pthread_self() || native_thread->execution.stack_size == 0) {
            r2_set_error(error, error_len, "stack query for an unknown guest pthread"); return R2_ERR_UNSUPPORTED;
        }
        regs[R2_X86_RAX] = strcmp(name, "pthread_get_stackaddr_np") == 0
            ? native_thread->execution.stack_base + native_thread->execution.stack_size : native_thread->execution.stack_size;
        return R2_OK;
    }
    if (strcmp(name, "__cxa_atexit") == 0 || strcmp(name, "atexit") == 0) {
        if (runtime->exit_count == sizeof(runtime->exits)/sizeof(*runtime->exits)) {
            regs[R2_X86_RAX] = UINT32_MAX; return R2_OK;
        }
        size_t i = runtime->exit_count++;
        runtime->exits[i].function = regs[R2_X86_RDI];
        runtime->exits[i].argument = strcmp(name, "atexit") == 0 ? 0 : regs[R2_X86_RSI];
        runtime->exits[i].dso = regs[R2_X86_RDX];
        regs[R2_X86_RAX] = 0;
        return R2_OK;
    }
    if (strcmp(name, "semaphore_create") == 0) {
        void *output = r2_native_pointer(runtime, regs[R2_X86_RSI], sizeof(semaphore_t), true, error, error_len);
        if (output == NULL) return R2_ERR_UNSUPPORTED;
        if (runtime->semaphore_count == sizeof(runtime->semaphores)/sizeof(*runtime->semaphores)) {
            regs[R2_X86_RAX] = KERN_RESOURCE_SHORTAGE; return R2_OK;
        }
        semaphore_t sem;
        kern_return_t status = semaphore_create((task_t)regs[R2_X86_RDI], &sem,
                                                (int)regs[R2_X86_RDX], (int)regs[R2_X86_RCX]);
        if (status == KERN_SUCCESS) {
            runtime->semaphores[runtime->semaphore_count++] = sem;
            memcpy(output, &sem, sizeof(sem));
        }
        regs[R2_X86_RAX] = (uint32_t)status;
        return R2_OK;
    }
    if (strcmp(name, "semaphore_destroy") == 0) {
        size_t i = 0;
        for (; i < runtime->semaphore_count && runtime->semaphores[i] != (semaphore_t)regs[R2_X86_RSI]; i++) {}
        if (i == runtime->semaphore_count) { regs[R2_X86_RAX] = KERN_INVALID_ARGUMENT; return R2_OK; }
        kern_return_t status = semaphore_destroy((task_t)regs[R2_X86_RDI], runtime->semaphores[i]);
        if (status == KERN_SUCCESS) runtime->semaphores[i] = runtime->semaphores[--runtime->semaphore_count];
        regs[R2_X86_RAX] = (uint32_t)status;
        return R2_OK;
    }
    if (strcmp(name, "pthread_key_create") == 0) {
        void *output = r2_native_pointer(runtime, regs[R2_X86_RDI], sizeof(pthread_key_t), true, error, error_len);
        if (output == NULL) return R2_ERR_UNSUPPORTED;
        if (runtime->key_count == sizeof(runtime->keys)/sizeof(*runtime->keys)) {
            regs[R2_X86_RAX] = EAGAIN; return R2_OK;
        }
        pthread_key_t key;
        int status = pthread_key_create(&key, NULL);
        if (status == 0) {
            /* Keep guest callbacks as guest addresses. The host key has no
             * destructor; a future guest pthread_exit must interpret these.
             * Returning from main/process exit does not run POSIX key destructors. */
            runtime->keys[runtime->key_count].key = key;
            runtime->keys[runtime->key_count++].destructor = regs[R2_X86_RSI];
            memcpy(output, &key, sizeof(key));
        }
        regs[R2_X86_RAX] = (uint32_t)status;
        return R2_OK;
    }
    if (strcmp(name, "pthread_key_delete") == 0 || strcmp(name, "pthread_getspecific") == 0 ||
        strcmp(name, "pthread_setspecific") == 0) {
        size_t i = 0;
        for (; i < runtime->key_count && runtime->keys[i].key != (pthread_key_t)regs[R2_X86_RDI]; i++) {}
        if (i == runtime->key_count) { regs[R2_X86_RAX] = strcmp(name, "pthread_getspecific") == 0 ? 0 : EINVAL; return R2_OK; }
        pthread_key_t key = runtime->keys[i].key;
        if (strcmp(name, "pthread_getspecific") == 0) regs[R2_X86_RAX] = (uintptr_t)native_thread->key_values[i];
        else if (strcmp(name, "pthread_setspecific") == 0) {native_thread->key_values[i]=(void *)(uintptr_t)regs[R2_X86_RSI];regs[R2_X86_RAX]=0;}
        else {
            regs[R2_X86_RAX] = (uint32_t)pthread_key_delete(key);
            runtime->keys[i] = runtime->keys[--runtime->key_count];
            native_thread_state *states[321];size_t count=0;states[count++]=runtime->root_thread;
            for(size_t j=0;j<runtime->thread_count;j++)if(runtime->threads[j])states[count++]=runtime->threads[j]->state;
            for(size_t j=0;j<64;j++)if(runtime->callback_threads[j])states[count++]=runtime->callback_threads[j];
            for(size_t j=0;j<count;j++) {states[j]->key_values[i]=states[j]->key_values[runtime->key_count];states[j]->key_values[runtime->key_count]=NULL;}
        }
        return R2_OK;
    }
    if (strcmp(name, "exit") == 0 || strcmp(name,"_exit")==0) {
        if(strcmp(name,"_exit")==0&&native_thread->execution.env&&native_thread->execution.env->process) {
            native_thread->execution.env->process->kind=R2_PROCESS_IMMEDIATE;
            native_thread->execution.env->process->code=regs[R2_X86_RDI];
        }
        regs[R2_X86_RAX] = regs[R2_X86_RDI]; *exited = true; return R2_OK;
    }
    if (strcmp(name, "abort") == 0) { r2_set_error(error, error_len, "guest called abort"); return R2_ERR_UNSUPPORTED; }
    if (strcmp(name, "__cxa_pure_virtual") == 0 || strcmp(name, "__assert_rtn") == 0 || strcmp(name, "__stack_chk_fail") == 0) {
        r2_set_error(error, error_len, "guest runtime failure: %s", name); return R2_ERR_UNSUPPORTED;
    }
    if (strcmp(name, "qsort") == 0||strcmp(name,"qsort_r")==0)
        return sort_call(runtime,name,regs,error,error_len);
    if (strcmp(name, "setjmp") == 0 || strcmp(name, "_setjmp") == 0 ||
        strcmp(name, "longjmp") == 0 || strcmp(name, "_longjmp") == 0 || strcmp(name,"sigsetjmp")==0 || strcmp(name,"siglongjmp")==0) {
        bool save = strcmp(name, "setjmp") == 0 || strcmp(name, "_setjmp") == 0 || strcmp(name,"sigsetjmp")==0;
        native_jump *jump = NULL, *empty = NULL;
        for (size_t i = 0; i < 128; i++) {
            if (native_thread->jumps[i].key == regs[R2_X86_RDI]) { jump = &native_thread->jumps[i]; break; }
            if (native_thread->jumps[i].key == 0 && empty == NULL) empty = &native_thread->jumps[i];
        }
        if (save && jump == NULL) jump = empty;
        if (jump == NULL || regs[R2_X86_RDI] == 0 || (!save && jump->key == 0)) {
            r2_set_error(error, error_len, "unknown or exhausted guest jump context"); return R2_ERR_UNSUPPORTED;
        }
        if (save) {
            void *stack = r2_native_pointer(runtime, regs[R2_X86_RSP], 8, false, error, error_len);
            if (stack == NULL) return R2_ERR_UNSUPPORTED;
            jump->key = regs[R2_X86_RDI];
            memcpy(jump->regs, regs, sizeof(jump->regs)); memcpy(jump->xmm, xmm, sizeof(jump->xmm));
            memcpy(&jump->return_address, stack, 8);
            jump->save_mask = strcmp(name, "setjmp") == 0 || (strcmp(name,"sigsetjmp")==0&&regs[R2_X86_RSI]!=0);
            jump->mask=*guest_signal_mask();jump->signal_frames=native_thread->execution.signal_frames?*native_thread->execution.signal_frames:0;
            jump->suspend_depth=native_thread->suspend_depth;jump->block=native_thread->execution.block;
            regs[R2_X86_RAX] = 0;
        } else {
            if(jump->block!=native_thread->execution.block) {r2_set_error(error,error_len,"guest longjmp crosses a native callback boundary");return R2_ERR_UNSUPPORTED;}
            uint32_t value = (uint32_t)regs[R2_X86_RSI];
            memcpy(regs, jump->regs, sizeof(jump->regs)); memcpy(xmm, jump->xmm, sizeof(jump->xmm));
            void *stack = r2_native_pointer(runtime, regs[R2_X86_RSP], 8, true, error, error_len);
            if (stack == NULL) return R2_ERR_UNSUPPORTED;
            memcpy(stack, &jump->return_address, 8);
            if (jump->save_mask && strcmp(name,"_longjmp")!=0)*guest_signal_mask()=jump->mask;
            if(native_thread->execution.signal_frames)*native_thread->execution.signal_frames=jump->signal_frames;
            native_thread->suspend_depth=jump->suspend_depth;
            regs[R2_X86_RAX] = value == 0 ? 1 : value;
        }
        return R2_OK;
    }
    if (strcmp(name, "__sincosf_stret") == 0) {
        float argument, sine, cosine; memcpy(&argument, xmm[0], 4); sine = sinf(argument); cosine = cosf(argument);
        memset(xmm[0], 0, 16); memcpy(xmm[0], &sine, 4); memcpy(xmm[0] + 4, &cosine, 4); return R2_OK;
    }
    if (strcmp(name, "__sincos_stret") == 0) {
        double argument, sine, cosine;
        memcpy(&argument, xmm[0], 8); sine = sin(argument); cosine = cos(argument);
        memcpy(xmm[0], &sine, 8); memcpy(xmm[1], &cosine, 8);
        return R2_OK;
    }
    if (strcmp(name, "signal") == 0) {
        unsigned signum = (unsigned)regs[R2_X86_RDI];
        if (signum == 0 || signum >= NSIG || signum == SIGKILL || signum == SIGSTOP) {
            errno = EINVAL; regs[R2_X86_RAX] = UINT64_MAX; return R2_OK;
        }
        uint64_t handler = regs[R2_X86_RSI], old_handler = runtime->signal_handlers[signum];
        struct sigaction host = {0}, previous;
        host.sa_flags = SA_RESTART;
        host.sa_handler = handler == 0 ? SIG_DFL : handler == 1 ? SIG_IGN : signal_proxy;
        bool synchronous = signum == SIGSEGV || signum == SIGBUS || signum == SIGILL || signum == SIGFPE;
        if (!synchronous) {
            if (sigaction((int)signum, &host, &previous) != 0) { regs[R2_X86_RAX] = UINT64_MAX; return R2_OK; }
            if (!runtime->installed_signals[signum]) runtime->original_signals[signum] = previous;
            runtime->installed_signals[signum] = true;
        }
        runtime->signal_handlers[signum] = handler;
        runtime->signal_masks[signum] = 0; runtime->signal_flags[signum] = SA_RESTART;
        signal_runtime = runtime;
        regs[R2_X86_RAX] = old_handler; return R2_OK;
    }
    if (strcmp(name, "SDL_OpenAudioDevice") == 0 && import->graphics_kind == 0) {
        const uint8_t *spec = r2_native_pointer(runtime, regs[R2_X86_RDX], 32, false, error, error_len);
        if (spec == NULL) return R2_ERR_UNSUPPORTED;
        uint64_t callback;
        memcpy(&callback, spec + 16, 8);
        if ((uint32_t)regs[R2_X86_RSI] != 0 || callback != 0) {
            r2_set_error(error, error_len, "host audio supports queued playback only; capture and guest callbacks are unsupported");
            return R2_ERR_UNSUPPORTED;
        }
    }
    void **music_objects = NULL;
    size_t music_slot = 32;
    if (strcmp(name, "NewMusicPlayer") == 0 || strcmp(name, "NewMusicSequence") == 0) {
        music_objects = strcmp(name, "NewMusicPlayer") == 0 ? runtime->music_players : runtime->music_sequences;
        for (size_t i = 0; i < 32; i++) if (music_objects[i] == NULL) { music_slot = i; break; }
        if (music_slot == 32) {
            r2_set_error(error, error_len, "too many native music objects"); return R2_ERR_UNSUPPORTED;
        }
    }
    if (strcmp(name, "___chkstk_darwin") == 0) {
        if (regs[R2_X86_R11] > 1024u * 1024u) {
            r2_set_error(error, error_len, "guest stack probe exceeds stack allocation"); return R2_ERR_UNSUPPORTED;
        }
        regs[R2_X86_RAX] = regs[R2_X86_R11];
        return R2_OK;
    }
    if (strcmp(name, "sigaction") == 0) {
        unsigned signum = (unsigned)regs[R2_X86_RDI];
        if (signum == 0 || signum >= NSIG || signum == SIGKILL || signum == SIGSTOP) {
            errno = EINVAL; regs[R2_X86_RAX] = UINT32_MAX; return R2_OK;
        }
        uint64_t action = regs[R2_X86_RSI], old_action = regs[R2_X86_RDX];
        uint64_t handler = 0;
        uint32_t mask = 0, flags = 0;
        if (action != 0) {
            uint8_t *bytes = r2_native_pointer(runtime, action, 16, false, error, error_len);
            if (bytes == NULL) return R2_ERR_UNSUPPORTED;
            memcpy(&handler, bytes, 8); memcpy(&mask, bytes + 8, 4); memcpy(&flags, bytes + 12, 4);
        }
        if (old_action != 0) {
            uint8_t *bytes = r2_native_pointer(runtime, old_action, 16, true, error, error_len);
            if (bytes == NULL) return R2_ERR_UNSUPPORTED;
            memcpy(bytes, &runtime->signal_handlers[signum], 8);
            memcpy(bytes + 8, &runtime->signal_masks[signum], 4);
            memcpy(bytes + 12, &runtime->signal_flags[signum], 4);
        }
        if (action != 0) {
            struct sigaction host = {0};
            host.sa_mask = mask;
            host.sa_flags = (int)(flags & (SA_RESTART | SA_NODEFER));
            host.sa_handler = handler == 0 ? SIG_DFL : handler == 1 ? SIG_IGN : signal_proxy;
            /* Synchronous guest faults originate in the interpreter. Preserve
             * native handlers (including ASan's) for faults in ARM host code. */
            bool synchronous = signum == SIGSEGV || signum == SIGBUS || signum == SIGILL || signum == SIGFPE;
            if (!synchronous) {
                struct sigaction previous;
                if (sigaction((int)signum, &host, &previous) != 0) { regs[R2_X86_RAX] = UINT32_MAX; return R2_OK; }
                if (!runtime->installed_signals[signum]) runtime->original_signals[signum] = previous;
                runtime->installed_signals[signum] = true;
            }
            runtime->signal_handlers[signum] = handler;
            runtime->signal_masks[signum] = mask;
            runtime->signal_flags[signum] = flags;
            signal_runtime = runtime;
        }
        regs[R2_X86_RAX] = 0;
        return R2_OK;
    }
    char types[NATIVE_MAX_ARGS];
    size_t fixed = strlen(definition->arguments), count = fixed;
    if (fixed > NATIVE_MAX_ARGS) { r2_set_error(error, error_len, "too many host ABI arguments"); return R2_ERR_UNSUPPORTED; }
    memcpy(types, definition->arguments, fixed);
    uint64_t values[NATIVE_MAX_ARGS] = {0};
    for (size_t i = 0; i < fixed; i += 1) values[i] = guest_argument(regs, i);
    void *function = import->function;
    int format_index = definition->format;
    bool va_list_call = strcmp(name, "vfprintf") == 0 || strcmp(name, "vsnprintf") == 0 || strcmp(name, "vsscanf") == 0;
    uint8_t *guest_va = NULL;
    if (va_list_call) {
        guest_va = (uint8_t *)(uintptr_t)values[fixed - 1];
        fixed -= 1; count = fixed; format_index = (int)fixed - 1;
        function = dlsym(RTLD_DEFAULT, strcmp(name, "vfprintf") == 0 ? "fprintf" : strcmp(name, "vsscanf") == 0 ? "sscanf" : "snprintf");
    }
    if (format_index >= 0) {
        const char *format = (const char *)(uintptr_t)values[format_index];
        if (strcmp(name, "sscanf") == 0 || strcmp(name, "sscanf_l") == 0 || strcmp(name, "vsscanf") == 0 || strcmp(name, "fscanf") == 0) {
            for (const char *p = format; *p; p += 1) {
                if (*p != '%') continue;
                p += 1;
                if (*p == '%') continue;
                bool suppressed = *p == '*';
                if (suppressed) p += 1;
                while (isdigit((unsigned char)*p)) p += 1;
                if (*p == 'L') {
                    r2_set_error(error, error_len, "long-double scanf is unsupported across host ABIs");
                    return R2_ERR_UNSUPPORTED;
                }
                while (*p && strchr("hljzt", *p)) p += 1;
                if (*p == '[') { p += 1; if (*p == '^') p += 1; if (*p == ']') p += 1; while (*p && *p != ']') p += 1; }
                if (!*p || strchr("diuoxXfFeEgGaAcspn]", *p) == NULL || count >= NATIVE_MAX_ARGS) {
                    r2_set_error(error, error_len, "unsupported host scanf format: %.80s", format);
                    return R2_ERR_UNSUPPORTED;
                }
                if (!suppressed) types[count++] = 'p';
            }
        } else if (!printf_types(format, types, &count)) {
            r2_set_error(error, error_len, "unsupported host variadic format in %s: %.80s", name, format);
            return R2_ERR_UNSUPPORTED;
        }
    }
    if (guest_va != NULL) {
        uint32_t gp_offset, fp_offset;
        uint64_t overflow, save;
        memcpy(&gp_offset, guest_va, 4); memcpy(&fp_offset, guest_va + 4, 4);
        memcpy(&overflow, guest_va + 8, 8); memcpy(&save, guest_va + 16, 8);
        for (size_t i = fixed; i < count; i += 1) {
            bool floating = types[i] == 'd' || types[i] == 'f';
            if (floating && fp_offset < 176) {
                memcpy(&values[i], (void *)(uintptr_t)(save + fp_offset), 8); fp_offset += 16;
            } else if (!floating && gp_offset < 48) {
                memcpy(&values[i], (void *)(uintptr_t)(save + gp_offset), 8); gp_offset += 8;
            } else { memcpy(&values[i], (void *)(uintptr_t)overflow, 8); overflow += 8; }
        }
    } else {
        size_t gp = 0, fp = 0, stack = 0;
        for (size_t i = 0; i < count; i += 1) {
            bool floating = types[i] == 'd' || types[i] == 'f';
            if (floating && fp < 8) {
                memcpy(&values[i], xmm[fp++], types[i] == 'f' ? 4 : 8);
            } else if (!floating && gp < 6) values[i] = guest_argument(regs, gp++);
            else {
                memcpy(&values[i], (void *)(uintptr_t)(regs[R2_X86_RSP] + 8u + stack * 8u), 8);
                stack += 1;
            }
        }
    }
    if (import->graphics_kind) {
        r2_status graphics_status = graphics_prepare(runtime, import, values, error, error_len);
        if (graphics_status != R2_OK) return graphics_status;
    }
    mach_port_t *new_port_output = NULL;
    bool io_port_result = strcmp(name, "IOIteratorNext") == 0 || strcmp(name, "IOServiceGetMatchingService") == 0 || strcmp(name, "IORegistryEntryFromPath") == 0;
    int new_port_argument = -1;
    if (strcmp(name, "IOMasterPort") == 0 || strcmp(name, "IOMainPort") == 0)
        new_port_argument = 1;
    else if (strcmp(name, "IOServiceGetMatchingServices") == 0 || strcmp(name, "IORegistryEntryGetChildIterator") == 0 ||
             strcmp(name, "IORegistryEntryGetParentIterator") == 0 || strcmp(name, "IORegistryEntryGetChildEntry") == 0 || strcmp(name, "IORegistryEntryGetParentEntry") == 0)
        new_port_argument = 2;
    if (new_port_argument >= 0) {
        new_port_output = r2_native_pointer(runtime, values[new_port_argument], sizeof(mach_port_t), true, error, error_len);
        if (!new_port_output) return R2_ERR_UNSUPPORTED;
    }
    size_t zslot = SIZE_MAX; z_stream *guest_stream = NULL;
    bool zinit = strcmp(name, "inflateInit2_") == 0 || strcmp(name, "deflateInit2_") == 0;
    bool zend = strcmp(name, "inflateEnd") == 0 || strcmp(name, "deflateEnd") == 0;
    if (strncmp(name, "inflate", 7) == 0 || strncmp(name, "deflate", 7) == 0) {
        _Static_assert(sizeof(z_stream) == 112, "Darwin x86_64 z_stream ABI");
        guest_stream = r2_native_pointer(runtime, values[0], sizeof(z_stream), true, error, error_len);
        if (guest_stream == NULL) return R2_ERR_UNSUPPORTED;
        if (guest_stream->zalloc != NULL || guest_stream->zfree != NULL) {
            r2_set_error(error, error_len, "guest zlib allocation callbacks are unsupported"); return R2_ERR_UNSUPPORTED;
        }
        for (size_t i = 0; i < 256; i++) if (runtime->zstreams[i].guest == values[0]) { zslot = i; break; }
        if (zinit) {
            if (zslot != SIZE_MAX) { regs[R2_X86_RAX] = (uint32_t)Z_STREAM_ERROR; return R2_OK; }
            for (size_t i = 0; i < 256; i++) if (!runtime->zstreams[i].guest) { zslot = i; break; }
            if (zslot == SIZE_MAX) { regs[R2_X86_RAX] = (uint32_t)Z_MEM_ERROR; return R2_OK; }
            runtime->zstreams[zslot].stream = *guest_stream;
            runtime->zstreams[zslot].deflating = name[0] == 'd';
        } else {
            if (zslot == SIZE_MAX || guest_stream->state != runtime->zstreams[zslot].stream.state) {
                regs[R2_X86_RAX] = (uint32_t)Z_STREAM_ERROR; return R2_OK;
            }
            z_stream *host = &runtime->zstreams[zslot].stream;
            host->next_in = guest_stream->next_in; host->avail_in = guest_stream->avail_in;
            host->next_out = guest_stream->next_out; host->avail_out = guest_stream->avail_out;
        }
        values[0] = (uintptr_t)&runtime->zstreams[zslot].stream;
    }
    ffi_type *ffi_types[NATIVE_MAX_ARGS];
    void *arguments[NATIVE_MAX_ARGS];
    for (size_t i = 0; i < count; i += 1) { ffi_types[i] = argument_type(types[i]); arguments[i] = &values[i]; }
    ffi_cif cif;
    bool system_variadic = strcmp(name, "open") == 0 || strcmp(name, "fcntl") == 0 || strcmp(name, "openat") == 0;
    ffi_status status = format_index >= 0 || system_variadic
        ? ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, system_variadic ? (strcmp(name, "openat") == 0 ? 3u : 2u) : (unsigned)fixed,
                           (unsigned)count, argument_type(definition->result), ffi_types)
        : ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)count, argument_type(definition->result), ffi_types);
    if (status != FFI_OK) { r2_set_error(error, error_len, "cannot prepare ABI call to %s", name); return R2_ERR_UNSUPPORTED; }
    uint64_t result = 0;
    unsigned call_depth = import->graphics_kind || strncmp(name,"__r2_objc_imp_",14)==0 || strcmp(name,"objc_alloc")==0 || strcmp(name,"objc_alloc_init")==0 || strcmp(name,"objc_release")==0 || strcmp(name,"objc_opt_respondsToSelector")==0 || strcmp(name, "getnameinfo") == 0 || strcmp(name, "usleep") == 0 || strcmp(name, "nanosleep") == 0 ||
        strcmp(name,"CFRelease")==0 || strcmp(name,"CGFontRelease")==0 || strcmp(name,"CGDataProviderRelease")==0 || strcmp(name,"CFRunLoopRunInMode")==0 || strcmp(name,"fsync")==0 || strcmp(name,"sendfile")==0 || strcmp(name,"fcopyfile")==0 || strcmp(name,"clonefile")==0
        ? engine_suspend(runtime) : 0;
    ffi_call(&cif, FFI_FN(function), &result, arguments);
    if (call_depth) { int call_errno = errno; engine_resume(runtime, call_depth); errno = call_errno; }
    if ((io_port_result && !remember_native_port(runtime, (mach_port_t)result)) ||
        (new_port_output && (uint32_t)result == KERN_SUCCESS && !remember_native_port(runtime, *new_port_output))) {
        r2_set_error(error, error_len, "guest IOKit port table exhausted"); return R2_ERR_UNSUPPORTED;
    }
    if (zslot != SIZE_MAX) {
        z_stream visible = runtime->zstreams[zslot].stream;
        visible.zalloc = NULL; visible.zfree = NULL; visible.opaque = guest_stream->opaque;
        memcpy(guest_stream, &visible, sizeof(visible));
        if (zinit && (int32_t)result == Z_OK) runtime->zstreams[zslot].guest = (uintptr_t)guest_stream;
        if (zend && (int32_t)result == Z_OK) runtime->zstreams[zslot].guest = 0;
    }
    if ((uint32_t)result == 0 && music_objects != NULL)
        memcpy(&music_objects[music_slot], (void *)(uintptr_t)values[0], sizeof(void *));
    if ((uint32_t)result == 0 && (strcmp(name, "DisposeMusicPlayer") == 0 || strcmp(name, "DisposeMusicSequence") == 0)) {
        void **objects = strcmp(name, "DisposeMusicPlayer") == 0 ? runtime->music_players : runtime->music_sequences;
        for (size_t i = 0; i < 32; i++) if (objects[i] == (void *)(uintptr_t)values[0]) objects[i] = NULL;
    }
    if (definition->result == 'd' || definition->result == 'f') {
        memset(xmm[0], 0, 16);
        memcpy(xmm[0], &result, definition->result == 'f' ? 4 : 8);
    } else regs[R2_X86_RAX] = definition->result == 'i' ? (uint32_t)result :
        definition->result == 'b' ? (uint8_t)result : definition->result == 'h' ? (uint16_t)result :
        definition->result == 'c' ? (uint32_t)(int32_t)(int8_t)result : definition->result == 's' ? (uint32_t)(int32_t)(int16_t)result : result;
    memset(runtime->memory, 0, sizeof(runtime->memory));
    return R2_OK;
}

r2_status r2_native_call(r2_native_runtime *runtime, uint64_t regs[16], uint8_t xmm[16][16],
                         bool *exited, char *error, size_t error_len)
{
    if (native_thread == NULL || native_thread->runtime != runtime) {
        r2_set_error(error, error_len, "host thread has no guest execution context"); return R2_ERR_UNSUPPORTED;
    }
    bool acquire = native_thread->engine_depth == 0;
    if (acquire) engine_enter(runtime);
    uint64_t previous_callback_stack = native_thread->callback_stack;
    native_thread->callback_stack = regs[R2_X86_RSP];
    r2_status status = native_call_impl(runtime, regs, xmm, exited, error, error_len);
    native_thread->callback_stack = previous_callback_stack;
    if (acquire) engine_leave(runtime);
    return status;
}
