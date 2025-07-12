/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2022-2023 Tomohiro Kusumi <tkusumi@netbsd.org>
 * Copyright (c) 2011-2022 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@dragonflybsd.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * HAMMER2 in-memory cache of media structures.
 *
 * This header file contains structures used internally by the HAMMER2
 * implementation.  See hammer2_disk.h for on-disk structures.
 *
 * There is an in-memory representation of all on-media data structure.
 * Almost everything is represented by a hammer2_chain structure in-memory.
 * Other higher-level structures typically map to chains.
 *
 * A great deal of data is accessed simply via its buffer cache buffer,
 * which is mapped for the duration of the chain's lock.  HAMMER2 must
 * implement its own buffer cache layer on top of the system layer to
 * allow for different threads to lock different sub-block-sized buffers.
 *
 * When modifications are made to a chain a new filesystem block must be
 * allocated.  Multiple modifications do not typically allocate new blocks
 * until the current block has been flushed.  Flushes do not block the
 * front-end unless the front-end operation crosses the current inode being
 * flushed.
 *
 * The in-memory representation may remain cached even after the related
 * data has been detached.
 */

#ifndef _FS_HAMMER2_HAMMER2_H_
#define _FS_HAMMER2_HAMMER2_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/errno.h>
#include <sys/buf.h>
#include <sys/namei.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/lock.h>
#include <sys/rwlock.h>
#include <sys/uuid.h>
#include <sys/stat.h>
#include <sys/atomic.h>

#include "hammer2_compat.h"
#include "hammer2_os.h"
#include "hammer2_disk.h"
#include "hammer2_ioctl.h"
#include "hammer2_rb.h"
//#include "dmsg.h"
#include <sys/malloc.h>

struct hammer2_io;
struct hammer2_chain;
struct hammer2_depend;
struct hammer2_inode;
struct hammer2_dev;
struct hammer2_pfs;
union hammer2_xop;

typedef struct hammer2_io hammer2_io_t;
typedef struct hammer2_chain hammer2_chain_t;
typedef struct hammer2_depend hammer2_depend_t;
typedef struct hammer2_inode hammer2_inode_t;
typedef struct hammer2_dev hammer2_dev_t;
typedef struct hammer2_pfs hammer2_pfs_t;
typedef union hammer2_xop hammer2_xop_t;

/* global list of PFS */
TAILQ_HEAD(hammer2_pfslist, hammer2_pfs); /* <-> hammer2_pfs::mntentry */
typedef struct hammer2_pfslist hammer2_pfslist_t;

/* per HAMMER2 list of device vnode */
TAILQ_HEAD(hammer2_devvp_list, hammer2_devvp); /* <-> hammer2_devvp::entry */
typedef struct hammer2_devvp_list hammer2_devvp_list_t;

/* per PFS list of inode */
LIST_HEAD(hammer2_ipdep_list, hammer2_inode); /* <-> hammer2_inode::ientry */
typedef struct hammer2_ipdep_list hammer2_ipdep_list_t;

/* per chain rbtree of sub-chain */
RB_HEAD(hammer2_chain_tree, hammer2_chain); /* <-> hammer2_chain::rbnode */
typedef struct hammer2_chain_tree hammer2_chain_tree_t;

/* per PFS list of depend */
TAILQ_HEAD(hammer2_depq_head, hammer2_depend); /* <-> hammer2_depend::entry */
typedef struct hammer2_depq_head hammer2_depq_head_t;

/* per PFS / depend list of inode */
TAILQ_HEAD(hammer2_inoq_head, hammer2_inode); /* <-> hammer2_inode::qentry */
typedef struct hammer2_inoq_head hammer2_inoq_head_t;

/*
 * Mesh network protocol structures.
 *
 *				CONN PROTOCOL
 *
 * The mesh is constructed via point-to-point streaming links with varying
 * levels of interconnectedness, forming a graph.  Leafs of the graph are
 * typically kernel devices (xdisk) or VFSs (HAMMER2).  Internal nodes are
 * usually (user level) hammer2 service demons.
 *
 * Upon connecting and after authentication, a LNK_CONN transaction is opened
 * to configure the link.  The SPAN protocol is then typically run over the
 * open LNK_CONN transaction.
 *
 * Terminating the LNK_CONN transaction terminates everything running over it
 * (typically open LNK_SPAN transactions), which in turn terminates everything
 * running over the LNK_SPANs.
 *
 *				SPAN PROTOCOL
 *
 * The SPAN protocol runs over an open LNK_CONN transaction and is used to
 * advertise any number of services.  For example, each PFS under a HAMMER2
 * mount will be advertised as an open LNK_SPAN transaction.
 *
 * Any network node on the graph running multiple connections is capable
 * of relaying LNK_SPANs from any connection to any other connection.  This
 * is typically done by the user-level hammer2 service demon, and typically
 * not done by kernel devices or VFSs (though these entities must be able
 * to manage multiple LNK_SPANs since they might advertise or need to talk
 * to multiple services).
 *
 * Relaying is not necessarily trivial as it requires internal nodes to
 * track two open transactions (on the two iocom interfaces) and translate
 * the msgid and circuit.  In addition, the relay may have to track multiple
 * SPANs from the same iocom or from multiple iocoms which represent the same
 * end-point and must select the best end-point, must send notifications when
 * a better path is available, and must allow (when connectivity is still
 * present) any existing, open, stacked sub-transactions to complete before
 * terminating the less efficient SPAN.
 *
 * Relaying is optional.  It is perfectly acceptable for the hammer2 service
 * to plug a received socket descriptor directly into the appropriate kernel
 * device driver.
 *
 *			       STACKED TRANSACTIONS
 *
 * Message transactions can be stacked.  That is, you can initiate a DMSG
 * transaction relative to another open transaction.  sub-transactions can
 * be initiate without waiting for the parent transaction to complete its
 * handshake.
 *
 * This is done by entering the open transaction's msgid as the circuit field
 * in the new transaction (typically by populating msg->parent).  The
 * transaction tracking structure will be referenced and will track the
 * sub-transaction.  Note that msgids must still be unique on an
 * iocom-by-iocom basis.
 *
 *			    MESSAGE TRANSACTIONAL STATES
 *
 * Message transactions are handled by the CREATE, DELETE, REPLY, ABORT, and
 * CREPLY flags.  Message state is typically recorded at the end points and
 * will be maintained (preventing reuse of the transaction id) until a DELETE
 * is both sent and received.
 *
 * One-way messages such as those used for debug commands are not recorded
 * and do not require any transactional state.  These are sent without
 * the CREATE, DELETE, or ABORT flags set.  ABORT is not supported for
 * one-off messages.  The REPLY bit can be used to distinguish between
 * command and status if desired.
 *
 * Transactional messages are messages which require a reply to be
 * returned.  These messages can also consist of multiple message elements
 * for the command or reply or both (or neither).  The command message
 * sequence sets CREATE on the first message and DELETE on the last message.
 * A single message command sets both (CREATE|DELETE).  The reply message
 * sequence works the same way but of course also sets the REPLY bit.
 *
 * Tansactional messages can be aborted by sending a message element
 * with the ABORT flag set.  This flag can be combined with either or both
 * the CREATE and DELETE flags.  When combined with the CREATE flag the
 * command is treated as non-blocking but still executes.  Whem combined
 * with the DELETE flag no additional message elements are required.
 *
 * Transactions are terminated by sending a message with DELETE set.
 * Transactions must be CREATEd and DELETEd in both directions.  If a
 * transaction is governing stacked sub-transactions the sub-transactions
 * are automatically terminated before the governing transaction is terminated.
 * Terminates are handled by simulating a received DELETE and expecting the
 * normal function callback and state machine to (ultimately) issue a
 * terminating (DELETE) response.
 *
 * Transactions can operate in full-duplex as both sides are fully open
 * (i.e. CREATE sent, CREATE|REPLY returned, DELETE not sent by anyone).
 * Additional commands can be initiated from either side of the transaction.
 *
 * ABORT SPECIAL CASE - Mid-stream aborts.  A mid-stream abort can be sent
 * when supported by the sender by sending an ABORT message with neither
 * CREATE or DELETE set.  This effectively turns the message into a
 * non-blocking message (but depending on what is being represented can also
 * cut short prior data elements in the stream).
 *
 * ABORT SPECIAL CASE - Abort-after-DELETE.  Transactional messages have to be
 * abortable if the stream/pipe/whatever is lost.  In this situation any
 * forwarding relay needs to unconditionally abort commands and replies that
 * are still active.  This is done by sending an ABORT|DELETE even in
 * situations where a DELETE has already been sent in that direction.  This
 * is done, for example, when links are in a half-closed state.  In this
 * situation it is possible for the abort request to race a transition to the
 * fully closed state.  ABORT|DELETE messages which race the fully closed
 * state are expected to be discarded by the other end.
 *
 * --
 *
 * All base and extended message headers are 64-byte aligned, and all
 * transports must support extended message headers up to DMSG_HDR_MAX.
 * Currently we allow extended message headers up to 2048 bytes.  Note
 * that the extended header size is encoded in the 'cmd' field of the header.
 *
 * Any in-band data is padded to a 64-byte alignment and placed directly
 * after the extended header (after the higher-level cmd/rep structure).
 * The actual unaligned size of the in-band data is encoded in the aux_bytes
 * field in this case.  Maximum data sizes are negotiated during registration.
 *
 * Auxillary data can be in-band or out-of-band.  In-band data sets aux_descr
 * equal to 0.  Any out-of-band data must be negotiated by the SPAN protocol.
 *
 * Auxillary data, whether in-band or out-of-band, must be at-least 64-byte
 * aligned.  The aux_bytes field contains the actual byte-granular length
 * and not the aligned length.  The crc is against the aligned length (so
 * a faster crc algorithm can be used, theoretically).
 *
 * hdr_crc is calculated over the entire, ALIGNED extended header.  For
 * the purposes of calculating the crc, the hdr_crc field is 0.  That is,
 * if calculating the crc in HW a 32-bit '0' must be inserted in place of
 * the hdr_crc field when reading the entire header and compared at the
 * end (but the actual hdr_crc must be left intact in memory).  A simple
 * counter to replace the field going into the CRC generator does the job
 * in HW.  The CRC endian is based on the magic number field and may have
 * to be byte-swapped, too (which is also easy to do in HW).
 *
 * aux_crc is calculated over the entire, ALIGNED auxillary data.
 *
 *			SHARED MEMORY IMPLEMENTATIONS
 *
 * Shared-memory implementations typically use a pipe to transmit the extended
 * message header and shared memory to store any auxilary data.  Auxillary
 * data in one-way (non-transactional) messages is typically required to be
 * inline.  CRCs are still recommended and required at the beginning, but
 * may be negotiated away later.
 */

struct globaldata {
        struct privatespace *gd_prvspace;       /* self-reference */
        struct thread   *gd_curthread;
        struct thread   *gd_freetd;             /* cache one free td */
        __uint32_t      gd_reqflags;            /* (see note above) */
        long            gd_flags;
        //lwkt_queue      gd_tdallq;              /* all threads */
        //lwkt_queue      gd_tdrunq;              /* runnable threads */
        __uint32_t      gd_cpuid;
        //cpumask_t       gd_cpumask;             /* mask = CPUMASK(cpuid) */
        //cpumask_t       gd_other_cpus;          /* mask of 'other' cpus */
        struct timeval  gd_stattv;
        int             gd_intr_nesting_level;  /* hard code, intrs, ipis */
        //struct vmmeter  gd_cnt;
        //struct vmtotal  gd_vmtotal;
        //cpumask_t       gd_ipimask;             /* pending ipis from cpus */
        //struct lwkt_ipiq *gd_ipiq;              /* array[ncpu] of ipiq's */
        //struct lwkt_ipiq gd_cpusyncq;           /* ipiq for cpu synchro */
        u_int           gd_npoll;               /* ipiq synchronization */
        int             gd_tdrunqcount;
        //struct thread   gd_unused02B;
        //struct thread   gd_idlethread;
        //SLGlobalData    gd_slab;                /* slab allocator */
        int             gd_trap_nesting_level;  /* track traps */
        int             gd_vme_avail;           /* vm_map_entry reservation */
        struct vm_map_entry *gd_vme_base;       /* vm_map_entry reservation */
        //struct systimerq gd_systimerq;          /* per-cpu system timers */
        int             gd_syst_nest;
        //struct systimer gd_hardclock;           /* scheduler periodic */
        //struct systimer gd_statclock;           /* statistics periodic */
        //struct systimer gd_schedclock;          /* scheduler periodic */
        volatile __uint32_t gd_time_seconds;    /* uptime in seconds */
        //volatile sysclock_t gd_cpuclock_base;   /* cpuclock relative base */

