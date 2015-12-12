/*
 * EIGRP Interface functions.
 * Copyright (C) 1999, 2000 Toshiaki Takada
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2, or (at your
 * option) any later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <zebra.h>

#include "thread.h"
#include "linklist.h"
#include "prefix.h"
#include "if.h"
#include "table.h"
#include "memory.h"
#include "command.h"
#include "stream.h"
#include "log.h"

#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_zebra.h"
#include "eigrpd/eigrp_vty.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"

static void eigrp_delete_from_if (struct interface *, struct eigrp_interface *);

static void
eigrp_add_to_if (struct interface *ifp, struct eigrp_interface *ei)
{
  struct route_node *rn;
  struct prefix p;

  p = *ei->address;
  p.prefixlen = IPV4_MAX_PREFIXLEN;

  rn = route_node_get (IF_OIFS (ifp), &p);
  /* rn->info should either be NULL or equal to this ei
   * as route_node_get may return an existing node
   */
  assert (!rn->info || rn->info == ei);
  rn->info = ei;
}

struct eigrp_interface *
eigrp_if_new (struct eigrp *eigrp, struct interface *ifp, struct prefix *p)
{
  struct eigrp_interface *ei;

  if ((ei = eigrp_if_table_lookup (ifp, p)) == NULL)
    {
      ei = XCALLOC (MTYPE_EIGRP_IF, sizeof (struct eigrp_interface));
      memset (ei, 0, sizeof (struct eigrp_interface));
    }
  else
    return ei;

  /* Set zebra interface pointer. */
  ei->ifp = ifp;
  ei->address = p;

  eigrp_add_to_if (ifp, ei);
  listnode_add (eigrp->eiflist, ei);

  ei->type = EIGRP_IFTYPE_BROADCAST;

  /* Initialize neighbor list. */
  ei->nbrs = route_table_init ();

  return ei;
}

/* lookup ei for specified prefix/ifp */
struct eigrp_interface *
eigrp_if_table_lookup (struct interface *ifp, struct prefix *prefix)
{
  struct prefix p;
  struct route_node *rn;
  struct eigrp_interface *rninfo = NULL;

  p = *prefix;
  p.prefixlen = IPV4_MAX_PREFIXLEN;

  /* route_node_get implicitly locks */
  if ((rn = route_node_lookup (IF_OIFS (ifp), &p)))
    {
      rninfo = (struct eigrp_interface *) rn->info;
      route_unlock_node (rn);
    }

  return rninfo;
}


int
eigrp_if_delete_hook (struct interface *ifp)
{
  int rc = 0;

  struct route_node *rn;

  route_table_finish (IF_OIFS (ifp));

  for (rn = route_top (IF_OIFS_PARAMS (ifp)); rn; rn = route_next (rn))
      if (rn->info)
        eigrp_del_if_params (rn->info);
    route_table_finish (IF_OIFS_PARAMS (ifp));

  return rc;
}

struct list *eigrp_iflist;

void
eigrp_if_init ()
{
  /* Initialize Zebra interface data structure. */
  if_init (VRF_DEFAULT, &eigrp_iflist);
  eigrp_om->iflist = iflist;
  if_add_hook (IF_NEW_HOOK, eigrp_if_new_hook);
  if_add_hook (IF_DELETE_HOOK, eigrp_if_delete_hook);
}

int
eigrp_if_new_hook (struct interface *ifp)
{
  int rc = 0;

  ifp->info = XCALLOC (MTYPE_EIGRP_IF_INFO, sizeof (struct eigrp_if_info));

  IF_OIFS (ifp) = route_table_init ();
  IF_OIFS_PARAMS (ifp) = route_table_init ();

  IF_DEF_PARAMS (ifp) = eigrp_new_if_params ();

  SET_IF_PARAM (IF_DEF_PARAMS (ifp), v_hello);
  IF_DEF_PARAMS (ifp)->v_hello = (u_int32_t) EIGRP_HELLO_INTERVAL_DEFAULT;

  SET_IF_PARAM (IF_DEF_PARAMS (ifp), v_wait);
  IF_DEF_PARAMS (ifp)->v_wait = (u_int16_t) EIGRP_HOLD_INTERVAL_DEFAULT;

  SET_IF_PARAM (IF_DEF_PARAMS (ifp), bandwidth);
  IF_DEF_PARAMS (ifp)->bandwidth = (u_int32_t) EIGRP_BANDWIDTH_DEFAULT;

  SET_IF_PARAM (IF_DEF_PARAMS (ifp), delay);
  IF_DEF_PARAMS (ifp)->delay = (u_int32_t) EIGRP_DELAY_DEFAULT;

  SET_IF_PARAM (IF_DEF_PARAMS (ifp), reliability);
  IF_DEF_PARAMS (ifp)->reliability = (u_char) EIGRP_RELIABILITY_DEFAULT;

  SET_IF_PARAM (IF_DEF_PARAMS (ifp), load);
  IF_DEF_PARAMS (ifp)->load = (u_char) EIGRP_LOAD_DEFAULT;


  return rc;
}

