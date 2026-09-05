/* net.c -- see net.h for the shape of the thing and why it is UDP over Wi-Fi. */
#include "net.h"
#include "dlg_data.h"       /* DLG_NET_sendFrameRate -- network.ini's own key */
#include "str_data.h"       /* the message log's own lines, 20200..20216 */
#include "tracks.h"
#include "rlog.h"

#include <stdarg.h>
#include <math.h>            /* sqrt -- the interpolated quaternion's own norm */
#include <stdio.h>
#include <string.h>

/* ============================================================ the socket
 *
 * ONE LAYER, TWO BACKENDS, and the POSIX one is not a stub: `net_test' opens
 * two real peers on the loopback and drives the protocol through actual
 * datagrams. A mock would test this file's idea of a socket.
 */

#ifdef __vita__

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

/* sceNet wants a pool of its own and will not take a small one. 512 KB is what
   two UDP sockets and their buffers need with room over; a race has better uses
   for the rest, which is why net_open is called when a multiplayer page opens
   and not at boot. */
#define NET_POOL (512 * 1024)
static char net_pool[NET_POOL] __attribute__((aligned(16)));
static int  net_stack_up;

typedef SceNetSockaddrIn nsaddr;

static int sock_up(void)
{
    SceNetInitParam p;
    if (net_stack_up)
        return 1;
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) {
        rlog("[net] sceSysmoduleLoadModule(NET) failed\n");
        return 0;
    }
    memset(&p, 0, sizeof p);
    p.memory = net_pool;
    p.size = NET_POOL;
    p.flags = 0;
    /* ALREADY UP IS NOT AN ERROR. Something else in the process may have
       brought the stack up (the LiveArea, a plugin); sceNetInit answers
       SCE_NET_ERROR_EBUSY and the socket calls work regardless. */
    if (sceNetInit(&p) < 0)
        rlog("[net] sceNetInit: already up or refused, carrying on\n");
    sceNetCtlInit();
    net_stack_up = 1;
    return 1;
}

static void sock_down(void) { /* the stack stays up; see net_close */ }

static int  sk_socket(void)
{
    return sceNetSocket("rccars", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
}
static void sk_close(int s) { sceNetSocketClose(s); }
static int  sk_opt(int s, int name, int v)
{
    return sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, name, &v, sizeof v);
}
static int  sk_nonblock(int s)   { return sk_opt(s, SCE_NET_SO_NBIO, 1); }
static int  sk_broadcast(int s)  { return sk_opt(s, SCE_NET_SO_BROADCAST, 1); }

static int sk_bind(int s, int port)
{
    nsaddr a;
    memset(&a, 0, sizeof a);
    a.sin_family = SCE_NET_AF_INET;
    a.sin_port = sceNetHtons((unsigned short)port);
    a.sin_addr.s_addr = 0;                  /* INADDR_ANY */
    return sceNetBind(s, (SceNetSockaddr *)&a, sizeof a);
}

static int sk_send(int s, const void *b, int n, unsigned int addr, int port)
{
    nsaddr a;
    memset(&a, 0, sizeof a);
    a.sin_family = SCE_NET_AF_INET;
    a.sin_port = sceNetHtons((unsigned short)port);
    a.sin_addr.s_addr = addr;
    return sceNetSendto(s, b, (unsigned int)n, 0, (SceNetSockaddr *)&a,
                        sizeof a);
}

static int sk_recv(int s, void *b, int n, unsigned int *addr, int *port)
{
    nsaddr a;
    unsigned int al = sizeof a;
    int r = sceNetRecvfrom(s, b, (unsigned int)n, 0, (SceNetSockaddr *)&a, &al);
    if (r > 0) {
        *addr = a.sin_addr.s_addr;
        *port = (int)sceNetHtons(a.sin_port);   /* htons is its own inverse */
    }
    return r;
}

static unsigned int sk_bcast(void) { return SCE_NET_INADDR_BROADCAST; }

static unsigned int sk_pton(const char *dotted)
{
    unsigned int v = 0;
    if (sceNetInetPton(SCE_NET_AF_INET, dotted, &v) <= 0)
        return 0;
    return v;
}

/* Our own address, for the log. 0 when the console is not associated. */
static unsigned int sk_local(void)
{
    SceNetCtlInfo info;
    memset(&info, 0, sizeof info);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0)
        return 0;
    return sk_pton(info.ip_address);
}

#else   /* ------------------------------------------------------- POSIX */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int sock_up(void) { return 1; }
static void sock_down(void) {}

static int  sk_socket(void) { return socket(AF_INET, SOCK_DGRAM, 0); }
static void sk_close(int s) { close(s); }
static int  sk_opt(int s, int name, int v)
{
    return setsockopt(s, SOL_SOCKET, name, &v, sizeof v);
}
static int  sk_nonblock(int s)
{
    int f = fcntl(s, F_GETFL, 0);
    return fcntl(s, F_SETFL, f | O_NONBLOCK);
}
static int  sk_broadcast(int s) { return sk_opt(s, SO_BROADCAST, 1); }

static int sk_bind(int s, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = INADDR_ANY;
    return bind(s, (struct sockaddr *)&a, sizeof a);
}

static int sk_send(int s, const void *b, int n, unsigned int addr, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = addr;
    return (int)sendto(s, b, (size_t)n, 0, (struct sockaddr *)&a, sizeof a);
}

static int sk_recv(int s, void *b, int n, unsigned int *addr, int *port)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int r = (int)recvfrom(s, b, (size_t)n, 0, (struct sockaddr *)&a, &al);
    if (r > 0) {
        *addr = a.sin_addr.s_addr;
        *port = (int)ntohs(a.sin_port);
    }
    return r;
}

static unsigned int sk_bcast(void) { return INADDR_BROADCAST; }
static unsigned int sk_pton(const char *d)
{
    struct in_addr a;
    return inet_aton(d, &a) ? (unsigned int)a.s_addr : 0u;
}
static unsigned int sk_local(void) { return sk_pton("127.0.0.1"); }

#endif

/* ============================================================ the packets
 *
 * EXPLICIT LITTLE-ENDIAN BYTES, not a struct written down the wire. Both ends
 * of this are ARM or x86 and both are little-endian, so a memcpy of a struct
 * would work today -- and it would encode this compiler's padding and this
 * build's field order into a format two machines have to agree on, which is a
 * bug that only appears between two builds. The codec is 40 lines and
 * `net_test' round-trips every packet through it.
 */
#define NET_MAGIC0 'R'
#define NET_MAGIC1 'C'
#define NET_MAGIC2 'C'
#define NET_MAGIC3 'N'
/* magic 4, version 1, kind 1, slot 1, and byte 7. Byte 7 was a pad, written 0
   and read by nobody; on an NP_STATE it now carries that state's own wrapping
   SEQUENCE, so a receiver can tell a reordered datagram from a new one without
   the packet growing by a byte. Every other kind still writes 0 there. */
#define NET_HDR    8
#define NET_MTU    512

enum {
    NP_ANNOUNCE = 1,
    NP_JOIN,
    NP_WELCOME,
    NP_LOBBY,
    NP_PEER,
    NP_START,
    NP_STATE,
    NP_FINISH,
    NP_BYE,
    NP_KICK,
    NP_N
};

typedef struct { unsigned char b[NET_MTU]; int n, o, bad; } pk;

