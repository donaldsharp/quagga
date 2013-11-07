/*
 * eigrp_structs.h
 *
 *  Created on: Nov 7, 2013
 *      Author: janovic
 */

#ifndef _ZEBRA_EIGRP_STRUCTS_H_
#define _ZEBRA_EIGRP_STRUCTS_H_

#include "eigrpd/eigrp_macros.h"

/* EIGRP master for system wide configuration and variables. */
struct eigrp_master
{
  /* EIGRP instance. */
  struct list *eigrp;

  /* EIGRP thread master. */
  struct thread_master *master;

  /* Zebra interface list. */
  struct list *iflist;

  /* EIGRP start time. */
  time_t start_time;

  /* Various EIGRP global configuration. */
    u_char options;

#define EIGRP_MASTER_SHUTDOWN (1 << 0) /* deferred-shutdown */
};

struct eigrp
{

  /* EIGRP Router ID. */
  struct in_addr router_id;             /* Configured automatically. */
  struct in_addr router_id_static;      /* Configured manually. */

  struct list *eiflist;                 /* eigrp interfaces */
  u_char passive_interface_default;   /* passive-interface default */

  int AS; /* Autonomous system number */

  unsigned int fd;
  unsigned int maxsndbuflen;

  u_int32_t sequence_number;    /*Global EIGRP sequence number*/

  struct stream *ibuf;
  struct list *oi_write_q;

  /*Threads*/
  struct thread *t_write;
  struct thread *t_read;

  struct route_table *networks;         /* EIGRP config networks. */

  u_char k_values[5]; /*Array for K values configuration*/

};

//------------------------------------------------------------------------------------------------------------------------------------------

/*EIGRP interface structure*/
struct eigrp_interface
{
  /* This interface's parent eigrp instance. */
    struct eigrp *eigrp;

    /* Interface data from zebra. */
    struct interface *ifp;

    /* Packet send buffer. */
    struct eigrp_fifo *obuf;               /* Output queue */

    /* To which multicast groups do we currently belong? */

    /* Configured varables. */
      struct eigrp_if_params *params;

    u_char multicast_memberships;
#define EI_MEMBER_FLAG(M) (1 << (M))
#define EI_MEMBER_COUNT(O,M) (IF_EIGRP_IF_INFO(ei->ifp)->membership_counts[(M)])
#define EI_MEMBER_CHECK(O,M) \
    (CHECK_FLAG((O)->multicast_memberships, EI_MEMBER_FLAG(M)))
#define EI_MEMBER_JOINED(O,M) \
  do { \
    SET_FLAG ((O)->multicast_memberships, EI_MEMBER_FLAG(M)); \
    IF_EIGRP_IF_INFO((O)->ifp)->membership_counts[(M)]++; \
  } while (0)
#define EI_MEMBER_LEFT(O,M) \
  do { \
    UNSET_FLAG ((O)->multicast_memberships, EI_MEMBER_FLAG(M)); \
    IF_EIGRP_IF_INFO((O)->ifp)->membership_counts[(M)]--; \
  } while (0)


    /* EIGRP Network Type. */
    u_char type;
 #define EIGRP_IFTYPE_NONE                0
 #define EIGRP_IFTYPE_POINTOPOINT         1
 #define EIGRP_IFTYPE_BROADCAST           2
 #define EIGRP_IFTYPE_NBMA                3
 #define EIGRP_IFTYPE_POINTOMULTIPOINT    4
 #define EIGRP_IFTYPE_LOOPBACK            5
 #define EIGRP_IFTYPE_MAX                 6

    struct prefix *address;             /* Interface prefix */
    struct connected *connected;          /* Pointer to connected */

    /* Neighbor information. */
      struct route_table *nbrs;             /* EIGRP Neighbor List */

    /* Threads. */
    struct thread *t_hello;               /* timer */

    int on_write_q;

    /* Statistics fields. */
      u_int32_t hello_in;           /* Hello message input count. */
      u_int32_t update_in;           /* Update message input count. */
};

struct eigrp_if_params
{
  DECLARE_IF_PARAM (u_char, passive_interface);      /* EIGRP Interface is passive: no sending or receiving (no need to join multicast groups) */
  DECLARE_IF_PARAM (u_int32_t, v_hello);             /* Hello Interval */
  DECLARE_IF_PARAM (u_int16_t, v_wait);              /* Router Hold Time Interval */
  DECLARE_IF_PARAM (u_char, type);                   /* type of interface */

#define EIGRP_IF_ACTIVE                  0
#define EIGRP_IF_PASSIVE                 1

};

enum
{
  MEMBER_ALLROUTERS = 0,
  MEMBER_MAX,
};

struct eigrp_if_info
{
  struct eigrp_if_params *def_params;
  struct route_table *params;
  struct route_table *eifs;
  unsigned int membership_counts[MEMBER_MAX];   /* multicast group refcnts */
};


//------------------------------------------------------------------------------------------------------------------------------------------

/* Neighbor Data Structure */
struct eigrp_neighbor
{
  /* This neighbor's parent eigrp interface. */
  struct eigrp_interface *ei;

  /* OSPF neighbor Information */
  u_char state;                               /* neigbor status. */
  u_int32_t recv_sequence_number;             /* Last received sequence Number. */
  u_int32_t ack;                              /* Acknowledgement number*/

  /*If packet is unacknowledged, we try to send it again 16 times*/
  u_char retrans_counter;

  struct in_addr src;                   /* Neighbor Src address. */

  u_char K1;
  u_char K2;
  u_char K3;
  u_char K4;
  u_char K5;
  u_char K6;

  /* Timer values. */
  u_int16_t v_holddown;

  /* Threads. */
  struct thread *t_holddown;

  struct eigrp_fifo *retrans_queue;
  struct thread *t_retrans_timer;
};

//---------------------------------------------------------------------------------------------------------------------------------------------

struct eigrp_packet
{
  struct eigrp_packet *next;
  struct eigrp_packet *previous;

  /* Pointer to data stream. */
  struct stream *s;

  /* IP destination address. */
  struct in_addr dst;

  /* EIGRP packet length. */
  u_int16_t length;
};

struct eigrp_fifo
{
  unsigned long count;

  struct eigrp_packet *head;

  struct eigrp_packet *tail;
};

struct eigrp_header_fifo
{
  unsigned long count;

  struct eigrp_header *head;

  struct eigrp_header *tail;
};

struct eigrp_header
{
  u_char version;
  u_char opcode;
  u_int16_t checksum;
  u_int32_t flags;
  u_int32_t sequence;
  u_int32_t ack;
  u_int16_t routerID;
  u_int16_t ASNumber;

} __attribute__((packed));

struct TLV_Parameter_Type
{
    u_int16_t type;
    u_int16_t length;
    u_char K1;
    u_char K2;
    u_char K3;
    u_char K4;
    u_char K5;
    u_char K6;
    u_int16_t hold_time;
} __attribute__((packed));

//---------------------------------------------------------------------------------------------------------------------------------------------

/* EIGRP Topology table node structure */
struct eigrp_topology_node
{
        struct list *entries;
        struct prefix_ipv4 *destination;
};

/* EIGRP Topology table record structure */
struct eigrp_topology_entry
{
        struct prefix *data;
        unsigned long distance;
        struct in_addr *adv_router;
        u_char flags;
};


#endif /* _ZEBRA_EIGRP_STRUCTURES_H_ */
