/* Copyright © 1994-1999 Lucent Technologies Inc.  All rights reserved.
 * Portions Copyright © 1997-1999 Vita Nuova Limited
 * Portions Copyright © 2000-2007 Vita Nuova Holdings Limited
 *                                (www.vitanuova.com)
 * Revisions Copyright © 2000-2007 Lucent Technologies Inc. and others
 *
 * Modified for the Akaros operating system:
 * Copyright (c) 2013-2014 The Regents of the University of California
 * Copyright (c) 2013-2015 Google Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */

#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>
#include <ip.h>

#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>
#include <ip.h>

enum {
	QMAX = 64 * 1024 - 1,
	IP_TCPPROTO = 6,

	TCP4_IPLEN = 8,
	TCP4_PHDRSIZE = 12,
	TCP4_HDRSIZE = 20,
	TCP4_TCBPHDRSZ = 40,
	TCP4_PKT = TCP4_IPLEN + TCP4_PHDRSIZE,

	TCP6_IPLEN = 0,
	TCP6_PHDRSIZE = 40,
	TCP6_HDRSIZE = 20,
	TCP6_TCBPHDRSZ = 60,
	TCP6_PKT = TCP6_IPLEN + TCP6_PHDRSIZE,

	TcptimerOFF = 0,
	TcptimerON = 1,
	TcptimerDONE = 2,
	MAX_TIME = (1 << 20),	/* Forever */
	TCP_ACK = 50,	/* Timed ack sequence in ms */
	MAXBACKMS = 9 * 60 * 1000,	/* longest backoff time (ms) before hangup */

	URG = 0x20,	/* Data marked urgent */
	ACK = 0x10,	/* Acknowledge is valid */
	PSH = 0x08,	/* Whole data pipe is pushed */
	RST = 0x04,	/* Reset connection */
	SYN = 0x02,	/* Pkt. is synchronise */
	FIN = 0x01,	/* Start close down */

	EOLOPT = 0,
	NOOPOPT = 1,
	MSSOPT = 2,
	MSS_LENGTH = 4,	/* max segment size header option length */
	WSOPT = 3,
	WS_LENGTH = 3,	/* WS header option length */
	MAX_WS_VALUE = 14,	/* RFC specified.  Limits available window to 2^30 */
	TS_OPT = 8,
	TS_LENGTH = 10,
	SACK_OK_OPT = 4,
	SACK_OK_LENGTH = 2,
	SACK_OPT = 5,
	MSL2 = 10,
	MSPTICK = 50,	/* Milliseconds per timer tick */
	DEF_MSS = 1460,	/* Default mean segment */
	DEF_MSS6 = 1280,	/* Default mean segment (min) for v6 */
	SACK_SUPPORTED = TRUE,	/* SACK is on by default */
	MAX_NR_SACKS_PER_PACKET = 4,	/* limited by TCP's opts size */
	MAX_NR_TRACKED_SACKS = 10,
	DEF_RTT = 500,	/* Default round trip */
	DEF_KAT = 120000,	/* Default time (ms) between keep alives */
	TCP_LISTEN = 0,	/* Listen connection */
	TCP_CONNECT = 1,	/* Outgoing connection */
	SYNACK_RXTIMER = 250,	/* ms between SYNACK retransmits */

	TCPREXMTTHRESH = 3,	/* dupack threshhold for recovery */
	SACK_RETRANS_RECOVERY = 1,
	FAST_RETRANS_RECOVERY = 2,
	CWIND_SCALE = 10,	/* initial CWIND will be MSS * this */

	FORCE = 1,
	CLONE = 2,
	RETRAN = 4,
	ACTIVE = 8,
	SYNACK = 16,
	TSO = 32,

	LOGAGAIN = 3,
	LOGDGAIN = 2,

	Closed = 0,	/* Connection states */
	Listen,
	Syn_sent,
	Established,
	Finwait1,
	Finwait2,
	Close_wait,
	Closing,
	Last_ack,
	Time_wait,

	Maxlimbo = 1000,	/* maximum procs waiting for response to SYN ACK */
	NLHT = 256,	/* hash table size, must be a power of 2 */
	LHTMASK = NLHT - 1,

	HaveWS = 1 << 8,
};

/* Must correspond to the enumeration above */
char *tcpstates[] = {
	"Closed", "Listen", "Syn_sent",
	"Established", "Finwait1", "Finwait2", "Close_wait",
	"Closing", "Last_ack", "Time_wait"
};

typedef struct Tcptimer Tcptimer;
struct Tcptimer {
	Tcptimer *next;
	Tcptimer *prev;
	Tcptimer *readynext;
	int state;
	uint64_t start;
	uint64_t count;
	void (*func) (void *);
	void *arg;
};

/*
 *  v4 and v6 pseudo headers used for
 *  checksuming tcp
 */
typedef struct Tcp4hdr Tcp4hdr;
struct Tcp4hdr {
	uint8_t vihl;				/* Version and header length */
	uint8_t tos;				/* Type of service */
	uint8_t length[2];			/* packet length */
	uint8_t id[2];				/* Identification */
	uint8_t frag[2];			/* Fragment information */
	uint8_t Unused;
	uint8_t proto;
	uint8_t tcplen[2];
	uint8_t tcpsrc[4];
	uint8_t tcpdst[4];
	uint8_t tcpsport[2];
	uint8_t tcpdport[2];
	uint8_t tcpseq[4];
	uint8_t tcpack[4];
	uint8_t tcpflag[2];
	uint8_t tcpwin[2];
	uint8_t tcpcksum[2];
	uint8_t tcpurg[2];
	/* Options segment */
	uint8_t tcpopt[1];
};

typedef struct Tcp6hdr Tcp6hdr;
struct Tcp6hdr {
	uint8_t vcf[4];
	uint8_t ploadlen[2];
	uint8_t proto;
	uint8_t ttl;
	uint8_t tcpsrc[IPaddrlen];
	uint8_t tcpdst[IPaddrlen];
	uint8_t tcpsport[2];
	uint8_t tcpdport[2];
	uint8_t tcpseq[4];
	uint8_t tcpack[4];
	uint8_t tcpflag[2];
	uint8_t tcpwin[2];
	uint8_t tcpcksum[2];
	uint8_t tcpurg[2];
	/* Options segment */
	uint8_t tcpopt[1];
};

struct sack_block {
	uint32_t left;
	uint32_t right;
};

/*
 *  this represents the control info
 *  for a single packet.  It is derived from
 *  a packet in ntohtcp{4,6}() and stuck into
 *  a packet in htontcp{4,6}().
 */
typedef struct Tcp Tcp;
struct Tcp {
	uint16_t source;
	uint16_t dest;
	uint32_t seq;
	uint32_t ack;
	uint8_t flags;
	uint16_t ws;				/* window scale option (if not zero) */
	uint32_t wnd;
	uint16_t urg;
	uint16_t mss;				/* max segment size option (if not zero) */
	uint16_t len;				/* size of data */
	uint32_t ts_val;			/* timestamp val from sender */
	uint32_t ts_ecr;			/* timestamp echo response from sender */
	bool sack_ok;				/* header had/should have SACK_PERMITTED */
	uint8_t nr_sacks;
	struct sack_block sacks[MAX_NR_SACKS_PER_PACKET];
};

/*
 *  this header is malloc'd to thread together fragments
 *  waiting to be coalesced
 */
typedef struct Reseq Reseq;
struct Reseq {
	Reseq *next;
	Tcp seg;
	struct block *bp;
	uint16_t length;
};

/*
 *  the qlock in the Conv locks this structure
 */
typedef struct Tcpctl Tcpctl;
struct Tcpctl {
	uint8_t state;				/* Connection state */
	uint8_t type;				/* Listening or active connection */
	uint8_t code;				/* Icmp code */
	struct {
		uint32_t una;			/* Left edge of unacked data region */
		uint32_t nxt;			/* Next seq to send, right edge of unacked */
		uint32_t ptr;			/* Data pointer, next to send for tcpoutput */
		uint32_t wnd;			/* Tcp send window */
		uint32_t urg;			/* Urgent data pointer */
		uint32_t wl2;
		int scale;				/* how much to right shift window in xmitted packets */
		uint32_t loss_hint;		/* number of loss hints rcvd */
		int recovery;			/* loss recovery flag */
		uint32_t recovery_pt;	/* right window for recovery point */

		uint32_t sack_rxt;		/* sack right edge (re-transed) */
		uint32_t pipe_bytes;	/* estimate of how much is in flight */

		// XXX if we don't need this, we can simplify update_sacks()
		uint32_t sack_amt;		/* bytes tracked by sacks */

		uint8_t nr_sacks;
		struct sack_block sacks[MAX_NR_TRACKED_SACKS];
	} snd;
	struct {
		uint32_t nxt;			/* Receive pointer to next uint8_t slot */
		uint32_t wnd;			/* Receive window incoming */
		uint32_t urg;			/* Urgent pointer */
		int blocked;
		int una;				/* unacked data segs */
		int scale;				/* how much to left shift window in rcved packets */
	} rcv;
	uint32_t iss;				/* Initial sequence number */
	int sawwsopt;				/* true if we saw a wsopt on the incoming SYN */
	uint32_t cwind;				/* Congestion window */
	int scale;					/* desired snd.scale */
	uint32_t ssthresh;			/* Slow start threshold */
	int resent;					/* Bytes just resent */
	int irs;					/* Initial received squence */
	uint16_t mss;				/* Max segment size */
	uint16_t typical_mss;		/* MSS for most packets (< MSS for some opts) */
	int rerecv;					/* Overlap of data rerecevived */
	uint32_t window;			/* Recevive window */
	uint8_t backoff;			/* Exponential backoff counter */
	int backedoff;				/* ms we've backed off for rexmits */
	uint8_t flags;				/* State flags */
	Reseq *reseq;				/* Resequencing queue */
	Tcptimer timer;				/* Activity timer */
	Tcptimer acktimer;			/* Acknowledge timer */
	Tcptimer rtt_timer;			/* Round trip timer */
	Tcptimer katimer;			/* keep alive timer */
	uint32_t rttseq;			/* Round trip sequence */
	int srtt;					/* Shortened round trip */
	int mdev;					/* Mean deviation of round trip */
	int kacounter;				/* count down for keep alive */
	uint64_t sndsyntime;		/* time syn sent */
	uint64_t time;				/* time Finwait2 was sent */
	int nochecksum;				/* non-zero means don't send checksums */
	int flgcnt;					/* number of flags in the sequence (FIN,SEQ) */
	uint32_t ts_recent;			/* timestamp received around last_ack_sent */
	uint32_t last_ack_sent;		/* to determine when to update timestamp */
	bool sack_ok;				/* Can use SACK for this connection */

	union {
		Tcp4hdr tcp4hdr;
		Tcp6hdr tcp6hdr;
	} protohdr;					/* prototype header */
};

/*
 *  New calls are put in limbo rather than having a conversation structure
 *  allocated.  Thus, a SYN attack results in lots of limbo'd calls but not
 *  any real Conv structures mucking things up.  Calls in limbo rexmit their
 *  SYN ACK every SYNACK_RXTIMER ms up to 4 times, i.e., they disappear after 1 second.
 *
 *  In particular they aren't on a listener's queue so that they don't figure
 *  in the input queue limit.
 *
 *  If 1/2 of a T3 was attacking SYN packets, we'ld have a permanent queue
 *  of 70000 limbo'd calls.  Not great for a linear list but doable.  Therefore
 *  there is no hashing of this list.
 */
typedef struct Limbo Limbo;
struct Limbo {
	Limbo *next;

	uint8_t laddr[IPaddrlen];
	uint8_t raddr[IPaddrlen];
	uint16_t lport;
	uint16_t rport;
	uint32_t irs;				/* initial received sequence */
	uint32_t iss;				/* initial sent sequence */
	uint16_t mss;				/* mss from the other end */
	uint16_t rcvscale;			/* how much to scale rcvd windows */
	uint16_t sndscale;			/* how much to scale sent windows */
	uint64_t lastsend;			/* last time we sent a synack */
	uint8_t version;			/* v4 or v6 */
	uint8_t rexmits;			/* number of retransmissions */
	bool sack_ok;				/* other side said SACK_OK */
	uint32_t ts_val;			/* timestamp val from sender */
};

int tcp_irtt = DEF_RTT;			/* Initial guess at round trip time */
uint16_t tcp_mss = DEF_MSS;		/* Maximum segment size to be sent */

enum {
	/* MIB stats */
	MaxConn,
	ActiveOpens,
	PassiveOpens,
	EstabResets,
	CurrEstab,
	InSegs,
	OutSegs,
	RetransSegs,
	RetransTimeouts,
	InErrs,
	OutRsts,

	/* non-MIB stats */
	CsumErrs,
	HlenErrs,
	LenErrs,
	OutOfOrder,

	Nstats
};

static char *statnames[] = {
	[MaxConn] "MaxConn",
	[ActiveOpens] "ActiveOpens",
	[PassiveOpens] "PassiveOpens",
	[EstabResets] "EstabResets",
	[CurrEstab] "CurrEstab",
	[InSegs] "InSegs",
	[OutSegs] "OutSegs",
	[RetransSegs] "RetransSegs",
	[RetransTimeouts] "RetransTimeouts",
	[InErrs] "InErrs",
	[OutRsts] "OutRsts",
	[CsumErrs] "CsumErrs",
	[HlenErrs] "HlenErrs",
	[LenErrs] "LenErrs",
	[OutOfOrder] "OutOfOrder",
};

typedef struct Tcppriv Tcppriv;
struct tcppriv {
	/* List of active timers */
	qlock_t tl;
	Tcptimer *timers;

	/* hash table for matching conversations */
	struct Ipht ht;

	/* calls in limbo waiting for an ACK to our SYN ACK */
	int nlimbo;
	Limbo *lht[NLHT];

	/* for keeping track of tcpackproc */
	qlock_t apl;
	int ackprocstarted;

	uint32_t stats[Nstats];
};

/*
 *  Setting tcpporthogdefense to non-zero enables Dong Lin's
 *  solution to hijacked systems staking out port's as a form
 *  of DoS attack.
 *
 *  To avoid stateless Conv hogs, we pick a sequence number at random.  If
 *  it that number gets acked by the other end, we shut down the connection.
 *  Look for tcpporthogedefense in the code.
 */
int tcpporthogdefense = 0;

int addreseq(Tcpctl *, struct tcppriv *, Tcp *, struct block *, uint16_t);
void getreseq(Tcpctl *, Tcp *, struct block **, uint16_t *);
void localclose(struct conv *, char *unused_char_p_t);
void procsyn(struct conv *, Tcp *);
void tcpiput(struct Proto *, struct Ipifc *, struct block *);
void tcpoutput(struct conv *);
int tcptrim(Tcpctl *, Tcp *, struct block **, uint16_t *);
void tcpstart(struct conv *, int);
void tcptimeout(void *);
void tcpsndsyn(struct conv *, Tcpctl *);
void tcprcvwin(struct conv *);
void tcpacktimer(void *);
void tcpkeepalive(void *);
void tcpsetkacounter(Tcpctl *);
void tcprxmit(struct conv *);
void tcpsettimer(Tcpctl *);
void tcpsynackrtt(struct conv *);
void tcpsetscale(struct conv *, Tcpctl *, uint16_t, uint16_t);
static void tcp_loss_event(Tcpctl *tcb);
static uint16_t derive_payload_mss(Tcpctl *tcb);
static int seq_within(uint32_t x, uint32_t low, uint32_t high);
static int seq_lt(uint32_t x, uint32_t y);
static int seq_le(uint32_t x, uint32_t y);
static int seq_gt(uint32_t x, uint32_t y);
static int seq_ge(uint32_t x, uint32_t y);
static uint32_t seq_abs_diff(uint32_t x, uint32_t y);

static void limborexmit(struct Proto *);
static void limbo(struct conv *, uint8_t * unused_uint8_p_t, uint8_t *, Tcp *,
				  int);