static void w_begin(pk *p, int kind, int slot)
{
    memset(p, 0, sizeof *p);
    p->b[0] = NET_MAGIC0; p->b[1] = NET_MAGIC1;
    p->b[2] = NET_MAGIC2; p->b[3] = NET_MAGIC3;
    p->b[4] = NET_VERSION;
    p->b[5] = (unsigned char)kind;
    p->b[6] = (unsigned char)(slot < 0 ? 0xff : slot);
    p->b[7] = 0;
    p->n = NET_HDR;
}
static void w_u8(pk *p, unsigned int v)
{
    if (p->n + 1 <= NET_MTU) p->b[p->n++] = (unsigned char)v;
}
static void w_u16(pk *p, unsigned int v)
{
    w_u8(p, v & 0xff); w_u8(p, (v >> 8) & 0xff);
}
static void w_u32(pk *p, unsigned int v)
{
    w_u16(p, v & 0xffff); w_u16(p, (v >> 16) & 0xffff);
}
static void w_i16(pk *p, int v) { w_u16(p, (unsigned int)(v & 0xffff)); }
/* A float as its own bits. Position and nothing else: everything about a car
   that is not a coordinate is quantised into an integer by ai_sample already. */
static void w_f32(pk *p, float f)
{
    unsigned int u;
    memcpy(&u, &f, 4);
    w_u32(p, u);
}
static void w_str(pk *p, const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        w_u8(p, (s && i < (int)strlen(s)) ? (unsigned char)s[i] : 0);
}

static void r_begin(pk *p, const unsigned char *b, int n)
{
    memset(p, 0, sizeof *p);
    if (n > NET_MTU) n = NET_MTU;
    memcpy(p->b, b, (size_t)n);
    p->n = n;
    p->o = NET_HDR;
}
static unsigned int r_u8(pk *p)
{
    if (p->o + 1 > p->n) { p->bad = 1; return 0; }
    return p->b[p->o++];
}
static unsigned int r_u16(pk *p)
{
    unsigned int a = r_u8(p), b = r_u8(p);
    return a | (b << 8);
}
static unsigned int r_u32(pk *p)
{
    unsigned int a = r_u16(p), b = r_u16(p);
    return a | (b << 16);
}
static int r_i16(pk *p)
{
    unsigned int v = r_u16(p);
    return (int)(short)(unsigned short)v;
}
static float r_f32(pk *p)
{
    unsigned int u = r_u32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static void r_str(pk *p, char *out, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned int c = r_u8(p);
        out[i] = (char)c;
    }
    out[n - 1] = 0;
}

/* ai_sample, field by field. 36 bytes on the wire, which is what the struct is
   -- but written out rather than memcpy'd, for the reason above. */
static void w_sample(pk *p, const ai_sample *s)
{
    int i;
    for (i = 0; i < 3; i++) w_f32(p, s->p[i]);
    for (i = 0; i < 4; i++) w_i16(p, s->q[i]);
    for (i = 0; i < 3; i++) w_i16(p, s->mom[i]);
    for (i = 0; i < 6; i++) w_u8(p, s->susp[i]);
    w_i16(p, s->steer);
    w_u16(p, s->dt);
}
static void r_sample(pk *p, ai_sample *s)
{
    int i;
    memset(s, 0, sizeof *s);
    for (i = 0; i < 3; i++) s->p[i] = r_f32(p);
    for (i = 0; i < 4; i++) s->q[i] = (short)r_i16(p);
    for (i = 0; i < 3; i++) s->mom[i] = (short)r_i16(p);
    for (i = 0; i < 6; i++) s->susp[i] = (unsigned char)r_u8(p);
    s->steer = (short)r_i16(p);
    s->dt = (unsigned short)r_u16(p);
}

/* ============================================================ the state */

unsigned int net_sent, net_recv, net_dropped;

static struct {
    int          sk;
    int          port;
    net_mode     mode;
    net_err      err;
    float        clock;
    int          slot;              /* our own, -1 when not in a game */
    net_peer     peer[NET_MAX];
    net_settings set;
    net_server   srv[NET_MAX_SERVERS];
    int          nsrv;
    /* our own identity, taken once when we host or join */
    char          myname[PL_NAME];
    unsigned char mycar, myup[3], myskin, myface;
    /* the host we are talking to */
    unsigned int  host_addr;
    int           host_port;
    /* WHEN WE LAST HEARD FROM IT, and this is CONNECTION state rather than a
     * row of the roster -- which is the whole point of it being here.
     *
     * The client's liveness check used to read `peer[0].heard', and slot 0 is
     * filled in from the host's own roster packet: `heard' is not on the wire,
     * so a row that had just become `used' carried a zero, and a reaper
     * comparing `clock - 0' against five seconds fired the instant the clock
     * was past five seconds. On a FIRST connection the clock is small and the
     * next packet stamped it in time; on a RECONNECT the clock is minutes old
     * and the session died on the frame the roster arrived. `first connection
     * goes ok, if disconnect and connect again, got disconnected'.
     *
     * Stamping it earlier only moved when it bit. Nothing about how long we
     * have been talking to the host belongs in data the host sent us. */
    float         host_heard;
    float         announce_at, lobby_at, state_at, join_at;
    /* OUR OWN STATE PACKETS' wrapping sequence, into header byte 7. Per
       MACHINE and not per destination: the same state goes to everybody in the
       same frame, so one counter numbers all of them and a peer that missed a
       datagram still sees the gap. */
    unsigned char state_seq;
    /* THE CLIENT'S OWN KEEP-ALIVE, on its own clock. See net_step. */
    float         peer_at;
    int           start_pending, race_over;
    char          log[NET_LOG_LINES][NET_LOG_CHARS];
    int           nlog;
    /* THE HARNESS'S OWN. A directed announce target, so the discovery path can
       be exercised over the loopback -- broadcast on 127.0.0.1 reaches nobody.
       It is also what a direct-connect would use; see net.h. */
    unsigned int  hint_addr;
    int           hint_port;
} N;

static int opened;

float net_time(void) { return N.clock; }
net_mode net_mode_now(void) { return N.mode; }
net_err  net_error(void) { return N.err; }
void     net_clear_error(void) { N.err = NET_ERR_NONE; }
int      net_port(void) { return N.port; }
int      net_slot(void) { return N.slot; }
int      net_is_host(void) { return N.mode == NET_HOSTING
                                 || (N.mode == NET_RACING && N.slot == 0); }
int      net_race_over(void) { return N.race_over; }

/* HOW LONG THE QUIETEST LIVE PEER HAS BEEN QUIET. See net.h. */
float net_link_silence(void)
{
    float worst = 0.f;
    int i;

    if (N.mode != NET_JOINED && N.mode != NET_RACING && N.mode != NET_HOSTING)
        return 0.f;
    for (i = 0; i < NET_MAX; i++) {
        const net_peer *q = &N.peer[i];
        float quiet;
        /* OUR OWN SLOT IS NOT A LINK, and neither is an empty one. A peer that
           has never been heard from at all is not counted either: `heard' is 0
           until its first packet, and a slot that has just been filled by a
           JOIN would otherwise report the whole clock as silence. */
        if (!q->used || i == N.slot || q->heard <= 0.f)
            continue;
        quiet = N.clock - q->heard;
        if (quiet > worst)
            worst = quiet;
    }
    return worst;
}
const net_settings *net_settings_now(void) { return &N.set; }

void net_test_advance(float s) { N.clock += s; }
void net_test_peer_hint(unsigned int addr, int port)
{
    N.hint_addr = addr; N.hint_port = port;
}

/* ------------------------------------------------------------- the log */