struct eigrp_if_params *
eigrp_new_if_params (void)
{
  struct eigrp_if_params *eip;

  eip = XCALLOC (MTYPE_EIGRP_IF_PARAMS, sizeof (struct eigrp_if_params));
  if (!eip)
      return NULL;

  UNSET_IF_PARAM (eip, passive_interface);
  UNSET_IF_PARAM (eip, v_hello);
  UNSET_IF_PARAM (eip, v_wait);
  UNSET_IF_PARAM (eip, bandwidth);
  UNSET_IF_PARAM (eip, delay);
  UNSET_IF_PARAM (eip, reliability);
  UNSET_IF_PARAM (eip, load);
  return eip;
}

void
eigrp_del_if_params (struct eigrp_if_params *eip)
{
  XFREE (MTYPE_EIGRP_IF_PARAMS, eip);
}

struct eigrp_if_params *
eigrp_lookup_if_params (struct interface *ifp, struct in_addr addr)
{
  struct prefix_ipv4 p;
  struct route_node *rn;

  p.family = AF_INET;
  p.prefixlen = IPV4_MAX_PREFIXLEN;
  p.prefix = addr;

  rn = route_node_lookup (IF_OIFS_PARAMS (ifp), (struct prefix*)&p);

  if (rn)
    {
      route_unlock_node (rn);
      return rn->info;
    }

  return NULL;
}

int
eigrp_if_up (struct eigrp_interface *ei)
{
  struct eigrp_topology_node *tn;
  struct eigrp_topology_entry *te;
  struct eigrp_metrics metric;

  if (ei == NULL)
    return 0;

  struct eigrp *eigrp= eigrp_lookup ();
  if (eigrp != NULL)
    eigrp_adjust_sndbuflen (eigrp, ei->ifp->mtu);
  else
    zlog_warn ("%s: eigrp_lookup() returned NULL", __func__);
  eigrp_if_stream_set (ei);

  /* Set multicast memberships appropriately for new state. */
    eigrp_if_set_multicast(ei);

  thread_add_event (master, eigrp_hello_timer, ei, (1));

  /*Prepare metrics*/
  metric.bandwith = EIGRP_IF_PARAM(ei,bandwidth);
  metric.delay = EIGRP_IF_PARAM(ei,delay)*100*256;
  metric.load =  EIGRP_IF_PARAM(ei,load);
  metric.reliability =  EIGRP_IF_PARAM(ei,reliability);

  /*Add connected entry to topology table*/
  tn = eigrp_topology_node_new();

  tn->destination->family = AF_INET;
  tn->destination->prefix = ei->connected->address->u.prefix4;
  tn->destination->prefixlen = ei->connected->address->prefixlen;
  tn->dest_type = EIGRP_TOPOLOGY_TYPE_CONNECTED;
  apply_mask_ipv4(tn->destination);

  tn->state = EIGRP_TOPOLOGY_NODE_PASSIVE;
  tn->fdistance = eigrp_calculate_metrics(&metric);

  te = eigrp_topology_entry_new();
  te->ei = ei;
  te->received_metric = metric;
  eigrp_topology_entry_add(tn,te);
  eigrp_topology_node_add(eigrp->topology_table,tn);

  return 1;
}

int
eigrp_if_down (struct eigrp_interface *ei)
{
  if (ei == NULL)
    return 0;

  THREAD_OFF(ei->t_hello);
  /* Shutdown packet reception and sending */
  eigrp_if_stream_unset (ei);

  return 1;
}

void
eigrp_if_stream_set (struct eigrp_interface *ei)
{
  /* set output fifo queue. */
  if (ei->obuf == NULL)
    ei->obuf = eigrp_fifo_new ();
}

void
eigrp_if_stream_unset (struct eigrp_interface *ei)
{
  struct eigrp *eigrp = ei->eigrp;

  if (ei->obuf)
    {
     eigrp_fifo_free (ei->obuf);
     ei->obuf = NULL;

     if (ei->on_write_q)
       {
         listnode_delete (eigrp->oi_write_q, ei);
         if (list_isempty(eigrp->oi_write_q))
           thread_cancel (eigrp->t_write);
         ei->on_write_q = 0;
       }
    }
}