        struct pipe     *gd_pipeq;              /* cache pipe structures */
        struct nchstats *gd_nchstats;           /* namecache effectiveness */
        int             gd_pipeqcount;          /* number of structures */
        //sysid_t         gd_sysid_alloc;         /* allocate unique sysid */

        struct tslpque  *gd_tsleep_hash;        /* tsleep/wakeup support */
        long            gd_processing_ipiq;
        int             gd_spinlocks;           /* Exclusive spinlocks held */
        struct systimer *gd_systimer_inprog;    /* in-progress systimer */
        int             gd_timer_running;
        u_int           gd_idle_repeat;         /* repeated switches to idle */
        int             gd_ireserved[7];
        const char      *gd_infomsg;            /* debugging */
        //struct lwkt_tokref gd_handoff;          /* hand-off tokref */
        void            *gd_delayed_wakeup[2];
        void            *gd_sample_pc;          /* sample program ctr/tr */
        void            *gd_preserved[5];       /* future fields */
        /* extended by <machine/globaldata.h> */
};

struct thread {
    TAILQ_ENTRY(thread) td_threadq;
    TAILQ_ENTRY(thread) td_allq;
    TAILQ_ENTRY(thread) td_sleepq;
    struct proc *td_proc;       /* (optional) associated process */
    struct pcb  *td_pcb;        /* points to pcb and top of kstack */
    struct globaldata *td_gd;   /* associated with this cpu */
    const char  *td_wmesg;      /* string name for blockage */
    const volatile void *td_wchan;      /* waiting on channel */
    int         td_pri;         /* 0-31, 31=highest priority (note 1) */
    int         td_critcount;   /* critical section priority */
    u_int       td_flags;       /* TDF flags */
    int         td_wdomain;     /* domain for wchan address (typ 0) */
    char        *td_kstack;     /* kernel stack */
    int         td_kstack_size; /* size of kernel stack */
    char        *td_sp;         /* kernel stack pointer for LWKT restore */
    __uint64_t  td_uticks;      /* Statclock hits in user mode (uS) */
    __uint64_t  td_sticks;      /* Statclock hits in system mode (uS) */
    __uint64_t  td_iticks;      /* Statclock hits processing intr (uS) */
    int         td_locks;       /* lockmgr lock debugging */
    void        *td_dsched_priv1;       /* priv data for I/O schedulers */
    int         td_refs;        /* hold position in gd_tdallq / hold free */
    int         td_nest_count;  /* prevent splz nesting */
    int         td_contended;   /* token contention count */
    u_int       td_mpflags;     /* flags can be set by foreign cpus */
    int         td_cscount;     /* cpu synchronization master */
    int         td_wakefromcpu; /* who woke me up? */
    int         td_upri;        /* user priority (sub-priority under td_pri) */
    int         td_type;        /* thread type, TD_TYPE_ */
    struct timeval td_start;    /* start time for a thread/process */
    char        td_comm[MAXCOMLEN+1]; /* typ 16+1 bytes */
    struct thread *td_preempted; /* we preempted this thread */
    struct ucred *td_ucred;             /* synchronized from p_ucred */
    void         *td_vmm;       /* vmm private data */
    int         td_fairq_load;          /* fairq */
    int         td_fairq_count;         /* fairq */
    struct globaldata *td_migrate_gd;   /* target gd for thread migration */
};

typedef struct thread *thread_t;

#define DMSG_TERMINATE_STRING(ary)	\
	do { (ary)[sizeof(ary) - 1] = 0; } while (0)

/*
 * dmsg_hdr must be 64 bytes
 */
struct dmsg_hdr {
	uint16_t	magic;		/* 00 sanity, synchro, endian */
	uint16_t	reserved02;	/* 02 */
	uint32_t	salt;		/* 04 random salt helps w/crypto */

	uint64_t	msgid;		/* 08 message transaction id */
	uint64_t	circuit;	/* 10 circuit id or 0	*/
	uint64_t	reserved18;	/* 18 */

	uint32_t	cmd;		/* 20 flags | cmd | hdr_size / ALIGN */
	uint32_t	aux_crc;	/* 24 auxillary data crc */
	uint32_t	aux_bytes;	/* 28 auxillary data length (bytes) */
	uint32_t	error;		/* 2C error code or 0 */
	uint64_t	aux_descr;	/* 30 negotiated OOB data descr */
	uint32_t	reserved38;	/* 38 */
	uint32_t	hdr_crc;	/* 3C (aligned) extended header crc */
};

typedef struct dmsg_hdr dmsg_hdr_t;

#define DMSG_HDR_MAGIC		0x4832
#define DMSG_HDR_MAGIC_REV	0x3248
#define DMSG_HDR_CRCOFF		offsetof(dmsg_hdr_t, salt)
#define DMSG_HDR_CRCBYTES	(sizeof(dmsg_hdr_t) - DMSG_HDR_CRCOFF)

/*
 * Administrative protocol limits.
 */
#define DMSG_HDR_MAX		2048	/* <= 65535 */
#define DMSG_AUX_MAX		65536	/* <= 1MB */
#define DMSG_BUF_SIZE		(DMSG_HDR_MAX * 4)
#define DMSG_BUF_MASK		(DMSG_BUF_SIZE - 1)

/*
 * The message (cmd) field also encodes various flags and the total size
 * of the message header.  This allows the protocol processors to validate
 * persistency and structural settings for every command simply by
 * switch()ing on the (cmd) field.
 */
#define DMSGF_CREATE		0x80000000U	/* msg start */
#define DMSGF_DELETE		0x40000000U	/* msg end */
#define DMSGF_REPLY		0x20000000U	/* reply path */
#define DMSGF_ABORT		0x10000000U	/* abort req */
#define DMSGF_REVTRANS		0x08000000U	/* opposite direction msgid */
#define DMSGF_REVCIRC		0x04000000U	/* opposite direction circuit */
#define DMSGF_FLAG1		0x02000000U
#define DMSGF_FLAG0		0x01000000U

#define DMSGF_FLAGS		0xFF000000U	/* all flags */
#define DMSGF_PROTOS		0x00F00000U	/* all protos */
#define DMSGF_CMDS		0x000FFF00U	/* all cmds */
#define DMSGF_SIZE		0x000000FFU	/* N*32 */

/*
 * XXX Future, flag that an in-line (not part of a CREATE/DELETE) command
 *     expects some sort of acknowledgement.  Allows protocol mismatches to
 *     be detected.
 */
#define DMSGF_CMDF_EXPECT_ACK	0x00080000U	/* in-line command no-ack */

#define DMSGF_CMDSWMASK		(DMSGF_CMDS |	\
					 DMSGF_SIZE |	\
					 DMSGF_PROTOS |	\
					 DMSGF_REPLY)

#define DMSGF_BASECMDMASK	(DMSGF_CMDS |	\
					 DMSGF_SIZE |	\
					 DMSGF_PROTOS)

#define DMSGF_TRANSMASK		(DMSGF_CMDS |	\
					 DMSGF_SIZE |	\
					 DMSGF_PROTOS |	\
					 DMSGF_REPLY |	\
					 DMSGF_CREATE |	\
					 DMSGF_DELETE)

#define DMSGF_BASEFLAGS		(DMSGF_CREATE | DMSGF_DELETE | DMSGF_REPLY)

#define DMSG_PROTO_LNK		0x00000000U
#define DMSG_PROTO_DBG		0x00100000U
#define DMSG_PROTO_HM2		0x00200000U
#define DMSG_PROTO_XX3		0x00300000U
#define DMSG_PROTO_XX4		0x00400000U
#define DMSG_PROTO_BLK		0x00500000U
#define DMSG_PROTO_VOP		0x00600000U

/*
 * Message command constructors, sans flags
 */
#define DMSG_ALIGN		64
#define DMSG_ALIGNMASK		(DMSG_ALIGN - 1)
#define DMSG_DOALIGN(bytes)	(((bytes) + DMSG_ALIGNMASK) &		\
				 ~DMSG_ALIGNMASK)

#define DMSG_HDR_ENCODE(elm)	(((uint32_t)sizeof(struct elm) +	\
				  DMSG_ALIGNMASK) /			\
				 DMSG_ALIGN)

#define DMSG_LNK(cmd, elm)	(DMSG_PROTO_LNK |			\
					 ((cmd) << 8) | 		\
					 DMSG_HDR_ENCODE(elm))

#define DMSG_DBG(cmd, elm)	(DMSG_PROTO_DBG |			\
					 ((cmd) << 8) | 		\
					 DMSG_HDR_ENCODE(elm))

#define DMSG_HM2(cmd, elm)	(DMSG_PROTO_HM2 |			\
					 ((cmd) << 8) | 		\
					 DMSG_HDR_ENCODE(elm))

#define DMSG_BLK(cmd, elm)	(DMSG_PROTO_BLK |			\
					 ((cmd) << 8) | 		\
					 DMSG_HDR_ENCODE(elm))

#define DMSG_VOP(cmd, elm)	(DMSG_PROTO_VOP |			\
					 ((cmd) << 8) | 		\
					 DMSG_HDR_ENCODE(elm))

/*
 * Link layer ops basically talk to just the other side of a direct
 * connection.
 *
 * LNK_PAD	- One-way message on circuit 0, ignored by target.  Used to
 *		  pad message buffers on shared-memory transports.  Not
 *		  typically used with TCP.
 *
 * LNK_PING	- One-way message on circuit-0, keep-alive, run by both sides
 *		  typically 1/sec on idle link, link is lost after 10 seconds
 *		  of inactivity.
 *
 * LNK_AUTH	- Authenticate the connection, negotiate administrative
 *		  rights & encryption, protocol class, etc.  Only PAD and
 *		  AUTH messages (not even PING) are accepted until
 *		  authentication is complete.  This message also identifies
 *		  the host.
 *
 * LNK_CONN	- Enable the SPAN protocol on circuit-0, possibly also
 *		  installing a PFS filter (by cluster id, unique id, and/or
 *		  wildcarded name).
 *
 * LNK_SPAN	- A SPAN transaction typically on iocom->state0 enables
 *		  messages to be relayed to/from a particular cluster node.
 *		  SPANs are received, sorted, aggregated, filtered, and
 *		  retransmitted back out across all applicable connections.
 *
 *		  The leaf protocol also uses this to make a PFS available
 *		  to the cluster (e.g. on-mount).
 */
#define DMSG_LNK_PAD		DMSG_LNK(0x000, dmsg_hdr)
#define DMSG_LNK_PING		DMSG_LNK(0x001, dmsg_hdr)
#define DMSG_LNK_AUTH		DMSG_LNK(0x010, dmsg_lnk_auth)
#define DMSG_LNK_CONN		DMSG_LNK(0x011, dmsg_lnk_conn)
#define DMSG_LNK_SPAN		DMSG_LNK(0x012, dmsg_lnk_span)
#define DMSG_LNK_ERROR		DMSG_LNK(0xFFF, dmsg_hdr)

/*
 * Reserved command codes for third party subsystems.  Structure size is
 * not known here so do not try to construct the full DMSG_LNK_ define.
 */
#define DMSG_LNK_CMD_HAMMER2_VOLCONF	0x20

#define DMSG_LABEL_SIZE		128	/* fixed at 128, do not change */

typedef struct uuid uuid_t;

/*
 * LNK_AUTH - Authentication (often omitted)
 */
struct dmsg_lnk_auth {
	dmsg_hdr_t	head;
	char		dummy[64];
};

/*
 * LNK_CONN - Register connection info for SPAN protocol
 *	      (transaction, left open, iocom->state0 only).
 *
 * LNK_CONN identifies a streaming connection into the cluster and serves
 * to identify, enable, and specify filters for the SPAN protocol.
 *
 * peer_mask serves to filter the SPANs we receive by peer_type.  A cluster
 * controller typically sets this to (uint64_t)-1, indicating that it wants
 * everything.  A block devfs interface might set it to 1 << DMSG_PEER_DISK,
 * and a hammer2 mount might set it to 1 << DMSG_PEER_HAMMER2.
 *
 * mediaid allows multiple (e.g. HAMMER2) connections belonging to the same
 * media to transmit duplicative LNK_VOLCONF updates without causing
 * confusion in the cluster controller.
 *
 * pfs_clid, pfs_fsid, pfs_type, and label are peer-specific and must be
 * left empty (zero-fill) if not supported by a particular peer.
 *
 * DMSG_PEER_CLUSTER		filter: none
 * DMSG_PEER_BLOCK		filter: label
 * DMSG_PEER_HAMMER2		filter: pfs_clid if not empty, and label
 */