static void logf_(const char *fmt, ...)
{
    va_list ap;
    char line[NET_LOG_CHARS];
    int i;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    /* OLDEST FIRST, scrolling. The panel is six lines and the game's own shot
       of it shows one, so a ring would be more machinery than the screen. */
    if (N.nlog >= NET_LOG_LINES) {
        for (i = 1; i < NET_LOG_LINES; i++)
            memcpy(N.log[i - 1], N.log[i], NET_LOG_CHARS);
        N.nlog = NET_LOG_LINES - 1;
    }
    snprintf(N.log[N.nlog], NET_LOG_CHARS, "%s", line);
    N.nlog++;
    rlog("[net] %s\n", line);
}

int net_n_log(void) { return N.nlog; }
const char *net_log_at(int i)
{
    return (i >= 0 && i < N.nlog) ? N.log[i] : "";
}
void net_log_clear(void) { N.nlog = 0; }

/* --------------------------------------------------------- the roster */

int net_n_peers(void)
{
    int i, n = 0;
    for (i = 0; i < NET_MAX; i++)
        if (N.peer[i].used) n++;
    return n;
}

const net_peer *net_peer_at(int i)
{
    return (i >= 0 && i < NET_MAX) ? &N.peer[i] : 0;
}

static void peer_clear(int i)
{
    memset(&N.peer[i], 0, sizeof N.peer[i]);
}

static void roster_clear(void)
{
    int i;
    for (i = 0; i < NET_MAX; i++) peer_clear(i);
}

/* Fill slot `i' from our own identity. */
static void peer_set_me(int i)
{
    net_peer *p = &N.peer[i];
    memset(p, 0, sizeof *p);
    p->used = 1;
    snprintf(p->name, sizeof p->name, "%s", N.myname);
    p->car = N.mycar;
    memcpy(p->up, N.myup, 3);
    p->skin = N.myskin;
    p->face = N.myface;
    p->heard = N.clock;
}

/* ------------------------------------------------------------ open/close */

int net_open(void)
{
    int i;
    if (opened)
        return 1;
    memset(&N, 0, sizeof N);
    N.slot = -1;
    N.sk = -1;
    if (!sock_up()) {
        N.err = NET_ERR_NO_SOCKET;
        return 0;
    }
    N.sk = sk_socket();
    if (N.sk < 0) {
        rlog("[net] no socket\n");
        N.err = NET_ERR_NO_SOCKET;
        return 0;
    }
    sk_broadcast(N.sk);
    sk_nonblock(N.sk);
    /* THE PORT WALKS, AND SO_REUSEADDR IS NOT SET -- which is the whole reason
       the walk works. This code had it, for no better reason than that a lot of
       socket code does: on UDP it lets a SECOND socket bind an address the first
       one already has, so both instances took 3658 and the datagrams went to
       whichever the kernel felt like. `net_test' failed on 23 of 42 checks with
       nothing else wrong. A UDP socket does not linger the way a TCP one does,
       so there is nothing for the option to fix here either.
     *
       Two instances on one machine -- which is how this gets tested, on a PC
       running two emulators -- therefore get 3658 and 3659, and the announce
       carries the sender's own port so the second one is still findable. */
    for (i = 0; i < NET_PORT_TRIES; i++) {
        if (sk_bind(N.sk, NET_PORT + i) >= 0) {
            N.port = NET_PORT + i;
            break;
        }
    }
    if (!N.port) {
        rlog("[net] could not bind %d..%d\n", NET_PORT,
             NET_PORT + NET_PORT_TRIES - 1);
        sk_close(N.sk);
        N.sk = -1;
        N.err = NET_ERR_NO_SOCKET;
        return 0;
    }
    opened = 1;
    N.mode = NET_OFF;
    rlog("[net] up on port %d, local %u.%u.%u.%u\n", N.port,
         sk_local() & 0xff, (sk_local() >> 8) & 0xff,
         (sk_local() >> 16) & 0xff, (sk_local() >> 24) & 0xff);
    return 1;
}

void net_close(void)
{
    if (!opened)
        return;
    if (N.mode == NET_HOSTING || N.mode == NET_JOINED || N.mode == NET_RACING)
        net_leave();
    if (N.sk >= 0)
        sk_close(N.sk);
    sock_down();
    memset(&N, 0, sizeof N);
    N.slot = -1;
    N.sk = -1;
    opened = 0;
}

/* --------------------------------------------------------------- sending */

static void send_to(pk *p, unsigned int addr, int port)
{
    if (N.sk < 0 || p->n <= 0)
        return;
    if (sk_send(N.sk, p->b, p->n, addr, port) < 0)
        net_dropped++;
    else
        net_sent++;
}

/* To every peer but our own slot. The addresses came out of NET_LOBBY. */
static void send_others(pk *p)
{
    int i;
    for (i = 0; i < NET_MAX; i++) {
        if (!N.peer[i].used || i == N.slot)
            continue;
        /* A ROW WITH NO ADDRESS IS A ROW WE CANNOT REACH, and it is counted:
           the one that used to be here silently was the host's own, seen from a
           client, and it cost half the traffic in the game -- see take_lobby. */
        if (!N.peer[i].addr) {
            net_dropped++;
            continue;
        }
        send_to(p, N.peer[i].addr, N.peer[i].port);
    }
}

static void send_announce(void)
{
    pk p;
    w_begin(&p, NP_ANNOUNCE, 0);
    w_str(&p, N.myname, PL_NAME);
    w_u8(&p, (unsigned int)net_n_peers());
    w_u8(&p, NET_MAX);
    w_u8(&p, N.set.track);
    w_u8(&p, N.set.laps);
    w_u16(&p, (unsigned int)N.port);
    if (N.hint_addr)
        send_to(&p, N.hint_addr, N.hint_port);
    else
        send_to(&p, sk_bcast(), NET_PORT);
    /* AND THE WHOLE PORT RANGE, not just NET_PORT. A second instance on one
       machine is bound to 3659, and a broadcast to 3658 never reaches it --
       which is exactly the case a PC with two emulators on it is. Four
       datagrams twice a second is nothing. */
    if (!N.hint_addr) {
        int i;
        for (i = 1; i < NET_PORT_TRIES; i++)
            send_to(&p, sk_bcast(), NET_PORT + i);
    }
}

static void w_settings(pk *p)
{
    int i;
    w_u8(p, N.set.track);
    w_u8(p, N.set.laps);
    for (i = 0; i < 3; i++) w_u8(p, N.set.restr[i]);
}
static void r_settings(pk *p, net_settings *s)
{
    int i;
    s->track = (unsigned char)r_u8(p);
    s->laps = (unsigned char)r_u8(p);
    for (i = 0; i < 3; i++) s->restr[i] = (unsigned char)r_u8(p);
}

static void send_lobby(void)
{
    pk p;
    int i;
    w_begin(&p, NP_LOBBY, N.slot);
    w_settings(&p);
    for (i = 0; i < NET_MAX; i++) {
        const net_peer *q = &N.peer[i];
        w_u8(&p, (unsigned int)q->used);
        w_str(&p, q->name, PL_NAME);
        w_u32(&p, q->addr);
        w_u16(&p, q->port);
        w_u8(&p, q->car);
        w_u8(&p, q->up[0]); w_u8(&p, q->up[1]); w_u8(&p, q->up[2]);
        w_u8(&p, q->skin);
        w_u8(&p, q->ready);
        w_u8(&p, q->face);
    }
    send_others(&p);
}

