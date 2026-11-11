/* JNI invocation driver, compiled as x86_64 alongside the unmodified x86 JVM.
 * It skips the Java launcher's Cocoa wrapper, not the JVM itself. */
#include <jni.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* OpenJDK's libjimage C API. These pointers refer to x86 guest functions. */
static int image_probe(int argc, char **argv)
{
    if (argc != 5) return 6;
    void *library = dlopen(argv[2], RTLD_NOW);
    if (library == NULL) { fprintf(stderr, "JIMAGE dlopen: %s\n", dlerror()); return 6; }
    void *(*open_image)(const char *, jint *) = dlsym(library, "JIMAGE_Open");
    void (*close_image)(void *) = dlsym(library, "JIMAGE_Close");
    jlong (*find_resource)(void *, const char *, const char *, const char *, jlong *) = dlsym(library, "JIMAGE_FindResource");
    jlong (*get_resource)(void *, jlong, char *, jlong) = dlsym(library, "JIMAGE_GetResource");
    if (!open_image || !close_image || !find_resource || !get_resource) return 6;
    jint error = 0;
    void *image = open_image(argv[3], &error);
    if (image == NULL) { fprintf(stderr, "JIMAGE open error=%i\n", error); return 6; }
    jlong size = 0;
    jlong location = find_resource(image, "java.base", "", "java/lang/Object.class", &size);
    if (!location || size < 100 || size > 10 * 1024 * 1024) { close_image(image); return 6; }
    unsigned char *bytes = malloc((size_t)size);
    if (bytes == NULL) { close_image(image); return 6; }
    jlong read = get_resource(image, location, (char *)bytes, size);
    int valid = read == size && memcmp(bytes, "\xca\xfe\xba\xbe", 4) == 0;
    if (valid) {
        FILE *output = fopen(argv[4], "wb");
        if (output == NULL) valid = 0;
        else {
            valid = fwrite(bytes, 1, (size_t)size, output) == (size_t)size;
            if (fclose(output) != 0) valid = 0;
        }
        printf("R2_JIMAGE java/lang/Object.class size=%lld major=%u\n", (long long)size, ((unsigned)bytes[6] << 8) | bytes[7]);
    }
    free(bytes); close_image(image);
    if (dlclose(library) != 0) valid = 0;
    return valid ? 0 : 6;
}

int main(int argc, char **argv)
{
    JavaVMInitArgs arguments;
    memset(&arguments, 0, sizeof(arguments));
    arguments.version = JNI_VERSION_24;
    jint status = JNI_GetDefaultJavaVMInitArgs(&arguments);
    printf("R2_JVM default_args status=%i version=0x%x\n", status, arguments.version);
    if (status != JNI_OK) return 1;
    JavaVM *existing = NULL;
    jsize count = -1;
    status = JNI_GetCreatedJavaVMs(&existing, 1, &count);
    printf("R2_JVM created_vms status=%i count=%i\n", status, count);
    fflush(stdout);
    if (status != JNI_OK || count != 0) return 2;
    if (argc >= 2 && strcmp(argv[1], "image") == 0) return image_probe(argc, argv);
    if (argc < 2 || strcmp(argv[1], "create") != 0) return 0;
    JavaVMOption options[] = {
        {.optionString = "-Xint"}, {.optionString = "-Xrs"},
        {.optionString = "-XX:+UseSerialGC"}, {.optionString = "-XX:ActiveProcessorCount=1"},
        {.optionString = "-Xms16m"}, {.optionString = "-Xmx64m"},
        {.optionString = "-XX:UseAVX=0"}, {.optionString = "-XX:UseSSE=2"},
    };
    arguments.nOptions = sizeof(options) / sizeof(options[0]);
    arguments.options = options;
    arguments.ignoreUnrecognized = JNI_FALSE;
    JavaVM *vm = NULL;
    JNIEnv *env = NULL;
    status = JNI_CreateJavaVM(&vm, (void **)&env, &arguments);
    printf("R2_JVM create_vm status=%i\n", status);
    fflush(stdout);
    if (status != JNI_OK) return 3;
    jclass system = (*env)->FindClass(env, "java/lang/System");
    if (system == NULL) { (*env)->ExceptionDescribe(env); (*vm)->DestroyJavaVM(vm); return 4; }
    jmethodID property = (*env)->GetStaticMethodID(env, system, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
    if (property == NULL) { (*env)->ExceptionDescribe(env); (*vm)->DestroyJavaVM(vm); return 4; }
    jstring key = (*env)->NewStringUTF(env, "java.version");
    if (key == NULL) { (*env)->ExceptionDescribe(env); (*vm)->DestroyJavaVM(vm); return 4; }
    jstring version = (*env)->CallStaticObjectMethod(env, system, property, key);
    if ((*env)->ExceptionCheck(env) || version == NULL) { (*env)->ExceptionDescribe(env); (*vm)->DestroyJavaVM(vm); return 4; }
    const char *text = (*env)->GetStringUTFChars(env, version, NULL);
    if (text == NULL) { (*env)->ExceptionDescribe(env); (*vm)->DestroyJavaVM(vm); return 4; }
    printf("R2_JVM java.version=%s\n", text);
    (*env)->ReleaseStringUTFChars(env, version, text);
    return (*vm)->DestroyJavaVM(vm) == JNI_OK ? 0 : 5;
}