struct dmsg_lnk_conn {
	dmsg_hdr_t	head;
	uuid_t		media_id;	/* media configuration id */
	uuid_t		peer_id;	/* unique peer uuid */
	uuid_t		reserved01;
	uint64_t	peer_mask;	/* PEER mask for SPAN filtering */
	uint8_t		peer_type;	/* see DMSG_PEER_xxx */
	uint8_t		reserved02;
	uint16_t	proto_version;	/* high level protocol support */
	uint32_t	status;		/* status flags */
	uint32_t	rnss;		/* node's generated rnss */
	uint8_t		reserved03[8];
	uint32_t	reserved04[14];
	char		peer_label[DMSG_LABEL_SIZE]; /* peer identity string */
};

typedef struct dmsg_lnk_conn dmsg_lnk_conn_t;

/*
 * PFSTYPEs 0-15 used by sys/dmsg.h 16-31 reserved by hammer2.
 */
#define DMSG_PFSTYPE_NONE		0
#define DMSG_PFSTYPE_ADMIN		1
#define DMSG_PFSTYPE_CLIENT		2
#define DMSG_PFSTYPE_SERVER		3
#define DMSG_PFSTYPE_MAX		32

#define DMSG_PEER_NONE		0
#define DMSG_PEER_CLUSTER	1	/* a cluster controller */
#define DMSG_PEER_BLOCK		2	/* block devices */
#define DMSG_PEER_HAMMER2	3	/* hammer2-mounted volumes */

/*
 * Structures embedded in LNK_SPAN
 */
struct dmsg_media_block {
	uint64_t	bytes;		/* media size in bytes */
	uint32_t	blksize;	/* media block size */
};

typedef struct dmsg_media_block dmsg_media_block_t;

/*
 * LNK_SPAN - Initiate or relay a SPAN
 *	      (transaction, left open, typically only on iocom->state0)
 *
 * This message registers an end-point with the other end of the connection,
 * telling the other end who we are and what we can provide or intend to
 * consume.  Multiple registrations can be maintained as open transactions
 * with each one specifying a unique end-point.
 *
 * Registrations are sent from {source}=S {1...n} to {target}=0 and maintained
 * as open transactions.  Registrations are also received and maintains as
 * open transactions, creating a matrix of linkid's.
 *
 * While these transactions are open additional transactions can be executed
 * between any two linkid's {source}=S (registrations we sent) to {target}=T
 * (registrations we received).
 *
 * Closure of any registration transaction will automatically abort any open
 * transactions using the related linkids.  Closure can be initiated
 * voluntarily from either side with either end issuing a DELETE, or they
 * can be ABORTed.
 *
 * Status updates are performed via the open transaction.
 *
 * --
 *
 * A registration identifies a node and its various PFS parameters including
 * the PFS_TYPE.  For example, a diskless HAMMER2 client typically identifies
 * itself as PFSTYPE_CLIENT.
 *
 * Any node may serve as a cluster controller, aggregating and passing
 * on received registrations, but end-points do not have to implement this
 * ability.  Most end-points typically implement a single client-style or
 * server-style PFS_TYPE and rendezvous at a cluster controller.
 *
 * The cluster controller does not aggregate/pass-on all received
 * registrations.  It typically filters what gets passed on based on what it
 * receives, passing on only the best candidates.
 *
 * If a symmetric spanning tree is desired additional candidates whos
 * {dist, rnss} fields match the last best candidate must also be propagated.
 * This feature is not currently enabled.
 *
 * STATUS UPDATES: Status updates use the same structure but typically
 *		   only contain incremental changes to e.g. pfs_type, with
 *		   a text description sent as out-of-band data.
 */
struct dmsg_lnk_span {
	dmsg_hdr_t	head;
	uuid_t		peer_id;
	uuid_t		pfs_id;		/* unique pfs id */
	uint8_t		pfs_type;	/* PFS type */
	uint8_t		peer_type;	/* PEER type */
	uint16_t	proto_version;	/* high level protocol support */
	uint32_t	status;		/* status flags */
	uint8_t		reserved02[8];
	uint32_t	dist;		/* span distance */
	uint32_t	rnss;		/* random number sub-sort */
	union {
		uint32_t	reserved03[14];
		dmsg_media_block_t block;
	} media;

	/*
	 * NOTE: for PEER_HAMMER2 cl_label is typically empty and fs_label
	 *	 is the superroot directory name.
	 *
	 *	 for PEER_BLOCK cl_label is typically host/device and
	 *	 fs_label is typically the serial number string.
	 */
	char		peer_label[DMSG_LABEL_SIZE];	/* peer label */
	char		pfs_label[DMSG_LABEL_SIZE];	/* PFS label */
};

typedef struct dmsg_lnk_span dmsg_lnk_span_t;

#define DMSG_SPAN_PROTO_1	1

/*
 * Debug layer ops operate on any link
 *
 * SHELL	- Persist stream, access the debug shell on the target
 *		  registration.  Multiple shells can be operational.
 */
#define DMSG_DBG_SHELL		DMSG_DBG(0x001, dmsg_dbg_shell)

struct dmsg_dbg_shell {
	dmsg_hdr_t	head;
};
typedef struct dmsg_dbg_shell dmsg_dbg_shell_t;

/*
 * Hammer2 layer ops (low-level chain manipulation used by cluster code)
 *
 * HM2_OPENPFS	- Attach a PFS
 * HM2_FLUSHPFS - Flush a PFS
 *
 * HM2_LOOKUP	- Lookup chain (parent-relative transaction)
 *		  (can request multiple chains)
 * HM2_NEXT	- Lookup next chain (parent-relative transaction)
 *		  (can request multiple chains)
 * HM2_LOCK	- [Re]lock a chain (chain-relative) (non-recursive)
 * HM2_UNLOCK	- Unlock a chain (chain-relative) (non-recursive)
 * HM2_RESIZE	- Resize a chain (chain-relative)
 * HM2_MODIFY	- Modify a chain (chain-relative)
 * HM2_CREATE	- Create a chain (parent-relative)
 * HM2_DUPLICATE- Duplicate a chain (target-parent-relative)
 * HM2_DELDUP	- Delete-Duplicate a chain (chain-relative)
 * HM2_DELETE	- Delete a chain (chain-relative)
 * HM2_SNAPSHOT	- Create a snapshot (snapshot-root-relative, w/clid override)
 */
#define DMSG_HM2_OPENPFS	DMSG_HM2(0x001, dmsg_hm2_openpfs)

/*
 * DMSG_PROTO_BLK Protocol
 *
 * BLK_OPEN	- Open device.  This transaction must be left open for the
 *		  duration and the returned keyid passed in all associated
 *		  BLK commands.  Multiple OPENs can be issued within the
 *		  transaction.
 *
 * BLK_CLOSE	- Close device.  This can be used to close one of the opens
 *		  within a BLK_OPEN transaction.  It may NOT initiate a
 *		  transaction.  Note that a termination of the transaction
 *		  (e.g. with LNK_ERROR or BLK_ERROR) closes all active OPENs
 *		  for that transaction.  XXX not well defined atm.
 *
 * BLK_READ	- Strategy read.  Not typically streaming.
 *
 * BLK_WRITE	- Strategy write.  Not typically streaming.
 *
 * BLK_FLUSH	- Strategy flush.  Not typically streaming.
 *
 * BLK_FREEBLKS	- Strategy freeblks.  Not typically streaming.
 */
#define DMSG_BLK_OPEN		DMSG_BLK(0x001, dmsg_blk_open)
#define DMSG_BLK_CLOSE		DMSG_BLK(0x002, dmsg_blk_open)
#define DMSG_BLK_READ		DMSG_BLK(0x003, dmsg_blk_read)
#define DMSG_BLK_WRITE		DMSG_BLK(0x004, dmsg_blk_write)
#define DMSG_BLK_FLUSH		DMSG_BLK(0x005, dmsg_blk_flush)
#define DMSG_BLK_FREEBLKS	DMSG_BLK(0x006, dmsg_blk_freeblks)
#define DMSG_BLK_ERROR		DMSG_BLK(0xFFF, dmsg_blk_error)

struct dmsg_blk_open {
	dmsg_hdr_t	head;
	uint32_t	modes;
	uint32_t	reserved01;
};

#define DMSG_BLKOPEN_RD		0x0001
#define DMSG_BLKOPEN_WR		0x0002

/*
 * DMSG_LNK_ERROR is returned for simple results,
 * DMSG_BLK_ERROR is returned for extended results.
 */
struct dmsg_blk_error {
	dmsg_hdr_t	head;
	uint64_t	keyid;
	uint32_t	resid;
	uint32_t	reserved02;
	char		buf[64];
};

struct dmsg_blk_read {
	dmsg_hdr_t	head;
	uint64_t	keyid;
	uint64_t	offset;
	uint32_t	bytes;
	uint32_t	flags;
	uint32_t	reserved01;
	uint32_t	reserved02;
};

struct dmsg_blk_write {
	dmsg_hdr_t	head;
	uint64_t	keyid;
	uint64_t	offset;
	uint32_t	bytes;
	uint32_t	flags;
	uint32_t	reserved01;
	uint32_t	reserved02;
};

struct dmsg_blk_flush {
	dmsg_hdr_t	head;
	uint64_t	keyid;
	uint64_t	offset;
	uint32_t	bytes;
	uint32_t	flags;
	uint32_t	reserved01;
	uint32_t	reserved02;
};

struct dmsg_blk_freeblks {
	dmsg_hdr_t	head;
	uint64_t	keyid;
	uint64_t	offset;
	uint32_t	bytes;
	uint32_t	flags;
	uint32_t	reserved01;
	uint32_t	reserved02;
};

typedef struct dmsg_blk_open		dmsg_blk_open_t;
typedef struct dmsg_blk_read		dmsg_blk_read_t;
typedef struct dmsg_blk_write		dmsg_blk_write_t;
typedef struct dmsg_blk_flush		dmsg_blk_flush_t;
typedef struct dmsg_blk_freeblks	dmsg_blk_freeblks_t;
typedef struct dmsg_blk_error		dmsg_blk_error_t;

/*
 * NOTE!!!! ALL EXTENDED HEADER STRUCTURES MUST BE 64-BYTE ALIGNED!!!
 *
 * General message errors
 *
 *	0x00 - 0x1F	Local iocomm errors
 *	0x20 - 0x2F	Global errors
 */
#define DMSG_ERR_NOSUPP		0x20
#define DMSG_ERR_LOSTLINK	0x21
#define DMSG_ERR_IO		0x22	/* generic */
#define DMSG_ERR_PARAM		0x23	/* generic */
#define DMSG_ERR_CANTCIRC	0x24	/* (typically means lost span) */

union dmsg_any {
	char			buf[DMSG_HDR_MAX];
	dmsg_hdr_t		head;

	dmsg_lnk_conn_t		lnk_conn;
	dmsg_lnk_span_t		lnk_span;

	dmsg_blk_open_t		blk_open;
	dmsg_blk_error_t	blk_error;
	dmsg_blk_read_t		blk_read;
	dmsg_blk_write_t	blk_write;
	dmsg_blk_flush_t	blk_flush;
	dmsg_blk_freeblks_t	blk_freeblks;
};

typedef union dmsg_any dmsg_any_t;

/*
 * Kernel iocom structures and prototypes for kern/kern_dmsg.c
 */
#if defined(_KERNEL) || defined(_KERNEL_STRUCTURES)

struct hammer2_mount;
struct xa_softc;
struct kdmsg_iocom;
struct kdmsg_state;
struct kdmsg_msg;

/*
 * msg_ctl flags (atomic)
 */
#define KDMSG_CLUSTERCTL_KILL		0x00000001
#define KDMSG_CLUSTERCTL_KILLRX		0x00000002 /* staged helper exit */
#define KDMSG_CLUSTERCTL_KILLTX		0x00000004 /* staged helper exit */
#define KDMSG_CLUSTERCTL_SLEEPING	0x00000008 /* interlocked w/msglk */

/*
 * Transactional state structure, representing an open transaction.  The
 * transaction might represent a cache state (and thus have a chain
 * association), or a VOP op, LNK_SPAN, or other things.
 */
TAILQ_HEAD(kdmsg_state_list, kdmsg_state);

struct kdmsg_state {
	RB_ENTRY(kdmsg_state) rbnode;		/* indexed by msgid */
	struct kdmsg_state_list	subq;		/* active stacked states */
	TAILQ_ENTRY(kdmsg_state) entry;		/* on parent subq */
	TAILQ_ENTRY(kdmsg_state) user_entry;	/* available to devices */
	struct kdmsg_iocom *iocom;
	struct kdmsg_state *parent;
	uint32_t	icmd;			/* record cmd creating state */
	uint32_t	txcmd;			/* mostly for CMDF flags */
	uint32_t	rxcmd;			/* mostly for CMDF flags */
	uint64_t	msgid;			/* {parent,msgid} uniq */
	int		flags;
	int		error;
	void		*chain;			/* (caller's state) */
	int (*func)(struct kdmsg_state *, struct kdmsg_msg *);
	union {
		void *any;
		struct hammer2_mount *hmp;
		struct xa_softc *xa_sc;
	} any;
};