static void send_peer(void)
{
    pk p;
    if (N.slot < 0)
        return;
    w_begin(&p, NP_PEER, N.slot);
    w_u8(&p, N.mycar);
    w_u8(&p, N.myup[0]); w_u8(&p, N.myup[1]); w_u8(&p, N.myup[2]);
    w_u8(&p, N.myskin);
    w_u8(&p, N.peer[N.slot].ready);
    w_u8(&p, N.myface);
    if (net_is_host())
        send_lobby();                       /* our own change IS the roster */
    else
        send_to(&p, N.host_addr, N.host_port);
}

/* ------------------------------------------------------------- receiving */

static int server_slot(unsigned int addr, int port)
{
    int i, oldest = 0;
    for (i = 0; i < N.nsrv; i++)
        if (N.srv[i].addr == addr && N.srv[i].port == port)
            return i;
    if (N.nsrv < NET_MAX_SERVERS)
        return N.nsrv++;
    for (i = 1; i < N.nsrv; i++)
        if (N.srv[i].seen < N.srv[oldest].seen) oldest = i;
    return oldest;
}

static int find_peer(unsigned int addr, int port)
{
    int i;
    for (i = 0; i < NET_MAX; i++)
        if (N.peer[i].used && N.peer[i].addr == addr && N.peer[i].port == port)
            return i;
    return -1;
}

static void host_take_join(pk *p, unsigned int addr, int port)
{
    int slot = find_peer(addr, port), i;
    pk r;

    if (slot < 0) {
        for (i = 1; i < NET_MAX; i++)       /* slot 0 is always the host */
            if (!N.peer[i].used) { slot = i; break; }
    }
    w_begin(&r, NP_WELCOME, 0);
    if (slot < 0) {
        w_u8(&r, 0xff);
        w_u8(&r, NET_ERR_FULL);
        send_to(&r, addr, port);
        return;
    }
    {
        net_peer *q = &N.peer[slot];
        char nm[PL_NAME];
        int fresh = !q->used;
        memset(q, 0, sizeof *q);
        q->used = 1;
        r_str(p, nm, PL_NAME);
        snprintf(q->name, sizeof q->name, "%s", nm);
        q->car = (unsigned char)r_u8(p);
        q->up[0] = (unsigned char)r_u8(p);
        q->up[1] = (unsigned char)r_u8(p);
        q->up[2] = (unsigned char)r_u8(p);
        q->skin = (unsigned char)r_u8(p);
        q->face = (unsigned char)r_u8(p);
        q->addr = addr;
        q->port = (unsigned short)port;
        q->heard = N.clock;
        if (fresh)
            logf_(STR_NET_CONNECTED, q->name);
    }
    w_u8(&r, (unsigned int)slot);
    w_u8(&r, NET_ERR_NONE);
    w_settings(&r);
    send_to(&r, addr, port);
    send_lobby();
}

static void take_lobby(pk *p)
{
    int i;
    net_settings was = N.set;
    r_settings(p, &N.set);
    for (i = 0; i < NET_MAX; i++) {
        net_peer *q = &N.peer[i];
        char nm[PL_NAME];
        unsigned int used = r_u8(p);
        r_str(p, nm, PL_NAME);
        if (!used) {
            if (q->used && i != N.slot)
                logf_(STR_NET_DISCONNECTED, q->name);
            /* OUR OWN SLOT IS NEVER CLEARED BY THE HOST'S ROSTER while we are
               in it: the state and the timers on it are ours, and a lobby
               packet that crossed our own join would blank them. */
            if (i != N.slot) peer_clear(i);
            else q->used = 1;
            (void)r_u32(p); (void)r_u16(p);
            (void)r_u8(p); (void)r_u8(p); (void)r_u8(p); (void)r_u8(p);
            (void)r_u8(p); (void)r_u8(p); (void)r_u8(p);
            continue;
        }
        q->used = 1;
        snprintf(q->name, sizeof q->name, "%s", nm);
        q->addr = r_u32(p);
        q->port = (unsigned short)r_u16(p);
        /* THE HOST'S OWN ROW CARRIES NO ADDRESS, and this is where the one it
         * does have goes in. `peer_set_me' leaves `addr' at zero -- a machine
         * does not reliably know its own address, and net.h says the field is
         * "0 for our own slot" -- so the roster the host broadcasts has a zero
         * in slot 0. `send_others' skips a peer with no address, so a CLIENT
         * never sent its car state to the HOST: the host's state reached the
         * client (it has the client's address from the join) and nothing came
         * back. The opponent was visible on one machine and not the other.
         *
         * We know the answer without being told it: it is where the welcome
         * came from. Filled in for whichever slot is the host's, not just 0, so
         * this stays right if the host is ever not slot 0. */
        if (!net_is_host() && !q->addr && N.host_addr && i == 0) {
            q->addr = N.host_addr;
            q->port = (unsigned short)N.host_port;
        }
        q->car = (unsigned char)r_u8(p);
        q->up[0] = (unsigned char)r_u8(p);
        q->up[1] = (unsigned char)r_u8(p);
        q->up[2] = (unsigned char)r_u8(p);
        q->skin = (unsigned char)r_u8(p);
        q->ready = (unsigned char)r_u8(p);
        q->face = (unsigned char)r_u8(p);
        if (i == N.slot) {
            /* OUR OWN ROW IS OURS. The host echoes what we told it; taking it
               back would fight our own screen for a frame every quarter
               second. */
            q->car = N.mycar;
            memcpy(q->up, N.myup, 3);
            q->skin = N.myskin;
        }
    }
    if (was.track != N.set.track)
        logf_(STR_NET_MAP,
              STR_TRACK_NAME[N.set.track < STR_N_TRACKS ? N.set.track : 0]);
    if (was.laps != N.set.laps)
        logf_(STR_NET_LAPS, (int)N.set.laps);
    if (memcmp(was.restr, N.set.restr, 3) != 0)
        logf_("%s", STR_NET_RESTR);
}

static void take_state(pk *p, int slot)
{
    net_peer *q;
    ai_sample in;
    unsigned char seq;
    unsigned int lap, place, best;

    if (slot < 0 || slot >= NET_MAX || slot == N.slot)
        return;
    q = &N.peer[slot];
    if (!q->used)
        return;
    /* INTO A LOCAL FIRST, and only committed once the whole packet has parsed.
       This read straight into `q->s' and checked `p->bad' afterwards, so a
       truncated datagram overwrote the last state that WAS good with a
       half-decoded one -- and the interpolator would then have two samples it
       had to believe. */
    seq = p->b[7];
    r_sample(p, &in);
    lap   = r_u8(p);
    place = r_u8(p);
    best  = r_u16(p);
    if (p->bad)
        return;

    /* AN OLDER DATAGRAM IS DROPPED. UDP may reorder, and with the car drawn
       between the last two samples a state that arrives out of order does not
       merely arrive late -- it becomes the NEWEST and the car is interpolated
       backwards into it. The compare is the wrapping one: `seq - q->seq' as an
       unsigned byte is small and non-zero for a newer packet and >= 128 for one
       that is behind, so a counter rolling through 255 is not a discontinuity.
       Not applied to the first packet of a race, which has nothing to be behind
       -- and `net_race_begin' clears `have' for exactly that. */
    if (q->have && ((unsigned char)(seq - q->seq) == 0
                    || (unsigned char)(seq - q->seq) >= 128)) {
        net_dropped++;
        return;
    }

    /* SHIFT, THEN STORE. The one that was newest becomes the older half of the
       pair the pose is interpolated across. */
    q->s0    = q->s;
    q->s0_at = q->s_at;
    q->s     = in;
    q->s_at  = N.clock;
    q->seq   = seq;
    if (q->have < 2)
        q->have++;
    q->lap      = (unsigned char)lap;
    q->place    = (unsigned char)place;
    q->best_lap = (float)best / 100.0f;
    q->heard    = N.clock;
}

