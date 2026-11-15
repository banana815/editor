#include "platform_library_source_internal.h"
#include <SDL3/SDL.h>
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct { jobject task; jmethodID cancel; } AndroidSource;
static PlatformLibrarySource *from_java(jlong value) { return (PlatformLibrarySource *)(uintptr_t)value; }
JNIEXPORT jboolean JNICALL Java_org_notelab_editor_LibraryHttpSource_nativeActive(JNIEnv *env,jclass type,jlong job) {
    (void)env;(void)type;return platform_library_source_active(from_java(job)) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_org_notelab_editor_LibraryHttpSource_nativeResponse(JNIEnv *env,jclass type,jlong job,jint status) {
    (void)env;(void)type;return platform_library_source_response(from_java(job),(unsigned)status) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_org_notelab_editor_LibraryHttpSource_nativeBytes(JNIEnv *env,jclass type,jlong job,jbyteArray data,jint size) {
    (void)type;
    unsigned char bytes[16384];
    if(!data || size<0 || (size_t)size>sizeof bytes || (*env)->GetArrayLength(env,data)<size) return JNI_FALSE;
    (*env)->GetByteArrayRegion(env,data,0,size,(jbyte *)bytes);
    if((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env);platform_library_source_error(from_java(job),"Cannot copy library download bytes");return JNI_FALSE; }
    return platform_library_source_append(from_java(job),bytes,(size_t)size) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_org_notelab_editor_LibraryHttpSource_nativeSuccess(JNIEnv *env,jclass type,jlong job) {
    (void)env;(void)type;platform_library_source_success(from_java(job));
}
JNIEXPORT void JNICALL Java_org_notelab_editor_LibraryHttpSource_nativeError(JNIEnv *env,jclass type,jlong job,jint reason) {
    (void)env;(void)type;
    platform_library_source_error(from_java(job),reason==1 ? "Library download exceeded its byte limit" :
        reason==3 ? "Cannot allocate library download buffer" : reason==4 ? "Library downloads require no global Java cookie or authentication handler" :
        reason==5 ? "Unsupported HTTP content encoding" : reason==6 ? "Incomplete HTTP response body" : "Library request failed: network, TLS, timeout, or permission error");
}
JNIEXPORT void JNICALL Java_org_notelab_editor_LibraryHttpSource_nativeDone(JNIEnv *env,jclass type,jlong job) {
    (void)env;(void)type;platform_library_source_backend_done(from_java(job));
}
static bool android_start(PlatformLibrarySource *job,void *userdata) {
    (void)userdata;
    JNIEnv *env=SDL_GetAndroidJNIEnv();
    if(!env) return false;
    if((*env)->PushLocalFrame(env,16)<0) { (*env)->ExceptionClear(env);return false; }
    bool ok=false;
    jobject activity=SDL_GetAndroidActivity();
    if(!activity || (*env)->ExceptionCheck(env)) goto done;
    jclass activity_type=(*env)->GetObjectClass(env,activity);
    if(!activity_type) goto done;
    jmethodID loader_method=(*env)->GetMethodID(env,activity_type,"getClassLoader","()Ljava/lang/ClassLoader;");
    if(!loader_method) goto done;
    jobject loader=(*env)->CallObjectMethod(env,activity,loader_method);
    if(!loader || (*env)->ExceptionCheck(env)) goto done;
    jclass loader_type=(*env)->GetObjectClass(env,loader);
    if(!loader_type) goto done;
    jmethodID load=(*env)->GetMethodID(env,loader_type,"loadClass","(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name=(*env)->NewStringUTF(env,"org.notelab.editor.LibraryHttpSource");
    if(!load || !name) goto done;
    jclass type=(jclass)(*env)->CallObjectMethod(env,loader,load,name);
    if(!type || (*env)->ExceptionCheck(env)) goto done;
    jmethodID create=(*env)->GetStaticMethodID(env,type,"create","(Ljava/lang/String;IJJ)Lorg/notelab/editor/LibraryHttpSource;");
    jmethodID start=(*env)->GetMethodID(env,type,"start","()Z");
    jmethodID cancel=(*env)->GetMethodID(env,type,"cancel","()V");
    jstring url=(*env)->NewStringUTF(env,platform_library_source_url(job)); /* Validated ASCII. */
    if(!create || !start || !cancel || !url) goto done;
    jobject local=(*env)->CallStaticObjectMethod(env,type,create,url,(jint)platform_library_source_timeout(job),
        (jlong)platform_library_source_byte_limit(job),(jlong)(uintptr_t)job);
    if(!local || (*env)->ExceptionCheck(env)) goto done;
    AndroidSource *context=calloc(1,sizeof *context);
    if(!context) goto done;
    context->task=(*env)->NewGlobalRef(env,local);context->cancel=cancel;
    if(!context->task) { free(context);goto done; }
    platform_library_source_set_context(job,context);
    ok=(*env)->CallBooleanMethod(env,local,start)==JNI_TRUE;
    /* start returns true only after thread.start succeeded. Its Java method
     * catches allocation/start failures, so false cannot have a late callback. */
done:
    if((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env);ok=false; }
    (*env)->PopLocalFrame(env,NULL);return ok;
}
static void android_cancel(PlatformLibrarySource *job) {
    AndroidSource *context=platform_library_source_context(job);
    JNIEnv *env=SDL_GetAndroidJNIEnv();
    if(context && env) {
        (*env)->CallVoidMethod(env,context->task,context->cancel);
        if((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
}
static void android_destroy(PlatformLibrarySource *job) {
    AndroidSource *context=platform_library_source_context(job);
    JNIEnv *env=SDL_GetAndroidJNIEnv();
    if(context) { if(env) (*env)->DeleteGlobalRef(env,context->task);free(context); }
}
const PlatformLibrarySourceTransport platform_library_source_transport={android_start,android_cancel,android_destroy};
