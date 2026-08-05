/* The browser half of the multiplayer transport registry.
 *
 * This was in platform/mp.c until the native build needed to compile that file
 * without emscripten in scope.  The bodies are unchanged; PortMpJsTransport is
 * the only new thing, and platform/native/mp_native.c is its counterpart.
 */

#include <string.h>

#include <emscripten.h>

#include "port/port.h"
#include "port/mp.h"

/* --- a JavaScript transport ----------------------------------------------
 *
 * The page sets Module.portMp to an object with open/close/poll/exchange, in
 * the same style as Module.portSramRestore and Module.portDiag.  Two of the
 * four are handed a heap address to write through rather than returning a
 * value, because they return more than one:
 *
 *   poll(linkPtr)         four bytes at linkPtr: up, selfId, players, error
 *   exchange(word, ptr)   four u16 at ptr, one per slot; returns truthy if the
 *                         transfer happened
 *
 * exchange is called up to sixteen times per frame and has to answer
 * synchronously, so a networked page transport buffers a frame in poll() and
 * serves exchange() out of that buffer.  See docs/MULTIPLAYER.md.
 *
 * Outside a browser every one of these falls through and the transport reports
 * a link that never comes up, which is what the headless harness sees unless
 * it attaches the loopback instead. */

EM_JS(int, PortMpJsOpen, (int players), {
    if (!Module.portMp || !Module.portMp.open)
        return 0;
    return Module.portMp.open(players) ? 1 : 0;
});

EM_JS(void, PortMpJsClose, (void), {
    if (Module.portMp && Module.portMp.close)
        Module.portMp.close();
});

EM_JS(void, PortMpJsPoll, (u8 *link), {
    if (Module.portMp && Module.portMp.poll)
        Module.portMp.poll(link);
});

EM_JS(int, PortMpJsExchange, (int send, u16 *recv), {
    if (!Module.portMp || !Module.portMp.exchange)
        return 0;
    return Module.portMp.exchange(send, recv) ? 1 : 0;
});

static int JsOpen(struct PortMpTransport *t, int players)
{
    (void)t;
    return PortMpJsOpen(players);
}

static void JsClose(struct PortMpTransport *t)
{
    (void)t;
    PortMpJsClose();
}

static void JsPoll(struct PortMpTransport *t, struct PortMpLink *link)
{
    (void)t;
    /* Zeroed first, so a page that writes nothing reports a link that is down
     * rather than whatever the last poll left behind. */
    memset(link, 0, sizeof(*link));
    PortMpJsPoll((u8 *)link);
}

static int JsExchange(struct PortMpTransport *t, u16 send,
                      u16 recv[PORT_MP_PLAYERS])
{
    (void)t;
    return PortMpJsExchange(send, recv);
}

static struct PortMpTransport sJsTransport = {
    "javascript", JsOpen, JsClose, JsPoll, JsExchange, NULL,
};

struct PortMpTransport *PortMpJsTransport(void)
{
    return &sJsTransport;
}
