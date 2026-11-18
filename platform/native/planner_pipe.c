/* The planner's transport on the native build (port/cpu_planner.h):
 * KATAM_PLANNER=CMD starts CMD (through /bin/sh) as a child process with a
 * pipe each way -- one JSON line per question on its stdin, one per answer
 * on its stdout.  tools/haiku_ai/bridge.py is that child.  The child goes
 * when the game does (its stdin closes); nothing is left running.
 *
 * Never blocks the game: both pipe ends are non-blocking, a question that
 * does not fit is "not now" (lines are kept under PIPE_BUF, so a write is
 * all or nothing), and a dead child is "gone".  Pictures for the planner
 * go to KATAM_PLANNER_DIR (PNG, the frame as drawn). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native.h"
#include "port/port.h"
#include "port/inputlog.h"
#include "port/cpu_planner.h"

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(_WIN32)

static int sToChild = -1, sFromChild = -1;
static pid_t sChild = -1;
static char sIn[16384];
static int sInLen;
static char sDir[400];

static int PipeSend(const char *line)
{
    size_t n = strlen(line);
    char buf[4096];
    ssize_t w;

    if (sToChild < 0 || n + 1 > sizeof buf)
        return -1;
    memcpy(buf, line, n);
    buf[n] = '\n';
    w = write(sToChild, buf, n + 1);
    return w == (ssize_t)(n + 1) ? 0 : -1;
}

static int PipeRecv(char *out, int cap, int waitMs)
{
    for (;;) {
        char *nl = (char *)memchr(sIn, '\n', (size_t)sInLen);
        ssize_t r;

        if (nl != NULL) {
            int n = (int)(nl - sIn);

            if (n > cap - 1)
                n = cap - 1;    /* an over-long line is cut (and then fails to parse) */
            memcpy(out, sIn, (size_t)n);
            out[n] = '\0';
            memmove(sIn, nl + 1, (size_t)(sInLen - (int)(nl + 1 - sIn)));
            sInLen -= (int)(nl + 1 - sIn);
            return n > 0 ? n : 1;
        }
        if (sFromChild < 0)
            return -1;
        if (sInLen >= (int)sizeof sIn - 1)
            sInLen = 0;         /* garbage without a newline: drop it */
        r = read(sFromChild, sIn + sInLen, sizeof sIn - 1 - (size_t)sInLen);
        if (r > 0) {
            sInLen += (int)r;
            continue;
        }
        if (r == 0) {           /* the child closed its stdout */
            close(sFromChild);
            sFromChild = -1;
            return -1;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return -1;
        if (waitMs <= 0)
            return 0;
        {
            struct pollfd p;

            p.fd = sFromChild;
            p.events = POLLIN;
            p.revents = 0;
            if (poll(&p, 1, waitMs) <= 0)
                return 0;
            waitMs = 0;
        }
    }
}

static int PipeShot(const char *path)
{
    return PortNativeWritePng(path, gPortFramebuffer, gPortViewW, gPortViewH);
}

static struct PortPlannerOps sOps = { PipeSend, PipeRecv, PipeShot, NULL };

int PortPlannerNativeStart(const char *cmd)
{
    int in[2], out[2];
    const char *dir = getenv("KATAM_PLANNER_DIR");

    if (PortInputLogReplaying()) {
        PortLog("[planner] replaying: the recording's cpugoal lines only, no planner");
        return 0;
    }
    if (pipe(in) != 0 || pipe(out) != 0) {
        PortError("[planner] pipe: %s", strerror(errno));
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);           /* a dead planner is "gone", not a crash */
    sChild = fork();
    if (sChild < 0) {
        PortError("[planner] fork: %s", strerror(errno));
        return 0;
    }
    if (sChild == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    sToChild = in[1];
    sFromChild = out[0];
    fcntl(sToChild, F_SETFL, fcntl(sToChild, F_GETFL) | O_NONBLOCK);
    fcntl(sFromChild, F_SETFL, fcntl(sFromChild, F_GETFL) | O_NONBLOCK);
    fcntl(sToChild, F_SETFD, FD_CLOEXEC);
    fcntl(sFromChild, F_SETFD, FD_CLOEXEC);
    if (dir != NULL && *dir) {
        snprintf(sDir, sizeof sDir, "%s", dir);
        mkdir(sDir, 0755);
        sOps.shotDir = sDir;
    }
    PortPlannerSetOps(&sOps);
    PortLog("[planner] started (pid %d): %s%s%s", (int)sChild, cmd, sOps.shotDir ? "; pictures in " : "",
            sOps.shotDir ? sOps.shotDir : "");
    return 1;
}

void PortPlannerNativeStop(void)
{
    int i, st;

    if (sChild <= 0)
        return;
    PortPlannerSetOps(NULL);
    if (sToChild >= 0)
        close(sToChild);                /* EOF: the bridge finishes and exits */
    sToChild = -1;
    for (i = 0; i < 50; i++) {          /* up to 5 s for its last log lines */
        if (waitpid(sChild, &st, WNOHANG) == sChild) {
            sChild = -1;
            break;
        }
        usleep(100000);
    }
    if (sChild > 0) {
        kill(sChild, SIGTERM);
        waitpid(sChild, &st, 0);
        sChild = -1;
    }
    if (sFromChild >= 0)
        close(sFromChild);
    sFromChild = -1;
}

#else   /* _WIN32: not yet */

int PortPlannerNativeStart(const char *cmd)
{
    (void)cmd;
    PortError("[planner] KATAM_PLANNER is not supported on Windows yet");
    return 0;
}

void PortPlannerNativeStop(void)
{
}

#endif

#ifdef __cplusplus
}
#endif