#define KDMSG_STATE_INSERTED	0x0001
#define KDMSG_STATE_DYNAMIC	0x0002
#define KDMSG_STATE_DELPEND	0x0004		/* transmit delete pending */
#define KDMSG_STATE_ABORTING	0x0008		/* avoids recursive abort */
#define KDMSG_STATE_OPPOSITE	0x0010		/* opposite direction */
#define KDMSG_STATE_DYING	0x0020		/* indicates circuit failure */

struct kdmsg_msg {
	TAILQ_ENTRY(kdmsg_msg) qentry;		/* serialized queue */
	struct kdmsg_state *state;
	size_t		hdr_size;
	size_t		aux_size;
	char		*aux_data;
	uint32_t	flags;
	uint32_t	tcmd;			/* outer transaction cmd */
	dmsg_any_t	any;			/* variable sized */
};

#define KDMSG_FLAG_AUXALLOC	0x0001

typedef struct kdmsg_link kdmsg_link_t;
typedef struct kdmsg_state kdmsg_state_t;
typedef struct kdmsg_msg kdmsg_msg_t;

struct kdmsg_state_tree;
int kdmsg_state_cmp(kdmsg_state_t *state1, kdmsg_state_t *state2);
RB_HEAD(kdmsg_state_tree, kdmsg_state);
RB_PROTOTYPE(kdmsg_state_tree, kdmsg_state, rbnode, kdmsg_state_cmp);

/*
 * Structure embedded in e.g. mount, master control structure for
 * DMSG stream handling.
 */
struct kdmsg_iocom {
	struct malloc_type	*mmsg;
	struct file		*msg_fp;	/* cluster pipe->userland */
	thread_t		msgrd_td;	/* cluster thread */
	thread_t		msgwr_td;	/* cluster thread */
	int			msg_ctl;	/* wakeup flags */
	int			msg_seq;	/* cluster msg sequence id */
	uint32_t		flags;
	struct rwlock 		msglk;		/* lockmgr lock */
	TAILQ_HEAD(, kdmsg_msg) msgq;		/* transmit queue */
	void			*handle;
	void			(*auto_callback)(kdmsg_msg_t *);
	int			(*rcvmsg)(kdmsg_msg_t *);
	void			(*exit_func)(struct kdmsg_iocom *);
	struct kdmsg_state	state0;		/* root state for stacking */
	struct kdmsg_state	*conn_state;	/* active LNK_CONN state */
	struct kdmsg_state	*freerd_state;	/* allocation cache */
	struct kdmsg_state	*freewr_state;	/* allocation cache */
	struct kdmsg_state_tree staterd_tree;	/* active messages */
	struct kdmsg_state_tree statewr_tree;	/* active messages */
	dmsg_lnk_conn_t		auto_lnk_conn;
	dmsg_lnk_span_t		auto_lnk_span;
};

typedef struct kdmsg_iocom	kdmsg_iocom_t;

#define KDMSG_IOCOMF_AUTOCONN	0x0001	/* handle RX/TX LNK_CONN */
#define KDMSG_IOCOMF_AUTORXSPAN	0x0002	/* handle RX LNK_SPAN */
#define KDMSG_IOCOMF_AUTOTXSPAN	0x0008	/* handle TX LNK_SPAN */
#define KDMSG_IOCOMF_EXITNOACC	0x8000	/* cannot accept writes */

#define KDMSG_IOCOMF_AUTOANY	(KDMSG_IOCOMF_AUTOCONN |	\
				 KDMSG_IOCOMF_AUTORXSPAN |	\
				 KDMSG_IOCOMF_AUTOTXSPAN)

uint32_t kdmsg_icrc32(const void *buf, size_t size);
uint32_t kdmsg_icrc32c(const void *buf, size_t size, uint32_t crc);

/*
 * kern_dmsg.c
 */
void kdmsg_iocom_init(kdmsg_iocom_t *iocom, void *handle, u_int32_t flags,
			struct malloc_type *mmsg,
			int (*rcvmsg)(kdmsg_msg_t *msg));
void kdmsg_iocom_reconnect(kdmsg_iocom_t *iocom, struct file *fp,
			const char *subsysname);
void kdmsg_iocom_autoinitiate(kdmsg_iocom_t *iocom,
			void (*conn_callback)(kdmsg_msg_t *msg));
void kdmsg_iocom_uninit(kdmsg_iocom_t *iocom);
void kdmsg_drain_msgq(kdmsg_iocom_t *iocom);

void kdmsg_msg_free(kdmsg_msg_t *msg);
kdmsg_msg_t *kdmsg_msg_alloc(kdmsg_state_t *state, uint32_t cmd,
				int (*func)(kdmsg_state_t *, kdmsg_msg_t *),
				void *data);
void kdmsg_msg_write(kdmsg_msg_t *msg);
void kdmsg_msg_reply(kdmsg_msg_t *msg, uint32_t error);
void kdmsg_msg_result(kdmsg_msg_t *msg, uint32_t error);
void kdmsg_state_reply(kdmsg_state_t *state, uint32_t error);
void kdmsg_state_result(kdmsg_state_t *state, uint32_t error);

#endif

/*
 * Cap the dynamic calculation for the maximum number of dirty
 * chains and dirty inodes allowed.
 */
#define HAMMER2_LIMIT_DIRTY_CHAINS	(1024*1024)
#define HAMMER2_LIMIT_DIRTY_INODES	(65536)

#define HAMMER2_IOHASH_SIZE		1024	/* OpenBSD: originally 32768 */
#define HAMMER2_IOHASH_MASK		(HAMMER2_IOHASH_SIZE - 1)

#define HAMMER2_INUMHASH_SIZE		1024	/* OpenBSD: originally 32768 */
#define HAMMER2_INUMHASH_MASK		(HAMMER2_IOHASH_SIZE - 1)

#define HAMMER2_DIRTYCHAIN_MASK		0x7FFFFFFF
#define HAMMER2_DIRTYCHAIN_WAITING	0x80000000
#define PINTERLOCKED	0x00000400	/* Interlocked tsleep */
/*
 * HAMMER2 dio - Management structure wrapping system buffer cache.
 *
 * HAMMER2 uses an I/O abstraction that allows it to cache and manipulate
 * fixed-sized filesystem buffers frontend by variable-sized hammer2_chain
 * structures.
 *
 * Note that DragonFly uses atomic + interlock for refs, atomic for
 * dedup_xxx, whereas other BSD's protect them with dio lock.
 */
struct hammer2_io {
	struct hammer2_io	*next;
	hammer2_mtx_t		lock;
	hammer2_dev_t		*hmp;
	struct vnode		*devvp;
	struct buf		*bp;
	uint32_t		refs;
	hammer2_off_t		dbase;		/* offset of devvp within volumes */
	hammer2_off_t		pbase;
	int			psize;
	int			act;		/* activity */
	int			btype;
	int			ticks;
	int			error;
	uint64_t		dedup_valid;	/* valid for dedup operation */
	uint64_t		dedup_alloc;	/* allocated / de-dupable */
};

struct hammer2_io_hash {
	hammer2_spin_t		spin;
	struct hammer2_io	*base;
};

typedef struct hammer2_io_hash	hammer2_io_hash_t;

#define HAMMER2_DIO_GOOD	0x40000000U	/* dio->bp is stable */
#define HAMMER2_DIO_DIRTY	0x10000000U	/* flush last drop */
#define HAMMER2_DIO_FLUSH	0x08000000U	/* immediate flush */
#define HAMMER2_DIO_MASK	0x00FFFFFFU
#define NOOFFSET	(-1LL)

struct hammer2_inum_hash {
	hammer2_spin_t		spin;
	struct hammer2_inode	*base;
};

typedef struct hammer2_inum_hash hammer2_inum_hash_t;

/*
 * The chain structure tracks a portion of the media topology from the
 * root (volume) down.  Chains represent volumes, inodes, indirect blocks,
 * data blocks, and freemap nodes and leafs.
 */
struct hammer2_reptrack {
	struct hammer2_reptrack	*next;
	hammer2_chain_t		*chain;
	hammer2_spin_t		spin;
};

typedef struct hammer2_reptrack hammer2_reptrack_t;

/*
 * Core topology for chain (embedded in chain).  Protected by a spinlock.
 */
struct hammer2_chain_core {
	hammer2_reptrack_t	*reptrack;
	hammer2_chain_tree_t	rbtree;		/* sub-chains */
	hammer2_spin_t		spin;
	int			live_zero;	/* blockref array opt */
	unsigned int		live_count;	/* live (not deleted) chains in tree */
	unsigned int		chain_count;	/* live + deleted chains under core */
	int			generation;	/* generation number (inserts only) */
};

typedef struct hammer2_chain_core hammer2_chain_core_t;

/*
 * Primary chain structure keeps track of the topology in-memory.
 */
struct hammer2_chain {
	RB_ENTRY(hammer2_chain) rbnode;		/* live chain(s) */
	hammer2_mtx_t		lock;
	hammer2_mtx_t		diolk;		/* xop focus interlock */
	hammer2_lk_t		inp_lock;
	hammer2_lkc_t		inp_cv;
	hammer2_chain_core_t	core;
	hammer2_blockref_t	bref;
	hammer2_dev_t		*hmp;
	hammer2_pfs_t		*pmp;		/* A PFS or super-root (spmp) */
	hammer2_chain_t		*parent;
	hammer2_io_t		*dio;		/* physical data buffer */
	hammer2_media_data_t	*data;		/* data pointer shortcut */
	unsigned int		refs;
	unsigned int		lockcnt;
	unsigned int		flags;		/* for HAMMER2_CHAIN_xxx */
	unsigned int		bytes;		/* physical data size */
	int			error;		/* on-lock data error state */
	int			cache_index;	/* heur speeds up lookup */
};

/*
 * Passed to hammer2_chain_create(), causes methods to be inherited from
 * parent.
 */
#define HAMMER2_METH_DEFAULT		-1

/*
 * Special notes on flags:
 *
 * INITIAL	- This flag allows a chain to be created and for storage to
 *		  be allocated without having to immediately instantiate the
 *		  related buffer.  The data is assumed to be all-zeros.  It
 *		  is primarily used for indirect blocks.
 *
 * MODIFIED	- The chain's media data has been modified.  Prevents chain
 *		  free on lastdrop if still in the topology.
 *
 * UPDATE	- Chain might not be modified but parent blocktable needs
 *		  an update.  Prevents chain free on lastdrop if still in
 *		  the topology.
 *
 * BLKMAPPED	- Indicates that the chain is present in the parent blockmap.
 *
 * BLKMAPUPD	- Indicates that the chain is present but needs to be updated
 *		  in the parent blockmap.
 */
#define HAMMER2_CHAIN_MODIFIED		0x00000001	/* dirty chain data */
#define HAMMER2_CHAIN_ALLOCATED		0x00000002	/* kmalloc'd chain */
#define HAMMER2_CHAIN_DESTROY		0x00000004
#define HAMMER2_CHAIN_DEDUPABLE		0x00000008	/* registered w/dedup */
#define HAMMER2_CHAIN_DELETED		0x00000010	/* deleted chain */
#define HAMMER2_CHAIN_INITIAL		0x00000020	/* initial create */
#define HAMMER2_CHAIN_UPDATE		0x00000040	/* need parent update */
#define HAMMER2_CHAIN_NOTTESTED		0x00000080	/* crc not generated */
#define HAMMER2_CHAIN_TESTEDGOOD	0x00000100	/* crc tested good */
#define HAMMER2_CHAIN_ONFLUSH		0x00000200	/* on a flush list */
#define HAMMER2_CHAIN_VOLUMESYNC	0x00000800	/* needs volume sync */
#define HAMMER2_CHAIN_COUNTEDBREFS	0x00002000	/* block table stats */
#define HAMMER2_CHAIN_ONRBTREE		0x00004000	/* on parent RB tree */
#define HAMMER2_CHAIN_RELEASE		0x00020000	/* don't keep around */
#define HAMMER2_CHAIN_BLKMAPPED		0x00040000	/* present in blkmap */
#define HAMMER2_CHAIN_BLKMAPUPD		0x00080000	/* +needs updating */
#define HAMMER2_CHAIN_IOINPROG		0x00100000	/* I/O interlock */
#define HAMMER2_CHAIN_IOSIGNAL		0x00200000	/* I/O interlock */
#define HAMMER2_CHAIN_PFSBOUNDARY	0x00400000	/* super->pfs inode */
#define HAMMER2_CHAIN_HINT_LEAF_COUNT	0x00800000	/* redo leaf count */