void tcpsetstate(struct conv *s, uint8_t newstate)
{
	Tcpctl *tcb;
	uint8_t oldstate;
	struct tcppriv *tpriv;

	tpriv = s->p->priv;

	tcb = (Tcpctl *) s->ptcl;

	oldstate = tcb->state;
	if (oldstate == newstate)
		return;

	if (oldstate == Established)
		tpriv->stats[CurrEstab]--;
	if (newstate == Established)
		tpriv->stats[CurrEstab]++;

	/**
	print( "%d/%d %s->%s CurrEstab=%d\n", s->lport, s->rport,
		tcpstates[oldstate], tcpstates[newstate], tpriv->tstats.tcpCurrEstab );
	**/

	switch (newstate) {
		case Closed:
			qclose(s->rq);
			qclose(s->wq);
			qclose(s->eq);
			break;

		case Close_wait:	/* Remote closes */
			qhangup(s->rq, NULL);
			break;
	}

	tcb->state = newstate;

	if (oldstate == Syn_sent && newstate != Closed)
		Fsconnected(s, NULL);
}

static void tcpconnect(struct conv *c, char **argv, int argc)
{
	Fsstdconnect(c, argv, argc);
	tcpstart(c, TCP_CONNECT);
}

static int tcpstate(struct conv *c, char *state, int n)
{
	Tcpctl *s;

	s = (Tcpctl *) (c->ptcl);

	return snprintf(state, n,
					"%s qin %d qout %d srtt %d mdev %d cwin %u swin %u>>%d rwin %u>>%d timer.start %llu timer.count %llu rerecv %d katimer.start %d katimer.count %d\n",
					tcpstates[s->state],
					c->rq ? qlen(c->rq) : 0,
					c->wq ? qlen(c->wq) : 0,
					s->srtt, s->mdev,
					s->cwind, s->snd.wnd, s->rcv.scale, s->rcv.wnd,
					s->snd.scale, s->timer.start, s->timer.count, s->rerecv,
					s->katimer.start, s->katimer.count);
}

static int tcpinuse(struct conv *c)
{
	Tcpctl *s;

	s = (Tcpctl *) (c->ptcl);
	return s->state != Closed;
}

static void tcpannounce(struct conv *c, char **argv, int argc)
{
	Fsstdannounce(c, argv, argc);
	tcpstart(c, TCP_LISTEN);
	Fsconnected(c, NULL);
}

static void tcpbypass(struct conv *cv, char **argv, int argc)
{
	struct tcppriv *tpriv = cv->p->priv;

	Fsstdbypass(cv, argv, argc);
	iphtadd(&tpriv->ht, cv);
}

static void tcpshutdown(struct conv *c, int how)
{
	Tcpctl *tcb = (Tcpctl*)c->ptcl;

	/* Do nothing for the read side */
	if (how == SHUT_RD)
		return;
	/* Sends a FIN.  If we're in another state (like Listen), we'll run into
	 * issues, since we'll never send the FIN.  We'll be shutdown on our end,
	 * but we'll never tell the distant end.  Might just be an app issue. */
	switch (tcb->state) {
	case Established:
		tcb->flgcnt++;
		tcb->snd.nxt++;
		tcpsetstate(c, Finwait1);
		tcpoutput(c);
		break;
	}
}

/*
 *  tcpclose is always called with the q locked
 */
static void tcpclose(struct conv *c)
{
	Tcpctl *tcb;

	tcb = (Tcpctl *) c->ptcl;

	qhangup(c->rq, NULL);
	qhangup(c->wq, NULL);
	qhangup(c->eq, NULL);
	qflush(c->rq);

	switch (tcb->state) {
		case Listen:
			/*
			 *  reset any incoming calls to this listener
			 */
			Fsconnected(c, "Hangup");

			localclose(c, NULL);
			break;
		case Closed:
		case Syn_sent:
			localclose(c, NULL);
			break;
		case Established:
			tcb->flgcnt++;
			tcb->snd.nxt++;
			tcpsetstate(c, Finwait1);
			tcpoutput(c);
			break;
		case Close_wait:
			tcb->flgcnt++;
			tcb->snd.nxt++;
			tcpsetstate(c, Last_ack);
			tcpoutput(c);
			break;
	}
}

void tcpkick(void *x)
{
	ERRSTACK(1);
	struct conv *s = x;
	Tcpctl *tcb;

	tcb = (Tcpctl *) s->ptcl;

	qlock(&s->qlock);
	if (waserror()) {
		qunlock(&s->qlock);
		nexterror();
	}

	switch (tcb->state) {
		case Syn_sent:
		case Established:
		case Close_wait:
			/*
			 * Push data
			 */
			tcprcvwin(s);
			tcpoutput(s);
			break;
		default:
			localclose(s, "Hangup");
			break;
	}

	qunlock(&s->qlock);
	poperror();
}

void tcprcvwin(struct conv *s)
{	/* Call with tcb locked */
	int w;
	Tcpctl *tcb;

	tcb = (Tcpctl *) s->ptcl;
	w = tcb->window - qlen(s->rq);
	/* Keep in mind that tcb->window - qlen can be < 0.
	 *
	 * If our real window is < mss, we'll advertise 0.  This should avoid the
	 * Silly Window Syndrome (RFC 813 and 1122) by not advertising a small
	 * buffer, where small is < MSS.  It will also help buggy TCP senders, such
	 * as qemu, that treat rcv.win < MSS as a stop condition. */
	if (w < tcb->mss)
		w = 0;
	tcb->rcv.wnd = w;
	if (w == 0)
		tcb->rcv.blocked = 1;
}

void tcpacktimer(void *v)
{
	ERRSTACK(1);
	Tcpctl *tcb;
	struct conv *s;

	s = v;
	tcb = (Tcpctl *) s->ptcl;

	qlock(&s->qlock);
	if (waserror()) {
		qunlock(&s->qlock);
		nexterror();
	}
	if (tcb->state != Closed) {
		tcb->flags |= FORCE;
		tcprcvwin(s);
		tcpoutput(s);
	}
	qunlock(&s->qlock);
	poperror();
}

static void tcpcreate(struct conv *c)
{
	c->rq = qopen(QMAX, Qcoalesce, 0, 0);
	c->wq = qopen(8 * QMAX, Qkick, tcpkick, c);
}

static void timerstate(struct tcppriv *priv, Tcptimer * t, int newstate)
{
	if (newstate != TcptimerON) {
		if (t->state == TcptimerON) {
			// unchain
			if (priv->timers == t) {
				priv->timers = t->next;
				if (t->prev != NULL)
					panic("timerstate1");
			}
			if (t->next)
				t->next->prev = t->prev;
			if (t->prev)
				t->prev->next = t->next;
			t->next = t->prev = NULL;
		}
	} else {
		if (t->state != TcptimerON) {
			// chain
			if (t->prev != NULL || t->next != NULL)
				panic("timerstate2");
			t->prev = NULL;
			t->next = priv->timers;
			if (t->next)
				t->next->prev = t;
			priv->timers = t;
		}
	}
	t->state = newstate;
}

void tcpackproc(void *a)
{
	ERRSTACK(1);
	Tcptimer *t, *tp, *timeo;
	struct Proto *tcp;
	struct tcppriv *priv;
	int loop;

	tcp = a;
	priv = tcp->priv;

	for (;;) {
		kthread_usleep(MSPTICK * 1000);

		qlock(&priv->tl);
		timeo = NULL;
		loop = 0;
		for (t = priv->timers; t != NULL; t = tp) {
			if (loop++ > 10000)
				panic("tcpackproc1");
			tp = t->next;
			/* this is a little odd.  overall, we wake up once per 'tick' (50ms,
			 * whatever).  then, we decrement count.  so the timer val is in
			 * units of 50 ms.  the timer list isn't sorted either.  once
			 * someone expires, we get moved to another LL, local, and we fire
			 * those alarms.
			 *
			 * the best anyone could do would be 50 ms granularity.
			 *
			 * if things are slow, you could skew later too.
			 *
			 * actually, you're expected value is 25ms for the first count.  so
			 * whatever your timer.start is, your wait time is start * 50 - 25.
			 * 		which is why we wait 25 ms to open up our window again.
			 *
			 * might be issues with concurrency.  once the alarm is set to done
			 * and yanked off the list, what's to stop a concurrent setter from
			 * putting it back on the list and setting TcptimerON?
			 * 		there's a lot of lockless peeks at the timer.state
			 *
			 * probably be better served with a kthread timer chain
			 * 		one assumption with the timerchain stuff is that the source
			 * 		is an IRQ, and thus IRQ context matters, etc.
			 *
			 * 		with a kth tchain, we're in kth context already.  and you
			 * 		probably don't want to send another RKM for each timer.
			 * 		unless the locking matters.
			 *
			 * 		interesting - even the pcpu tchains - should those be a
			 * 		per-core kth?  does any alarm need to run from IRQ ctx?
			 * 				maybe.
			 * */
			if (t->state == TcptimerON) {
				t->count--;
				if (t->count == 0) {
					timerstate(priv, t, TcptimerDONE);
					t->readynext = timeo;
					timeo = t;
				}
			}
		}
		qunlock(&priv->tl);

		loop = 0;
		for (t = timeo; t != NULL; t = t->readynext) {
			if (loop++ > 10000)
				panic("tcpackproc2");
			if (t->state == TcptimerDONE && t->func != NULL) {
				/* discard error style */
				if (!waserror())
					(*t->func) (t->arg);
				poperror();
			}
		}

		limborexmit(tcp);
	}
}

void tcpgo(struct tcppriv *priv, Tcptimer * t)
{
	if (t == NULL || t->start == 0)
		return;

	qlock(&priv->tl);
	t->count = t->start;
	timerstate(priv, t, TcptimerON);
	qunlock(&priv->tl);
}

void tcphalt(struct tcppriv *priv, Tcptimer * t)
{
	if (t == NULL)
		return;

	qlock(&priv->tl);
	timerstate(priv, t, TcptimerOFF);
	qunlock(&priv->tl);
}

int backoff(int n)
{
	return 1 << n;
}

void localclose(struct conv *s, char *reason)
{	/* called with tcb locked */
	Tcpctl *tcb;
	Reseq *rp, *rp1;
	struct tcppriv *tpriv;

	tpriv = s->p->priv;
	tcb = (Tcpctl *) s->ptcl;

	iphtrem(&tpriv->ht, s);

	tcphalt(tpriv, &tcb->timer);
	tcphalt(tpriv, &tcb->rtt_timer);
	tcphalt(tpriv, &tcb->acktimer);
	tcphalt(tpriv, &tcb->katimer);

	/* Flush reassembly queue; nothing more can arrive */
	for (rp = tcb->reseq; rp != NULL; rp = rp1) {
		rp1 = rp->next;
		freeblist(rp->bp);
		kfree(rp);
	}
	tcb->reseq = NULL;

	if (tcb->state == Syn_sent)
		Fsconnected(s, reason);

	qhangup(s->rq, reason);
	qhangup(s->wq, reason);

	tcpsetstate(s, Closed);

	/* listener will check the rq state */
	if (s->state == Announced)
		rendez_wakeup(&s->listenr);
}

/* mtu (- TCP + IP hdr len) of 1st hop */
int tcpmtu(struct Proto *tcp, uint8_t * addr, int version, int *scale,
	   uint8_t *flags)
{
	struct Ipifc *ifc;
	int mtu;

	ifc = findipifc(tcp->f, addr, 0);
	switch (version) {
		default:
		case V4:
			mtu = DEF_MSS;
			if (ifc != NULL)
				mtu = ifc->maxtu - ifc->m->hsize - (TCP4_PKT + TCP4_HDRSIZE);
			break;
		case V6:
			mtu = DEF_MSS6;
			if (ifc != NULL)
				mtu = ifc->maxtu - ifc->m->hsize - (TCP6_PKT + TCP6_HDRSIZE);
			break;
	}
	*flags &= ~TSO;

	if (ifc != NULL) {
		if (ifc->mbps > 100)
			*scale = HaveWS | 3;
		else if (ifc->mbps > 10)
			*scale = HaveWS | 1;
		else
			*scale = HaveWS | 0;
		if (ifc->feat & NETF_TSO)
			*flags |= TSO;
	} else
		*scale = HaveWS | 0;

//// BW dependent.  don't know RTT.
//if (printx_on)
	//*scale = HaveWS | 8;

	return mtu;
}

void inittcpctl(struct conv *s, int mode)
{
	Tcpctl *tcb;
	Tcp4hdr *h4;
	Tcp6hdr *h6;
	int mss;

	tcb = (Tcpctl *) s->ptcl;

	memset(tcb, 0, sizeof(Tcpctl));

	tcb->ssthresh = UINT32_MAX;
	tcb->srtt = tcp_irtt << LOGAGAIN;
	tcb->mdev = 0;

	/* setup timers */
	tcb->timer.start = tcp_irtt / MSPTICK;
	tcb->timer.func = tcptimeout;
	tcb->timer.arg = s;
	tcb->rtt_timer.start = MAX_TIME;
	tcb->acktimer.start = TCP_ACK / MSPTICK;
	tcb->acktimer.func = tcpacktimer;
	tcb->acktimer.arg = s;
	tcb->katimer.start = DEF_KAT / MSPTICK;
	tcb->katimer.func = tcpkeepalive;
	tcb->katimer.arg = s;

	mss = DEF_MSS;

	/* create a prototype(pseudo) header */
	if (mode != TCP_LISTEN) {
		if (ipcmp(s->laddr, IPnoaddr) == 0)
			findlocalip(s->p->f, s->laddr, s->raddr);

		switch (s->ipversion) {
			case V4:
				h4 = &tcb->protohdr.tcp4hdr;
				memset(h4, 0, sizeof(*h4));
				h4->proto = IP_TCPPROTO;
				hnputs(h4->tcpsport, s->lport);
				hnputs(h4->tcpdport, s->rport);
				v6tov4(h4->tcpsrc, s->laddr);
				v6tov4(h4->tcpdst, s->raddr);
				break;
			case V6:
				h6 = &tcb->protohdr.tcp6hdr;
				memset(h6, 0, sizeof(*h6));
				h6->proto = IP_TCPPROTO;
				hnputs(h6->tcpsport, s->lport);
				hnputs(h6->tcpdport, s->rport);
				ipmove(h6->tcpsrc, s->laddr);
				ipmove(h6->tcpdst, s->raddr);
				mss = DEF_MSS6;
				break;
			default:
				panic("inittcpctl: version %d", s->ipversion);
		}
	}

	tcb->mss = mss;
	tcb->typical_mss = mss;
	/* TODO: is this always overwritten during the 3WHS? */
	tcb->cwind = tcb->typical_mss * CWIND_SCALE;

	/* default is no window scaling */
	tcb->window = QMAX;
	tcb->rcv.wnd = QMAX;
	tcb->rcv.scale = 0;
	tcb->snd.scale = 0;
	qsetlimit(s->rq, QMAX);
}

/*
 *  called with s qlocked
 */
void tcpstart(struct conv *s, int mode)
{
	Tcpctl *tcb;
	struct tcppriv *tpriv;
	char *kpname;

	tpriv = s->p->priv;

	if (tpriv->ackprocstarted == 0) {
		qlock(&tpriv->apl);
		if (tpriv->ackprocstarted == 0) {
			/* tcpackproc needs to free this if it ever exits */
			kpname = kmalloc(KNAMELEN, MEM_WAIT);
			snprintf(kpname, KNAMELEN, "#I%dtcpack", s->p->f->dev);
			ktask(kpname, tcpackproc, s->p);
			tpriv->ackprocstarted = 1;
		}
		qunlock(&tpriv->apl);
	}

	tcb = (Tcpctl *) s->ptcl;

	inittcpctl(s, mode);

	iphtadd(&tpriv->ht, s);
	switch (mode) {
		case TCP_LISTEN:
			tpriv->stats[PassiveOpens]++;
			tcb->flags |= CLONE;
			tcpsetstate(s, Listen);
			break;

		case TCP_CONNECT:
			tpriv->stats[ActiveOpens]++;
			tcb->flags |= ACTIVE;
			tcpsndsyn(s, tcb);
			tcpsetstate(s, Syn_sent);
			tcpoutput(s);
			break;
	}
}