static void handle(pk *p, int kind, int slot, unsigned int addr, int port)
{
    switch (kind) {
    case NP_ANNOUNCE:
        if (N.mode != NET_BROWSING)
            return;
        {
            int i = server_slot(addr, port);
            net_server *s = &N.srv[i];
            char nm[PL_NAME];
            r_str(p, nm, PL_NAME);
            snprintf(s->name, sizeof s->name, "%s", nm);
            s->players = (unsigned char)r_u8(p);
            s->maxplayers = (unsigned char)r_u8(p);
            s->track = (unsigned char)r_u8(p);
            s->laps = (unsigned char)r_u8(p);
            s->port = (unsigned short)r_u16(p);
            s->addr = addr;
            if (p->bad) { s->name[0] = 0; return; }
            /* THE ANNOUNCE'S OWN PORT WINS over the one it arrived from: a
               datagram's source port is the sender's socket, which is the same
               here, but saying so in the packet is what makes a NAT or a
               relay work later. */
            if (!s->port) s->port = (unsigned short)port;
            s->seen = N.clock;
        }
        return;
    case NP_JOIN:
        if (N.mode != NET_HOSTING)
            return;
        host_take_join(p, addr, port);
        return;
    case NP_WELCOME:
        if (N.mode != NET_JOINING)
            return;
        {
            unsigned int s = r_u8(p);
            unsigned int e = r_u8(p);
            if (s == 0xff) {
                N.err = (net_err)e;
                N.mode = NET_BROWSING;
                return;
            }
            if (s >= NET_MAX) return;
            N.slot = (int)s;
            r_settings(p, &N.set);
            roster_clear();
            peer_set_me(N.slot);
            N.host_addr = addr;
            N.host_port = port;
            N.host_heard = N.clock;
            N.mode = NET_JOINED;
            logf_(STR_NET_CONNECTED, N.myname);
        }
        return;
    case NP_LOBBY:
        if (N.mode != NET_JOINED && N.mode != NET_RACING)
            return;
        if (addr != N.host_addr)
            return;
        take_lobby(p);
        return;
    case NP_PEER:
        if (N.mode != NET_HOSTING && N.mode != NET_RACING)
            return;
        if (slot < 0 || slot >= NET_MAX || !N.peer[slot].used)
            return;
        if (N.peer[slot].addr != addr)
            return;
        {
            net_peer *q = &N.peer[slot];
            int was = q->ready;
            q->car = (unsigned char)r_u8(p);
            q->up[0] = (unsigned char)r_u8(p);
            q->up[1] = (unsigned char)r_u8(p);
            q->up[2] = (unsigned char)r_u8(p);
            q->skin = (unsigned char)r_u8(p);
            q->ready = (unsigned char)r_u8(p);
            q->face = (unsigned char)r_u8(p);
            q->heard = N.clock;
            if (!was && q->ready)
                logf_(STR_NET_READY, q->name);
            send_lobby();
        }
        return;
    case NP_START:
        if (N.mode != NET_JOINED)
            return;
        if (addr != N.host_addr)
            return;
        r_settings(p, &N.set);
        N.start_pending = 1;
        return;
    case NP_STATE:
        if (N.mode != NET_RACING)
            return;
        take_state(p, slot);
        return;
    case NP_FINISH:
        if (slot < 0 || slot >= NET_MAX)
            return;
        {
            net_peer *q = &N.peer[slot];
            q->finished = 1;
            q->total = (float)r_u32(p) / 1000.0f;
            q->best_lap = (float)r_u32(p) / 1000.0f;
            q->heard = N.clock;
            if (q->used)
                logf_(STR_NET_FINISHED, q->name);
        }
        return;
    case NP_BYE:
        if (N.mode == NET_JOINED || N.mode == NET_RACING) {
            if (addr == N.host_addr) {   /* the host went */
                N.err = NET_ERR_TIMEOUT;
                net_leave();
                return;
            }
        }
        {
            int i = find_peer(addr, port);
            if (i > 0) {
                logf_(STR_NET_DISCONNECTED, N.peer[i].name);
                peer_clear(i);
                if (net_is_host()) send_lobby();
            }
        }
        return;
    case NP_KICK:
        if (N.mode != NET_JOINED && N.mode != NET_RACING)
            return;
        if (addr != N.host_addr)
            return;
        if ((int)r_u8(p) != N.slot)
            return;
        N.err = NET_ERR_KICKED;
        net_leave();
        return;
    default:
        return;
    }
}

/* ------------------------------------------------------------- the step */

/* Drain the socket. Bounded: a flood must not be able to hold the frame. */
#define NET_DRAIN 64

/* OUR OWN ADDRESS, asked for ONCE. sk_local() is a call into the network
   control library on the Vita and pump() would make it per packet. */
static unsigned int my_addr(void)
{
    static unsigned int a;
    static int asked;
    if (!asked) { a = sk_local(); asked = 1; }
    return a;
}
static unsigned int loop_addr(void)
{
    static unsigned int a;
    static int asked;
    if (!asked) { a = sk_pton("127.0.0.1"); asked = 1; }
    return a;
}

static void pump(void)
{
    int i;
    for (i = 0; i < NET_DRAIN; i++) {
        unsigned char buf[NET_MTU];
        unsigned int addr = 0;
        int port = 0, n, kind, slot;
        pk p;
        n = sk_recv(N.sk, buf, (int)sizeof buf, &addr, &port);
        if (n < NET_HDR)
            return;
        if (buf[0] != NET_MAGIC0 || buf[1] != NET_MAGIC1
            || buf[2] != NET_MAGIC2 || buf[3] != NET_MAGIC3)
            continue;
        /* A BUILD THAT DISAGREES IS NOT HALF-COMPATIBLE. Its packets are
           dropped in silence rather than parsed with our own layout. */
        if (buf[4] != NET_VERSION) {
            net_dropped++;
            continue;
        }
        kind = buf[5];
        slot = (buf[6] == 0xff) ? -1 : buf[6];
        /* OUR OWN BROADCAST COMES BACK TO US. Drop anything from our own
           socket before it can add us to our own server list. */
        if (kind == NP_ANNOUNCE && port == N.port
            && (addr == my_addr() || addr == loop_addr()))
            continue;
        net_recv++;
        /* THE HOST IS ALIVE BECAUSE WE HEARD IT, and this is the one line that
         * says so. Without it every client was dropped after exactly
         * NET_TIMEOUT: `take_lobby' fills slot 0 in from the roster -- name,
         * address, car, everything -- and never touches `heard', nothing else
         * on a client's side does either, so `N.peer[0].heard' stayed 0 and
         * `reap()' compared the clock against zero and left five seconds after
         * the join. Connected, then disconnected, then connected, five seconds
         * apart, for ever.
         *
         * STAMPED HERE, on ANY packet from the host, rather than inside the
         * roster handler: the host also sends starts, kicks, states and
         * finishes, and a client that had gone quiet on the lobby but was still
         * being sent a race would otherwise reap a host it can hear. One place,
         * every packet, before anything decides what the packet means. */
        if (!net_is_host() && N.host_addr && addr == N.host_addr)
            N.host_heard = N.clock;
        r_begin(&p, buf, n);
        handle(&p, kind, slot, addr, port);
    }
}