void
eigrp_if_set_multicast(struct eigrp_interface *ei)
{
  if ((EIGRP_IF_PASSIVE_STATUS(ei) == EIGRP_IF_ACTIVE))
    {
      /* The interface should belong to the EIGRP-all-routers group. */
      if (!EI_MEMBER_CHECK(ei, MEMBER_ALLROUTERS) &&
          (eigrp_if_add_allspfrouters(ei->eigrp, ei->address,
                                     ei->ifp->ifindex) >= 0))
          /* Set the flag only if the system call to join succeeded. */
          EI_MEMBER_JOINED(ei, MEMBER_ALLROUTERS);
    }
  else
    {
      /* The interface should NOT belong to the EIGRP-all-routers group. */
      if (EI_MEMBER_CHECK(ei, MEMBER_ALLROUTERS))
        {
          /* Only actually drop if this is the last reference */
          if (EI_MEMBER_COUNT(ei, MEMBER_ALLROUTERS) == 1)
            eigrp_if_drop_allspfrouters (ei->eigrp, ei->address,
                                        ei->ifp->ifindex);
          /* Unset the flag regardless of whether the system call to leave
             the group succeeded, since it's much safer to assume that
             we are not a member. */
          EI_MEMBER_LEFT(ei,MEMBER_ALLROUTERS);
        }
    }
}

u_char
eigrp_default_iftype(struct interface *ifp)
{
  if (if_is_pointopoint (ifp))
    return EIGRP_IFTYPE_POINTOPOINT;
  else if (if_is_loopback (ifp))
    return EIGRP_IFTYPE_LOOPBACK;
  else
    return EIGRP_IFTYPE_BROADCAST;
}

void
eigrp_if_free (struct eigrp_interface *ei)
{
  eigrp_if_down (ei);

  route_table_finish (ei->nbrs);

  eigrp_delete_from_if (ei->ifp, ei);

  listnode_delete (ei->eigrp->eiflist, ei);

  thread_cancel_event (master, ei);

  memset (ei, 0, sizeof (*ei));
  XFREE (MTYPE_EIGRP_IF, ei);
}

static void
eigrp_delete_from_if (struct interface *ifp, struct eigrp_interface *ei)
{
  struct route_node *rn;
  struct prefix p;

  p = *ei->address;
  p.prefixlen = IPV4_MAX_PREFIXLEN;

  rn = route_node_lookup (IF_OIFS (ei->ifp), &p);
  assert (rn);
  assert (rn->info);
  rn->info = NULL;
  route_unlock_node (rn);
  route_unlock_node (rn);
}

/* Simulate down/up on the interface.  This is needed, for example, when
   the MTU changes. */
void
eigrp_if_reset(struct interface *ifp)
{
  struct route_node *rn;

  for (rn = route_top (IF_OIFS (ifp)); rn; rn = route_next (rn))
    {
      struct eigrp_interface *ei;

      if ( (ei = rn->info) == NULL)
        continue;

      eigrp_if_down(ei);
      eigrp_if_up(ei);
    }
}

struct eigrp_interface *
eigrp_if_lookup_by_local_addr (struct eigrp *eigrp,
                              struct interface *ifp, struct in_addr address)
{
  struct listnode *node;
  struct eigrp_interface *ei;

  for (ALL_LIST_ELEMENTS_RO (eigrp->eiflist, node, ei))
    {
      if (ifp && ei->ifp != ifp)
        continue;

      if (IPV4_ADDR_SAME (&address, &ei->address->u.prefix4))
        return ei;
    }

  return NULL;
}

/* determine receiving interface by ifp and source address */
struct eigrp_interface *
eigrp_if_lookup_recv_if (struct eigrp *eigrp, struct in_addr src,
                        struct interface *ifp)
{
  struct route_node *rn;
  struct prefix_ipv4 addr;
  struct eigrp_interface *ei, *match;

  addr.family = AF_INET;
  addr.prefix = src;
  addr.prefixlen = IPV4_MAX_BITLEN;

  match = NULL;

  for (rn = route_top (IF_OIFS (ifp)); rn; rn = route_next (rn))
    {
      ei = rn->info;

      if (!ei) /* oi can be NULL for PtP aliases */
        continue;

      if (if_is_loopback (ei->ifp))
        continue;

      if (prefix_match (CONNECTED_PREFIX(ei->connected),
                        (struct prefix *) &addr))
        {
          if ( (match == NULL) ||
               (match->address->prefixlen < ei->address->prefixlen)
             )
            match = ei;
        }
    }

  return match;
}
