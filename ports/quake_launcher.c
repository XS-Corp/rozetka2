/* Native bundle launcher; the game itself executes as x86_64 guest code. */
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char executable[PATH_MAX], resources[PATH_MAX], runtime[PATH_MAX];
    uint32_t length = sizeof(executable);
    if (_NSGetExecutablePath(executable, &length) != 0) return 1;
    char *slash = strrchr(executable, '/');
    if (slash == NULL) return 1;
    *slash = 0;
    if (snprintf(resources, sizeof(resources), "%s/../Resources", executable) >= (int)sizeof(resources) ||
        snprintf(runtime, sizeof(runtime), "%s/rozetka2", executable) >= (int)sizeof(runtime)) return 1;
    if (chdir(resources) != 0) { perror("Quake resources"); return 1; }
    char *arguments[] = {runtime, "launch", "--host-libs", "quake-x86_64", "--",
        "-basedir", ".", "-noconinput", "-noudp", "-noextmusic", "+viewsize", "90", "+map", "e1m1", NULL};
    execv(runtime, arguments);
    perror("launch rozetka2");
    return 1;
}