static void reap(void)
{
    int i;
    /* THE HOST DROPS A SILENT PEER; A CLIENT DROPS THE HOST. Both after
       NET_TIMEOUT, which is the engine's own `Max lag' idea. */
    if (net_is_host()) {
        for (i = 1; i < NET_MAX; i++) {
            if (!N.peer[i].used) continue;
            if (N.clock - N.peer[i].heard > NET_TIMEOUT) {
                logf_(STR_NET_DISCONNECTED, N.peer[i].name);
                peer_clear(i);
                send_lobby();
            }
        }
    } else if (N.mode == NET_JOINED || N.mode == NET_RACING) {
        /* ON `host_heard' AND NOT ON A ROSTER ROW -- see the field. It is
           stamped when the join goes out and on every packet from the host
           after it, so there is no window in which it is zero and the mode is
           one that gets reaped. */
        if (N.clock - N.host_heard > NET_TIMEOUT) {
            N.err = NET_ERR_TIMEOUT;
            net_leave();
        }
    }
    /* AND A JOIN THAT IS NEVER ANSWERED GIVES UP. Without this the request goes
       out four times a second for ever and the page sits on `Connecting...'
       with nothing to tell the player -- which is a dialog that cannot be
       distinguished from a hung app. Twice the timeout, because a join is one
       round trip and worth more patience than a session already up. */
    else if (N.mode == NET_JOINING
             && N.clock - N.host_heard > NET_TIMEOUT * 2.f) {
        N.err = NET_ERR_TIMEOUT;
        N.host_addr = 0;
        N.mode = NET_BROWSING;
    }
    /* A server that has stopped announcing leaves the list. */
    for (i = 0; i < N.nsrv; ) {
        if (N.clock - N.srv[i].seen > NET_TIMEOUT) {
            N.srv[i] = N.srv[--N.nsrv];
            memset(&N.srv[N.nsrv], 0, sizeof N.srv[N.nsrv]);
        } else i++;
    }
}

void net_step(float dt)
{
    if (!opened || N.sk < 0)
        return;
    if (dt > 0.f) N.clock += dt;

    pump();

    if (N.mode == NET_HOSTING || N.mode == NET_RACING) {
        if (net_is_host() && N.mode == NET_HOSTING
            && N.clock - N.announce_at >= 1.0f / NET_ANNOUNCE_HZ) {
            N.announce_at = N.clock;
            send_announce();
        }
        if (net_is_host() && N.clock - N.lobby_at >= 1.0f / NET_LOBBY_HZ) {
            N.lobby_at = N.clock;
            /* the host's own row is the truth about the host */
            if (N.slot >= 0) {
                N.peer[N.slot].car = N.mycar;
                memcpy(N.peer[N.slot].up, N.myup, 3);
                N.peer[N.slot].skin = N.myskin;
                N.peer[N.slot].heard = N.clock;
            }
            send_lobby();
        }
    }
    /* A JOIN IS RETRIED. One datagram can be lost and the page would sit on
       "Connecting..." for ever; four tries a second for as long as the player
       leaves it there is what a UDP handshake costs. */
    if (N.mode == NET_JOINING && N.clock - N.join_at >= 0.25f) {
        pk p;
        N.join_at = N.clock;
        w_begin(&p, NP_JOIN, -1);
        w_str(&p, N.myname, PL_NAME);
        w_u8(&p, N.mycar);
        w_u8(&p, N.myup[0]); w_u8(&p, N.myup[1]); w_u8(&p, N.myup[2]);
        w_u8(&p, N.myskin);
        w_u8(&p, N.myface);
        send_to(&p, N.host_addr, N.host_port);
    }
    /* AND A CLIENT KEEPS ITS OWN ROW ALIVE so the host's reaper leaves it be --
     * IN A RACE AS WELL AS IN THE LOBBY, and on a timer of its own.
     *
     * In the lobby nothing else is going out. In a RACE the state packets
     * normally do the job, but they are sent from the frame's physics block and
     * that block does not run while the world is frozen -- the START menu, or a
     * pause. A player who opened the menu for six seconds was reaped by every
     * other machine in the game.
     *
     * ITS OWN TIMER, not `state_at': that one is the state rate, which is
     * `sendFrameRate' and slower than this, and sharing it made the two fight
     * over which had last sent. */
    if ((N.mode == NET_JOINED || N.mode == NET_RACING) && !net_is_host()
        && N.clock - N.peer_at >= 1.0f / NET_LOBBY_HZ) {
        N.peer_at = N.clock;
        send_peer();
    }
    reap();
}

/* -------------------------------------------------------------- the lobby */

static void take_identity(const char *name, int car, const unsigned char up[3],
                          int skin, int face)
{
    snprintf(N.myname, sizeof N.myname, "%s", name ? name : "Player");
    N.mycar = (unsigned char)((car >= 0 && car < 3) ? car : 0);
    N.myup[0] = up ? up[0] : 0;
    N.myup[1] = up ? up[1] : 0;
    N.myup[2] = up ? up[2] : 0;
    N.myskin = (unsigned char)(skin > 0 ? skin : 0);
    N.myface = (unsigned char)(face > 0 ? face : 0);
}

int net_browse(void)
{
    if (!net_open())
        return 0;
    roster_clear();
    N.nsrv = 0;
    N.slot = -1;
    N.mode = NET_BROWSING;
    return 1;
}

int net_host(const char *name, int car, const unsigned char up[3], int skin,
             int face)
{
    int i;
    if (!net_open())
        return 0;
    take_identity(name, car, up, skin, face);
    roster_clear();
    N.slot = 0;
    peer_set_me(0);
    N.host_addr = 0;
    N.host_port = N.port;
    N.set.track = 0;
    N.set.laps = 3;
    for (i = 0; i < 3; i++)
        N.set.restr[i] = NET_RESTR_N - 1;    /* everything, fully upgradable */
    N.nsrv = 0;
    N.mode = NET_HOSTING;
    N.announce_at = N.lobby_at = -1000.f;
    net_log_clear();
    logf_(STR_NET_CONNECTED, N.myname);
    return 1;
}

int net_join_addr(const char *addr, int port, const char *name, int car,
                  const unsigned char up[3], int skin, int face)
{
    if (!net_open())
        return 0;
    N.host_addr = sk_pton(addr);
    if (!N.host_addr)
        return 0;
    N.host_port = port > 0 ? port : NET_PORT;
    take_identity(name, car, up, skin, face);
    roster_clear();
    N.slot = -1;
    N.mode = NET_JOINING;
    N.join_at = -1000.f;
    N.host_heard = N.clock;
    net_log_clear();
    return 1;
}

int net_join(int i, const char *name, int car, const unsigned char up[3],
             int skin, int face)
{
    if (i < 0 || i >= N.nsrv)
        return 0;
    if (!net_open())
        return 0;
    N.host_addr = N.srv[i].addr;
    N.host_port = N.srv[i].port ? N.srv[i].port : NET_PORT;
    take_identity(name, car, up, skin, face);
    roster_clear();
    N.slot = -1;
    N.mode = NET_JOINING;
    N.join_at = -1000.f;
    N.host_heard = N.clock;
    net_log_clear();
    return 1;
}