#define HAMMER2_CHAIN_FLUSH_MASK	(HAMMER2_CHAIN_MODIFIED |	\
					 HAMMER2_CHAIN_UPDATE |		\
					 HAMMER2_CHAIN_ONFLUSH |	\
					 HAMMER2_CHAIN_DESTROY)

/*
 * HAMMER2 error codes, used by chain->error and cluster->error.  The error
 * code is typically set on-lock unless no I/O was requested, and set on
 * I/O otherwise.  If set for a cluster it generally means that the cluster
 * code could not find a valid copy to present.
 *
 * All HAMMER2 error codes are flags and can be accumulated by ORing them
 * together.
 *
 * EIO		- An I/O error occurred
 * CHECK	- I/O succeeded but did not match the check code
 *
 * NOTE: API allows callers to check zero/non-zero to determine if an error
 *	 condition exists.
 *
 * NOTE: Chain's data field is usually NULL on an IO error but not necessarily
 *	 NULL on other errors.  Check chain->error, not chain->data.
 */
#define HAMMER2_ERROR_EIO		0x00000001	/* device I/O error */
#define HAMMER2_ERROR_CHECK		0x00000002	/* check code error */
#define HAMMER2_ERROR_BADBREF		0x00000010	/* illegal bref */
#define HAMMER2_ERROR_ENOSPC		0x00000020	/* allocation failure */
#define HAMMER2_ERROR_ENOENT		0x00000040	/* entry not found */
#define HAMMER2_ERROR_ENOTEMPTY		0x00000080	/* dir not empty */
#define HAMMER2_ERROR_EAGAIN		0x00000100	/* retry */
#define HAMMER2_ERROR_ENOTDIR		0x00000200	/* not directory */
#define HAMMER2_ERROR_EISDIR		0x00000400	/* is directory */
#define HAMMER2_ERROR_ABORTED		0x00001000	/* aborted operation */
#define HAMMER2_ERROR_EOF		0x00002000	/* end of scan */
#define HAMMER2_ERROR_EINVAL		0x00004000	/* catch-all */
#define HAMMER2_ERROR_EEXIST		0x00008000	/* entry exists */
#define HAMMER2_ERROR_EOPNOTSUPP	0x10000000	/* unsupported */

/*
 * Flags passed to hammer2_chain_lookup() and hammer2_chain_next().
 *
 * NOTES:
 *	NODATA	    - Asks that the chain->data not be resolved in order
 *		      to avoid I/O.
 *
 *	NODIRECT    - Prevents a lookup of offset 0 in an inode from returning
 *		      the inode itself if the inode is in DIRECTDATA mode
 *		      (i.e. file is <= 512 bytes).  Used by the synchronization
 *		      code to prevent confusion.
 *
 *	SHARED	    - The input chain is expected to be locked shared,
 *		      and the output chain is locked shared.
 *
 *	MATCHIND    - Allows an indirect block / freemap node to be returned
 *		      when the passed key range matches the radix.  Remember
 *		      that key_end is inclusive (e.g. {0x000,0xFFF},
 *		      not {0x000,0x1000}).
 *
 *		      (Cannot be used for remote or cluster ops).
 *
 *	ALWAYS	    - Always resolve the data.  If ALWAYS and NODATA are both
 *		      missing, bulk file data is not resolved but inodes and
 *		      other meta-data will.
 */
#define HAMMER2_LOOKUP_NODATA		0x00000002	/* data left NULL */
#define HAMMER2_LOOKUP_NODIRECT		0x00000004	/* no offset=0 DD */
#define HAMMER2_LOOKUP_SHARED		0x00000100
#define HAMMER2_LOOKUP_MATCHIND		0x00000200	/* return all chains */
#define HAMMER2_LOOKUP_ALWAYS		0x00000800	/* resolve data */

/*
 * Flags passed to hammer2_chain_modify() and hammer2_chain_resize().
 *
 * NOTE: OPTDATA allows us to avoid instantiating buffers for INDIRECT
 *	 blocks in the INITIAL-create state.
 */
#define HAMMER2_MODIFY_OPTDATA		0x00000002	/* data can be NULL */

/*
 * Flags passed to hammer2_chain_lock().
 *
 * NOTE: NONBLOCK is only used for hammer2_chain_repparent() and getparent(),
 *	 other functions (e.g. hammer2_chain_lookup(), etc) can't handle its
 *	 operation.
 */
#define HAMMER2_RESOLVE_NEVER		1
#define HAMMER2_RESOLVE_MAYBE		2
#define HAMMER2_RESOLVE_ALWAYS		3
#define HAMMER2_RESOLVE_MASK		0x0F

#define HAMMER2_RESOLVE_SHARED		0x10	/* request shared lock */
#define HAMMER2_RESOLVE_LOCKAGAIN	0x20	/* another shared lock */
#define HAMMER2_RESOLVE_NONBLOCK	0x80	/* non-blocking */

/*
 * Flags passed to hammer2_chain_delete().
 */
#define HAMMER2_DELETE_PERMANENT	0x0001

/*
 * Flags passed to hammer2_chain_insert() or hammer2_chain_rename()
 * or hammer2_chain_create().
 */
#define HAMMER2_INSERT_PFSROOT		0x0004
#define HAMMER2_INSERT_SAMEPARENT	0x0008

/*
 * Flags passed to hammer2_freemap_adjust().
 */
#define HAMMER2_FREEMAP_DORECOVER	1

/*
 * HAMMER2 cluster - A set of chains representing the same entity.
 *
 * Currently a valid cluster can only have 1 set of chains (nchains)
 * representing the same entity.
 */
#define HAMMER2_XOPFIFO		16

#define HAMMER2_MAXCLUSTER	8
#define HAMMER2_XOPMASK_VOP	((uint32_t)0x80000000U)

#define HAMMER2_XOPMASK_ALLDONE	(HAMMER2_XOPMASK_VOP)

struct hammer2_cluster_item {
	hammer2_chain_t		*chain;
	uint32_t		flags;		/* for HAMMER2_CITEM_xxx */
	int			error;
};

typedef struct hammer2_cluster_item hammer2_cluster_item_t;

#define HAMMER2_CITEM_NULL	0x00000004

struct hammer2_cluster {
	hammer2_cluster_item_t	array[HAMMER2_MAXCLUSTER];
	hammer2_pfs_t		*pmp;
	hammer2_chain_t		*focus;		/* current focus (or mod) */
	int			nchains;
	int			error;		/* error code valid on lock */
};

typedef struct hammer2_cluster	hammer2_cluster_t;

struct hammer2_depend {
	TAILQ_ENTRY(hammer2_depend) entry;
	hammer2_inoq_head_t	sideq;
	long			count;
	int			pass2;
};

/*
 * HAMMER2 inode.
 */
struct hammer2_inode {
	struct hammer2_inode	*next;		/* inode tree */
	TAILQ_ENTRY(hammer2_inode) qentry;	/* SYNCQ/SIDEQ */
	LIST_ENTRY(hammer2_inode) ientry;
	hammer2_depend_t	*depend;	/* non-NULL if SIDEQ */
	hammer2_depend_t	depend_static;	/* (in-place allocation) */
	hammer2_mtx_t		lock;		/* inode lock */
	hammer2_mtx_t		truncate_lock;	/* prevent truncates */
	hammer2_mtx_t		vhold_lock;
	struct rrwlock		vnlock;		/* OpenBSD: vnode lock */
	hammer2_spin_t		cluster_spin;	/* update cluster */
	hammer2_cluster_t	cluster;
	hammer2_cluster_item_t	ccache[HAMMER2_MAXCLUSTER];
	int			ccache_nchains;
	hammer2_inode_meta_t	meta;		/* copy of meta-data */
	hammer2_pfs_t		*pmp;		/* PFS mount */
	hammer2_off_t		osize;
	struct vnode		*vp;
	unsigned int		refs;		/* +vpref, +flushref */
	unsigned int		flags;		/* for HAMMER2_INODE_xxx */
	uint8_t			comp_heuristic;
	int			ipdep_idx;
	int			vhold;
	int			in_seek;	/* FIOSEEKXXX */
};

/*
 * MODIFIED	- Inode is in a modified state, ip->meta may have changes.
 * RESIZED	- Inode truncated (any) or inode extended beyond
 *		  EMBEDDED_BYTES.
 *
 * SYNCQ	- Inode is included in the current filesystem sync.  The
 *		  DELETING and CREATING flags will be acted upon.
 *
 * SIDEQ	- Inode has likely been disconnected from the vnode topology
 *		  and so is not visible to the vnode-based filesystem syncer
 *		  code, but is dirty and must be included in the next
 *		  filesystem sync.  These inodes are moved to the SYNCQ at
 *		  the time the sync occurs.
 *
 *		  Inodes are not placed on this queue simply because they have
 *		  become dirty, if a vnode is attached.
 *
 * DELETING	- Inode is flagged for deletion during the next filesystem
 *		  sync.  That is, the inode's chain is currently connected
 *		  and must be deleting during the current or next fs sync.
 *
 * CREATING	- Inode is flagged for creation during the next filesystem
 *		  sync.  That is, the inode's chain topology exists (so
 *		  kernel buffer flushes can occur), but is currently
 *		  disconnected and must be inserted during the current or
 *		  next fs sync.  If the DELETING flag is also set, the
 *		  topology can be thrown away instead.
 *
 * If an inode that is already part of the current filesystem sync is
 * modified by the frontend, including by buffer flushes, the inode lock
 * code detects the SYNCQ flag and moves the inode to the head of the
 * flush-in-progress, then blocks until the flush has gotten past it.
 */
#define HAMMER2_INODE_MODIFIED		0x0001
#define HAMMER2_INODE_ONHASH		0x0008
#define HAMMER2_INODE_RESIZED		0x0010	/* requires inode_chain_sync */
#define HAMMER2_INODE_ISUNLINKED	0x0040
#define HAMMER2_INODE_SIDEQ		0x0100	/* on side processing queue */
#define HAMMER2_INODE_NOSIDEQ		0x0200	/* disable sideq operation */
#define HAMMER2_INODE_DIRTYDATA		0x0400	/* interlocks inode flush */
#define HAMMER2_INODE_SYNCQ		0x0800	/* sync interlock, sequenced */
#define HAMMER2_INODE_DELETING		0x1000	/* sync interlock, chain topo */
#define HAMMER2_INODE_CREATING		0x2000	/* sync interlock, chain topo */
#define HAMMER2_INODE_SYNCQ_WAKEUP	0x4000	/* sync interlock wakeup */
#define HAMMER2_INODE_SYNCQ_PASS2	0x8000	/* force retry delay */

/*
 * Transaction management sub-structure under hammer2_pfs.
 */
struct hammer2_trans {
	uint32_t		flags;
};

typedef struct hammer2_trans hammer2_trans_t;

#define HAMMER2_TRANS_ISFLUSH		0x80000000	/* flush code */
#define HAMMER2_TRANS_BUFCACHE		0x40000000	/* bio strategy */
#define HAMMER2_TRANS_SIDEQ		0x20000000	/* run sideq */
#define HAMMER2_TRANS_WAITING		0x08000000	/* someone waiting */
#define HAMMER2_TRANS_RESCAN		0x04000000	/* rescan sideq */
#define HAMMER2_TRANS_MASK		0x00FFFFFF	/* count mask */

#define HAMMER2_FREEMAP_HEUR_NRADIX	4	/* pwr 2 PBUFRADIX-LBUFRADIX */
#define HAMMER2_FREEMAP_HEUR_TYPES	8
#define HAMMER2_FREEMAP_HEUR_SIZE	(HAMMER2_FREEMAP_HEUR_NRADIX * \
					 HAMMER2_FREEMAP_HEUR_TYPES)

#define HAMMER2_DEDUP_HEUR_SIZE		(65536 * 4)
#define HAMMER2_DEDUP_HEUR_MASK		(HAMMER2_DEDUP_HEUR_SIZE - 1)

#define HAMMER2_FLUSH_TOP		0x0001
#define HAMMER2_FLUSH_ALL		0x0002
#define HAMMER2_FLUSH_INODE_STOP	0x0004	/* stop at sub-inode */
#define HAMMER2_FLUSH_FSSYNC		0x0008	/* part of filesystem sync */

/*
 * Support structure for dedup heuristic.
 */
struct hammer2_dedup {
	hammer2_off_t		data_off;
	uint64_t		data_crc;
	uint32_t		ticks;
	uint32_t		saved_error;
};

typedef struct hammer2_dedup hammer2_dedup_t;