static char *tcpflag(uint16_t flag)
{
	static char buf[128];

	snprintf(buf, sizeof(buf), "%d", flag >> 10);	/* Head len */
	if (flag & URG)
		snprintf(buf, sizeof(buf), "%s%s", buf, " URG");
	if (flag & ACK)
		snprintf(buf, sizeof(buf), "%s%s", buf, " ACK");
	if (flag & PSH)
		snprintf(buf, sizeof(buf), "%s%s", buf, " PSH");
	if (flag & RST)
		snprintf(buf, sizeof(buf), "%s%s", buf, " RST");
	if (flag & SYN)
		snprintf(buf, sizeof(buf), "%s%s", buf, " SYN");
	if (flag & FIN)
		snprintf(buf, sizeof(buf), "%s%s", buf, " FIN");

	return buf;
}

/* Helper, determine if we should send a TCP timestamp.  ts_val was the
 * timestamp from our distant end.  We'll also send a TS on SYN (no ACK). */
static bool tcp_seg_has_ts(Tcp *tcph)
{
	return tcph->ts_val || ((tcph->flags & SYN) && !(tcph->flags & ACK));
}

// XXX if we want to send a bunch of SACKs, we'll need to hang them off the tcph
// prob with rx too.
// careful of going beyond header size.  (40 byte max)
//
// 		will probably maintain a chain on the tcb of all SACKs that haven't been
// 			two directions for SACK.  the sacks we send and the ones we rx
// 		ACKED yet, and we're supposed to send them out each time
// 			"should be sent in all ACKS that do not ack the highest seq number
// 			in the rx q"
// 		limited to 3 sack blocks with TCP opts
// 			header size issue
// 			consider TS is added
// 		first block MUST contain the block triggering the ACK (most recently
// 		received)
// 			this will grow as we keep feeding it
// 		the blocks will change over time.  they might renege too.
// 			decide to change them in place?
// 		
// 		could just have an array, and never handle more than N of them
// 			throw away the old ones.  whatevs.  ?
// 			might get them in a different order (if the triggering packet added
// 			something out of order)
// 			- might depend if we're using the blocks as the 'SACK'd bits or not

/* Given a TCP header/segment and default header size (e.g. TCP4_HDRSIZE),
 * return the actual hdr_len and opt_pad */
static void compute_hdrlen_optpad(Tcp *tcph, uint16_t default_hdrlen,
                                  uint16_t *ret_hdrlen, uint16_t *ret_optpad)
{
	uint16_t hdrlen = default_hdrlen;
	uint16_t optpad = 0;

	if (tcph->flags & SYN) {
		if (tcph->mss)
			hdrlen += MSS_LENGTH;
		if (tcph->ws)
			hdrlen += WS_LENGTH;
		if (tcph->sack_ok)
			hdrlen += SACK_OK_LENGTH;
		// XXX support sending SACKs
	}
	if (tcp_seg_has_ts(tcph))
		hdrlen += TS_LENGTH;
	optpad = hdrlen & 3;
	if (optpad)
		optpad = 4 - optpad;
	hdrlen += optpad;
	*ret_hdrlen = hdrlen;
	*ret_optpad = optpad;
}

/* Writes the TCP options for tcph to opt. */
static void write_opts(Tcp *tcph, uint8_t *opt, uint16_t optpad)
{
	if (tcph->flags & SYN) {
		if (tcph->mss != 0) {
			*opt++ = MSSOPT;
			*opt++ = MSS_LENGTH;
			hnputs(opt, tcph->mss);
			opt += 2;
		}
		if (tcph->ws != 0) {
			*opt++ = WSOPT;
			*opt++ = WS_LENGTH;
			*opt++ = tcph->ws;
		}
		if (tcph->sack_ok) {
			*opt++ = SACK_OK_OPT;
			*opt++ = SACK_OK_LENGTH;
		}
		// XXX support sending SACKs
	}
	if (tcp_seg_has_ts(tcph)) {
		*opt++ = TS_OPT;
		*opt++ = TS_LENGTH;
		/* Setting TSval, our time */
		hnputl(opt, milliseconds());
		opt += 4;
		/* Setting TSecr, the time we last saw from them, stored in ts_val */
		hnputl(opt, tcph->ts_val);
		opt += 4;
	}
	while (optpad-- > 0)
		*opt++ = NOOPOPT;
}

/* Given a data block (or NULL) returns a block with enough header room that we
 * can send out.  block->wp is set to the beginning of the payload.  Returns
 * NULL on some sort of error. */
static struct block *alloc_or_pad_block(struct block *data,
                                        uint16_t total_hdr_size)
{
	if (data) {
		data = padblock(data, total_hdr_size);
		if (data == NULL)
			return NULL;
	} else {
		/* the 64 pad is to meet mintu's */
		data = block_alloc(total_hdr_size + 64, MEM_WAIT);
		if (data == NULL)
			return NULL;
		data->wp += total_hdr_size;
	}
	return data;
}

struct block *htontcp6(Tcp * tcph, struct block *data, Tcp6hdr * ph,
					   Tcpctl * tcb)
{
	int dlen = blocklen(data);
	Tcp6hdr *h;
	uint16_t csum;
	uint16_t hdrlen, optpad;

	compute_hdrlen_optpad(tcph, TCP6_HDRSIZE, &hdrlen, &optpad);

	data = alloc_or_pad_block(data, hdrlen + TCP6_PKT);
	if (data == NULL)
		return NULL;
	/* relative to the block start (bp->rp) */
	data->transport_header_end = hdrlen + TCP6_PKT;

	/* copy in pseudo ip header plus port numbers */
	h = (Tcp6hdr *) (data->rp);
	memmove(h, ph, TCP6_TCBPHDRSZ);

	/* compose pseudo tcp header, do cksum calculation */
	hnputl(h->vcf, hdrlen + dlen);
	h->ploadlen[0] = h->ploadlen[1] = h->proto = 0;
	h->ttl = ph->proto;

	/* copy in variable bits */
	hnputl(h->tcpseq, tcph->seq);
	hnputl(h->tcpack, tcph->ack);
	hnputs(h->tcpflag, (hdrlen << 10) | tcph->flags);
	hnputs(h->tcpwin, tcph->wnd >> (tcb != NULL ? tcb->snd.scale : 0));
	hnputs(h->tcpurg, tcph->urg);

	write_opts(tcph, h->tcpopt, optpad);

	if (tcb != NULL && tcb->nochecksum) {
		h->tcpcksum[0] = h->tcpcksum[1] = 0;
	} else {
		csum = ptclcsum(data, TCP6_IPLEN, hdrlen + dlen + TCP6_PHDRSIZE);
		hnputs(h->tcpcksum, csum);
	}

	/* move from pseudo header back to normal ip header */
	memset(h->vcf, 0, 4);
	h->vcf[0] = IP_VER6;
	hnputs(h->ploadlen, hdrlen + dlen);
	h->proto = ph->proto;

	return data;
}

struct block *htontcp4(Tcp * tcph, struct block *data, Tcp4hdr * ph,
					   Tcpctl * tcb)
{
	int dlen = blocklen(data);
	Tcp4hdr *h;
	uint16_t csum;
	uint16_t hdrlen, optpad;

	compute_hdrlen_optpad(tcph, TCP4_HDRSIZE, &hdrlen, &optpad);

	data = alloc_or_pad_block(data, hdrlen + TCP4_PKT);
	if (data == NULL)
		return NULL;
	/* relative to the block start (bp->rp) */
	data->transport_header_end = hdrlen + TCP4_PKT;

	/* copy in pseudo ip header plus port numbers */
	h = (Tcp4hdr *) (data->rp);
	memmove(h, ph, TCP4_TCBPHDRSZ);

	/* copy in variable bits */
	hnputs(h->tcplen, hdrlen + dlen);
	hnputl(h->tcpseq, tcph->seq);
	hnputl(h->tcpack, tcph->ack);
	hnputs(h->tcpflag, (hdrlen << 10) | tcph->flags);
	hnputs(h->tcpwin, tcph->wnd >> (tcb != NULL ? tcb->snd.scale : 0));
	hnputs(h->tcpurg, tcph->urg);

	write_opts(tcph, h->tcpopt, optpad);

	if (tcb != NULL && tcb->nochecksum) {
		h->tcpcksum[0] = h->tcpcksum[1] = 0;
	} else {
		csum = ~ptclcsum(data, TCP4_IPLEN, TCP4_PHDRSIZE);
		hnputs(h->tcpcksum, csum);
		data->checksum_start = TCP4_IPLEN + TCP4_PHDRSIZE;
		data->checksum_offset = ph->tcpcksum - ph->tcpsport;
		data->flag |= Btcpck;
	}

	return data;
}

static void parse_inbound_sacks(Tcp *tcph, uint8_t *opt, uint16_t optlen)
{
	uint8_t nr_sacks;
	uint32_t left, right;

	nr_sacks = (optlen - 2) / 8;
	if (nr_sacks > MAX_NR_SACKS_PER_PACKET)
		return;
	opt += 2;
	for (int i = 0; i < nr_sacks; i++, opt += 8) {
		left = nhgetl(opt);
		right = nhgetl(opt + 4);
		if (seq_ge(left, right)) {
			/* bad / malicious SACK.  Skip it, and adjust. */
			nr_sacks--;
			i--;	/* stay on this array element next loop */
			continue;
		}
		tcph->sacks[i].left = left;
		tcph->sacks[i].right = right;
	}
	tcph->nr_sacks = nr_sacks;
}

static void parse_inbound_opts(Tcp *tcph, uint8_t *opt, uint16_t optsize)
{
	uint16_t optlen;

	while (optsize > 0 && *opt != EOLOPT) {
		if (*opt == NOOPOPT) {
			optsize--;
			opt++;
			continue;
		}
		optlen = opt[1];
		if (optlen < 2 || optlen > optsize)
			break;
		switch (*opt) {
			case MSSOPT:
				if (optlen == MSS_LENGTH)
					tcph->mss = nhgets(opt + 2);
				break;
			case WSOPT:
				if (optlen == WS_LENGTH && *(opt + 2) <= MAX_WS_VALUE)
					tcph->ws = HaveWS | *(opt + 2);
				break;
			case SACK_OK_OPT:
				if (optlen == SACK_OK_LENGTH)
					tcph->sack_ok = TRUE;
				break;
			case SACK_OPT:
				parse_inbound_sacks(tcph, opt, optlen);
				break;
			case TS_OPT:
				if (optlen == TS_LENGTH) {
					tcph->ts_val = nhgetl(opt + 2);
					tcph->ts_ecr = nhgetl(opt + 6);
				}
				break;
		}
		optsize -= optlen;
		opt += optlen;
	}
}

/* Helper, clears the opts.  We'll later set them with e.g. parse_inbound_opts,
 * set them manually, or something else. */
static void clear_tcph_opts(Tcp *tcph)
{
	tcph->mss = 0;
	tcph->ws = 0;
	tcph->sack_ok = FALSE;
	tcph->nr_sacks = 0;
	tcph->ts_val = 0;
	tcph->ts_ecr = 0;
}

int ntohtcp6(Tcp * tcph, struct block **bpp)
{
	Tcp6hdr *h;
	uint16_t hdrlen;

	*bpp = pullupblock(*bpp, TCP6_PKT + TCP6_HDRSIZE);
	if (*bpp == NULL)
		return -1;

	h = (Tcp6hdr *) ((*bpp)->rp);
	tcph->source = nhgets(h->tcpsport);
	tcph->dest = nhgets(h->tcpdport);
	tcph->seq = nhgetl(h->tcpseq);
	tcph->ack = nhgetl(h->tcpack);
	hdrlen = (h->tcpflag[0] >> 2) & ~3;
	if (hdrlen < TCP6_HDRSIZE) {
		freeblist(*bpp);
		return -1;
	}

	tcph->flags = h->tcpflag[1];
	tcph->wnd = nhgets(h->tcpwin);
	tcph->urg = nhgets(h->tcpurg);
	clear_tcph_opts(tcph);
	tcph->len = nhgets(h->ploadlen) - hdrlen;

	*bpp = pullupblock(*bpp, hdrlen + TCP6_PKT);
	if (*bpp == NULL)
		return -1;
	parse_inbound_opts(tcph, h->tcpopt, hdrlen - TCP6_HDRSIZE);
	return hdrlen;
}

int ntohtcp4(Tcp * tcph, struct block **bpp)
{
	Tcp4hdr *h;
	uint16_t hdrlen;

	*bpp = pullupblock(*bpp, TCP4_PKT + TCP4_HDRSIZE);
	if (*bpp == NULL)
		return -1;

	h = (Tcp4hdr *) ((*bpp)->rp);
	tcph->source = nhgets(h->tcpsport);
	tcph->dest = nhgets(h->tcpdport);
	tcph->seq = nhgetl(h->tcpseq);
	tcph->ack = nhgetl(h->tcpack);

	hdrlen = (h->tcpflag[0] >> 2) & ~3;
	if (hdrlen < TCP4_HDRSIZE) {
		freeblist(*bpp);
		return -1;
	}

	tcph->flags = h->tcpflag[1];
	tcph->wnd = nhgets(h->tcpwin);
	tcph->urg = nhgets(h->tcpurg);
	clear_tcph_opts(tcph);
	tcph->len = nhgets(h->length) - (hdrlen + TCP4_PKT);

	*bpp = pullupblock(*bpp, hdrlen + TCP4_PKT);
	if (*bpp == NULL)
		return -1;
	parse_inbound_opts(tcph, h->tcpopt, hdrlen - TCP4_HDRSIZE);
	return hdrlen;
}

/*
 *  For outgiing calls, generate an initial sequence
 *  number and put a SYN on the send queue
 */
void tcpsndsyn(struct conv *s, Tcpctl * tcb)
{
	urandom_read(&tcb->iss, sizeof(tcb->iss));
	tcb->rttseq = tcb->iss;
	tcb->snd.wl2 = tcb->iss;
	tcb->snd.una = tcb->iss;
	tcb->snd.ptr = tcb->rttseq;
	tcb->snd.nxt = tcb->rttseq;
	tcb->flgcnt++;
	tcb->flags |= FORCE;
	tcb->sndsyntime = NOW;

	/* set desired mss and scale */
	tcb->mss = tcpmtu(s->p, s->laddr, s->ipversion, &tcb->scale,
			  &tcb->flags);
}

void
sndrst(struct Proto *tcp, uint8_t * source, uint8_t * dest,
	   uint16_t length, Tcp * seg, uint8_t version, char *reason)
{
	struct block *hbp;
	uint8_t rflags;
	struct tcppriv *tpriv;
	Tcp4hdr ph4;
	Tcp6hdr ph6;

	netlog(tcp->f, Logtcp, "sndrst: %s\n", reason);

	tpriv = tcp->priv;

	if (seg->flags & RST)
		return;

	/* make pseudo header */
	switch (version) {
		case V4:
			memset(&ph4, 0, sizeof(ph4));
			ph4.vihl = IP_VER4;
			v6tov4(ph4.tcpsrc, dest);
			v6tov4(ph4.tcpdst, source);
			ph4.proto = IP_TCPPROTO;
			hnputs(ph4.tcplen, TCP4_HDRSIZE);
			hnputs(ph4.tcpsport, seg->dest);
			hnputs(ph4.tcpdport, seg->source);
			break;
		case V6:
			memset(&ph6, 0, sizeof(ph6));
			ph6.vcf[0] = IP_VER6;
			ipmove(ph6.tcpsrc, dest);
			ipmove(ph6.tcpdst, source);
			ph6.proto = IP_TCPPROTO;
			hnputs(ph6.ploadlen, TCP6_HDRSIZE);
			hnputs(ph6.tcpsport, seg->dest);
			hnputs(ph6.tcpdport, seg->source);
			break;
		default:
			panic("sndrst: version %d", version);
	}

	tpriv->stats[OutRsts]++;
	rflags = RST;

	/* convince the other end that this reset is in band */
	if (seg->flags & ACK) {
		seg->seq = seg->ack;
		seg->ack = 0;
	} else {
		rflags |= ACK;
		seg->ack = seg->seq;
		seg->seq = 0;
		if (seg->flags & SYN)
			seg->ack++;
		seg->ack += length;
		if (seg->flags & FIN)
			seg->ack++;
	}
	seg->flags = rflags;
	seg->wnd = 0;
	seg->urg = 0;
	// XXX syn clearables/ignorables
	seg->mss = 0;
	seg->ws = 0;
	seg->sack_ok = FALSE;
	seg->nr_sacks = 0;
	/* seg->ts_val is already set with their timestamp */
	switch (version) {
		case V4:
			hbp = htontcp4(seg, NULL, &ph4, NULL);
			if (hbp == NULL)
				return;
			ipoput4(tcp->f, hbp, 0, MAXTTL, DFLTTOS, NULL);
			break;
		case V6:
			hbp = htontcp6(seg, NULL, &ph6, NULL);
			if (hbp == NULL)
				return;
			ipoput6(tcp->f, hbp, 0, MAXTTL, DFLTTOS, NULL);
			break;
		default:
			panic("sndrst2: version %d", version);
	}
}

