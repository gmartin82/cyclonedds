// Copyright(c) 2006 to 2022 ZettaScale Technology and others
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License v. 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
// v. 1.0 which is available at
// http://www.eclipse.org/org/documents/edl-v10.php.
//
// SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

#ifndef DDSI_DOMAINGV_H
#define DDSI_DOMAINGV_H

#include <stdio.h>

#include "dds/export.h"
#include "dds/features.h"

#include "dds/ddsrt/atomics.h"
#include "dds/ddsrt/sockets.h"
#include "dds/ddsrt/sync.h"
#include "dds/ddsrt/fibheap.h"
#include "dds/ddsrt/avl.h"
#include "dds/ddsrt/random.h"

#include "dds/ddsi/ddsi_plist.h"
#include "dds/ddsi/ddsi_nwinterfaces.h"
#include "dds/ddsi/ddsi_protocol.h"
#include "dds/ddsi/ddsi_sockwaitset.h"

#if defined (__cplusplus)
extern "C" {
#endif

struct ddsi_xmsgpool;
struct ddsi_dqueue;
struct ddsi_reorder;
struct ddsi_defrag;
struct ddsi_addrset;
struct ddsi_xeventq;
struct ddsi_gcreq_queue;
struct ddsi_entity_index;
struct ddsi_lease;
struct ddsi_tran_conn;
struct ddsi_tran_listener;
struct ddsi_tran_factory;
struct ddsi_debug_monitor;
struct ddsi_tkmap;
struct dds_security_context;
struct dds_security_match_index;
struct ddsi_hsadmin;
struct spdp_admin;

struct ddsi_config_in_addr_node {
   ddsi_locator_t loc;
   struct ddsi_config_in_addr_node *next;
};

enum ddsi_recvips_mode {
  DDSI_RECVIPS_MODE_ALL,             /* all MC capable interfaces */
  DDSI_RECVIPS_MODE_ANY,             /* kernel-default interface */
  DDSI_RECVIPS_MODE_PREFERRED,       /* selected interface only */
  DDSI_RECVIPS_MODE_NONE,            /* no interfaces at all */
  DDSI_RECVIPS_MODE_SOME             /* explicit list of interfaces; only one requiring recvips */
};

enum ddsi_recv_thread_mode {
  DDSI_RTM_SINGLE,
  DDSI_RTM_MANY
};

struct ddsi_recv_thread_arg {
  enum ddsi_recv_thread_mode mode;
  struct ddsi_rbufpool *rbpool;
  struct ddsi_domaingv *gv;
  union {
    struct {
      const ddsi_locator_t *loc;
      struct ddsi_tran_conn *conn;
    } single;
    struct {
      struct ddsi_sock_waitset *ws;
    } many;
  } u;
};

struct ddsi_deleted_participants_admin;

struct ddsi_domaingv {
  volatile int terminate;
  volatile int deaf;
  volatile int mute;

  struct ddsrt_log_cfg logconfig;
  struct ddsi_config config;

  struct ddsi_tkmap * m_tkmap;

  /* Hash tables for participants, readers, writers, proxy
     participants, proxy readers and proxy writers by GUID. */
  struct ddsi_entity_index *entity_index;

  /* Timed events admin */
  struct ddsi_xeventq *xevents;

  /* Queue for garbage collection requests */
  struct ddsi_gcreq_queue *gcreq_queue;

  /* Lease junk */
  ddsrt_mutex_t leaseheap_lock;
  ddsrt_fibheap_t leaseheap;

  /* Transport factories & selected factory */
  struct ddsi_tran_factory *ddsi_tran_factories;
  struct ddsi_tran_factory *m_factory;

  /* Connections for multicast discovery & data, and those that correspond
     to the one DDSI participant index that the DDSI service uses. The
     DCPS participant of DDSI itself will be mirrored in a DDSI
     participant, and in multi-socket mode that one gets its own
     socket. */
  struct ddsi_tran_conn * disc_conn_mc;
  struct ddsi_tran_conn * data_conn_mc;
  struct ddsi_tran_conn * disc_conn_uc;
  struct ddsi_tran_conn * data_conn_uc;

  /* Connection used for all output (for connectionless transports), this
     used to simply be data_conn_uc, but:

     - Windows has a quirk that makes multicast delivery within a machine
       utterly unreliable if the transmitting socket is bound to 0.0.0.0
       (despite all sockets having multicast interfaces set correctly),
       but apparently only in the presence of sockets transmitting to the
       same multicast group that have been bound to non-0.0.0.0 ...
     - At least Fast-RTPS and Connext fail to honour the set of advertised
       addresses and substitute 127.0.0.1 for the advertised IP address and
       expect it to work.
     - Fast-RTPS (at least) binds the socket it uses for transmitting
       multicasts to non-0.0.0.0

     So binding to 0.0.0.0 means the unicasts from Fast-RTPS & Connext will
     arrive but the multicasts from Cyclone get dropped often on Windows
     when trying to interoperate; and binding to the IP address means
     unicast messages from the others fail to arrive (because they fail to
     arrive).

     The only work around is to use a separate socket for sending.  It is
     rather sad that Cyclone needs to work around the bugs of the others,
     but it seems the only way to get the users what they expect. */
#define MAX_XMIT_CONNS 4
  struct ddsi_tran_conn * xmit_conns[MAX_XMIT_CONNS];
  ddsi_xlocator_t intf_xlocators[MAX_XMIT_CONNS];

  /* TCP listener */
  struct ddsi_tran_listener * listener;

  /* In many sockets mode, the receive threads maintain a local array
     with participant GUIDs and sockets, participant_set_generation is
     used to notify them. */
  ddsrt_atomic_uint32_t participant_set_generation;

  /* nparticipants is used primarily for limiting the number of active
     participants, but also during shutdown to determine when it is
     safe to stop the GC thread. */
  ddsrt_mutex_t participant_set_lock;
  ddsrt_cond_t participant_set_cond;
  uint32_t nparticipants;

  /* For tracking (recently) deleted participants */
  struct ddsi_deleted_participants_admin *deleted_participants;

  /* GUID to be used in next call to new_participant */
  struct ddsi_guid ppguid_base;

  /* number of selected interfaces. */
  int n_interfaces;
  struct ddsi_network_interface interfaces[MAX_XMIT_CONNS];
  /* whether we're using a link-local address (and therefore
     only listening to multicasts on that interface) */
  int using_link_local_intf;

  /* Addressing: actual own (preferred) IP address, IP address
     advertised in discovery messages (so that an external IP address on
     a NAT may be advertised), and the DDSI multi-cast address. */
  enum ddsi_recvips_mode recvips_mode;
  struct ddsi_config_in_addr_node *recvips;
  ddsi_locator_t extmask;

  /* Locators */
  ddsi_locator_t loc_spdp_mc;
  ddsi_locator_t loc_meta_mc;
  ddsi_locator_t loc_meta_uc;
  ddsi_locator_t loc_default_mc;
  ddsi_locator_t loc_default_uc;

  /*
    Initial discovery address set, and the current discovery address
    set. These are the addresses that SPDP pings get sent to.
  */
  struct spdp_admin *spdp_schedule;

  ddsrt_mutex_t lock;

  /* Receive thread. (We can only has one for now, cos of the signal
     trigger socket.) Receive buffer pool is per receive thread,
     it is only a global variable because it needs to be freed way later
     than the receive thread itself terminates */
#define MAX_RECV_THREADS 3
  uint32_t n_recv_threads;
  struct recv_thread {
    const char *name;
    struct ddsi_thread_state *thrst;
    struct ddsi_recv_thread_arg arg;
  } recv_threads[MAX_RECV_THREADS];

  /* Listener thread for connection based transports */
  struct ddsi_thread_state *listen_ts;

  /* Flag cleared when stopping (receive threads). FIXME. */
  ddsrt_atomic_uint32_t rtps_keepgoing;

  /* Start time of the DDSI service, for logging relative time stamps,
     should I ever so desire. */
  ddsrt_wctime_t tstart;

  /* Default QoSs for participant, readers and writers (needed for
     eliminating default values in outgoing discovery packets, and for
     supplying values for missing QoS settings in incoming discovery
     packets); plus the actual QoSs needed for the builtin
     endpoints. */
  dds_qos_t default_local_xqos_pp;
  dds_qos_t spdp_endpoint_xqos;
  dds_qos_t builtin_endpoint_xqos_rd;
  dds_qos_t builtin_endpoint_xqos_wr;
#ifdef DDS_HAS_TYPE_DISCOVERY
  dds_qos_t builtin_volatile_xqos_rd;
  dds_qos_t builtin_volatile_xqos_wr;
#endif
#ifdef DDS_HAS_SECURITY
  dds_qos_t builtin_secure_volatile_xqos_rd;
  dds_qos_t builtin_secure_volatile_xqos_wr;
  dds_qos_t builtin_stateless_xqos_rd;
  dds_qos_t builtin_stateless_xqos_wr;
#endif

  /* SPDP packets get very special treatment (they're the only packets
     we accept from writers we don't know) and have their very own
     do-nothing defragmentation and reordering thingummies, as well as a
     global mutex to in lieu of the proxy writer lock. */
  ddsrt_mutex_t spdp_lock;
  struct ddsi_defrag *spdp_defrag;
  struct ddsi_reorder *spdp_reorder;

  /* Built-in stuff other than SPDP gets funneled through the builtins
     delivery queue; currently just SEDP and PMD */
  struct ddsi_dqueue *builtins_dqueue;

  struct ddsi_debug_monitor *debmon;

  uint32_t networkQueueId;
  struct ddsi_thread_state *channel_reader_thrst;

  /* Application data gets its own delivery queue */
  struct ddsi_dqueue *user_dqueue;

  /* Transmit side: pool for transmit queue*/
  struct ddsi_xmsgpool *xmsgpool;
  struct ddsi_sertype *spdp_type; /* key = participant GUID */
  struct ddsi_sertype *sedp_reader_type; /* key = endpoint GUID */
  struct ddsi_sertype *sedp_writer_type; /* key = endpoint GUID */
  struct ddsi_sertype *sedp_topic_type; /* key = topic GUID */
  struct ddsi_sertype *pmd_type; /* participant message data */
#ifdef DDS_HAS_TYPE_DISCOVERY
  struct ddsi_sertype *tl_svc_request_type; /* TypeLookup service request, no key */
  struct ddsi_sertype *tl_svc_reply_type; /* TypeLookup service reply, no key */
#endif
#ifdef DDS_HAS_SECURITY
  struct ddsi_sertype *spdp_secure_type; /* key = participant GUID */
  struct ddsi_sertype *sedp_reader_secure_type; /* key = endpoint GUID */
  struct ddsi_sertype *sedp_writer_secure_type; /* key = endpoint GUID */
  struct ddsi_sertype *pmd_secure_type; /* participant message data */
  struct ddsi_sertype *pgm_stateless_type; /* participant generic message */
  struct ddsi_sertype *pgm_volatile_type; /* participant generic message */
#endif

  ddsrt_mutex_t sendq_lock;
  ddsrt_cond_t sendq_cond;
  unsigned sendq_length;
  struct ddsi_xpack *sendq_head;
  struct ddsi_xpack *sendq_tail;
  int sendq_stop;
  struct ddsi_thread_state *sendq_ts;
  bool sendq_running;
  ddsrt_mutex_t sendq_running_lock;

  /* File for dumping captured packets, NULL if disabled */
  FILE *pcap_fp;
  ddsrt_mutex_t pcap_lock;

  struct ddsi_builtin_topic_interface *builtin_topic_interface;

  struct ddsi_mcgroup_membership *mship;

  ddsrt_mutex_t sertypes_lock;
  struct ddsrt_hh *sertypes;

#ifdef DDS_HAS_TYPELIB
  ddsrt_mutex_t typelib_lock;
  ddsrt_avl_tree_t typelib;
  ddsrt_avl_tree_t typedeps;
  ddsrt_avl_tree_t typedeps_reverse;
  ddsrt_cond_t typelib_resolved_cond;
#endif
#ifdef DDS_HAS_TOPIC_DISCOVERY
  ddsrt_mutex_t topic_defs_lock;
  struct ddsrt_hh *topic_defs;
#endif

  ddsrt_mutex_t new_topic_lock;
  ddsrt_cond_t new_topic_cond;
  uint32_t new_topic_version;

  /* security globals */
#ifdef DDS_HAS_SECURITY
  struct dds_security_context *security_context;
  struct ddsi_hsadmin *hsadmin;
  bool handshake_include_optional;
#endif

  /* naming */
  ddsrt_mutex_t naming_lock;
  ddsrt_prng_t naming_rng;
};

#if defined (__cplusplus)
}
#endif

#endif /* DDSI_DOMAINGV_H */