void net_leave(void)
{
    pk p;
    if (!opened)
        return;
    if (N.mode == NET_HOSTING || N.mode == NET_JOINED
        || N.mode == NET_RACING) {
        w_begin(&p, NP_BYE, N.slot);
        if (net_is_host())
            send_others(&p);
        else if (N.host_addr)
            send_to(&p, N.host_addr, N.host_port);
    }
    roster_clear();
    N.slot = -1;
    N.nsrv = 0;
    N.host_addr = 0;
    N.host_heard = 0.f;
    N.peer_at = 0.f;
    N.start_pending = 0;
    N.race_over = 0;
    N.mode = NET_OFF;
}

int net_n_servers(void) { return N.nsrv; }
const net_server *net_server_at(int i)
{
    return (i >= 0 && i < N.nsrv) ? &N.srv[i] : 0;
}

void net_set_track(int track)
{
    if (!net_is_host() || track < 0 || track >= N_TRACKS)
        return;
    if (N.set.track == (unsigned char)track)
        return;
    N.set.track = (unsigned char)track;
    /* THE NAME THE MENU SHOWS, which is what 20214 is written for: the game's
       own screenshot of the lobby has ">Map is changed to: Fishers", not
       ">... Country_3". `TRACKS[].base' is the engine's internal name. */
    logf_(STR_NET_MAP, STR_TRACK_NAME[track < STR_N_TRACKS ? track : 0]);
    send_lobby();
}

void net_set_laps(int laps)
{
    if (!net_is_host() || laps < 1 || laps > 99)
        return;
    if (N.set.laps == (unsigned char)laps)
        return;
    N.set.laps = (unsigned char)laps;
    logf_(STR_NET_LAPS, laps);
    send_lobby();
}

void net_set_restr(int car, int v)
{
    if (!net_is_host() || car < 0 || car >= 3)
        return;
    if (v < 0) v = 0;
    if (v >= NET_RESTR_N) v = NET_RESTR_N - 1;
    if (N.set.restr[car] == (unsigned char)v)
        return;
    N.set.restr[car] = (unsigned char)v;
    logf_("%s", STR_NET_RESTR);
    send_lobby();
}

int net_car_allowed(int car)
{
    if (car < 0 || car >= 3)
        return 0;
    return N.set.restr[car] != NET_RESTR_OFF;
}

int net_max_upgrade(int car)
{
    if (!net_car_allowed(car))
        return -1;
    return (int)N.set.restr[car] - 1;       /* 1 -> Default, 4 -> Level 3 */
}

void net_set_me(int car, const unsigned char up[3], int skin)
{
    unsigned char nc = (unsigned char)((car >= 0 && car < 3) ? car : 0);
    unsigned char nu[3];
    unsigned char ns = (unsigned char)(skin > 0 ? skin : 0);
    int i, moved;
    for (i = 0; i < 3; i++) nu[i] = up ? up[i] : 0;
    moved = nc != N.mycar || ns != N.myskin || memcmp(nu, N.myup, 3) != 0;
    N.mycar = nc;
    memcpy(N.myup, nu, 3);
    N.myskin = ns;
    if (N.slot >= 0 && N.slot < NET_MAX) {
        N.peer[N.slot].car = nc;
        memcpy(N.peer[N.slot].up, nu, 3);
        N.peer[N.slot].skin = ns;
    }
    if (moved && (N.mode == NET_HOSTING || N.mode == NET_JOINED))
        send_peer();
}

void net_set_ready(int ready)
{
    if (N.slot < 0 || N.slot >= NET_MAX)
        return;
    if (N.peer[N.slot].ready == (unsigned char)!!ready)
        return;
    N.peer[N.slot].ready = (unsigned char)!!ready;
    if (ready)
        logf_(STR_NET_READY, N.myname);
    send_peer();
}

int net_kick(int slot)
{
    pk p;
    if (!net_is_host() || slot < 0 || slot >= NET_MAX || !N.peer[slot].used)
        return -1;
    if (slot == N.slot)
        return -2;                          /* 42907 */
    w_begin(&p, NP_KICK, N.slot);
    w_u8(&p, (unsigned int)slot);
    send_to(&p, N.peer[slot].addr, N.peer[slot].port);
    logf_(STR_NET_KICKED, N.peer[slot].name);
    peer_clear(slot);
    send_lobby();
    return 0;
}

int net_can_start(void)
{
    int i, n = 0;
    if (!net_is_host())
        return 0;
    for (i = 0; i < NET_MAX; i++) {
        if (!N.peer[i].used) continue;
        n++;
        /* THE HOST'S OWN READY IS NOT ASKED FOR. It is the one pressing Race,
           which is the same statement. */
        if (i != N.slot && !N.peer[i].ready)
            return 0;
    }
    return n >= 2;
}

void net_start(void)
{
    pk p;
    int i;
    if (!net_is_host())
        return;
    w_begin(&p, NP_START, N.slot);
    w_settings(&p);
    send_others(&p);
    for (i = 0; i < NET_MAX; i++) {
        N.peer[i].have = 0;
        N.peer[i].finished = 0;
        N.peer[i].lap = 0;
        N.peer[i].best_lap = 0.f;
        N.peer[i].total = 0.f;
    }
    N.race_over = 0;
    N.mode = NET_RACING;
    N.state_at = -1000.f;
    N.peer_at = -1000.f;
}

int net_take_start(void)
{
    if (!N.start_pending)
        return 0;
    N.start_pending = 0;
    return 1;
}

/* -------------------------------------------------------------- the race */

void net_race_begin(void)
{
    int i;
    for (i = 0; i < NET_MAX; i++) {
        N.peer[i].have = 0;
        N.peer[i].finished = 0;
    }
    N.race_over = 0;
    N.mode = NET_RACING;
    N.state_at = -1000.f;
    N.peer_at = -1000.f;
}

/* THE STATE RATE IS network.ini's OWN, read as sends per second, with a FLOOR
   under it. See gen_dlg_data.py on why the reading is stated rather than
   certain, and net.h at NET_STATE_FLOOR for why 5 is not enough here. */
#define NET_STATE_HZ ((float)DLG_NET_sendFrameRate > NET_STATE_FLOOR \
                      ? (float)DLG_NET_sendFrameRate : NET_STATE_FLOOR)

void net_send_state(const ai_sample *s, int lap, int place, float best_lap)
{
    pk p;
    ai_sample out;
    float since;
    if (N.mode != NET_RACING || N.slot < 0 || !s)
        return;
    since = N.clock - N.state_at;
    if (since < 1.0f / NET_STATE_HZ)
        return;
    /* THE SENDER'S OWN CADENCE GOES ON THE WIRE, in the sample's own `dt' --
     * the field a recording uses for exactly this and which this packet has
     * been shipping as zero since it was written.
     *
     * It is what the receiver's render delay is (net.h at NET_INTERP_MIN), and
     * it is measured rather than computed from NET_STATE_HZ because the two are
     * not the same number: this is called from the frame's physics block, so
     * the real interval is the first frame boundary at or after 1/rate and at
     * 30 fps that is 33 ms of difference. A receiver that buffered for the
     * NOMINAL interval would run dry every second packet.
     *
     * The first send has no previous one -- `state_at' is -1000 -- so it is
     * sent as the nominal interval and nothing extrapolates off it anyway
     * (interpolation needs two). */
    out = *s;
    if (since > 10.f)
        since = 1.0f / NET_STATE_HZ;
    {
        float ticks = since * AI_DT_SCALE;
        out.dt = (unsigned short)(ticks < 0.f ? 0.f
                                  : ticks > 65535.f ? 65535.f : ticks);
    }
    N.state_at = N.clock;
    w_begin(&p, NP_STATE, N.slot);
    p.b[7] = N.state_seq++;
    w_sample(&p, &out);
    w_u8(&p, (unsigned int)(lap < 0 ? 0 : lap));
    w_u8(&p, (unsigned int)(place < 0 ? 0 : place));
    w_u16(&p, (unsigned int)(best_lap > 0.f ? (int)(best_lap * 100.f) : 0));
    send_others(&p);
    if (N.slot < NET_MAX) {
        N.peer[N.slot].lap = (unsigned char)lap;
        N.peer[N.slot].place = (unsigned char)place;
        N.peer[N.slot].best_lap = best_lap;
    }
}