/*
 *  send a reset to the remote side and close the conversation
 *  called with s qlocked
 */
static void tcphangup(struct conv *s)
{
	ERRSTACK(1);
	Tcp seg;
	Tcpctl *tcb;
	struct block *hbp;

	tcb = (Tcpctl *) s->ptcl;
	if (ipcmp(s->raddr, IPnoaddr)) {
		/* discard error style, poperror regardless */
		if (!waserror()) {
			seg.flags = RST | ACK;
			seg.ack = tcb->rcv.nxt;
			tcb->last_ack_sent = seg.ack;
			tcb->rcv.una = 0;
			seg.seq = tcb->snd.ptr;
			seg.wnd = 0;
			seg.urg = 0;
			// XXX syn clearables/ignorables
			seg.mss = 0;
			seg.ws = 0;
			seg.sack_ok = FALSE;
			seg.nr_sacks = 0;
			seg.ts_val = tcb->ts_recent;
			switch (s->ipversion) {
				case V4:
					tcb->protohdr.tcp4hdr.vihl = IP_VER4;
					hbp = htontcp4(&seg, NULL, &tcb->protohdr.tcp4hdr, tcb);
					ipoput4(s->p->f, hbp, 0, s->ttl, s->tos, s);
					break;
				case V6:
					tcb->protohdr.tcp6hdr.vcf[0] = IP_VER6;
					hbp = htontcp6(&seg, NULL, &tcb->protohdr.tcp6hdr, tcb);
					ipoput6(s->p->f, hbp, 0, s->ttl, s->tos, s);
					break;
				default:
					panic("tcphangup: version %d", s->ipversion);
			}
		}
		poperror();
	}
	localclose(s, NULL);
}

/*
 *  (re)send a SYN ACK
 */
int sndsynack(struct Proto *tcp, Limbo * lp)
{
	struct block *hbp;
	Tcp4hdr ph4;
	Tcp6hdr ph6;
	Tcp seg;
	int scale;
	uint8_t flag = 0;

	/* make pseudo header */
	switch (lp->version) {
		case V4:
			memset(&ph4, 0, sizeof(ph4));
			ph4.vihl = IP_VER4;
			v6tov4(ph4.tcpsrc, lp->laddr);
			v6tov4(ph4.tcpdst, lp->raddr);
			ph4.proto = IP_TCPPROTO;
			hnputs(ph4.tcplen, TCP4_HDRSIZE);
			hnputs(ph4.tcpsport, lp->lport);
			hnputs(ph4.tcpdport, lp->rport);
			break;
		case V6:
			memset(&ph6, 0, sizeof(ph6));
			ph6.vcf[0] = IP_VER6;
			ipmove(ph6.tcpsrc, lp->laddr);
			ipmove(ph6.tcpdst, lp->raddr);
			ph6.proto = IP_TCPPROTO;
			hnputs(ph6.ploadlen, TCP6_HDRSIZE);
			hnputs(ph6.tcpsport, lp->lport);
			hnputs(ph6.tcpdport, lp->rport);
			break;
		default:
			panic("sndrst: version %d", lp->version);
	}

	seg.seq = lp->iss;
	seg.ack = lp->irs + 1;
	seg.flags = SYN | ACK;
	seg.urg = 0;
	seg.mss = tcpmtu(tcp, lp->laddr, lp->version, &scale, &flag);
	seg.wnd = QMAX;	// XXX probably wrong.  we should grow this etc
	seg.ts_val = lp->ts_val;
	seg.nr_sacks = 0;

	/* if the other side set scale, we should too */
	if (lp->rcvscale) {
		seg.ws = scale;
		lp->sndscale = scale;
	} else {
		seg.ws = 0;
		lp->sndscale = 0;
	}
	if (SACK_SUPPORTED)
		seg.sack_ok = lp->sack_ok;
	else
		seg.sack_ok = FALSE;

	switch (lp->version) {
		case V4:
			hbp = htontcp4(&seg, NULL, &ph4, NULL);
			if (hbp == NULL)
				return -1;
			ipoput4(tcp->f, hbp, 0, MAXTTL, DFLTTOS, NULL);
			break;
		case V6:
			hbp = htontcp6(&seg, NULL, &ph6, NULL);
			if (hbp == NULL)
				return -1;
			ipoput6(tcp->f, hbp, 0, MAXTTL, DFLTTOS, NULL);
			break;
		default:
			panic("sndsnack: version %d", lp->version);
	}
	lp->lastsend = NOW;
	return 0;
}

#define hashipa(a, p) ( ( (a)[IPaddrlen-2] + (a)[IPaddrlen-1] + p )&LHTMASK )

/*
 *  put a call into limbo and respond with a SYN ACK
 *
 *  called with proto locked
 */
static void
limbo(struct conv *s, uint8_t * source, uint8_t * dest, Tcp * seg, int version)
{
	Limbo *lp, **l;
	struct tcppriv *tpriv;
	int h;

	tpriv = s->p->priv;
	h = hashipa(source, seg->source);

	for (l = &tpriv->lht[h]; *l != NULL; l = &lp->next) {
		lp = *l;
		if (lp->lport != seg->dest || lp->rport != seg->source
			|| lp->version != version)
			continue;
		if (ipcmp(lp->raddr, source) != 0)
			continue;
		if (ipcmp(lp->laddr, dest) != 0)
			continue;

		/* each new SYN restarts the retransmits */
		lp->irs = seg->seq;
		break;
	}
	lp = *l;
	if (lp == NULL) {
		if (tpriv->nlimbo >= Maxlimbo && tpriv->lht[h]) {
			lp = tpriv->lht[h];
			tpriv->lht[h] = lp->next;
			lp->next = NULL;
		} else {
			lp = kzmalloc(sizeof(*lp), 0);
			if (lp == NULL)
				return;
			tpriv->nlimbo++;
		}
		*l = lp;
		lp->version = version;
		ipmove(lp->laddr, dest);
		ipmove(lp->raddr, source);
		lp->lport = seg->dest;
		lp->rport = seg->source;
		lp->mss = seg->mss;
		lp->rcvscale = seg->ws;
		lp->sack_ok = seg->sack_ok;
		lp->irs = seg->seq;
		lp->ts_val = seg->ts_val;
		urandom_read(&lp->iss, sizeof(lp->iss));
	}

	if (sndsynack(s->p, lp) < 0) {
		*l = lp->next;
		tpriv->nlimbo--;
		kfree(lp);
	}
}

/*
 *  resend SYN ACK's once every SYNACK_RXTIMER ms.
 */
static void limborexmit(struct Proto *tcp)
{
	struct tcppriv *tpriv;
	Limbo **l, *lp;
	int h;
	int seen;
	uint64_t now;

	tpriv = tcp->priv;

	if (!canqlock(&tcp->qlock))
		return;
	seen = 0;
	now = NOW;
	for (h = 0; h < NLHT && seen < tpriv->nlimbo; h++) {
		for (l = &tpriv->lht[h]; *l != NULL && seen < tpriv->nlimbo;) {
			lp = *l;
			seen++;
			if (now - lp->lastsend < (lp->rexmits + 1) * SYNACK_RXTIMER)
				continue;

			/* time it out after 1 second */
			if (++(lp->rexmits) > 5) {
				tpriv->nlimbo--;
				*l = lp->next;
				kfree(lp);
				continue;
			}

			/* if we're being attacked, don't bother resending SYN ACK's */
			if (tpriv->nlimbo > 100)
				continue;

			if (sndsynack(tcp, lp) < 0) {
				tpriv->nlimbo--;
				*l = lp->next;
				kfree(lp);
				continue;
			}

			l = &lp->next;
		}
	}
	qunlock(&tcp->qlock);
}

/*
 *  lookup call in limbo.  if found, throw it out.
 *
 *  called with proto locked
 */
static void
limborst(struct conv *s, Tcp * segp, uint8_t * src, uint8_t * dst,
		 uint8_t version)
{
	Limbo *lp, **l;
	int h;
	struct tcppriv *tpriv;

	tpriv = s->p->priv;

	/* find a call in limbo */
	h = hashipa(src, segp->source);
	for (l = &tpriv->lht[h]; *l != NULL; l = &lp->next) {
		lp = *l;
		if (lp->lport != segp->dest || lp->rport != segp->source
			|| lp->version != version)
			continue;
		if (ipcmp(lp->laddr, dst) != 0)
			continue;
		if (ipcmp(lp->raddr, src) != 0)
			continue;

		/* RST can only follow the SYN */
		if (segp->seq == lp->irs + 1) {
			tpriv->nlimbo--;
			*l = lp->next;
			kfree(lp);
		}
		break;
	}
}

/* The advertised MSS (e.g. 1460) includes any per-packet TCP options, such as
 * TCP timestamps.  A given packet will contain mss bytes, but only typical_mss
 * bytes of *data*.  If we know we'll use those options, we should adjust our
 * typical_mss, which will affect the cwnd. */
static void adjust_typical_mss_for_opts(Tcp *tcph, Tcpctl *tcb)
{
	uint16_t opt_size = 0;

	if (tcph->ts_val)
		opt_size += TS_LENGTH;
	opt_size = ROUNDUP(opt_size, 4);
	tcb->typical_mss -= opt_size;
}

/*
 *  come here when we finally get an ACK to our SYN-ACK.
 *  lookup call in limbo.  if found, create a new conversation
 *
 *  called with proto locked
 */
static struct conv *tcpincoming(struct conv *s, Tcp * segp, uint8_t * src,
								uint8_t * dst, uint8_t version)
{
	struct conv *new;
	Tcpctl *tcb;
	struct tcppriv *tpriv;
	Tcp4hdr *h4;
	Tcp6hdr *h6;
	Limbo *lp, **l;
	int h;

	/* unless it's just an ack, it can't be someone coming out of limbo */
	if ((segp->flags & SYN) || (segp->flags & ACK) == 0)
		return NULL;

	tpriv = s->p->priv;

	/* find a call in limbo */
	h = hashipa(src, segp->source);
	for (l = &tpriv->lht[h]; (lp = *l) != NULL; l = &lp->next) {
		netlog(s->p->f, Logtcp,
			   "tcpincoming s %I!%d/%I!%d d %I!%d/%I!%d v %d/%d\n", src,
			   segp->source, lp->raddr, lp->rport, dst, segp->dest, lp->laddr,
			   lp->lport, version, lp->version);

		if (lp->lport != segp->dest || lp->rport != segp->source
			|| lp->version != version)
			continue;
		if (ipcmp(lp->laddr, dst) != 0)
			continue;
		if (ipcmp(lp->raddr, src) != 0)
			continue;

		/* we're assuming no data with the initial SYN */
		if (segp->seq != lp->irs + 1 || segp->ack != lp->iss + 1) {
			netlog(s->p->f, Logtcp, "tcpincoming s 0x%lx/0x%lx a 0x%lx 0x%lx\n",
				   segp->seq, lp->irs + 1, segp->ack, lp->iss + 1);
			lp = NULL;
		} else {
			tpriv->nlimbo--;
			*l = lp->next;
		}
		break;
	}
	if (lp == NULL)
		return NULL;

	new = Fsnewcall(s, src, segp->source, dst, segp->dest, version);
	if (new == NULL)
		return NULL;

	memmove(new->ptcl, s->ptcl, sizeof(Tcpctl));
	tcb = (Tcpctl *) new->ptcl;
	tcb->flags &= ~CLONE;
	tcb->timer.arg = new;
	tcb->timer.state = TcptimerOFF;
	tcb->acktimer.arg = new;
	tcb->acktimer.state = TcptimerOFF;
	tcb->katimer.arg = new;
	tcb->katimer.state = TcptimerOFF;
	tcb->rtt_timer.arg = new;
	tcb->rtt_timer.state = TcptimerOFF;

	tcb->irs = lp->irs;
	tcb->rcv.nxt = tcb->irs + 1;
	tcb->rcv.urg = tcb->rcv.nxt;

	tcb->iss = lp->iss;
	tcb->rttseq = tcb->iss;
	tcb->snd.wl2 = tcb->iss;
	tcb->snd.una = tcb->iss + 1;
	tcb->snd.ptr = tcb->iss + 1;
	tcb->snd.nxt = tcb->iss + 1;
	tcb->flgcnt = 0;
	tcb->flags |= SYNACK;

	/* our sending max segment size cannot be bigger than what he asked for */
	if (lp->mss != 0 && lp->mss < tcb->mss) {
		tcb->mss = lp->mss;
		tcb->typical_mss = tcb->mss;
	}
	adjust_typical_mss_for_opts(segp, tcb);

	/* Here's where we record the previously-decided header options.  They were
	 * actually decided on when we agreed to them in the SYNACK we sent.  We
	 * didn't create an actual TCB until now, so we can copy those decisions out
	 * of the limbo tracker and into the TCB. */
	tcb->sack_ok = lp->sack_ok;
	/* window scaling */
	tcpsetscale(new, tcb, lp->rcvscale, lp->sndscale);

	tcb->snd.wnd = segp->wnd;
	tcb->cwind = tcb->typical_mss * CWIND_SCALE;

	/* set initial round trip time */
	tcb->sndsyntime = lp->lastsend + lp->rexmits * SYNACK_RXTIMER;
	tcpsynackrtt(new);

	kfree(lp);

	/* set up proto header */
	switch (version) {
		case V4:
			h4 = &tcb->protohdr.tcp4hdr;
			memset(h4, 0, sizeof(*h4));
			h4->proto = IP_TCPPROTO;
			hnputs(h4->tcpsport, new->lport);
			hnputs(h4->tcpdport, new->rport);
			v6tov4(h4->tcpsrc, dst);
			v6tov4(h4->tcpdst, src);
			break;
		case V6:
			h6 = &tcb->protohdr.tcp6hdr;
			memset(h6, 0, sizeof(*h6));
			h6->proto = IP_TCPPROTO;
			hnputs(h6->tcpsport, new->lport);
			hnputs(h6->tcpdport, new->rport);
			ipmove(h6->tcpsrc, dst);
			ipmove(h6->tcpdst, src);
			break;
		default:
			panic("tcpincoming: version %d", new->ipversion);
	}

	tcpsetstate(new, Established);

	iphtadd(&tpriv->ht, new);

	return new;
}

int seq_within(uint32_t x, uint32_t low, uint32_t high)
{
	if (low <= high) {
		if (low <= x && x <= high)
			return 1;
	} else {
		if (x >= low || x <= high)
			return 1;
	}
	return 0;
}

int seq_lt(uint32_t x, uint32_t y)
{
	return (int)(x - y) < 0;
}

int seq_le(uint32_t x, uint32_t y)
{
	return (int)(x - y) <= 0;
}

