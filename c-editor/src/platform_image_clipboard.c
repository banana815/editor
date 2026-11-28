#include "platform_image_clipboard.h"
#include "image_codec.h"
#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <jni.h>
static int android_image(void **bytes,size_t *size) {
    JNIEnv *env=SDL_GetAndroidJNIEnv(); if(!env) return -1;
    if((*env)->PushLocalFrame(env,32)<0) { if((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); return -1; }
    int result=0; unsigned char *data=NULL; size_t used=0,capacity=0; jobject stream=NULL; jmethodID close=NULL;
#define JFAIL() ((*env)->ExceptionCheck(env))
    jobject activity=SDL_GetAndroidActivity(); if(JFAIL() || !activity) goto failed;
    jclass activity_class=(*env)->GetObjectClass(env,activity); if(JFAIL() || !activity_class) goto failed;
    jmethodID service=activity_class?(*env)->GetMethodID(env,activity_class,"getSystemService","(Ljava/lang/String;)Ljava/lang/Object;"):NULL;
    if(JFAIL() || !service) goto failed;
    jstring service_name=(*env)->NewStringUTF(env,"clipboard"); if(JFAIL() || !service_name) goto failed;
    jobject manager=service && service_name?(*env)->CallObjectMethod(env,activity,service,service_name):NULL;
    if(JFAIL() || !manager) goto failed;
    jclass manager_class=(*env)->GetObjectClass(env,manager); if(JFAIL() || !manager_class) goto failed;
    jmethodID primary=(*env)->GetMethodID(env,manager_class,"getPrimaryClip","()Landroid/content/ClipData;");
    if(JFAIL() || !primary) goto failed;
    jobject clip=primary?(*env)->CallObjectMethod(env,manager,primary):NULL;
    if(JFAIL()) goto failed; if(!clip) goto done;
    jclass clip_class=(*env)->GetObjectClass(env,clip); if(JFAIL() || !clip_class) goto failed;
    jmethodID count=(*env)->GetMethodID(env,clip_class,"getItemCount","()I");
    if(!count || JFAIL()) goto failed;
    jint n=(*env)->CallIntMethod(env,clip,count); if(JFAIL()) goto failed; if(n<1) goto done;
    jmethodID get_item=(*env)->GetMethodID(env,clip_class,"getItemAt","(I)Landroid/content/ClipData$Item;");
    if(JFAIL() || !get_item) goto failed;
    jobject item=get_item?(*env)->CallObjectMethod(env,clip,get_item,0):NULL;
    if(JFAIL() || !item) goto failed;
    jclass item_class=(*env)->GetObjectClass(env,item); if(JFAIL() || !item_class) goto failed;
    jmethodID get_uri=(*env)->GetMethodID(env,item_class,"getUri","()Landroid/net/Uri;");
    if(JFAIL() || !get_uri) goto failed;
    jobject uri=get_uri?(*env)->CallObjectMethod(env,item,get_uri):NULL;
    if(JFAIL()) goto failed; if(!uri) goto done;
    jclass uri_class=(*env)->GetObjectClass(env,uri); if(JFAIL() || !uri_class) goto failed; jmethodID scheme_method=(*env)->GetMethodID(env,uri_class,"getScheme","()Ljava/lang/String;");
    if(JFAIL() || !scheme_method) goto failed;
    jstring scheme=scheme_method?(*env)->CallObjectMethod(env,uri,scheme_method):NULL;
    if(JFAIL() || !scheme) goto failed;
    const char *scheme_utf=(*env)->GetStringUTFChars(env,scheme,NULL); if(!scheme_utf) goto failed;
    bool content=!SDL_strcmp(scheme_utf,"content"); (*env)->ReleaseStringUTFChars(env,scheme,scheme_utf); if(!content) goto done;
    jmethodID resolver_method=(*env)->GetMethodID(env,activity_class,"getContentResolver","()Landroid/content/ContentResolver;");
    if(JFAIL() || !resolver_method) goto failed;
    jobject resolver=resolver_method?(*env)->CallObjectMethod(env,activity,resolver_method):NULL;
    if(JFAIL() || !resolver) goto failed;
    jclass resolver_class=(*env)->GetObjectClass(env,resolver); if(JFAIL() || !resolver_class) goto failed;
    jmethodID type_method=(*env)->GetMethodID(env,resolver_class,"getType","(Landroid/net/Uri;)Ljava/lang/String;");
    if(JFAIL() || !type_method) goto failed;
    jstring type=type_method?(*env)->CallObjectMethod(env,resolver,type_method,uri):NULL;
    if(JFAIL() || !type) goto failed;
    const char *mime=(*env)->GetStringUTFChars(env,type,NULL); if(!mime) goto failed;
    bool image=!SDL_strncmp(mime,"image/",6); (*env)->ReleaseStringUTFChars(env,type,mime); if(!image) goto done;
    /* Resolve read/close and allocate the transfer buffer before opening the
       provider stream, so later allocation failure can always close it. */
    jclass stream_class=(*env)->FindClass(env,"java/io/InputStream"); if(JFAIL() || !stream_class) goto failed;
    close=(*env)->GetMethodID(env,stream_class,"close","()V"); if(JFAIL() || !close) goto failed;
    jmethodID read=(*env)->GetMethodID(env,stream_class,"read","([B)I"); if(JFAIL() || !read) goto failed;
    jbyteArray buffer=(*env)->NewByteArray(env,65536); if(JFAIL() || !buffer) goto failed;
    jmethodID open=(*env)->GetMethodID(env,resolver_class,"openInputStream","(Landroid/net/Uri;)Ljava/io/InputStream;");
    if(JFAIL() || !open) goto failed;
    stream=(*env)->CallObjectMethod(env,resolver,open,uri); if(JFAIL() || !stream) goto failed;
    for(;;) {
        jint amount=(*env)->CallIntMethod(env,stream,read,buffer); if(JFAIL()) goto failed;
        if(amount<0) break; if(!amount || amount>65536 || (size_t)amount>CE_IMAGE_MAX_BYTES-used) goto failed;
        size_t need=used+(size_t)amount;
        if(need>capacity) { size_t next_capacity=capacity?capacity*2:65536; if(next_capacity<need) next_capacity=need; if(next_capacity>CE_IMAGE_MAX_BYTES) next_capacity=CE_IMAGE_MAX_BYTES; capacity=next_capacity; void *next=SDL_realloc(data,capacity); if(!next) goto failed; data=next; }
        (*env)->GetByteArrayRegion(env,buffer,0,amount,(jbyte*)(data+used)); if(JFAIL()) goto failed; used=need;
    }
    if(!used) goto failed; *bytes=data; *size=used; data=NULL; result=1; goto done;
failed:
    result=-1; SDL_SetError("无法读取剪贴板图片（需要本地图片内容，最多 16 MiB）");
done:
    if(JFAIL()) (*env)->ExceptionClear(env);
    if(stream && close) { (*env)->CallVoidMethod(env,stream,close); if(JFAIL()) (*env)->ExceptionClear(env); }
    SDL_free(data); (*env)->PopLocalFrame(env,NULL); return result;
#undef JFAIL
}
#endif
#ifdef SDL_PLATFORM_IOS
int platform_image_ios_clipboard(void **bytes,size_t *size);
#endif
int platform_image_clipboard(void **bytes,size_t *size) {
    if(!bytes || !size) return -1;
    *bytes=NULL; *size=0;
#ifdef SDL_PLATFORM_IOS
    return platform_image_ios_clipboard(bytes,size);
#elif defined(__ANDROID__)
    return android_image(bytes,size);
#else
    static const char *types[]={"image/png","image/jpeg","image/bmp"};
    for(size_t i=0;i<3;i++) if(SDL_HasClipboardData(types[i])) {
        void *data=SDL_GetClipboardData(types[i],size);
        if(!data || *size>CE_IMAGE_MAX_BYTES) { SDL_free(data); SDL_SetError("剪贴板图片超过限制或无法读取"); return -1; }
        *bytes=data; return 1;
    }
    return 0;
#endif
}
