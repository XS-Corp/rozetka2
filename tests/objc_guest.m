#include <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
__attribute__((objc_root_class)) @interface R2GuestClass
+(long)add:(long)a to:(long)b;
@end
@implementation R2GuestClass
+(long)add:(long)a to:(long)b {return a*7+b;}
@end
static unsigned destroyed;
@interface R2GuestObject:NSObject {long counter;void *tag;}
-(id)initWithValue:(long)value;
-(long)add:(long)value;
@end
@implementation R2GuestObject
-(id)initWithValue:(long)value {counter=value;tag=(void *)42;return self;}
-(long)add:(long)value {if(tag!=(void *)42)return -999;counter+=value;return counter;}
-(void)dealloc {destroyed++;[super dealloc];}
@end
static void require(int ok,const char *message){if(!ok){fprintf(stderr,"FAIL ObjC: %s\n",message);exit(91);}}
static void exception_handler(void *exception){(void)exception;}
int main(int argc,char **argv) {
    void *foundation=dlopen("/System/Library/Frameworks/Foundation.framework/Foundation",RTLD_NOW|RTLD_LOCAL);
    (void)foundation;
    void *runtime=dlopen("libobjc.A.dylib",RTLD_NOW|RTLD_LOCAL);require(runtime!=NULL,"named Objective-C runtime scope");
    if(argc==3&&strcmp(argv[1],"audit")==0) {
        FILE *list=fopen(argv[2],"r");require(list!=NULL,"open actual LWJGL runtime API list");char name[256];unsigned count=0;
        while(fgets(name,sizeof(name),list)){name[strcspn(name,"\r\n")]=0;if(!name[0])continue;(void)dlsym(runtime,name);count++;}
        require(!ferror(list)&&count>0,"read actual consumer API list");fclose(list);printf("PASS resolved %u actual LWJGL Objective-C API names without undeclared ABI\n",count);return 0;
    }
    Class (*get_class)(const char *)=dlsym(runtime,"objc_lookUpClass");SEL (*selector)(const char *)=dlsym(runtime,"sel_getUid");
    void *send=dlsym(runtime,"objc_msgSend");require(get_class&&selector&&send,"typed Objective-C symbols");
    /* Resolve Foundation through an ordinary declared function before classes. */
    const char *home=(const char *)dlsym(RTLD_DEFAULT,"NSHomeDirectory");require(home!=NULL,"Foundation function stub");((id (*)(void))home)();
    Class string_class=get_class("NSString");require(string_class!=NULL,"native NSString class");
    id (*send_string)(id,SEL,const char *)=send;id text=send_string((id)string_class,selector("stringWithUTF8String:"),"rozetka2");require(text!=NULL,"native NSString initializer from runtime encoding");
    unsigned long (*send_length)(id,SEL)=send;require(send_length(text,selector("length"))==8,"NSUInteger method result");
    IMP native_imp=method_getImplementation(class_getInstanceMethod(object_getClass(text),selector("length")));
    require(native_imp&&((unsigned long(*)(id,SEL))native_imp)(text,selector("length"))==8,"native IMP getter returns typed guest-callable stub");
    void *floating=dlsym(runtime,"objc_msgSend_fpret");require(floating!=NULL,"x86-only Objective-C entry point has a guest ABI adapter");
    const char *(*send_bytes)(id,SEL)=send;require(strcmp(send_bytes(text,selector("UTF8String")),"rozetka2")==0,"native UTF-8 method result");
    id signature=((id (*)(id,SEL,SEL))send)(text,selector("methodSignatureForSelector:"),selector("length"));
    require(signature&&send_length(signature,selector("numberOfArguments"))==2,"native method signature introspection");
    require(strcmp(send_bytes(signature,selector("methodReturnType")),"Q")==0,"native type encoding");
    id number=((id (*)(id,SEL,double))send)((id)get_class("NSNumber"),selector("numberWithDouble:"),2.5);
    require(((double (*)(id,SEL))send)(number,selector("doubleValue"))==2.5,"XMM floating argument and result ABI");
    require(((double(*)(id,SEL))floating)(number,selector("doubleValue"))==2.5,"JNA floating Objective-C entry point preserves double result");
    if(argc>1&&strcmp(argv[1],"block")==0){((id (*)(id,SEL,void *))send)((id)get_class("NSBlockOperation"),selector("blockOperationWithBlock:"),(void *)(uintptr_t)1);return 90;}
    void (*set_handler)(void *)=dlsym(RTLD_DEFAULT,"NSSetUncaughtExceptionHandler");void *(*get_handler)(void)=dlsym(RTLD_DEFAULT,"NSGetUncaughtExceptionHandler");
    require(set_handler&&get_handler,"native exception handler procedures");void *old_handler=get_handler();set_handler((void *)exception_handler);require(get_handler()==(void *)exception_handler,"exception callback getter retains guest function identity");set_handler(old_handler);
    NSNumber *boxed=[NSNumber numberWithDouble:3.5];require([boxed doubleValue]==3.5&&[boxed respondsToSelector:@selector(doubleValue)],"guest image selector references are canonicalized for native dispatch and SEL arguments");
    require(class_respondsToSelector(get_class("NSNumber"),@selector(doubleValue))&&class_getInstanceMethod(get_class("NSNumber"),@selector(doubleValue))!=NULL,"C runtime queries canonicalize guest selector references");
    require(sel_isEqual(@selector(doubleValue),selector("doubleValue")),"selector equality across guest image and native runtime");
    NSString *literal=@"rozetka2 literal";require([literal length]==16&&strcmp([literal UTF8String],"rozetka2 literal")==0,"guest constant NSString object");
    R2GuestObject *object=[[R2GuestObject alloc] initWithValue:20];require(object&&[object add:5]==25,"guest ObjC allocation, ivar layout and x86 instance IMP");
    Class guest_class=(Class)dlsym(RTLD_DEFAULT,"OBJC_CLASS_$_R2GuestObject");require(guest_class!=Nil&&((BOOL(*)(id,SEL,Class))send)(object,selector("isKindOfClass:"),guest_class),"guest Class arguments map to native class adapters");
    IMP guest_imp=class_getMethodImplementation(object_getClass(object),@selector(add:));require(guest_imp&&((long(*)(id,SEL,long))guest_imp)(object,@selector(add:),0)==25,"guest IMP getter returns executable x86 entry point");
    NSMethodSignature *sig=[object methodSignatureForSelector:@selector(add:)];NSInvocation *invocation=[NSInvocation invocationWithMethodSignature:sig];
    long amount=17,answer=0;[invocation setTarget:object];[invocation setSelector:@selector(add:)];[invocation setArgument:&amount atIndex:2];[invocation invoke];[invocation getReturnValue:&answer];
    require(answer==42,"native NSInvocation reenters guest x86 IMP");[object release];require(destroyed==1,"native release, guest dealloc and native super dispatch");
    NSLog(@"rozetka2 fixture %@ %d %.1f",@"checked",42,2.5);
    require([R2GuestClass add:5 to:9]==44,"guest Objective-C class method executes x86 implementation");
    require(((id (*)(id,SEL))send)(nil,selector("unknownNilSelector"))==nil,"nil message result");
    require(dlclose(runtime)==0,"close Objective-C scope");puts("PASS Objective-C native encodings, metadata, FP ABI and guest class methods");return 0;
}