int seq_gt(uint32_t x, uint32_t y)
{
	return (int)(x - y) > 0;
}

int seq_ge(uint32_t x, uint32_t y)
{
	return (int)(x - y) >= 0;
}

uint32_t seq_abs_diff(uint32_t x, uint32_t y)
{
	if (x > y)
		return x - y;
	else
		return y - x;
}

/*
 *  use the time between the first SYN and it's ack as the
 *  initial round trip time
 */
void tcpsynackrtt(struct conv *s)
{
	Tcpctl *tcb;
	uint64_t delta;
	struct tcppriv *tpriv;

	tcb = (Tcpctl *) s->ptcl;
	tpriv = s->p->priv;

	delta = NOW - tcb->sndsyntime;
	tcb->srtt = delta << LOGAGAIN;
	tcb->mdev = delta << LOGDGAIN;

	/* halt round trip timer */
	tcphalt(tpriv, &tcb->rtt_timer);
}

/* For LFNs (long/fat), our default tx queue doesn't hold enough data, and TCP
 * blocks on the application - even if the app already has the data ready to go.
 * We need to hold the sent, unacked data (1x cwnd), plus all the data we might
 * send next RTT (1x cwnd).  Note this is called after cwnd was expanded. */
static void adjust_tx_qio_limit(struct conv *s)
{
	Tcpctl *tcb = (Tcpctl *) s->ptcl;
	size_t ideal_limit = tcb->cwind * 2;

	/* This is called for every ACK, and it's not entirely free to update the
	 * limit (locks, CVs, taps).  Updating in chunks of mss seems reasonable.
	 * During SS, we'll update this on most ACKs (given each ACK increased the
	 * cwind by > MSS).
	 *
	 * We also don't want a lot of tiny blocks from the user, but the way qio
	 * works, you can put in as much as you want (Maxatomic) and then get
	 * flow-controlled. */
	if (qgetlimit(s->wq) + tcb->typical_mss < ideal_limit)
		qsetlimit(s->wq, ideal_limit);
	/* TODO: we could shrink the qio limit too, if we had a better idea what the
	 * actual threshold was.  We want the limit to be the 'stable' cwnd * 2. */
}

static bool is_dup_ack(Tcp *seg, Tcpctl *tcb)
{
	/* this is a pure ack w/o window update */
	return (seg->ack == tcb->snd.una) &&
	       (tcb->snd.una != tcb->snd.nxt) &&
	       (seg->len == 0) &&
	       (seg->wnd == tcb->snd.wnd);
}

static bool has_sacks(Tcp *seg)
{
	return seg->nr_sacks > 0;
}

static void update_or_insert_sack(Tcpctl *tcb, struct sack_block *seg_sack)
{
	struct sack_block *tcb_sack = &tcb->snd.sacks[0];

	for (int i = 0; i < tcb->snd.nr_sacks; i++, tcb_sack++) {

		/* This won't update if they changed the left mark, but that'd be a
		 * bug on their part.  We can ignore it.  We also won't bother with
		 * merging sacks. */
		if (tcb_sack->left == seg_sack->left) {
			tcb->snd.sack_amt += seq_abs_diff(tcb_sack->right, seg_sack->right);
			tcb_sack->right = seg_sack->right;
			return;
		}

		/* Out of room - need to always maintain the rightmost sack */
		if ((i == MAX_NR_TRACKED_SACKS - 1)) {
			if (seq_gt(seg_sack->right, tcb_sack->right)) {
				tcb->snd.sack_amt += seq_abs_diff(tcb_sack->left,
				                                  tcb_sack->right);
				tcb->snd.sack_amt += seq_abs_diff(seg_sack->left,
				                                  seg_sack->right);
				tcb_sack->left = seg_sack->left;
				tcb_sack->right = seg_sack->right;
			}
			return;
		}

		if (seq_gt(tcb_sack->left, seg_sack->left)) {
			/* At this point, we found our slot, and it's not the max slot. */
			memmove(&tcb->snd.sacks[i + 1], &tcb->snd.sacks[i],
			        sizeof(struct sack_block) * (tcb->snd.nr_sacks - i));
			/* Fill in the slot below */
			break;
		}
	}
	/* Either via break or by running through the loop, tcb_sack points to our
	 * slot */
	tcb->snd.sack_amt += seq_abs_diff(seg_sack->left,
	                                  seg_sack->right);
	tcb_sack->left = seg_sack->left;
	tcb_sack->right = seg_sack->right;
	tcb->snd.nr_sacks++;
}

/* Given the packet seg, track the sacks in TCB.  There are a few things: if seg
 * acks new data, some sacks might no longer be needed.  Some sacks might grow,
 * and we might add new sacks.
 *
 * Note that we keep sacks that are below sack_rxt (and above
 * seg.ack/tcb->snd.una.  We'll need those for the in_flight est.
 *
 * When we run out of room, we can throw away any sack above sack_rxt - we'll
 * just retrans that block later, but it won't mess up our in_flight
 * measurement.  The uppermost sack is always above sack_rxt (unless it has been
 * acked fully), so we can safely replace it.  We'll always maintain the
 * uppermost sack. */
static void update_sacks(Tcpctl *tcb, Tcp *seg)
{
	int prune = 0;
	struct sack_block *tcb_sack;

	for (int i = 0; i < tcb->snd.nr_sacks; i++) {
		tcb_sack = &tcb->snd.sacks[i];
		if (seq_lt(seg->ack, tcb_sack->left))
			break;
		prune++;
		tcb->snd.sack_amt -= seq_abs_diff(tcb_sack->right, tcb_sack->left);
	}
	if (prune) {
		memmove(tcb->snd.sacks, tcb->snd.sacks + prune,
		        sizeof(struct sack_block) * prune);
		tcb->snd.nr_sacks -= prune;
	}
	for (int i = 0; i < seg->nr_sacks; i++) {
		/* old sacks */
		if (seq_lt(seg->sacks[i].left, seg->ack))
			continue;
		/* new sack for something we already retransed, drop it. */
		if (seq_lt(seg->sacks[i].left, tcb->snd.sack_rxt))
			continue;
		/* buggy sack: out of range */
		if (seq_gt(seg->sacks[i].right, tcb->snd.nxt))
			continue;
		update_or_insert_sack(tcb, &seg->sacks[i]);
	}


	// XXX REMOVE ME
	static int x = 0;
	if (has_sacks(seg)) {
		if (x++ < 10) {
			trace_printk("POST PROCESSED SACKS: %d\n", tcb->snd.nr_sacks);
			for (int i = 0; i < tcb->snd.nr_sacks; i++) {
				tcb_sack = &tcb->snd.sacks[i];
				trace_printk("\t%d: [%u, %u)\n", i, tcb_sack->left, tcb_sack->right);
			}
		}
	}
}

/* This is a little bit of an under estimate, since we assume a packet is lost
 * once we have any sacks above it.  Overall, it's at most 2 * MSS of an
 * overestimate. */
static void set_pipe(Tcpctl *tcb)
{
	struct sack_block *tcb_sack;
	uint32_t in_flight = 0;
	uint32_t from;

	/* Everything to the right of the unsacked */
	if (tcb->snd.nr_sacks) {
		tcb_sack = &tcb->snd.sacks[tcb->snd.nr_sacks - 1];
		in_flight += seq_abs_diff(tcb_sack->right, tcb->snd.nxt);
	}

	/* Everything retransed (from una to sack_rxt, minus sacked regions.  Note
	 * we only retrans at most the last sack's left edge. */
	from = tcb->snd.una;
	for (int i = 0; i < tcb->snd.nr_sacks; i++) {
		tcb_sack = &tcb->snd.sacks[i];
		if (seq_ge(tcb_sack->left, tcb->snd.sack_rxt))
			break;
		in_flight += seq_abs_diff(from, tcb_sack->left);
		from = tcb_sack->right;
	}
	in_flight += seq_abs_diff(from, tcb->snd.sack_rxt);

	tcb->snd.pipe_bytes = in_flight;
}

static void reset_recovery(Tcpctl *tcb)
{
	tcb->snd.loss_hint = 0;
	tcb->snd.recovery = 0;

	tcb->snd.recovery_pt = 0;
	tcb->snd.sack_rxt = 0;

	/* Keep the sacks - we'll drop them over time */
		// XXX though we should drop them at some point.  how many recoveries?
	//tcb->snd.nr_sacks = 0;
}

// XXX SACK and FAST are the same now, might not matter
/* For sack retrans, if they sent an ACK with no sacks, we're done. */
static bool sack_recovery_done(Tcpctl *tcb, Tcp *seg)
{
	return (tcb->snd.recovery == SACK_RETRANS_RECOVERY) &&
	        seq_ge(seg->ack, tcb->snd.recovery_pt);
}
	
/* For fast retrans, if they've acked beyond where we started fast retrans
 * (recovery_pt), we're done */
static bool fast_rx_recovery_done(Tcpctl *tcb, Tcp *seg)
{
	return (tcb->snd.recovery == FAST_RETRANS_RECOVERY) &&
	        seq_ge(seg->ack, tcb->snd.recovery_pt);
}
	
void update(struct conv *s, Tcp * seg)
{
	int rtt, delta;
	Tcpctl *tcb;
	uint32_t acked;
	uint32_t expand;
	struct tcppriv *tpriv;

	tpriv = s->p->priv;
	tcb = (Tcpctl *) s->ptcl;

	/* if everything has been acked, force output(?) */
	if (seq_gt(seg->ack, tcb->snd.nxt)) {
		tcb->flags |= FORCE;
		return;
	}

	update_sacks(tcb, seg);

	/* We treat either a dupack or the presence of SACKs as a hint that there is
	 * a loss.  The RFCs suggest three dupacks before treating it as a loss (the
	 * alternative is reordered packets).  We'll treat three SACKs the same way,
	 * and enter some form of fast loss recovery. */
	if (is_dup_ack(seg, tcb) || has_sacks(seg)) {
		tcb->snd.loss_hint++;
		netlog(s->p->f, Logtcprxmt,
		       "loss_hint %lu ack %lu sndwnd %d advwin %d\n",
		       tcb->snd.loss_hint, seg->ack, tcb->snd.wnd, seg->wnd);
		if (tcb->snd.loss_hint == TCPREXMTTHRESH) {
			tcp_loss_event(tcb);
			if (has_sacks(seg)) {
				tcb->snd.recovery = SACK_RETRANS_RECOVERY;
				/* Start retrans from the current ACK point.  Note ack >= una */
				tcb->snd.sack_rxt = seg->ack;
				tcb->snd.recovery_pt = tcb->snd.nxt;
			} else {
				/* fast retrans */
				tcb->snd.recovery = FAST_RETRANS_RECOVERY;
				tcb->snd.recovery_pt = tcb->snd.nxt;
				netlog(s->p->f, Logtcprxmt, "fast rxt %lu, nxt %lu\n",
				       tcb->snd.una, tcb->snd.nxt);
				tcprxmit(s);
			}
		}
	}
	if (tcb->snd.recovery == SACK_RETRANS_RECOVERY) {
		if (seq_gt(seg->ack, tcb->snd.sack_rxt))
			tcb->snd.sack_rxt = seg->ack;
		// XXX
		//consider resetting sack_rxt every so often
		//			cover a double loss, which i get on occasion
		//			though you need to wait a RTT, or o/w have proof they should
		//			have had it (like maybe some sack growth?)

		set_pipe(tcb);
	}

	/*
	 *  update window
	 */
	// XXX is wl2 useless?  ack always moves forward (within seq_ space).  this
	// says if ack has moved forward at all, update window.  or if window
	// increased.  so we can only shrink snd.wnd on a forward ack?  wtf.
	if (seq_gt(seg->ack, tcb->snd.wl2)
		|| (tcb->snd.wl2 == seg->ack && seg->wnd > tcb->snd.wnd)) {
		tcb->snd.wnd = seg->wnd;
		tcb->snd.wl2 = seg->ack;
	}

	if (!seq_gt(seg->ack, tcb->snd.una)) {
		/*
		 *  don't let us hangup if sending into a closed window and
		 *  we're still getting acks
		 */
		if ((tcb->flags & RETRAN) && tcb->snd.wnd == 0) {
			tcb->backedoff = MAXBACKMS / 4;
		}
		return;
	}
	/* At this point, they have acked something new. (positive ack, ack > una).
	 *
	 * If we hadn't reached the threshold for recovery yet, the positive ACK
	 * will reset our loss_hint count. */
	if (!tcb->snd.recovery)
		tcb->snd.loss_hint = 0;
	else if (sack_recovery_done(tcb, seg) || fast_rx_recovery_done(tcb, seg))
		reset_recovery(tcb);

	/* Compute the new send window size */
	acked = seg->ack - tcb->snd.una;

	/* avoid slow start and timers for SYN acks */
	if ((tcb->flags & SYNACK) == 0) {
		tcb->flags |= SYNACK;
		acked--;
		tcb->flgcnt--;
		goto done;
	}

	/* slow start as long as we're not recovering from lost packets */
	// XXX maybe CA here during recovery
	if (tcb->cwind < tcb->snd.wnd && !tcb->snd.recovery) {
		if (tcb->cwind < tcb->ssthresh) {
			/* We increase the cwind by every byte we receive.  We want to
			 * increase the cwind by one MSS for every MSS that gets ACKed.
			 * Note that multiple MSSs can be ACKed in a single ACK.  If we had
			 * a remainder of acked / MSS, we'd add just that remainder - not 0
			 * or 1 MSS. */
			expand = acked;
		} else {
			/* Every RTT, which consists of CWND bytes, we're supposed to expand
			 * by MSS bytes.  The classic algorithm was
			 * 		expand = (tcb->mss * tcb->mss) / tcb->cwind;
			 * which assumes the ACK was for MSS bytes.  Instead, for every
			 * 'acked' bytes, we increase the window by acked / CWND (in units
			 * of MSS). */
			expand = MAX(acked, tcb->typical_mss) * tcb->typical_mss
			         / tcb->cwind;
		}

		// XXX sanity check this
		if (tcb->cwind + expand < tcb->cwind)
			expand = tcb->snd.wnd - tcb->cwind;
		if (tcb->cwind + expand > tcb->snd.wnd)
			expand = tcb->snd.wnd - tcb->cwind;
		tcb->cwind += expand;
	}
	adjust_tx_qio_limit(s);

	/* Adjust the timers according to the round trip time */
	if (tcb->rtt_timer.state == TcptimerON && seq_ge(seg->ack, tcb->rttseq)) {
		tcphalt(tpriv, &tcb->rtt_timer);
		if ((tcb->flags & RETRAN) == 0) {
			tcb->backoff = 0;
			tcb->backedoff = 0;
			rtt = tcb->rtt_timer.start - tcb->rtt_timer.count;
			if (rtt == 0)
				rtt = 1;	/* otherwise all close systems will rexmit in 0 time */
			rtt *= MSPTICK;
			if (tcb->srtt == 0) {
				tcb->srtt = rtt << LOGAGAIN;
				tcb->mdev = rtt << LOGDGAIN;
			} else {
				delta = rtt - (tcb->srtt >> LOGAGAIN);
				tcb->srtt += delta;
				if (tcb->srtt <= 0)
					tcb->srtt = 1;

				delta = abs(delta) - (tcb->mdev >> LOGDGAIN);
				tcb->mdev += delta;
				if (tcb->mdev <= 0)
					tcb->mdev = 1;
			}
			tcpsettimer(tcb);
		}
	}

done:
	if (qdiscard(s->wq, acked) < acked)
		tcb->flgcnt--;

	tcb->snd.una = seg->ack;
	if (seq_gt(seg->ack, tcb->snd.urg))
		tcb->snd.urg = seg->ack;

	if (tcb->snd.una != tcb->snd.nxt)
		tcpgo(tpriv, &tcb->timer);
	else
		tcphalt(tpriv, &tcb->timer);

	if (seq_lt(tcb->snd.ptr, tcb->snd.una))
		tcb->snd.ptr = tcb->snd.una;

	tcb->flags &= ~RETRAN;
	tcb->backoff = 0;
	tcb->backedoff = 0;
}

