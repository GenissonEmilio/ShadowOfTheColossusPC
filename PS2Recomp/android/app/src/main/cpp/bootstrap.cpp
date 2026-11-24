#include <android/native_activity.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>

namespace
{
ANativeActivity_createFunc *createGameActivity;
constexpr auto kJniPrefix = "Java_com_genisson_sotc_NativeGameActivity_";
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_genisson_sotc_NativeGameActivity_nativeAttachRunner(JNIEnv *env, jclass activityClass, jstring path)
{
    if (createGameActivity != nullptr)
        return nullptr;
    const char *filename = env->GetStringUTFChars(path, nullptr);
    if (filename == nullptr)
        return nullptr;
    void *runner = dlopen(filename, RTLD_NOW | RTLD_LOCAL);
    env->ReleaseStringUTFChars(path, filename);
    if (runner == nullptr)
    {
        const char *error = dlerror();
        return env->NewStringUTF(error != nullptr ? error : "dlopen");
    }

    struct Method
    {
        const char *name;
        const char *signature;
    };
    constexpr Method methods[] = {
        {"nativeHasGame", "()Z"},
        {"nativeConfigure", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V"},
        {"nativeSetInput", "(IFFFF)V"},
        {"nativeReleaseInput", "()V"},
        {"nativePause", "(Z)V"},
        {"nativeSetWidescreen", "(ZF)V"},
    };
    JNINativeMethod bindings[sizeof(methods) / sizeof(methods[0])]{};
    for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i)
    {
        char symbol[96];
        snprintf(symbol, sizeof(symbol), "%s%s", kJniPrefix, methods[i].name);
        void *entry = dlsym(runner, symbol);
        if (entry == nullptr)
        {
            dlclose(runner);
            return env->NewStringUTF(methods[i].name);
        }
        bindings[i] = {const_cast<char *>(methods[i].name), const_cast<char *>(methods[i].signature), entry};
    }

    auto *create = reinterpret_cast<ANativeActivity_createFunc *>(dlsym(runner, "ANativeActivity_onCreate"));
    auto hasGame = reinterpret_cast<jboolean (*)(JNIEnv *, jclass)>(bindings[0].fnPtr);
    if (create == nullptr || !hasGame(env, activityClass))
    {
        dlclose(runner);
        return env->NewStringUTF("ANativeActivity_onCreate / nativeHasGame");
    }
    if (env->RegisterNatives(activityClass, bindings, sizeof(bindings) / sizeof(bindings[0])) != JNI_OK)
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->UnregisterNatives(activityClass);
        dlclose(runner);
        return env->NewStringUTF("RegisterNatives");
    }

    // Keep the imported library resident for the rest of this game process.
    createGameActivity = create;
    return nullptr;
}

extern "C" JNIEXPORT void
ANativeActivity_onCreate(ANativeActivity *activity, void *savedState, size_t savedStateSize)
{
    if (createGameActivity != nullptr)
        createGameActivity(activity, savedState, savedStateSize);
}