/*
 * HAMMER2 XOP - container for VOP/XOP operation.
 *
 * This structure is used to distribute a VOP operation across multiple
 * nodes.  Unlike DragonFly HAMMER2, XOP is currently just a function called
 * by VOP to handle chains.
 */
typedef void (*hammer2_xop_func_t)(union hammer2_xop *, void *, int);

struct hammer2_xop_desc {
	hammer2_xop_func_t	storage_func;	/* local storage function */
	const char		*id;
};

typedef struct hammer2_xop_desc hammer2_xop_desc_t;

struct hammer2_xop_fifo {
	hammer2_chain_t		**array;
	int			*errors;
	int			ri;
	int			wi;
	int			flags;
};

typedef struct hammer2_xop_fifo hammer2_xop_fifo_t;

struct hammer2_xop_head {
	hammer2_tid_t		mtid;
	hammer2_xop_fifo_t	collect[HAMMER2_MAXCLUSTER];
	hammer2_cluster_t	cluster;
	hammer2_xop_desc_t	*desc;
	hammer2_inode_t		*ip1;
	hammer2_inode_t		*ip2;
	hammer2_inode_t		*ip3;
	hammer2_inode_t		*ip4;
	hammer2_io_t		*focus_dio;
	hammer2_key_t		collect_key;
	uint32_t		run_mask;
	uint32_t		chk_mask;
	int			flags;
	int			fifo_size;
	int			error;
	char			*name1;
	size_t			name1_len;
	char			*name2;
	size_t			name2_len;
	void			*scratch;
};

typedef struct hammer2_xop_head hammer2_xop_head_t;

#define fifo_mask(xop_head)	((xop_head)->fifo_size - 1)

struct hammer2_xop_ipcluster {
	hammer2_xop_head_t	head;
};

struct hammer2_xop_readdir {
	hammer2_xop_head_t	head;
	hammer2_key_t		lkey;
};

struct hammer2_xop_nresolve {
	hammer2_xop_head_t	head;
};

struct hammer2_xop_unlink {
	hammer2_xop_head_t	head;
	int			isdir;
	int			dopermanent;
};

#define H2DOPERM_PERMANENT	0x01
#define H2DOPERM_FORCE		0x02
#define H2DOPERM_IGNINO		0x04

struct hammer2_xop_nrename {
	hammer2_xop_head_t	head;
	hammer2_tid_t		lhc;
	int			ip_key;
};

struct hammer2_xop_scanlhc {
	hammer2_xop_head_t	head;
	hammer2_key_t		lhc;
};

struct hammer2_xop_scanall {
	hammer2_xop_head_t	head;
	hammer2_key_t		key_beg;	/* inclusive */
	hammer2_key_t		key_end;	/* inclusive */
	int			resolve_flags;
	int			lookup_flags;
};

struct hammer2_xop_lookup {
	hammer2_xop_head_t	head;
	hammer2_key_t		lhc;
};

struct hammer2_xop_mkdirent {
	hammer2_xop_head_t	head;
	hammer2_dirent_head_t	dirent;
	hammer2_key_t		lhc;
};

struct hammer2_xop_create {
	hammer2_xop_head_t	head;
	hammer2_inode_meta_t	meta;
	hammer2_key_t		lhc;
	int			flags;
};

struct hammer2_xop_destroy {
	hammer2_xop_head_t	head;
};

struct hammer2_xop_fsync {
	hammer2_xop_head_t	head;
	hammer2_inode_meta_t	meta;
	hammer2_off_t		osize;
	u_int			ipflags;
	int			clear_directdata;
};

struct hammer2_xop_unlinkall {
	hammer2_xop_head_t	head;
	hammer2_key_t		key_beg;
	hammer2_key_t		key_end;
};

struct hammer2_xop_connect {
	hammer2_xop_head_t	head;
	hammer2_key_t		lhc;
};

struct hammer2_xop_flush {
	hammer2_xop_head_t	head;
};

struct hammer2_xop_strategy {
	hammer2_xop_head_t	head;
	hammer2_key_t		lbase;
	struct buf		*bp;
};

struct hammer2_xop_bmap {
	hammer2_xop_head_t	head;
	daddr_t			lbn;
	int			runp;
	int			runb;
	hammer2_off_t		offset;
	hammer2_off_t		loffset;
};

typedef struct hammer2_xop_ipcluster hammer2_xop_ipcluster_t;
typedef struct hammer2_xop_readdir hammer2_xop_readdir_t;
typedef struct hammer2_xop_nresolve hammer2_xop_nresolve_t;
typedef struct hammer2_xop_unlink hammer2_xop_unlink_t;
typedef struct hammer2_xop_nrename hammer2_xop_nrename_t;
typedef struct hammer2_xop_scanlhc hammer2_xop_scanlhc_t;
typedef struct hammer2_xop_scanall hammer2_xop_scanall_t;
typedef struct hammer2_xop_lookup hammer2_xop_lookup_t;
typedef struct hammer2_xop_mkdirent hammer2_xop_mkdirent_t;
typedef struct hammer2_xop_create hammer2_xop_create_t;
typedef struct hammer2_xop_destroy hammer2_xop_destroy_t;
typedef struct hammer2_xop_fsync hammer2_xop_fsync_t;
typedef struct hammer2_xop_unlinkall hammer2_xop_unlinkall_t;
typedef struct hammer2_xop_connect hammer2_xop_connect_t;
typedef struct hammer2_xop_flush hammer2_xop_flush_t;
typedef struct hammer2_xop_strategy hammer2_xop_strategy_t;
typedef struct hammer2_xop_bmap hammer2_xop_bmap_t;

union hammer2_xop {
	hammer2_xop_head_t	head;
	hammer2_xop_ipcluster_t	xop_ipcluster;
	hammer2_xop_readdir_t	xop_readdir;
	hammer2_xop_nresolve_t	xop_nresolve;
	hammer2_xop_unlink_t	xop_unlink;
	hammer2_xop_nrename_t	xop_nrename;
	hammer2_xop_scanlhc_t	xop_scanlhc;
	hammer2_xop_scanall_t	xop_scanall;
	hammer2_xop_lookup_t	xop_lookup;
	hammer2_xop_mkdirent_t	xop_mkdirent;
	hammer2_xop_create_t	xop_create;
	hammer2_xop_destroy_t	xop_destroy;
	hammer2_xop_fsync_t	xop_fsync;
	hammer2_xop_unlinkall_t	xop_unlinkall;
	hammer2_xop_connect_t	xop_connect;
	hammer2_xop_flush_t	xop_flush;
	hammer2_xop_strategy_t	xop_strategy;
	hammer2_xop_bmap_t	xop_bmap;
};

/*
 * flags to hammer2_xop_collect().
 */
#define HAMMER2_XOP_COLLECT_NOWAIT	0x00000001
#define HAMMER2_XOP_COLLECT_WAITALL	0x00000002

/*
 * flags to hammer2_xop_alloc().
 *
 * MODIFYING	- This is a modifying transaction, allocate a mtid.
 */
#define HAMMER2_XOP_MODIFYING		0x00000001
#define HAMMER2_XOP_STRATEGY		0x00000002
#define HAMMER2_XOP_INODE_STOP		0x00000004
#define HAMMER2_XOP_VOLHDR		0x00000008
#define HAMMER2_XOP_FSSYNC		0x00000010

/*
 * Device vnode management structure.
 */
struct hammer2_devvp {
	TAILQ_ENTRY(hammer2_devvp) entry;
	struct vnode		*devvp;		/* device vnode */
	char			*path;		/* device vnode path */
	char			*fname;		/* OpenBSD */
	int			open;		/* 1 if devvp open */
	int			xflags;		/* OpenBSD */
};

typedef struct hammer2_devvp hammer2_devvp_t;

/*
 * Volume management structure.
 */
struct hammer2_volume {
	hammer2_devvp_t		*dev;		/* device vnode management */
	hammer2_off_t		offset;		/* offset within volumes */
	hammer2_off_t		size;		/* volume size */
	int			id;		/* volume id */
};

typedef struct hammer2_volume hammer2_volume_t;

/*
 * I/O stat structure.
 */
struct hammer2_iostat_unit {
	unsigned long		count;
	unsigned long		bytes;
};

typedef struct hammer2_iostat_unit hammer2_iostat_unit_t;

struct hammer2_iostat {
	hammer2_iostat_unit_t	inode;
	hammer2_iostat_unit_t	indirect;
	hammer2_iostat_unit_t	data;
	hammer2_iostat_unit_t	dirent;
	hammer2_iostat_unit_t	freemap_node;
	hammer2_iostat_unit_t	freemap_leaf;
	hammer2_iostat_unit_t	freemap;
	hammer2_iostat_unit_t	volume;
};

typedef struct hammer2_iostat hammer2_iostat_t;


/*
 * Global (per partition) management structure, represents a hard block
 * device.  Typically referenced by hammer2_chain structures when applicable.
 *
 * Note that a single hammer2_dev can be indirectly tied to multiple system
 * mount points.  There is no direct relationship.  System mounts are
 * per-cluster-id, not per-block-device, and a single hard mount might contain
 * many PFSs.
 */
struct hammer2_dev {
	TAILQ_ENTRY(hammer2_dev) mntentry;	/* hammer2_mntlist */
	hammer2_devvp_list_t	devvp_list;	/* list of device vnodes including *devvp */
	hammer2_io_hash_t	iohash[HAMMER2_IOHASH_SIZE];
	hammer2_mtx_t		iohash_lock;
	hammer2_pfs_t		*spmp;		/* super-root pmp for transactions */
	struct vnode		*devvp;		/* device vnode for root volume */
	struct malloc_type *mmsg;
	hammer2_chain_t		vchain;		/* anchor chain (topology) */
	hammer2_chain_t		fchain;		/* anchor chain (freemap) */
	hammer2_volume_data_t	voldata;
	hammer2_volume_data_t	volsync;	/* synchronized voldata */
	hammer2_volume_t	volumes[HAMMER2_MAX_VOLUMES]; /* list of volumes */
	hammer2_off_t		total_size;	/* total size of volumes */
	uint32_t		hflags;		/* HMNT2 flags applicable to device */
	int			rdonly;		/* read-only mount */
	int			mount_count;	/* number of actively mounted PFSs */
	int			nvolumes;	/* total number of volumes */
	int			volhdrno;	/* last volhdrno written */
	int			iofree_count;
	int			io_iterator;
	hammer2_lk_t		vollk;		/* lockmgr lock */
	hammer2_lk_t		bulklk;		/* bulkfree operation lock */
	hammer2_lk_t		bflk;		/* bulk-free manual function lock */
	int			freemap_relaxed;
	char		devrepname[64];	/* for hprintf */
	hammer2_off_t		free_reserved;	/* nominal free reserved */
	hammer2_off_t		heur_freemap[HAMMER2_FREEMAP_HEUR_SIZE];
	hammer2_dedup_t		heur_dedup[HAMMER2_DEDUP_HEUR_SIZE];
	hammer2_iostat_t	iostat_read;	/* read I/O stat */
	hammer2_iostat_t	iostat_write;	/* write I/O stat */
	kdmsg_iocom_t	iocom;		/* volume-level dmsg interface */
	//struct lock	vollk;		/* lockmgr lock */
};



/*
 * Per-cluster management structure.  This structure will be tied to a
 * system mount point if the system is mounting the PFS.
 *
 * This structure is also used to represent the super-root that hangs off
 * of a hard mount point.  The super-root is not really a cluster element.
 * In this case the spmp_hmp field will be non-NULL.  It's just easier to do
 * this than to special case super-root manipulation in the hammer2_chain*
 * code as being only hammer2_dev-related.
 *
 * WARNING! The chains making up pfs->iroot's cluster are accounted for in
 *	    hammer2_dev->mount_count when the pfs is associated with a mount
 *	    point.
 */
#define HAMMER2_IHASH_SIZE	32