static void update_tcb_ts(Tcpctl *tcb, Tcp *seg)
{
	/* Get timestamp info from the tcp header.  Even though the timestamps
	 * aren't sequence numbers, we still need to protect for wraparound.  Though
	 * if the values were 0, assume that means we need an update.  We could have
	 * an initial ts_val that appears negative (signed). */
	if (!tcb->ts_recent || !tcb->last_ack_sent ||
	    (seq_ge(seg->ts_val, tcb->ts_recent) &&
	     seq_le(seg->seq, tcb->last_ack_sent)))
		tcb->ts_recent = seg->ts_val;
}

void tcpiput(struct Proto *tcp, struct Ipifc *unused, struct block *bp)
{
	ERRSTACK(1);
	Tcp seg;
	Tcp4hdr *h4;
	Tcp6hdr *h6;
	int hdrlen;
	Tcpctl *tcb;
	uint16_t length;
	uint8_t source[IPaddrlen], dest[IPaddrlen];
	struct conv *s;
	struct Fs *f;
	struct tcppriv *tpriv;
	uint8_t version;

	f = tcp->f;
	tpriv = tcp->priv;

	tpriv->stats[InSegs]++;

	h4 = (Tcp4hdr *) (bp->rp);
	h6 = (Tcp6hdr *) (bp->rp);

	if ((h4->vihl & 0xF0) == IP_VER4) {
		uint8_t ttl;

		version = V4;
		length = nhgets(h4->length);
		v4tov6(dest, h4->tcpdst);
		v4tov6(source, h4->tcpsrc);

		/* ttl isn't part of the xsum pseudo header, but bypass needs it. */
		ttl = h4->Unused;
		h4->Unused = 0;
		hnputs(h4->tcplen, length - TCP4_PKT);
		if (!(bp->flag & Btcpck) && (h4->tcpcksum[0] || h4->tcpcksum[1]) &&
			ptclcsum(bp, TCP4_IPLEN, length - TCP4_IPLEN)) {
			tpriv->stats[CsumErrs]++;
			tpriv->stats[InErrs]++;
			netlog(f, Logtcp, "bad tcp proto cksum\n");
			freeblist(bp);
			return;
		}
		h4->Unused = ttl;

		hdrlen = ntohtcp4(&seg, &bp);
		if (hdrlen < 0) {
			tpriv->stats[HlenErrs]++;
			tpriv->stats[InErrs]++;
			netlog(f, Logtcp, "bad tcp hdr len\n");
			return;
		}

		s = iphtlook(&tpriv->ht, source, seg.source, dest, seg.dest);
		if (s && s->state == Bypass) {
			bypass_or_drop(s, bp);
			return;
		}

		/* trim the packet to the size claimed by the datagram */
		length -= hdrlen + TCP4_PKT;
		bp = trimblock(bp, hdrlen + TCP4_PKT, length);
		if (bp == NULL) {
			tpriv->stats[LenErrs]++;
			tpriv->stats[InErrs]++;
			netlog(f, Logtcp, "tcp len < 0 after trim\n");
			return;
		}
	} else {
		int ttl = h6->ttl;
		int proto = h6->proto;

		version = V6;
		length = nhgets(h6->ploadlen);
		ipmove(dest, h6->tcpdst);
		ipmove(source, h6->tcpsrc);

		h6->ploadlen[0] = h6->ploadlen[1] = h6->proto = 0;
		h6->ttl = proto;
		hnputl(h6->vcf, length);
		if ((h6->tcpcksum[0] || h6->tcpcksum[1]) &&
			ptclcsum(bp, TCP6_IPLEN, length + TCP6_PHDRSIZE)) {
			tpriv->stats[CsumErrs]++;
			tpriv->stats[InErrs]++;
			netlog(f, Logtcp, "bad tcp proto cksum\n");
			freeblist(bp);
			return;
		}
		h6->ttl = ttl;
		h6->proto = proto;
		hnputs(h6->ploadlen, length);

		hdrlen = ntohtcp6(&seg, &bp);
		if (hdrlen < 0) {
			tpriv->stats[HlenErrs]++;
			tpriv->stats[InErrs]++;
			netlog(f, Logtcp, "bad tcp hdr len\n");
			return;
		}

		s = iphtlook(&tpriv->ht, source, seg.source, dest, seg.dest);
		if (s && s->state == Bypass) {
			bypass_or_drop(s, bp);
			return;
		}

		/* trim the packet to the size claimed by the datagram */
		length -= hdrlen;
		bp = trimblock(bp, hdrlen + TCP6_PKT, length);
		if (bp == NULL) {
			tpriv->stats[LenErrs]++;
			tpriv->stats[InErrs]++;
			netlog(f, Logtcp, "tcp len < 0 after trim\n");
			return;
		}
	}

	/* s, the conv matching the n-tuple, was set above */
	if (s == NULL) {
		netlog(f, Logtcp, "iphtlook failed: src %I:%u, dst %I:%u\n",
		       source, seg.source, dest, seg.dest);
reset:
		sndrst(tcp, source, dest, length, &seg, version, "no conversation");
		freeblist(bp);
		return;
	}

	/* lock protocol for unstate Plan 9 invariants.  funcs like limbo or
	 * incoming might rely on it. */
	qlock(&tcp->qlock);

	/* if it's a listener, look for the right flags and get a new conv */
	tcb = (Tcpctl *) s->ptcl;
	if (tcb->state == Listen) {
		if (seg.flags & RST) {
			limborst(s, &seg, source, dest, version);
			qunlock(&tcp->qlock);
			freeblist(bp);
			return;
		}

		/* if this is a new SYN, put the call into limbo */
		if ((seg.flags & SYN) && (seg.flags & ACK) == 0) {
			limbo(s, source, dest, &seg, version);
			qunlock(&tcp->qlock);
			freeblist(bp);
			return;
		}

		/* if there's a matching call in limbo, tcpincoming will return it */
		s = tcpincoming(s, &seg, source, dest, version);
		if (s == NULL) {
			qunlock(&tcp->qlock);
			goto reset;
		}
	}

	/* The rest of the input state machine is run with the control block
	 * locked and implements the state machine directly out of the RFC.
	 * Out-of-band data is ignored - it was always a bad idea.
	 */
	tcb = (Tcpctl *) s->ptcl;
	if (waserror()) {
		qunlock(&s->qlock);
		nexterror();
	}
	qlock(&s->qlock);
	qunlock(&tcp->qlock);

	update_tcb_ts(tcb, &seg);
	/* fix up window */
	seg.wnd <<= tcb->rcv.scale;

	/* every input packet in puts off the keep alive time out */
	tcpsetkacounter(tcb);

	switch (tcb->state) {
		case Closed:
			sndrst(tcp, source, dest, length, &seg, version,
				   "sending to Closed");
			goto raise;
		case Syn_sent:
			if (seg.flags & ACK) {
				if (!seq_within(seg.ack, tcb->iss + 1, tcb->snd.nxt)) {
					sndrst(tcp, source, dest, length, &seg, version,
						   "bad seq in Syn_sent");
					goto raise;
				}
			}
			if (seg.flags & RST) {
				if (seg.flags & ACK)
					localclose(s, "connection refused");
				goto raise;
			}

			if (seg.flags & SYN) {
				procsyn(s, &seg);
				if (seg.flags & ACK) {
					update(s, &seg);
					tcpsynackrtt(s);
					tcpsetstate(s, Established);
					/* Here's where we get the results of header option
					 * negotiations for connections we started. (SYNACK has the
					 * response) */
					tcpsetscale(s, tcb, seg.ws, tcb->scale);
					tcb->sack_ok = seg.sack_ok;
				} else {
					sndrst(tcp, source, dest, length, &seg, version,
						   "Got SYN with no ACK");
					goto raise;
				}

				if (length != 0 || (seg.flags & FIN))
					break;

				freeblist(bp);
				goto output;
			} else
				freeblist(bp);

			qunlock(&s->qlock);
			poperror();
			return;
	}

	/*
	 *  One DOS attack is to open connections to us and then forget about them,
	 *  thereby tying up a conv at no long term cost to the attacker.
	 *  This is an attempt to defeat these stateless DOS attacks.  See
	 *  corresponding code in tcpsendka().
	 */
	if ((seg.flags & RST) == 0) {
		if (tcpporthogdefense
			&& seq_within(seg.ack, tcb->snd.una - (1 << 31),
						  tcb->snd.una - (1 << 29))) {
			printd("stateless hog %I.%d->%I.%d f 0x%x 0x%lx - 0x%lx - 0x%lx\n",
				   source, seg.source, dest, seg.dest, seg.flags,
				   tcb->snd.una - (1 << 31), seg.ack, tcb->snd.una - (1 << 29));
			localclose(s, "stateless hog");
		}
	}

	/* Cut the data to fit the receive window */
	if (tcptrim(tcb, &seg, &bp, &length) == -1) {
		netlog(f, Logtcp, "tcp len < 0, %lu %d\n", seg.seq, length);
		update(s, &seg);
		if (qlen(s->wq) + tcb->flgcnt == 0 && tcb->state == Closing) {
			tcphalt(tpriv, &tcb->rtt_timer);
			tcphalt(tpriv, &tcb->acktimer);
			tcphalt(tpriv, &tcb->katimer);
			tcpsetstate(s, Time_wait);
			tcb->timer.start = MSL2 * (1000 / MSPTICK);
			tcpgo(tpriv, &tcb->timer);
		}
		if (!(seg.flags & RST)) {
			tcb->flags |= FORCE;
			goto output;
		}
		qunlock(&s->qlock);
		poperror();
		return;
	}

	/* Cannot accept so answer with a rst */
	if (length && tcb->state == Closed) {
		sndrst(tcp, source, dest, length, &seg, version, "sending to Closed");
		goto raise;
	}

	/* The segment is beyond the current receive pointer so
	 * queue the data in the resequence queue
	 */
	if (seg.seq != tcb->rcv.nxt)
		if (length != 0 || (seg.flags & (SYN | FIN))) {
			update(s, &seg);
			if (addreseq(tcb, tpriv, &seg, bp, length) < 0)
				printd("reseq %I.%d -> %I.%d\n", s->raddr, s->rport, s->laddr,
					   s->lport);
			tcb->flags |= FORCE;
			goto output;
		}

	/*
	 *  keep looping till we've processed this packet plus any
	 *  adjacent packets in the resequence queue
	 */
	for (;;) {
		if (seg.flags & RST) {
			if (tcb->state == Established) {
				tpriv->stats[EstabResets]++;
				if (tcb->rcv.nxt != seg.seq)
					printd
						("out of order RST rcvd: %I.%d -> %I.%d, rcv.nxt 0x%lx seq 0x%lx\n",
						 s->raddr, s->rport, s->laddr, s->lport, tcb->rcv.nxt,
						 seg.seq);
			}
			localclose(s, "connection refused");
			goto raise;
		}

		if ((seg.flags & ACK) == 0)
			goto raise;

		switch (tcb->state) {
			case Established:
			case Close_wait:
				update(s, &seg);
				break;
			case Finwait1:
				update(s, &seg);
				if (qlen(s->wq) + tcb->flgcnt == 0) {
					tcphalt(tpriv, &tcb->rtt_timer);
					tcphalt(tpriv, &tcb->acktimer);
					tcpsetkacounter(tcb);
					tcb->time = NOW;
					tcpsetstate(s, Finwait2);
					tcb->katimer.start = MSL2 * (1000 / MSPTICK);
					tcpgo(tpriv, &tcb->katimer);
				}
				break;
			case Finwait2:
				update(s, &seg);
				break;
			case Closing:
				update(s, &seg);
				if (qlen(s->wq) + tcb->flgcnt == 0) {
					tcphalt(tpriv, &tcb->rtt_timer);
					tcphalt(tpriv, &tcb->acktimer);
					tcphalt(tpriv, &tcb->katimer);
					tcpsetstate(s, Time_wait);
					tcb->timer.start = MSL2 * (1000 / MSPTICK);
					tcpgo(tpriv, &tcb->timer);
				}
				break;
			case Last_ack:
				update(s, &seg);
				if (qlen(s->wq) + tcb->flgcnt == 0) {
					localclose(s, NULL);
					goto raise;
				}
			case Time_wait:
				tcb->flags |= FORCE;
				if (tcb->timer.state != TcptimerON)
					tcpgo(tpriv, &tcb->timer);
		}

		if ((seg.flags & URG) && seg.urg) {
			if (seq_gt(seg.urg + seg.seq, tcb->rcv.urg)) {
				tcb->rcv.urg = seg.urg + seg.seq;
				pullblock(&bp, seg.urg);
			}
		} else if (seq_gt(tcb->rcv.nxt, tcb->rcv.urg))
			tcb->rcv.urg = tcb->rcv.nxt;

		if (length == 0) {
			if (bp != NULL)
				freeblist(bp);
		} else {
			switch (tcb->state) {
				default:
					/* Ignore segment text */
					if (bp != NULL)
						freeblist(bp);
					break;

				case Established:
				case Finwait1:
					/* If we still have some data place on
					 * receive queue
					 */
					if (bp) {
						bp = packblock(bp);
						if (bp == NULL)
							panic("tcp packblock");
						qpassnolim(s->rq, bp);
						bp = NULL;

						/*
						 *  Force an ack every 2 data messages.  This is
						 *  a hack for rob to make his home system run
						 *  faster.
						 *
						 *  this also keeps the standard TCP congestion
						 *  control working since it needs an ack every
						 *  2 max segs worth.  This is not quite that,
						 *  but under a real stream is equivalent since
						 *  every packet has a max seg in it.
						 */
						if (++(tcb->rcv.una) >= 2)
							tcb->flags |= FORCE;
					}
					tcb->rcv.nxt += length;

					/*
					 *  update our rcv window
					 */
					tcprcvwin(s);

					/*
					 *  turn on the acktimer if there's something
					 *  to ack
					 */
					if (tcb->acktimer.state != TcptimerON)
						tcpgo(tpriv, &tcb->acktimer);

					break;
				case Finwait2:
					/* no process to read the data, send a reset */
					if (bp != NULL)
						freeblist(bp);
					sndrst(tcp, source, dest, length, &seg, version,
						   "send to Finwait2");
					qunlock(&s->qlock);
					poperror();
					return;
			}
		}

		if (seg.flags & FIN) {
			tcb->flags |= FORCE;

			switch (tcb->state) {
				case Established:
					tcb->rcv.nxt++;
					tcpsetstate(s, Close_wait);
					break;
				case Finwait1:
					tcb->rcv.nxt++;
					if (qlen(s->wq) + tcb->flgcnt == 0) {
						tcphalt(tpriv, &tcb->rtt_timer);
						tcphalt(tpriv, &tcb->acktimer);
						tcphalt(tpriv, &tcb->katimer);
						tcpsetstate(s, Time_wait);
						tcb->timer.start = MSL2 * (1000 / MSPTICK);
						tcpgo(tpriv, &tcb->timer);
					} else
						tcpsetstate(s, Closing);
					break;
				case Finwait2:
					tcb->rcv.nxt++;
					tcphalt(tpriv, &tcb->rtt_timer);
					tcphalt(tpriv, &tcb->acktimer);
					tcphalt(tpriv, &tcb->katimer);
					tcpsetstate(s, Time_wait);
					tcb->timer.start = MSL2 * (1000 / MSPTICK);
					tcpgo(tpriv, &tcb->timer);
					break;
				case Close_wait:
				case Closing:
				case Last_ack:
					break;
				case Time_wait:
					tcpgo(tpriv, &tcb->timer);
					break;
			}
		}

		/*
		 *  get next adjacent segment from the resequence queue.
		 *  dump/trim any overlapping segments
		 */
		for (;;) {
			if (tcb->reseq == NULL)
				goto output;

			if (seq_ge(tcb->rcv.nxt, tcb->reseq->seg.seq) == 0)
				goto output;

			getreseq(tcb, &seg, &bp, &length);

			if (tcptrim(tcb, &seg, &bp, &length) == 0)
				break;
		}
	}
output:
	tcpoutput(s);
	qunlock(&s->qlock);
	poperror();
	return;
raise:
	qunlock(&s->qlock);
	poperror();
	freeblist(bp);
	tcpkick(s);
}

