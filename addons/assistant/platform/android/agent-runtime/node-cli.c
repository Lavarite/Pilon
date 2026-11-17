#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    const char *variable = !strcmp(name, "npm") ? "PILON_NPM_CLI"
        : !strcmp(name, "npx") ? "PILON_NPX_CLI"
        : !strcmp(name, "vite") ? "PILON_VITE_CLI" : NULL;
    const char *node = getenv("PILON_NODE");
    const char *script = variable ? getenv(variable) : NULL;
    if (!node || !script || node[0] != '/' || script[0] != '/') {
        fputs("The shared project runtime is unavailable. Restart Pilon.\n", stderr);
        return 127;
    }
    char **arguments = calloc((size_t)argc + 2, sizeof(char *));
    if (!arguments) return 127;
    arguments[0] = (char *)node;
    arguments[1] = (char *)script;
    for (int index = 1; index < argc; ++index) arguments[index + 1] = argv[index];
    execv(node, arguments);
    perror("Unable to start the project runtime");
    free(arguments);
    return 127;
}
