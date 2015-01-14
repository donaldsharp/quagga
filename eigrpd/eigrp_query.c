/*
 * EIGRP Sending and Receiving EIGRP Query Packets.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <zebra.h>

#include "thread.h"
#include "memory.h"
#include "linklist.h"
#include "prefix.h"
#include "if.h"
#include "table.h"
#include "sockunion.h"
#include "stream.h"
#include "log.h"
#include "sockopt.h"
#include "checksum.h"
#include "md5.h"

#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_zebra.h"
#include "eigrpd/eigrp_vty.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_macros.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_fsm.h"


u_int32_t
eigrp_query_send_all (struct eigrp *eigrp, struct eigrp_neighbor_entry *te)
{
  struct eigrp_interface *iface;
  struct listnode *node, *node2, *nnode2;
  struct eigrp_neighbor *nbr;
  u_int32_t counter;

  if (eigrp == NULL)
    {
      zlog_debug("EIGRP Routing Process not enabled");
      return 0;
    }

  counter=0;
  for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, node, iface))
    {
      for (ALL_LIST_ELEMENTS(iface->nbrs, node2, nnode2, nbr))
        {
          if (nbr->state == EIGRP_NEIGHBOR_UP){
            eigrp_send_query(nbr, te);
            counter++;
          }
        }
    }
  return counter;
}

/*EIGRP QUERY read function*/
void
eigrp_query_receive (struct eigrp *eigrp, struct ip *iph, struct eigrp_header *eigrph,
                     struct stream * s, struct eigrp_interface *ei, int size)
{
  struct eigrp_neighbor *nbr;
  struct TLV_IPv4_Internal_type *tlv;
  struct eigrp_prefix_entry *temp_tn;
  struct eigrp_neighbor_entry *temp_te;

  u_int16_t type;

  /* increment statistics. */
  ei->query_in++;

  /* get neighbor struct */
  nbr = eigrp_nbr_get(ei, eigrph, iph);

  /* neighbor must be valid, eigrp_nbr_get creates if none existed */
  assert(nbr);

  nbr->recv_sequence_number = ntohl(eigrph->sequence);

  while (s->endp > s->getp)
    {
      type = stream_getw(s);
      if (type == EIGRP_TLV_IPv4_INT)
        {
          stream_set_getp(s, s->getp - sizeof(u_int16_t));

          tlv = eigrp_read_ipv4_tlv(s);

          struct prefix_ipv4 *dest_addr;
          dest_addr = prefix_ipv4_new();
          dest_addr->prefix = tlv->destination;
          dest_addr->prefixlen = tlv->prefix_length;
          struct eigrp_prefix_entry *dest = eigrp_topology_table_lookup_ipv4(
              eigrp->topology_table, dest_addr);

//          temp_te = XCALLOC(MTYPE_EIGRP_NEIGHBOR_ENTRY,
//              sizeof(struct eigrp_neighbor_entry));
//          temp_tn = XCALLOC(MTYPE_EIGRP_PREFIX_ENTRY,
//              sizeof(struct eigrp_prefix_entry));
//          temp_te->total_metric.delay = 0xFFFFFFFF;
//          temp_te->prefix = temp_tn;
//          temp_tn->destination_ipv4 = dest_addr;
//
//          XFREE(MTYPE_EIGRP_NEIGHBOR_ENTRY, temp_te);
//          XFREE(MTYPE_EIGRP_PREFIX_ENTRY, temp_tn);

          /* If the destination exists (it should, but one never know)*/
          if (dest != NULL)
            {
              struct eigrp_fsm_action_message *msg;
              msg = XCALLOC(MTYPE_EIGRP_FSM_MSG,
                  sizeof(struct eigrp_fsm_action_message));
              struct eigrp_neighbor_entry *entry = eigrp_prefix_entry_lookup(
                  dest->entries, nbr);
              msg->packet_type = EIGRP_OPC_QUERY;
              msg->eigrp = eigrp;
              msg->data_type = EIGRP_TLV_IPv4_INT;
              msg->adv_router = nbr;
              msg->data.ipv4_int_type = tlv;
              msg->entry = entry;
              msg->prefix = dest;
              int event = eigrp_get_fsm_event(msg);
              EIGRP_FSM_EVENT_SCHEDULE(msg, event);
            }
          eigrp_IPv4_InternalTLV_free (tlv);
        }
    }
  eigrp_hello_send_ack(nbr);
}

void
eigrp_send_query (struct eigrp_neighbor *nbr, struct eigrp_neighbor_entry *te)
{
  struct eigrp_packet *ep;
  u_int16_t length = EIGRP_HEADER_LEN;

  ep = eigrp_packet_new(nbr->ei->ifp->mtu);

  /* Prepare EIGRP INIT UPDATE header */
  eigrp_packet_header_init(EIGRP_OPC_QUERY, nbr->ei, ep->s, 0,
                           nbr->ei->eigrp->sequence_number, 0);

  length += eigrp_add_internalTLV_to_stream(ep->s, te);

  listnode_add(te->prefix->rij, nbr);
  /* EIGRP Checksum */
  eigrp_packet_checksum(nbr->ei, ep->s, length);

  ep->length = length;

  ep->dst.s_addr = nbr->src.s_addr;

  /*This ack number we await from neighbor*/
  ep->sequence_number = nbr->ei->eigrp->sequence_number;

  /*Put packet to retransmission queue*/
  eigrp_fifo_push_head(nbr->retrans_queue, ep);

  if (nbr->retrans_queue->count == 1)
    {
      eigrp_send_packet_reliably(nbr);
    }
}
