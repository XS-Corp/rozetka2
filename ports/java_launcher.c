/* JNI launcher: all code in this binary and in libjvm executes as x86 guest. */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    JavaVMOption options[128] = {
        {.optionString="-Xint"}, {.optionString="-Xrs"},
        {.optionString="-XX:+UseSerialGC"}, {.optionString="-XX:ActiveProcessorCount=1"},
        {.optionString="-Xms64m"}, {.optionString="-Xmx1g"},
        {.optionString="-XX:UseAVX=0"}, {.optionString="-XX:UseSSE=2"},
    };
    int count=8, cursor=1;
    while (cursor<argc && strcmp(argv[cursor],"--")!=0) {
        if (count==128) { fputs("Too many JVM options\n",stderr); return 2; }
        options[count++].optionString=argv[cursor++];
    }
    if (cursor==argc || ++cursor==argc) { fputs("Usage: java-x86_64 [JVM options] -- class [arguments]\n",stderr); return 2; }
    JavaVMInitArgs arguments={.version=JNI_VERSION_24,.nOptions=count,.options=options,.ignoreUnrecognized=JNI_FALSE};
    JavaVM *vm=NULL; JNIEnv *env=NULL;
    jint status=JNI_CreateJavaVM(&vm,(void **)&env,&arguments);
    if(status!=JNI_OK) { fprintf(stderr,"JNI_CreateJavaVM failed: %d\n",status); return 3; }
    char *name=strdup(argv[cursor++]); if(!name)return 4;
    for(char *p=name;*p;p++)if(*p=='.')*p='/';
    printf("R2_JAVA main=%s\n",name); fflush(stdout);
    jclass main_class=(*env)->FindClass(env,name); free(name);
    if(main_class==NULL)goto exception;
    puts("R2_JAVA class_loaded"); fflush(stdout);
    jmethodID main_method=(*env)->GetStaticMethodID(env,main_class,"main","([Ljava/lang/String;)V");
    if(main_method==NULL)goto exception;
    puts("R2_JAVA method_resolved"); fflush(stdout);
    jclass string_class=(*env)->FindClass(env,"java/lang/String");
    if(string_class==NULL)goto exception;
    jobjectArray strings=(*env)->NewObjectArray(env,argc-cursor,string_class,NULL);
    if(strings==NULL)goto exception;
    for(int i=cursor;i<argc;i++) {
        jstring value=(*env)->NewStringUTF(env,argv[i]);
        if(value==NULL)goto exception;
        (*env)->SetObjectArrayElement(env,strings,i-cursor,value);
        (*env)->DeleteLocalRef(env,value);
        if((*env)->ExceptionCheck(env))goto exception;
    }
    puts("R2_JAVA invoking_main"); fflush(stdout);
    (*env)->CallStaticVoidMethod(env,main_class,main_method,strings);
    if((*env)->ExceptionCheck(env))goto exception;
    return (*vm)->DestroyJavaVM(vm)==JNI_OK?0:5;
exception:
    (*env)->ExceptionDescribe(env);
    (*vm)->DestroyJavaVM(vm);
    return 6;
}