struct hammer2_pfs {
	TAILQ_ENTRY(hammer2_pfs) mntentry;	/* hammer2_pfslist */
	hammer2_ipdep_list_t	*ipdep_lists;	/* inode dependencies for XOP */
	hammer2_spin_t          blockset_spin;
	hammer2_spin_t		list_spin;
	hammer2_lk_t		xop_lock[HAMMER2_IHASH_SIZE];
	hammer2_lkc_t		xop_cv[HAMMER2_IHASH_SIZE];
	hammer2_lk_t		trans_lock;	/* XXX temporary */
	hammer2_lkc_t		trans_cv;
	struct mount		*mp;
	struct uuid		pfs_clid;
	hammer2_trans_t		trans;
	hammer2_inode_t		*iroot;		/* PFS root inode */
	hammer2_dev_t		*spmp_hmp;	/* only if super-root pmp */
	hammer2_dev_t		*force_local;	/* only if 'local' mount */
	hammer2_dev_t		*pfs_hmps[HAMMER2_MAXCLUSTER];
	char			*pfs_names[HAMMER2_MAXCLUSTER];
	uint8_t			pfs_types[HAMMER2_MAXCLUSTER];
	hammer2_blockset_t	pfs_iroot_blocksets[HAMMER2_MAXCLUSTER];
	int			flags;		/* for HAMMER2_PMPF_xxx */
	int			rdonly;		/* read-only mount */
	int			free_ticks;	/* free_* calculations */
	unsigned long		ipdep_mask;
	hammer2_off_t		free_reserved;
	hammer2_off_t		free_nominal;
	uint32_t		inmem_dirty_chains;
	hammer2_tid_t		modify_tid;	/* modify transaction id */
	hammer2_tid_t		inode_tid;	/* inode allocator */
	hammer2_inoq_head_t	syncq;		/* SYNCQ flagged inodes */
	hammer2_depq_head_t	depq;		/* SIDEQ flagged inodes */
	long			sideq_count;	/* total inodes on depq */
	/* note: inumhash not applicable to spmp */
	hammer2_inum_hash_t	inumhash[HAMMER2_INUMHASH_SIZE];
	char			*fspec;		/* OpenBSD */
	struct netexport	pm_export;	/* OpenBSD: export information */
};

typedef struct hammer2_pfs hammer2_pfs_t;

hammer2_pfs_t *
MPTOPMP(struct mount *mp)
{
	return ((hammer2_pfs_t *)mp->mnt_data);
}

typedef enum buf_cmd {
	BUF_CMD_DONE = 0,
	BUF_CMD_READ,
	BUF_CMD_WRITE,
	BUF_CMD_FREEBLKS,
	BUF_CMD_FORMAT,
	BUF_CMD_FLUSH,
	BUF_CMD_SEEK,
} buf_cmd_t;

struct vop_bmap_args_hammer2 {
	struct vnode *a_vp;
	daddr_t a_bn;
	struct vnode **a_vpp;
	daddr_t *a_bnp;
	int *a_runp;
	int *a_runb;
	off_t a_loffset;
	off_t *a_doffsetp;
    buf_cmd_t a_cmd;	/* BUF_CMD_READ, BUF_CMD_WRITE, etc */
};

#define HAMMER2_PMPF_SPMP	0x00000001
#define HAMMER2_PMPF_EMERG	0x00000002
#define HAMMER2_PMPF_WAITING	0x10000000

#define HAMMER2_CHECK_NULL	0x00000001

#define MPTOPMP(mp)	((hammer2_pfs_t *)(mp)->mnt_data)
#define VTOI(vp)	((hammer2_inode_t *)(vp)->v_data)

extern struct hammer2_pfslist hammer2_pfslist;

extern hammer2_lk_t hammer2_mntlk;

extern int hammer2_debug;

extern int hammer2_dedup_enable;
extern int hammer2_count_inode_allocated;
extern int hammer2_count_chain_allocated;
extern int hammer2_count_chain_modified;
extern int hammer2_count_dio_allocated;
extern int hammer2_dio_limit;
extern int hammer2_bulkfree_tps;
extern int hammer2_limit_scan_depth;
extern int hammer2_limit_saved_chains;
extern int hammer2_always_compress;

extern long hammer2_iod_file_read;
extern long hammer2_iod_meta_read;
extern long hammer2_iod_indr_read;
extern long hammer2_iod_fmap_read;
extern long hammer2_iod_volu_read;
extern long hammer2_iod_file_write;
extern long hammer2_iod_file_wembed;
extern long hammer2_iod_file_wzero;
extern long hammer2_iod_file_wdedup;
extern long hammer2_iod_meta_write;
extern long hammer2_iod_indr_write;
extern long hammer2_iod_fmap_write;
extern long hammer2_iod_volu_write;

extern hammer2_xop_desc_t hammer2_ipcluster_desc;
extern hammer2_xop_desc_t hammer2_readdir_desc;
extern hammer2_xop_desc_t hammer2_nresolve_desc;
extern hammer2_xop_desc_t hammer2_unlink_desc;
extern hammer2_xop_desc_t hammer2_nrename_desc;
extern hammer2_xop_desc_t hammer2_scanlhc_desc;
extern hammer2_xop_desc_t hammer2_scanall_desc;
extern hammer2_xop_desc_t hammer2_lookup_desc;
extern hammer2_xop_desc_t hammer2_delete_desc;
extern hammer2_xop_desc_t hammer2_inode_mkdirent_desc;
extern hammer2_xop_desc_t hammer2_inode_create_desc;
extern hammer2_xop_desc_t hammer2_inode_create_det_desc;
extern hammer2_xop_desc_t hammer2_inode_create_ins_desc;
extern hammer2_xop_desc_t hammer2_inode_destroy_desc;
extern hammer2_xop_desc_t hammer2_inode_chain_sync_desc;
extern hammer2_xop_desc_t hammer2_inode_unlinkall_desc;
extern hammer2_xop_desc_t hammer2_inode_connect_desc;
extern hammer2_xop_desc_t hammer2_inode_flush_desc;
extern hammer2_xop_desc_t hammer2_strategy_read_desc;
extern hammer2_xop_desc_t hammer2_strategy_write_desc;
extern hammer2_xop_desc_t hammer2_bmap_desc;

/* hammer2_admin.c */
void *hammer2_xop_alloc(hammer2_inode_t *, int);
void hammer2_xop_setname(hammer2_xop_head_t *, const char *, size_t);
void hammer2_xop_setname2(hammer2_xop_head_t *, const char *, size_t);
size_t hammer2_xop_setname_inum(hammer2_xop_head_t *, hammer2_key_t);
void hammer2_xop_setip2(hammer2_xop_head_t *, hammer2_inode_t *);
void hammer2_xop_setip3(hammer2_xop_head_t *, hammer2_inode_t *);
void hammer2_xop_setip4(hammer2_xop_head_t *, hammer2_inode_t *);
void hammer2_xop_start(hammer2_xop_head_t *, hammer2_xop_desc_t *);
void hammer2_xop_retire(hammer2_xop_head_t *, uint32_t);
int hammer2_xop_feed(hammer2_xop_head_t *, hammer2_chain_t *, int, int);
int hammer2_xop_collect(hammer2_xop_head_t *, int);

/* hammer2_bulkfree.c */
void hammer2_bulkfree_init(hammer2_dev_t *);
void hammer2_bulkfree_uninit(hammer2_dev_t *);
int hammer2_bulkfree_pass(hammer2_dev_t *, hammer2_chain_t *,
    struct hammer2_ioc_bulkfree *);

/* hammer2_chain.c */
int hammer2_chain_cmp(const hammer2_chain_t *, const hammer2_chain_t *);
void hammer2_chain_setflush(hammer2_chain_t *);
void hammer2_chain_init(hammer2_chain_t *);
void hammer2_chain_ref(hammer2_chain_t *);
void hammer2_chain_ref_hold(hammer2_chain_t *);
void hammer2_chain_drop(hammer2_chain_t *);
void hammer2_chain_unhold(hammer2_chain_t *);
void hammer2_chain_drop_unhold(hammer2_chain_t *);
void hammer2_chain_rehold(hammer2_chain_t *);
int hammer2_chain_lock(hammer2_chain_t *, int);
void hammer2_chain_unlock(hammer2_chain_t *);
int hammer2_chain_resize(hammer2_chain_t *, hammer2_tid_t, hammer2_off_t, int,
    int);
int hammer2_chain_modify(hammer2_chain_t *, hammer2_tid_t, hammer2_off_t, int);
int hammer2_chain_modify_ip(hammer2_inode_t *, hammer2_chain_t *, hammer2_tid_t,
    int);
hammer2_chain_t *hammer2_chain_lookup_init(hammer2_chain_t *, int);
void hammer2_chain_lookup_done(hammer2_chain_t *);
hammer2_chain_t *hammer2_chain_getparent(hammer2_chain_t *, int);
hammer2_chain_t *hammer2_chain_lookup(hammer2_chain_t **, hammer2_key_t *,
    hammer2_key_t, hammer2_key_t, int *, int);
//hammer2_chain_t *hammer2_chain_next(hammer2_chain_t **, hammer2_chain_t *, hammer2_key_t *, hammer2_key_t, int *, int);
hammer2_chain_t *hammer2_chain_next(hammer2_chain_t **parentp,
				hammer2_chain_t *chain,
				hammer2_key_t *key_nextp,
				hammer2_key_t key_beg, hammer2_key_t key_end,
				int *errorp, int flags);
int hammer2_chain_scan(hammer2_chain_t *, hammer2_chain_t **,
    hammer2_blockref_t *, int *, int);
int hammer2_chain_create(hammer2_chain_t **, hammer2_chain_t **,
    hammer2_dev_t *, hammer2_pfs_t *, int, hammer2_key_t, int, int, size_t,
    hammer2_tid_t, hammer2_off_t, int);
int hammer2_chain_indirect_maintenance(hammer2_chain_t *, hammer2_chain_t *);
int hammer2_chain_delete(hammer2_chain_t *, hammer2_chain_t *, hammer2_tid_t,
    int);
void hammer2_base_delete(hammer2_chain_t *, hammer2_blockref_t *, int,
    hammer2_chain_t *, hammer2_blockref_t *);
void hammer2_base_insert(hammer2_chain_t *, hammer2_blockref_t *, int,
    hammer2_chain_t *, hammer2_blockref_t *);
void hammer2_chain_setcheck(hammer2_chain_t *, void *);
int hammer2_chain_inode_find(hammer2_pfs_t *, hammer2_key_t, int, int,
    hammer2_chain_t **, hammer2_chain_t **);
hammer2_chain_t *hammer2_chain_bulksnap(hammer2_dev_t *);
void hammer2_chain_bulkdrop(hammer2_chain_t *);
int hammer2_chain_dirent_test(const hammer2_chain_t *, const char *, size_t);
void hammer2_dump_chain(hammer2_chain_t *, int, int, int, char);

RB_PROTOTYPE(hammer2_chain_tree, hammer2_chain, rbnode, hammer2_chain_cmp);
RB_PROTOTYPE_SCAN(hammer2_chain_tree, hammer2_chain, rbnode);

/* hammer2_cluster.c */
uint8_t hammer2_cluster_type(const hammer2_cluster_t *);
void hammer2_cluster_bref(const hammer2_cluster_t *, hammer2_blockref_t *);
void hammer2_dummy_xop_from_chain(hammer2_xop_head_t *, hammer2_chain_t *);
void hammer2_cluster_unhold(hammer2_cluster_t *);
void hammer2_cluster_rehold(hammer2_cluster_t *);
int hammer2_cluster_check(hammer2_cluster_t *, hammer2_key_t, int);

/* hammer2_flush.c */
void hammer2_trans_manage_init(hammer2_pfs_t *);
void hammer2_trans_init(hammer2_pfs_t *, uint32_t);
void hammer2_trans_setflags(hammer2_pfs_t *, uint32_t);
void hammer2_trans_clearflags(hammer2_pfs_t *, uint32_t);
hammer2_tid_t hammer2_trans_sub(hammer2_pfs_t *);
void hammer2_trans_done(hammer2_pfs_t *, uint32_t);
hammer2_tid_t hammer2_trans_newinum(hammer2_pfs_t *);
void hammer2_trans_assert_strategy(hammer2_pfs_t *);
int hammer2_flush(hammer2_chain_t *, int);
void hammer2_xop_inode_flush(hammer2_xop_t *, void *, int);

/* hammer2_freemap.c */
int hammer2_freemap_alloc(hammer2_chain_t *, size_t);
void hammer2_freemap_adjust(hammer2_dev_t *, hammer2_blockref_t *, int);

/* hammer2_inode.c */
void hammer2_inum_hash_init(hammer2_pfs_t *);
void hammer2_inum_hash_destroy(hammer2_pfs_t *);
void hammer2_inode_delayed_sideq(hammer2_inode_t *);
void hammer2_inode_lock(hammer2_inode_t *, int);
void hammer2_inode_lock4(hammer2_inode_t *, hammer2_inode_t *,
    hammer2_inode_t *, hammer2_inode_t *);
void hammer2_inode_unlock(hammer2_inode_t *);
void hammer2_inode_depend(hammer2_inode_t *, hammer2_inode_t *);
hammer2_chain_t *hammer2_inode_chain(hammer2_inode_t *, int, int);
hammer2_chain_t *hammer2_inode_chain_and_parent(hammer2_inode_t *, int,
    hammer2_chain_t **, int);