void net_send_finished(float total, float best_lap)
{
    pk p;
    int i;
    if (N.slot < 0 || N.slot >= NET_MAX)
        return;
    if (N.peer[N.slot].finished)
        return;
    N.peer[N.slot].finished = 1;
    N.peer[N.slot].total = total;
    N.peer[N.slot].best_lap = best_lap;
    w_begin(&p, NP_FINISH, N.slot);
    w_u32(&p, (unsigned int)(total * 1000.f));
    w_u32(&p, (unsigned int)(best_lap * 1000.f));
    send_others(&p);
    logf_(STR_NET_FINISHED, N.myname);
    /* Everyone home? Then the race is over for the screen as well. */
    for (i = 0; i < NET_MAX; i++)
        if (N.peer[i].used && !N.peer[i].finished)
            return;
    N.race_over = 1;
}

/* CARRY THE NEWEST SAMPLE FORWARD on its own reported velocity. Only the
 * POSITION moves: the angular velocity is not in the sample (ai.h -- state
 * slots 10..12 are not carried, and the recordings do not have them either), so
 * there is nothing to extrapolate an orientation FROM, and a guessed spin is a
 * car that visibly rotates the wrong way, which is worse than one that holds
 * its heading for a packet.
 *
 * CAPPED at NET_DR_MAX: a peer whose packets have stopped must coast to a halt
 * rather than fly off the level, and NET_TIMEOUT then takes the slot away. */
static void dr_forward(const ai_sample *s, float age, ai_sample *out)
{
    int i;
    *out = *s;
    if (age < 0.f) age = 0.f;
    if (age > NET_DR_MAX) age = NET_DR_MAX;
    for (i = 0; i < 3; i++)
        out->p[i] += (float)s->mom[i] / AI_VEL_SCALE * age;
}

/* A SAMPLE `u' OF THE WAY FROM `a' TO `b', field by field.
 *
 * Everything the pose is built from is interpolated and nothing is held: the
 * position, the ORIENTATION, the six suspension lengths, the steer angle and
 * the velocity. Holding any one of them is a visible step at the packet rate --
 * the springs stop breathing over a kerb, the wheels snap to a new lock -- and
 * they are all continuous quantities sampled from one moving car, so there is
 * nothing to guess: the two ends of the interval are both MEASURED.
 *
 * THE QUATERNION IS NLERPED, not slerped, and the double cover is handled by
 * flipping `b' onto `a's hemisphere first -- without which two samples either
 * side of the sign change interpolate the LONG way round and the car snaps
 * through a half turn. nlerp against slerp is an error in the RATE and not in
 * the endpoints, and it is bounded by the angle between the two: at
 * NET_STATE_FLOOR a car yawing at the camera's own 180 deg/s clamp covers 9
 * degrees between samples, where nlerp's worst deviation from constant angular
 * speed is under a hundredth of a degree. Slerp's two trig calls per car per
 * frame buy nothing measurable.
 *
 * `dt' is not interpolated -- it is the sender's cadence, not a state. */
static void lerp_sample(const ai_sample *a, const ai_sample *b, float u,
                        ai_sample *out)
{
    double qa[4], qb[4], qm[4], len, dot = 0.0;
    int i;

    for (i = 0; i < 3; i++)
        out->p[i] = a->p[i] + (b->p[i] - a->p[i]) * u;
    for (i = 0; i < 3; i++)
        out->mom[i] = (short)(a->mom[i] + (b->mom[i] - a->mom[i]) * u);
    for (i = 0; i < 6; i++)
        out->susp[i] = (unsigned char)(a->susp[i]
                                       + (b->susp[i] - a->susp[i]) * u + 0.5f);
    out->steer = (short)(a->steer + (b->steer - a->steer) * u);
    out->dt = b->dt;

    for (i = 0; i < 4; i++) {
        qa[i] = (double)a->q[i];
        qb[i] = (double)b->q[i];
        dot += qa[i] * qb[i];
    }
    if (dot < 0.0)
        for (i = 0; i < 4; i++) qb[i] = -qb[i];
    len = 0.0;
    for (i = 0; i < 4; i++) {
        qm[i] = qa[i] + (qb[i] - qa[i]) * (double)u;
        len += qm[i] * qm[i];
    }
    /* Back onto the sphere, and back into the sample's own quantisation. A pair
       that cancelled -- which needs two samples a full turn apart and cannot
       happen at any send rate a car can be driven at -- keeps `b'. */
    if (len > 1e-9) {
        len = AI_Q_SCALE / sqrt(len);
        for (i = 0; i < 4; i++)
            out->q[i] = (short)(qm[i] * len);
    } else {
        for (i = 0; i < 4; i++)
            out->q[i] = b->q[i];
    }
}

int net_remote_pose(int slot, ai_sample *out)
{
    const net_peer *q;
    float gap, t, u;

    if (slot < 0 || slot >= NET_MAX || slot == N.slot || !out)
        return 0;
    q = &N.peer[slot];
    if (!q->used || !q->have)
        return 0;

    /* ONE SAMPLE AND NOTHING TO INTERPOLATE ACROSS -- the first packet of a
       race. Dead reckoning is all there is. */
    if (q->have < 2) {
        dr_forward(&q->s, N.clock - q->s_at, out);
        return 1;
    }

    /* THE RENDER DELAY IS THE SENDER'S OWN CADENCE, which it stamped into the
     * sample's `dt' (net_send_state). A peer that reports none -- and only a
     * build that predates the field does -- is measured off its own arrival
     * interval instead, which is the same number plus this machine's jitter. */
    gap = q->s.dt ? (float)q->s.dt / AI_DT_SCALE : (q->s_at - q->s0_at);
    if (gap < NET_INTERP_MIN) gap = NET_INTERP_MIN;
    if (gap > NET_INTERP_MAX) gap = NET_INTERP_MAX;

    t = N.clock - gap;

    /* PAST THE NEWEST: a packet is late or lost, and the car carries on rather
       than stopping dead. Continuous with the branch below -- at t == s_at the
       interpolation returns `s' exactly and this returns `s' plus nothing. */
    if (t >= q->s_at) {
        dr_forward(&q->s, t - q->s_at, out);
        return 1;
    }
    /* BEFORE THE OLDER ONE: two packets arrived in the same frame, or the clock
       was pushed. Nothing to do but sit on the older sample. */
    if (t <= q->s0_at || q->s_at - q->s0_at < 1e-6f) {
        *out = q->s0;
        return 1;
    }
    u = (t - q->s0_at) / (q->s_at - q->s0_at);
    lerp_sample(&q->s0, &q->s, u, out);
    return 1;
}

/* --------------------------------------------------------------- for main */

int net_slot_car(int slot)
{
    if (slot < 0 || slot >= NET_MAX || !N.peer[slot].used)
        return -1;
    return N.peer[slot].car;
}
