/* Remaining function imports from Oracle JDK 25.0.4 macOS x64 libjvm/libjimage.
 * Diagnostic bindings only: every entry traps if called.
 * Implemented ABI declarations live in native.c. */
    {"__cxa_begin_catch", "", '!', -1},
    {"__cxa_allocate_exception", "", '!', -1},
    {"__cxa_free_exception", "", '!', -1},
    {"__cxa_end_catch", "", '!', -1},
    {"__cxa_rethrow", "", '!', -1},
    {"__cxa_throw", "", '!', -1},
    {"__gxx_personality_v0", "", '!', -1},
    {"_Unwind_Resume", "", '!', -1},
    {"mach_vm_remap", "", '!', -1},
    {"sleep", "", '!', -1},
    {"umask", "", '!', -1},
    {"waitpid", "", '!', -1},
    {"execve", "", '!', -1},
    {"execvp", "", '!', -1},
    {"fork", "", '!', -1},
    {"waitid", "", '!', -1},

/* Imports in the installed x86 JDK network/NIO libraries. */
{"CFNetworkExecuteProxyAutoConfigurationURL", "", '!', -1},
{"copyfile_state_set", "", '!', -1},

/* Guest libc++ paths whose return representation or waits need emulation. */
{"strtold", "", '!', -1},
{"strtold_l", "", '!', -1},
{"wcstold", "", '!', -1},
{"swprintf", "", '!', -1},

/* Exception-only paths in the unmodified x86 Java/Objective-C adapter. */
{"objc_begin_catch", "", '!', -1},
{"objc_end_catch", "", '!', -1},
{"objc_enumerationMutation", "", '!', -1},
{"__objc_personality_v0", "", '!', -1},

/* These runtime APIs expose IMPs or aggregate values and must not leak ARM code. */
{"class_getMethodImplementation_stret", "", '!', -1},
{"class_replaceMethod", "", '!', -1},
{"method_exchangeImplementations", "", '!', -1},
{"method_setImplementation", "", '!', -1},
{"objc_msgSendSuper", "", '!', -1},
{"objc_msgSendSuper_stret", "", '!', -1},
{"protocol_getMethodDescription", "", '!', -1},

/* IMP installation and global mutation callbacks need callback conversion. */
{"class_addMethod", "", '!', -1},
{"objc_setEnumerationMutationHandler", "", '!', -1},
{"imp_implementationWithBlock", "", '!', -1},
{"imp_getBlock", "", '!', -1},
{"imp_removeBlock", "", '!', -1},
