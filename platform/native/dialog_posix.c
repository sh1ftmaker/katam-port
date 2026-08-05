/* "Which ROM?", on a desktop Unix.
 *
 * There is no file dialog in SDL2 (SDL3 grew SDL_ShowOpenFileDialog), and no
 * dependency here is worth adding one for, so this shells out to whichever
 * portal the desktop already has.  If none of them is installed the function
 * returns 0 and the caller falls back to drag-and-drop, which needs nothing.
 *
 * Nothing is executed through a shell: popen would be, so the command is built
 * as an argv and run with execvp.  The path comes back on stdout.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "native.h"

static int Run(char *const argv[], char *out, size_t outSize)
{
    int fds[2];
    pid_t pid;
    ssize_t n;
    int status;

    if (pipe(fds) != 0)
        return 0;

    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);             /* not installed */
    }

    close(fds[1]);
    n = read(fds[0], out, outSize - 1);
    close(fds[0]);
    waitpid(pid, &status, 0);

    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 0;

    out[n] = '\0';
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] != '\0';
}

int PortHostPickRomFile(char *out, size_t outSize)
{
    char *const zenity[] = {
        (char *)"zenity", (char *)"--file-selection",
        (char *)"--title=Choose your Kirby & The Amazing Mirror ROM",
        (char *)"--file-filter=Game Boy Advance ROM | *.gba *.agb *.bin",
        NULL
    };
    char *const kdialog[] = {
        (char *)"kdialog", (char *)"--getopenfilename", (char *)".",
        (char *)"*.gba *.agb *.bin", NULL
    };

    if (outSize < 2)
        return 0;
    if (Run(zenity, out, outSize))
        return 1;
    if (Run(kdialog, out, outSize))
        return 1;
    return 0;
}