/* The advertised mss = data + TCP headers */
static uint16_t derive_payload_mss(Tcpctl *tcb)
{
	uint16_t payload_mss = tcb->mss;
	uint16_t opt_size = 0;

	if (tcb->ts_recent)
		opt_size += TS_LENGTH;
	opt_size = ROUNDUP(opt_size, 4);
	payload_mss -= opt_size;
	return payload_mss;
}

/* Decreases the xmit amt, given the MSS / TSO. */
static uint32_t throttle_for_mss(Tcpctl *tcb, uint32_t xmit_amt,
                                 uint16_t payload_mss, bool retrans)
{
	if (xmit_amt > payload_mss) {
		if ((tcb->flags & TSO) == 0) {
			xmit_amt = payload_mss;
		} else {
			/* Don't send too much.  32K is arbitrary.. */
			if (xmit_amt > 32 * 1024)
				xmit_amt = 32 * 1024;
			if (!retrans) {
				/* Clamp xmit to an integral MSS to avoid ragged tail segments
				 * causing poor link utilization. */
				xmit_amt = ROUNDDOWN(xmit_amt, payload_mss);
			}
		}
	}
	return xmit_amt;
}

/* Helper, picks the next segment to send, which is possibly a retransmission.
 * Returns TRUE if we have a segment, FALSE o/w.  Returns by reference the qio
 * offset, the len of the segment, and whether we are retransmitting.
 *
 * Note that we could be in recovery and not retrans a segment. */
static bool get_xmit_segment(struct conv *s, Tcpctl *tcb, uint16_t payload_mss,
                             uint32_t *xmit_off_p, uint32_t *xmit_amt_p,
                             bool *retrans_p)
{
	uint32_t xmit_amt, xmit_off, in_flight, usable;
	bool retrans = FALSE;
	struct sack_block *tcb_sack = 0;

int desired = 0;

// XXX do we need to do tcb->flgcnt with retrans xmit_amt ?
// 	probably.  might get off-by-ones or something, or corruption
// 		in non recovery, are we yanking flgcnt bytes out of the qio?  seems
// 		fucked.
// 		when is the flgcnt non-zero?  do we ever have data and flgcnt?
// 		could it go negative?
// 		might be something to advance the seq during 3WHS and teardown
// 			or accounting for the seq nr for packets with no length
//
// btw, part of the reason to do the "yield every 4" is so we can handle inbound
// ACKs that might make us change our mind.  prob not a big deal though.
	if (tcb->snd.recovery == SACK_RETRANS_RECOVERY) {
		in_flight = tcb->snd.pipe_bytes;
		for (int i = 0; i < tcb->snd.nr_sacks; i++) {
			tcb_sack = &tcb->snd.sacks[i];
			if (seq_lt(tcb->snd.sack_rxt, tcb_sack->left)) {
				xmit_off = tcb->snd.sack_rxt - tcb->snd.una;
				xmit_amt = seq_abs_diff(tcb_sack->left, tcb->snd.sack_rxt);
				retrans = TRUE;

				// XXX remove me
				desired = xmit_amt;
				break;
			}
		}
	} else {
		in_flight = tcb->snd.ptr - tcb->snd.una;
	}
	/* During recovery, SACK has first dibs, but we can still advance the right
	 * edge of unsent.  Note retrans != recovery. */
	if (!retrans) {
		xmit_off = tcb->snd.ptr - tcb->snd.una;
		xmit_amt = qlen(s->wq) + tcb->flgcnt - xmit_off;
	}

	/* Compute usable segment based on offered window and limit
	 * window probes to one */
	if (tcb->snd.wnd == 0) {
		if (in_flight != 0) {
			if ((tcb->flags & FORCE) == 0)
				return FALSE;
		}
		usable = 1;
	} else {
		usable = tcb->cwind;
		if (tcb->snd.wnd < usable)
			usable = tcb->snd.wnd;
		if (usable > in_flight)
			usable -= in_flight;
		else
			usable = 0;
	}
	/* TODO: consider the SWS sender alg around here. (send nothing if less than
	 * MSS).  careful to only do this when we're not forcing or syn/acking. */
			// XXX basically, if usable < typical_mss, abort
	if (xmit_amt && usable < 2)
		netlog(s->p->f, Logtcp, "throttled snd.wnd %lu cwind %lu\n",
			   tcb->snd.wnd, tcb->cwind);
	if (usable < xmit_amt)
		xmit_amt = usable;

	xmit_amt = throttle_for_mss(tcb, xmit_amt, payload_mss, retrans);

	tcb->snd.pipe_bytes += xmit_amt;	/* can update outside recovery */
	if (retrans) {
		/* If we'll send up to the left edge, advance sack_rxt to the right. */

		// XXX is this true?  will we ever move sack_rxt beyond the last sack?
		// in xmit_off func, if we sent up to the sack border, we advance over
		// the sack.  that's wrong for the last one.  need to fix it
		// 		by advancing sack_rxt to that point, we might count it as
		// 		in_flight, if we get rid of that sack.
		// 			better to underestimate the pipe, perhaps, than definitely
		// 			resend a chunk of data.  same amount usable for one RTT,
		// 			without the clutter..

		if (xmit_amt == seq_abs_diff(tcb_sack->left, tcb->snd.sack_rxt))
			tcb->snd.sack_rxt = tcb_sack->right;
		else
			tcb->snd.sack_rxt += xmit_amt;
	}
	*xmit_off_p = xmit_off;
	*xmit_amt_p = xmit_amt;
	*retrans_p = retrans;


// XXX HERE
//
//	had a case where it looks like we didn't send any retrans, just kept
//	blasting away.
//		wireshark in-flight was  much higher than cwnd should have been
//		check that logic again, it's probably wrong in this func.
//
//
// we advanced the ack, maybe past recovery too
// 		in which case, no recovery, no sack_rxt
//
// 		but still have sacks.  what good are they if we don't use them?
// 			USE THEM
//
// looks like we might have missed something, but sack rxmit was already ahead
// 		since they had a hole and were growing a different sack block
//
// 		maybe if we update a sack block that is less than rxmit
// 			meaning, they got a retrans
// 		that tells us that anything between snd.una and less than this block was
// 		lost
// 				possible reordering, so maybe within MSS * 2
//
//


	// XXX REMOVE ME
	if (retrans) {
		static int x = 0;
		if (x++ < 100) {
			trace_printk("XMIT recovery!\n");
			trace_printk("\tSACKS: %d\n", tcb->snd.nr_sacks);
			for (int i = 0; i < tcb->snd.nr_sacks; i++) {
				tcb->snd.sacks[i];
				trace_printk("\t\t%d: [%u, %u)\n", i, tcb->snd.sacks[i].left, tcb->snd.sacks[i].right);
			}
			trace_printk("\txmit_off %u, xmit_amt %u, sack_rxt %u, in_flight %u, usable %u, desired %u\n",
			             xmit_off, xmit_amt, tcb->snd.sack_rxt, in_flight, usable, desired);
		}
	}


	return TRUE;
}

/*
 *  always enters and exits with the s locked.  We drop
 *  the lock to ipoput the packet so some care has to be
 *  taken by callers.
 */
void tcpoutput(struct conv *s)
{
	Tcp seg;
	int msgs;
	Tcpctl *tcb;
	struct block *hbp, *bp;
	int n;
	uint32_t ssize, dsize, xmit_off;
	struct Fs *f;
	struct tcppriv *tpriv;
	uint8_t version;
	uint16_t payload_mss;
	bool retrans = FALSE;

	f = s->p->f;
	tpriv = s->p->priv;
	version = s->ipversion;

	for (msgs = 0; msgs < 100; msgs++) {
		tcb = (Tcpctl *) s->ptcl;

		switch (tcb->state) {
			case Listen:
			case Closed:
			case Finwait2:
				return;
		}

		/* force an ack when a window has opened up */
		if (tcb->rcv.blocked && tcb->rcv.wnd > 0) {
			tcb->rcv.blocked = 0;
			tcb->flags |= FORCE;
		}

		/* Don't send anything else until our SYN has been acked */
		if (tcb->snd.ptr != tcb->iss && (tcb->flags & SYNACK) == 0)
			break;

		/* payload_mss is the actual amount of data in the packet, which is the
		 * advertised (mss - header opts).  This varies from packet to packet,
		 * based on the options that might be present (e.g. always timestamps,
		 * sometimes SACKs) */
		payload_mss = derive_payload_mss(tcb);

		if (!get_xmit_segment(s, tcb, payload_mss, &xmit_off, &ssize, &retrans))
			break;

		dsize = ssize;
		seg.urg = 0;

		if (ssize == 0)
			if ((tcb->flags & FORCE) == 0)
				break;

		tcb->flags &= ~FORCE;
		tcprcvwin(s);

		/* By default we will generate an ack, so we can normally turn off the
		 * timer.  If we're blocked, we'll want the timer so we can send a
		 * window update. */
		if (!tcb->rcv.blocked)
			tcphalt(tpriv, &tcb->acktimer);
		tcb->rcv.una = 0;
		seg.source = s->lport;
		seg.dest = s->rport;
		seg.flags = ACK;
		// XXX syn clearables
		seg.mss = 0;
		seg.ws = 0;
		seg.sack_ok = FALSE;
		seg.nr_sacks = 0;
		/* When outputting, Syn_sent means "send the Syn", for connections we
		 * initiate.  SYNACKs are sent from sndsynack directly. */
		if (tcb->state == Syn_sent) {
			seg.flags = 0;
			seg.sack_ok = SACK_SUPPORTED;	/* here's where we advertise SACK */
			if (tcb->snd.ptr == tcb->iss) {
				seg.flags |= SYN;
				dsize--;
				seg.mss = tcb->mss;
				seg.ws = tcb->scale;
			} else {
				/* TODO: Not sure why we'd get here. */
				warn("TCP: weird Syn_sent state, tell someone you saw this");
			}
		}

		seg.seq = tcb->snd.una + xmit_off;
		seg.ack = tcb->rcv.nxt;
		tcb->last_ack_sent = seg.ack;
		seg.wnd = tcb->rcv.wnd;
		seg.ts_val = tcb->ts_recent;

		/* Pull out data to send */
		bp = NULL;
		if (dsize != 0) {
			bp = qcopy(s->wq, dsize, xmit_off);
			if (BLEN(bp) != dsize) {

				// XXX
				static int foo = 0;
				if (foo++ < 40)
				trace_printk("couldn't get dsize out of qio, setting FIN.  qlen %u, dsize %u, xmit_off %u, BLEN %u\n", qlen(s->wq), dsize, xmit_off, BLEN(bp));

				seg.flags |= FIN;
				dsize--;
			}
			if (BLEN(bp) > payload_mss) {
				bp->flag |= Btso;
				bp->mss = payload_mss;
			}
		}

		if (xmit_off + dsize == qlen(s->wq) + tcb->flgcnt)
			seg.flags |= PSH;

		/* keep track of balance of resent data */
			// XXX only counts dong's fast rxmit, not sack rxmit
		if (seq_lt(tcb->snd.ptr, tcb->snd.nxt)) {
			n = tcb->snd.nxt - tcb->snd.ptr;
			if (ssize < n)
				n = ssize;
			tcb->resent += n;
			netlog(f, Logtcp, "rexmit: %I.%d -> %I.%d ptr 0x%lx nxt 0x%lx\n",
				   s->raddr, s->rport, s->laddr, s->lport, tcb->snd.ptr,
				   tcb->snd.nxt);
			tpriv->stats[RetransSegs]++;
		}

		if (!retrans)
			tcb->snd.ptr += ssize;

		/* Pull up the send pointer so we can accept acks
		 * for this window
		 */
		if (seq_gt(tcb->snd.ptr, tcb->snd.nxt))
			tcb->snd.nxt = tcb->snd.ptr;

		/* Build header, link data and compute cksum */
		switch (version) {
			case V4:
				tcb->protohdr.tcp4hdr.vihl = IP_VER4;
				hbp = htontcp4(&seg, bp, &tcb->protohdr.tcp4hdr, tcb);
				if (hbp == NULL) {
					freeblist(bp);
					return;
				}
				break;
			case V6:
				tcb->protohdr.tcp6hdr.vcf[0] = IP_VER6;
				hbp = htontcp6(&seg, bp, &tcb->protohdr.tcp6hdr, tcb);
				if (hbp == NULL) {
					freeblist(bp);
					return;
				}
				break;
			default:
				hbp = NULL;	/* to suppress a warning */
				panic("tcpoutput: version %d", version);
		}

		/* Start the transmission timers if there is new data and we
		 * expect acknowledges
		 */
		if (ssize != 0) {
			if (tcb->timer.state != TcptimerON)
				tcpgo(tpriv, &tcb->timer);

			/* If round trip timer isn't running, start it. */
			if (tcb->rtt_timer.state != TcptimerON) {
				tcpgo(tpriv, &tcb->rtt_timer);
				tcb->rttseq = tcb->snd.ptr;
			}
		}

		tpriv->stats[OutSegs]++;

		/* put off the next keep alive */
		tcpgo(tpriv, &tcb->katimer);

		switch (version) {
			case V4:
				if (ipoput4(f, hbp, 0, s->ttl, s->tos, s) < 0) {
					/* a negative return means no route */
					localclose(s, "no route");
				}
				break;
			case V6:
				if (ipoput6(f, hbp, 0, s->ttl, s->tos, s) < 0) {
					/* a negative return means no route */
					localclose(s, "no route");
				}
				break;
			default:
				panic("tcpoutput2: version %d", version);
		}
		if ((msgs % 4) == 1) {
			qunlock(&s->qlock);
			kthread_yield();
			qlock(&s->qlock);
		}
	}
}

/*
 *  the BSD convention (hack?) for keep alives.  resend last uint8_t acked.
 */
void tcpsendka(struct conv *s)
{
	Tcp seg;
	Tcpctl *tcb;
	struct block *hbp, *dbp;

	tcb = (Tcpctl *) s->ptcl;

	dbp = NULL;
	seg.urg = 0;
	seg.source = s->lport;
	seg.dest = s->rport;
	seg.flags = ACK | PSH;
	// XXX syn clearables
	seg.mss = 0;
	seg.ws = 0;
	seg.sack_ok = FALSE;
	seg.nr_sacks = 0;
	if (tcpporthogdefense)
		urandom_read(&seg.seq, sizeof(seg.seq));
	else
		seg.seq = tcb->snd.una - 1;
	seg.ack = tcb->rcv.nxt;
	tcb->last_ack_sent = seg.ack;
	tcb->rcv.una = 0;
	seg.wnd = tcb->rcv.wnd;
	seg.ts_val = tcb->ts_recent;
	if (tcb->state == Finwait2) {
		seg.flags |= FIN;
	} else {
		dbp = block_alloc(1, MEM_WAIT);
		dbp->wp++;
	}

	if (isv4(s->raddr)) {
		/* Build header, link data and compute cksum */
		tcb->protohdr.tcp4hdr.vihl = IP_VER4;
		hbp = htontcp4(&seg, dbp, &tcb->protohdr.tcp4hdr, tcb);
		if (hbp == NULL) {
			freeblist(dbp);
			return;
		}
		ipoput4(s->p->f, hbp, 0, s->ttl, s->tos, s);
	} else {
		/* Build header, link data and compute cksum */
		tcb->protohdr.tcp6hdr.vcf[0] = IP_VER6;
		hbp = htontcp6(&seg, dbp, &tcb->protohdr.tcp6hdr, tcb);
		if (hbp == NULL) {
			freeblist(dbp);
			return;
		}
		ipoput6(s->p->f, hbp, 0, s->ttl, s->tos, s);
	}
}