hammer2_inode_t *hammer2_inode_lookup(hammer2_pfs_t *, hammer2_tid_t);
void hammer2_inode_ref(hammer2_inode_t *);
void hammer2_inode_drop(hammer2_inode_t *);
int hammer2_igetv(hammer2_inode_t *, struct vnode **);
hammer2_inode_t *hammer2_inode_get(hammer2_pfs_t *, hammer2_xop_head_t *,
    hammer2_tid_t, int);
hammer2_inode_t *hammer2_inode_create_pfs(hammer2_pfs_t *, const char *,
    size_t, int *);
hammer2_inode_t *hammer2_inode_create_normal(hammer2_inode_t *, struct vattr *,
    struct ucred *, hammer2_key_t, int *);
int hammer2_dirent_create(hammer2_inode_t *, const char *, size_t,
    hammer2_key_t, uint8_t);
hammer2_key_t hammer2_inode_data_count(const hammer2_inode_t *);
hammer2_key_t hammer2_inode_inode_count(const hammer2_inode_t *);
int hammer2_inode_unlink_finisher(hammer2_inode_t *, struct vnode **);
void hammer2_inode_modify(hammer2_inode_t *);
void hammer2_inode_vhold(hammer2_inode_t *);
void hammer2_inode_vdrop(hammer2_inode_t *, int);
int hammer2_inode_chain_sync(hammer2_inode_t *);
int hammer2_inode_chain_ins(hammer2_inode_t *);
int hammer2_inode_chain_des(hammer2_inode_t *);
int hammer2_inode_chain_flush(hammer2_inode_t *, int);

/* hammer2_io.c */
void hammer2_io_hash_init(hammer2_dev_t *);
void hammer2_io_hash_destroy(hammer2_dev_t *);
hammer2_io_t *hammer2_io_getblk(hammer2_dev_t *, int, hammer2_off_t, int, int);
void hammer2_io_putblk(hammer2_io_t **);
void hammer2_io_hash_cleanup_all(hammer2_dev_t *);
char *hammer2_io_data(hammer2_io_t *, hammer2_off_t);
int hammer2_io_new(hammer2_dev_t *, int, hammer2_off_t, int, hammer2_io_t **);
int hammer2_io_newnz(hammer2_dev_t *, int, hammer2_off_t, int, hammer2_io_t **);
int hammer2_io_bread(hammer2_dev_t *, int, hammer2_off_t, int, hammer2_io_t **);
hammer2_io_t *hammer2_io_getquick(hammer2_dev_t *, off_t, int);
void hammer2_io_bawrite(hammer2_io_t **);
void hammer2_io_bdwrite(hammer2_io_t **);
int hammer2_io_bwrite(hammer2_io_t **);
void hammer2_io_setdirty(hammer2_io_t *);
void hammer2_io_brelse(hammer2_io_t **);
void hammer2_io_bqrelse(hammer2_io_t **);
uint64_t hammer2_dedup_mask(hammer2_io_t *, hammer2_off_t, u_int);
void hammer2_io_dedup_set(hammer2_dev_t *, hammer2_blockref_t *);
void hammer2_io_dedup_delete(hammer2_dev_t *, uint8_t, hammer2_off_t,
    unsigned int);
void hammer2_io_dedup_assert(hammer2_dev_t *, hammer2_off_t, unsigned int);

/* hammer2_ioctl.c */
int hammer2_ioctl_impl(struct vnode *, unsigned long, void *, int,
    struct ucred *);

/* hammer2_ondisk.c */
int hammer2_open_devvp(struct mount *, const hammer2_devvp_list_t *,
    struct proc *);
int hammer2_close_devvp(const hammer2_devvp_list_t *, struct proc *);
int hammer2_init_devvp(struct mount *, const char *,
    hammer2_devvp_list_t *, struct nameidata *, struct proc *);
void hammer2_cleanup_devvp(hammer2_devvp_list_t *);
int hammer2_init_volumes(const hammer2_devvp_list_t *, hammer2_volume_t *,
    hammer2_volume_data_t *, int *, struct vnode **);
hammer2_volume_t *hammer2_get_volume(hammer2_dev_t *, hammer2_off_t);

/*
 * hammer2_iocom.c
 */
void hammer2_iocom_init(hammer2_dev_t *hmp);
void hammer2_iocom_uninit(hammer2_dev_t *hmp);
void hammer2_cluster_reconnect(hammer2_dev_t *hmp, struct file *fp);
void hammer2_volconf_update(hammer2_dev_t *hmp, int index);

/* hammer2_strategy.c */
int hammer2_strategy(void *v);
void hammer2_xop_strategy_read(hammer2_xop_t *, void *, int);
void hammer2_xop_strategy_write(hammer2_xop_t *, void *, int);
void hammer2_bioq_sync(hammer2_pfs_t *);
void hammer2_dedup_clear(hammer2_dev_t *);

/* hammer2_subr.c */
int hammer2_get_dtype(uint8_t);
int hammer2_get_vtype(uint8_t);
uint8_t hammer2_get_obj_type(uint8_t);
void hammer2_time_to_timespec(uint64_t, struct timespec *);
uint64_t hammer2_timespec_to_time(const struct timespec *);
uint32_t hammer2_to_unix_xid(const struct uuid *);
void hammer2_guid_to_uuid(struct uuid *, uint32_t);
hammer2_key_t hammer2_dirhash(const char *, size_t);
int hammer2_getradix(size_t);
int hammer2_calc_logical(hammer2_inode_t *, hammer2_off_t, hammer2_key_t *,
    hammer2_key_t *);
int hammer2_get_logical(void);
int hammer2_calc_physical(hammer2_inode_t *, hammer2_key_t);
void hammer2_update_time(uint64_t *);
void hammer2_adjreadcounter(int btype, size_t bytes);
void hammer2_inc_iostat(hammer2_iostat_t *, int, size_t);
void hammer2_print_iostat(const hammer2_iostat_t *, const char *);
int hammer2_signal_check(void);
const char *hammer2_breftype_to_str(uint8_t);

/* hammer2_vfsops.c */
hammer2_pfs_t *hammer2_pfsalloc(hammer2_chain_t *, const hammer2_inode_data_t *,
    hammer2_dev_t *);
void hammer2_pfsdealloc(hammer2_pfs_t *, int, int);
int hammer2_sync(struct mount *, int, int, struct ucred *, struct proc *);
int hammer2_vfs_sync_pmp(hammer2_pfs_t *, int);
void hammer2_voldata_lock(hammer2_dev_t *);
void hammer2_voldata_unlock(hammer2_dev_t *);
void hammer2_voldata_modify(hammer2_dev_t *);
int hammer2_vfs_enospace(hammer2_inode_t *, off_t, struct ucred *);
void hammer2_pfs_memory_inc(hammer2_pfs_t *pmp);
void hammer2_pfs_memory_wakeup(hammer2_pfs_t *pmp, int count);

/* hammer2_vnops.c */
int hammer2_vinit(struct mount *, struct vnode **);

/* hammer2_xops.c */
void hammer2_xop_ipcluster(hammer2_xop_t *, void *, int);
void hammer2_xop_readdir(hammer2_xop_t *, void *, int);
void hammer2_xop_nresolve(hammer2_xop_t *, void *, int);
void hammer2_xop_unlink(hammer2_xop_t *, void *, int);
void hammer2_xop_nrename(hammer2_xop_t *, void *, int);
void hammer2_xop_scanlhc(hammer2_xop_t *, void *, int);
void hammer2_xop_scanall(hammer2_xop_t *, void *, int);
void hammer2_xop_lookup(hammer2_xop_t *, void *, int);
void hammer2_xop_delete(hammer2_xop_t *, void *, int);
void hammer2_xop_inode_mkdirent(hammer2_xop_t *, void *, int);
void hammer2_xop_inode_create(hammer2_xop_t *, void *, int);
void hammer2_xop_inode_create_det(hammer2_xop_t *, void *, int);
void hammer2_xop_inode_create_ins(hammer2_xop_t *, void *, int);
void hammer2_xop_inode_destroy(hammer2_xop_t *, void *, int);
void hammer2_xop_inode_chain_sync(hammer2_xop_t *, void *, int);
void hammer2_xop_inode_unlinkall(hammer2_xop_t *, void *, int);
void hammer2_xop_inode_connect(hammer2_xop_t *, void *, int);
void hammer2_xop_bmap(hammer2_xop_t *, void *, int);

/* XXX no way to return multiple errnos */
static __inline int
hammer2_error_to_errno(int error)
{
	if (!error)
		return (0);
	else if (error & HAMMER2_ERROR_EIO)
		return (EIO);
	else if (error & HAMMER2_ERROR_CHECK)
		return (EDOM);
	else if (error & HAMMER2_ERROR_BADBREF)
		return (EIO); /* no EBADBREF */
	else if (error & HAMMER2_ERROR_ENOSPC)
		return (ENOSPC);
	else if (error & HAMMER2_ERROR_ENOENT)
		return (ENOENT);
	else if (error & HAMMER2_ERROR_ENOTEMPTY)
		return (ENOTEMPTY);
	else if (error & HAMMER2_ERROR_EAGAIN)
		return (EAGAIN);
	else if (error & HAMMER2_ERROR_ENOTDIR)
		return (ENOTDIR);
	else if (error & HAMMER2_ERROR_EISDIR)
		return (EISDIR);
	else if (error & HAMMER2_ERROR_ABORTED)
		return (EINTR);
	//else if (error & HAMMER2_ERROR_EOF)
	//	return (xxx);
	else if (error & HAMMER2_ERROR_EINVAL)
		return (EINVAL);
	else if (error & HAMMER2_ERROR_EEXIST)
		return (EEXIST);
	else if (error & HAMMER2_ERROR_EOPNOTSUPP)
		return (EOPNOTSUPP);
	else
		return (EDOM);
}

static __inline int
hammer2_errno_to_error(int error)
{
	switch (error) {
	case 0:
		return (0);
	case EIO:
		return (HAMMER2_ERROR_EIO);
	case EDOM:
		return (HAMMER2_ERROR_CHECK);
	//case EIO:
	//	return (HAMMER2_ERROR_BADBREF);
	case ENOSPC:
		return (HAMMER2_ERROR_ENOSPC);
	case ENOENT:
		return (HAMMER2_ERROR_ENOENT);
	case ENOTEMPTY:
		return (HAMMER2_ERROR_ENOTEMPTY);
	case EAGAIN:
		return (HAMMER2_ERROR_EAGAIN);
	case ENOTDIR:
		return (HAMMER2_ERROR_ENOTDIR);
	case EISDIR:
		return (HAMMER2_ERROR_EISDIR);
	case EINTR:
		return (HAMMER2_ERROR_ABORTED);
	//case xxx:
	//	return (HAMMER2_ERROR_EOF);
	case EINVAL:
		return (HAMMER2_ERROR_EINVAL);
	case EEXIST:
		return (HAMMER2_ERROR_EEXIST);
	case EOPNOTSUPP:
		return (HAMMER2_ERROR_EOPNOTSUPP);
	default:
		return (HAMMER2_ERROR_EINVAL);
	}
}

static __inline const hammer2_media_data_t *
hammer2_xop_gdata(hammer2_xop_head_t *xop)
{
	hammer2_chain_t *focus = xop->cluster.focus;
	const void *data;

	if (focus->dio) {
		hammer2_mtx_sh(&focus->diolk);
		if ((xop->focus_dio = focus->dio) != NULL)
			atomic_add_32(&xop->focus_dio->refs, 1);
		data = focus->data;
		hammer2_mtx_unlock(&focus->diolk);
	} else {
		data = focus->data;
	}

	return (data);
}

static __inline void
hammer2_xop_pdata(hammer2_xop_head_t *xop)
{
	if (xop->focus_dio)
		hammer2_io_putblk(&xop->focus_dio);
}

static __inline void
hammer2_assert_cluster(const hammer2_cluster_t *cluster)
{
	/* Currently a valid cluster can only have 1 nchains. */
	KASSERTMSG(cluster->nchains == 1,
	    "unexpected cluster nchains %d", cluster->nchains);
}

static __inline void
hammer2_assert_inode_meta(const hammer2_inode_t *ip)
{
	KASSERTMSG(ip, "NULL ip");
	KASSERTMSG(ip->meta.mode, "mode 0");
	KASSERTMSG(ip->meta.type, "type 0");
}

uint32_t iscsi_crc32(const void *, size_t);
uint32_t iscsi_crc32_ext(const void *, size_t, uint32_t);

#define hammer2_icrc32(buf, size)	iscsi_crc32((buf), (size))
#define hammer2_icrc32c(buf, size, crc)	iscsi_crc32_ext((buf), (size), (crc))

#endif /* !_FS_HAMMER2_HAMMER2_H_ */
