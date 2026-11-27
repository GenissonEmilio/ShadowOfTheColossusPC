#include <dlfcn.h>
#include <jni.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

typedef int (*tool_entry)(int, char **);
typedef jboolean (*game_probe)(JNIEnv *, jclass);

static void *load(const char *path, int mode)
{
    void *handle = dlopen(path, mode);
    if (!handle)
        fprintf(stderr, "dlopen %s: %s\n", path, dlerror());
    return handle;
}

static void *symbol(void *handle, const char *name)
{
    void *entry = dlsym(handle, name);
    if (!entry)
        fprintf(stderr, "dlsym %s: %s\n", name, dlerror());
    return entry;
}

int main(int argc, char **argv)
{
    // The launcher owns this worker even when Android kills the app process.
    const pid_t parent = getppid();
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || parent == 1 || parent != getppid())
        return 1;
    if (argc >= 4 && strcmp(argv[1], "run") == 0)
    {
        execv(argv[2], argv + 3);
        perror(argv[2]);
        return 1;
    }
    if (argc >= 3 && strcmp(argv[1], "python") == 0)
    {
        void *python = load("libpython3.14.so", RTLD_NOW | RTLD_GLOBAL);
        if (!python)
            return 1;
        tool_entry run_python = (tool_entry)symbol(python, "Py_BytesMain");
        return run_python ? run_python(argc - 1, argv + 1) : 1;
    }
    if (argc == 3 && strcmp(argv[1], "probe") == 0)
    {
        void *runner = load(argv[2], RTLD_NOW | RTLD_LOCAL);
        if (!runner || !symbol(runner, "ANativeActivity_onCreate") || !symbol(runner, "main"))
            return 1;
        game_probe has_game = (game_probe)symbol(runner, "Java_com_genisson_sotc_NativeGameActivity_nativeHasGame");
        if (!has_game || !has_game(NULL, NULL))
        {
            fputs("The generated runner has no registered game code\n", stderr);
            return 1;
        }
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "capture") == 0)
    {
        if (setenv("PS2X_MODULE_CAPTURE", "1", 1) != 0 ||
            setenv("PS2X_DUMP_RAM_ON_MISSING", argv[4], 1) != 0)
            return 1;
        void *runner = load(argv[2], RTLD_NOW | RTLD_LOCAL);
        if (!runner)
            return 1;
        tool_entry capture = (tool_entry)symbol(runner, "main");
        char *args[] = {argv[2], argv[3], NULL};
        return capture ? capture(2, args) : 1;
    }
    fputs("Usage: SotcPrepare run executable argv0 [args] | python script [args] | probe runner | capture runner boot dump\n", stderr);
    return 2;
}