/*
 *  set connection to time out after 12 minutes
 */
void tcpsetkacounter(Tcpctl * tcb)
{
	tcb->kacounter = (12 * 60 * 1000) / (tcb->katimer.start * MSPTICK);
	if (tcb->kacounter < 3)
		tcb->kacounter = 3;
}

/*
 *  if we've timed out, close the connection
 *  otherwise, send a keepalive and restart the timer
 */
void tcpkeepalive(void *v)
{
	ERRSTACK(1);
	Tcpctl *tcb;
	struct conv *s;

	s = v;
	tcb = (Tcpctl *) s->ptcl;
	qlock(&s->qlock);
	if (waserror()) {
		qunlock(&s->qlock);
		nexterror();
	}
	if (tcb->state != Closed) {
		if (--(tcb->kacounter) <= 0) {
			localclose(s, "connection timed out");
		} else {
			tcpsendka(s);
			tcpgo(s->p->priv, &tcb->katimer);
		}
	}
	qunlock(&s->qlock);
	poperror();
}

/*
 *  start keepalive timer
 */
static void tcpstartka(struct conv *s, char **f, int n)
{
	Tcpctl *tcb;
	int x;

	tcb = (Tcpctl *) s->ptcl;
	if (tcb->state != Established)
		error(ENOTCONN, "connection must be in Establised state");
	if (n > 1) {
		x = atoi(f[1]);
		if (x >= MSPTICK)
			tcb->katimer.start = x / MSPTICK;
	}
	tcpsetkacounter(tcb);
	tcpgo(s->p->priv, &tcb->katimer);
}

/*
 *  turn checksums on/off
 */
static void tcpsetchecksum(struct conv *s, char **f, int unused)
{
	Tcpctl *tcb;

	tcb = (Tcpctl *) s->ptcl;
	tcb->nochecksum = !atoi(f[1]);
}

// XXX consider not taking loss events on SYN/SYNACK, for weird connection
// start ups
static void tcp_loss_event(Tcpctl *tcb)
{
	trace_printk("loss event, cwnd was %d, now %d\n", tcb->cwind, tcb->cwind / 2);
	/* Reno */
	tcb->ssthresh = tcb->cwind / 2;
#if 1
	// XXX dong's fast rxmit and even our sack rxmit will likely lose packets
	// and have to do the full timeout
	tcb->cwind = tcb->ssthresh;
#else
	// dong's fast rxmit takes too long.  probably need to grow cwind during it
	// (like the RFC says), basically like a CA phase.
	// 		XXX maybe try cwind incrementing and tahoe.
	/* tahoe */
	tcb->cwind = tcb->typical_mss * CWIND_SCALE;
#endif
}

/* Called when we need to retrans the entire outstanding window (everything
 * previously sent, but unacknowledged. */
void tcprxmit(struct conv *s)
{
	Tcpctl *tcb;

	tcb = (Tcpctl *) s->ptcl;

	tcb->flags |= RETRAN | FORCE;
	tcb->snd.ptr = tcb->snd.una;

	tcpoutput(s);
}

void tcptimeout(void *arg)
{
	ERRSTACK(1);
	struct conv *s;
	Tcpctl *tcb;
	int maxback;
	struct tcppriv *tpriv;

	s = (struct conv *)arg;
	tpriv = s->p->priv;
	tcb = (Tcpctl *) s->ptcl;

	qlock(&s->qlock);
	if (waserror()) {
		qunlock(&s->qlock);
		nexterror();
	}
	switch (tcb->state) {
		default:
			tcb->backoff++;
			if (tcb->state == Syn_sent)
				maxback = MAXBACKMS / 2;
			else
				maxback = MAXBACKMS;
			tcb->backedoff += tcb->timer.start * MSPTICK;
			if (tcb->backedoff >= maxback) {
				localclose(s, "connection timed out");
				break;
			}
			netlog(s->p->f, Logtcprxmt, "timeout rexmit 0x%lx %llu/%llu\n",
				   tcb->snd.una, tcb->timer.start, NOW);
			tcpsettimer(tcb);
			tcp_loss_event(tcb);
			tcprxmit(s);
			tpriv->stats[RetransTimeouts]++;
			reset_recovery(tcb);
			break;
		case Time_wait:
			localclose(s, NULL);
			break;
		case Closed:
			break;
	}
	qunlock(&s->qlock);
	poperror();
}

int inwindow(Tcpctl * tcb, int seq)
{
	return seq_within(seq, tcb->rcv.nxt, tcb->rcv.nxt + tcb->rcv.wnd - 1);
}

/*
 *  set up state for a received SYN (or SYN ACK) packet
 */
void procsyn(struct conv *s, Tcp * seg)
{
	Tcpctl *tcb;

	tcb = (Tcpctl *) s->ptcl;
	tcb->flags |= FORCE;

	tcb->rcv.nxt = seg->seq + 1;
	tcb->rcv.urg = tcb->rcv.nxt;
	tcb->irs = seg->seq;

	/* our sending max segment size cannot be bigger than what he asked for */
	if (seg->mss != 0 && seg->mss < tcb->mss) {
		tcb->mss = seg->mss;
		tcb->typical_mss = tcb->mss;
	}
	adjust_typical_mss_for_opts(seg, tcb);

	tcb->snd.wnd = seg->wnd;
	tcb->cwind = tcb->typical_mss * CWIND_SCALE;
}

int
addreseq(Tcpctl * tcb, struct tcppriv *tpriv, Tcp * seg,
		 struct block *bp, uint16_t length)
{
	Reseq *rp, *rp1;
	int i, rqlen, qmax;

	rp = kzmalloc(sizeof(Reseq), 0);
	if (rp == NULL) {
		freeblist(bp);	/* bp always consumed by add_reseq */
		return 0;
	}

	rp->seg = *seg;
	rp->bp = bp;
	rp->length = length;

	/* Place on reassembly list sorting by starting seq number */
	rp1 = tcb->reseq;
	if (rp1 == NULL || seq_lt(seg->seq, rp1->seg.seq)) {
		rp->next = rp1;
		tcb->reseq = rp;
		if (rp->next != NULL)
			tpriv->stats[OutOfOrder]++;
		return 0;
	}

	rqlen = 0;
	for (i = 0;; i++) {
		rqlen += rp1->length;
		if (rp1->next == NULL || seq_lt(seg->seq, rp1->next->seg.seq)) {
			rp->next = rp1->next;
			rp1->next = rp;
			if (rp->next != NULL)
				tpriv->stats[OutOfOrder]++;
			break;
		}
		rp1 = rp1->next;
	}
	qmax = QMAX << tcb->rcv.scale;
	if (rqlen > qmax) {
		printd("resequence queue > window: %d > %d\n", rqlen, qmax);
		i = 0;
		for (rp1 = tcb->reseq; rp1 != NULL; rp1 = rp1->next) {
			printd("0x%#lx 0x%#lx 0x%#x\n", rp1->seg.seq,
				   rp1->seg.ack, rp1->seg.flags);
			if (i++ > 10) {
				printd("...\n");
				break;
			}
		}

		// delete entire reassembly queue; wait for retransmit.
		// - should we be smarter and only delete the tail?
		for (rp = tcb->reseq; rp != NULL; rp = rp1) {
			rp1 = rp->next;
			freeblist(rp->bp);
			kfree(rp);
		}
		tcb->reseq = NULL;

		return -1;
	}
	return 0;
}

void getreseq(Tcpctl * tcb, Tcp * seg, struct block **bp, uint16_t * length)
{
	Reseq *rp;

	rp = tcb->reseq;
	if (rp == NULL)
		return;

	tcb->reseq = rp->next;

	*seg = rp->seg;
	*bp = rp->bp;
	*length = rp->length;

	kfree(rp);
}

int tcptrim(Tcpctl * tcb, Tcp * seg, struct block **bp, uint16_t * length)
{
	uint16_t len;
	uint8_t accept;
	int dupcnt, excess;

	accept = 0;
	len = *length;
	if (seg->flags & SYN)
		len++;
	if (seg->flags & FIN)
		len++;

	if (tcb->rcv.wnd == 0) {
		if (len == 0 && seg->seq == tcb->rcv.nxt)
			return 0;
	} else {
		/* Some part of the segment should be in the window */
		if (inwindow(tcb, seg->seq))
			accept++;
		else if (len != 0) {
			if (inwindow(tcb, seg->seq + len - 1) ||
				seq_within(tcb->rcv.nxt, seg->seq, seg->seq + len - 1))
				accept++;
		}
	}
	if (!accept) {
		freeblist(*bp);
		return -1;
	}
	dupcnt = tcb->rcv.nxt - seg->seq;
	if (dupcnt > 0) {
		tcb->rerecv += dupcnt;
		if (seg->flags & SYN) {
			seg->flags &= ~SYN;
			seg->seq++;

			if (seg->urg > 1)
				seg->urg--;
			else
				seg->flags &= ~URG;
			dupcnt--;
		}
		if (dupcnt > 0) {
			pullblock(bp, (uint16_t) dupcnt);
			seg->seq += dupcnt;
			*length -= dupcnt;

			if (seg->urg > dupcnt)
				seg->urg -= dupcnt;
			else {
				seg->flags &= ~URG;
				seg->urg = 0;
			}
		}
	}
	excess = seg->seq + *length - (tcb->rcv.nxt + tcb->rcv.wnd);
	if (excess > 0) {
		tcb->rerecv += excess;
		*length -= excess;
		*bp = trimblock(*bp, 0, *length);
		if (*bp == NULL)
			panic("presotto is a boofhead");
		seg->flags &= ~FIN;
	}
	return 0;
}

void tcpadvise(struct Proto *tcp, struct block *bp, char *msg)
{
	Tcp4hdr *h4;
	Tcp6hdr *h6;
	Tcpctl *tcb;
	uint8_t source[IPaddrlen];
	uint8_t dest[IPaddrlen];
	uint16_t psource, pdest;
	struct conv *s, **p;

	h4 = (Tcp4hdr *) (bp->rp);
	h6 = (Tcp6hdr *) (bp->rp);

	if ((h4->vihl & 0xF0) == IP_VER4) {
		v4tov6(dest, h4->tcpdst);
		v4tov6(source, h4->tcpsrc);
		psource = nhgets(h4->tcpsport);
		pdest = nhgets(h4->tcpdport);
	} else {
		ipmove(dest, h6->tcpdst);
		ipmove(source, h6->tcpsrc);
		psource = nhgets(h6->tcpsport);
		pdest = nhgets(h6->tcpdport);
	}

	/* Look for a connection */
	for (p = tcp->conv; *p; p++) {
		s = *p;
		tcb = (Tcpctl *) s->ptcl;
		if (s->rport == pdest)
			if (s->lport == psource)
				if (tcb->state != Closed)
					if (ipcmp(s->raddr, dest) == 0)
						if (ipcmp(s->laddr, source) == 0) {
							qlock(&s->qlock);
							switch (tcb->state) {
								case Syn_sent:
									localclose(s, msg);
									break;
							}
							qunlock(&s->qlock);
							freeblist(bp);
							return;
						}
	}
	freeblist(bp);
}

static void tcpporthogdefensectl(char *val)
{
	if (strcmp(val, "on") == 0)
		tcpporthogdefense = 1;
	else if (strcmp(val, "off") == 0)
		tcpporthogdefense = 0;
	else
		error(EINVAL, "unknown value for tcpporthogdefense");
}

/* called with c qlocked */
static void tcpctl(struct conv *c, char **f, int n)
{
	if (n == 1 && strcmp(f[0], "hangup") == 0)
		tcphangup(c);
	else if (n >= 1 && strcmp(f[0], "keepalive") == 0)
		tcpstartka(c, f, n);
	else if (n >= 1 && strcmp(f[0], "checksum") == 0)
		tcpsetchecksum(c, f, n);
	else if (n >= 1 && strcmp(f[0], "tcpporthogdefense") == 0)
		tcpporthogdefensectl(f[1]);
	else
		error(EINVAL, "unknown command to %s", __func__);
}

int tcpstats(struct Proto *tcp, char *buf, int len)
{
	struct tcppriv *priv;
	char *p, *e;
	int i;

	priv = tcp->priv;
	p = buf;
	e = p + len;
	for (i = 0; i < Nstats; i++)
		p = seprintf(p, e, "%s: %u\n", statnames[i], priv->stats[i]);
	return p - buf;
}

/*
 *  garbage collect any stale conversations:
 *	- SYN received but no SYN-ACK after 5 seconds (could be the SYN attack)
 *	- Finwait2 after 5 minutes
 *
 *  this is called whenever we run out of channels.  Both checks are
 *  of questionable validity so we try to use them only when we're
 *  up against the wall.
 */
int tcpgc(struct Proto *tcp)
{
	struct conv *c, **pp, **ep;
	int n;
	Tcpctl *tcb;

	n = 0;
	ep = &tcp->conv[tcp->nc];
	for (pp = tcp->conv; pp < ep; pp++) {
		c = *pp;
		if (c == NULL)
			break;
		if (!canqlock(&c->qlock))
			continue;
		tcb = (Tcpctl *) c->ptcl;
		if (tcb->state == Finwait2) {
			if (NOW - tcb->time > 5 * 60 * 1000) {
				localclose(c, "timed out");
				n++;
			}
		}
		qunlock(&c->qlock);
	}
	return n;
}

void tcpsettimer(Tcpctl * tcb)
{
	int x;

	/* round trip dependency */
	x = backoff(tcb->backoff) *
		(tcb->mdev + (tcb->srtt >> LOGAGAIN) + MSPTICK) / MSPTICK;

	// XXX why?
	/* bounded twixt 1/2 and 64 seconds */
	if (x < 500 / MSPTICK) {
		//trace_printk("RETRANS timer, wanted %d, plan 9 says %d\n", x, 500 / MSPTICK);
		x = 500 / MSPTICK;
	} else if (x > (64000 / MSPTICK))
		x = 64000 / MSPTICK;
	tcb->timer.start = x;
}

static struct tcppriv *debug_priv;

/* Kfunc this */
int dump_tcp_ht(void)
{
	if (!debug_priv)
		return -1;
	dump_ipht(&debug_priv->ht);
	return 0;
}

void tcpinit(struct Fs *fs)
{
	struct Proto *tcp;
	struct tcppriv *tpriv;

	tcp = kzmalloc(sizeof(struct Proto), 0);
	tpriv = tcp->priv = kzmalloc(sizeof(struct tcppriv), 0);
	debug_priv = tpriv;
	qlock_init(&tpriv->tl);
	qlock_init(&tpriv->apl);
	tcp->name = "tcp";
	tcp->connect = tcpconnect;
	tcp->announce = tcpannounce;
	tcp->bypass = tcpbypass;
	tcp->ctl = tcpctl;
	tcp->state = tcpstate;
	tcp->create = tcpcreate;
	tcp->close = tcpclose;
	tcp->shutdown = tcpshutdown;
	tcp->rcv = tcpiput;
	tcp->advise = tcpadvise;
	tcp->stats = tcpstats;
	tcp->inuse = tcpinuse;
	tcp->gc = tcpgc;
	tcp->ipproto = IP_TCPPROTO;
	tcp->nc = 4096;
	tcp->ptclsize = sizeof(Tcpctl);
	tpriv->stats[MaxConn] = tcp->nc;

	Fsproto(fs, tcp);
}

void
tcpsetscale(struct conv *s, Tcpctl * tcb, uint16_t rcvscale, uint16_t sndscale)
{
	if (rcvscale) {
		tcb->rcv.scale = rcvscale & 0xff;
		tcb->snd.scale = sndscale & 0xff;
		// XXX what happens if they send us more than the window?  will
		// ethermedium block?  or drop?
		tcb->window = QMAX << tcb->snd.scale;
		qsetlimit(s->rq, tcb->window);
	} else {
		tcb->rcv.scale = 0;
		tcb->snd.scale = 0;
		tcb->window = QMAX;
		qsetlimit(s->rq, tcb->window);
	}
}
