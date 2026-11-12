/* Ordinary C program with the default MinGW C runtime startup (no custom entry point). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    int v = 42;
    printf("hello %d from %s\n", v, argc > 0 ? "crt" : "none");
    char *p = malloc(16);
    strcpy(p, "heap");
    printf("%s %zu\n", p, strlen(p));
    printf("argc %d argv1 %s\n", argc, argc > 1 ? argv[1] : "-");
    printf("pi %.2f e %g\n", 3.14159, 2.5);
    atexit(NULL);
    free(p);
    fflush(stdout);
    return 3;
}
