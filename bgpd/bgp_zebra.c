/* zebra client
   Copyright (C) 1997, 98, 99 Kunihiro Ishiguro

This file is part of GNU Zebra.

GNU Zebra is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

GNU Zebra is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Zebra; see the file COPYING.  If not, write to the
Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

#include <zebra.h>

#include "command.h"
#include "stream.h"
#include "network.h"
#include "prefix.h"
#include "log.h"
#include "sockunion.h"
#include "zclient.h"
#include "routemap.h"
#include "thread.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_nexthop.h"
#include "bgpd/bgp_zebra.h"
#include "bgpd/bgp_fsm.h"
#include "bgpd/bgp_debug.h"
#include "bgpd/bgp_mpath.h"
#include "bgpd/bgp_nexthop.h"
#include "bgpd/bgp_nht.h"

/* All information about zebra. */
struct zclient *zclient = NULL;
struct in_addr router_id_zebra;

/* Growable buffer for nexthops sent to zebra */
struct stream *bgp_nexthop_buf = NULL;
struct stream *bgp_ifindices_buf = NULL;

/* These array buffers are used in making a copy of the attributes for
   route-map apply. Arrays are being used here to minimize mallocs and
   frees for the temporary copy of the attributes.
   Given the zapi api expects the nexthop buffer to contain pointer to
   pointers for nexthops, we couldnt have used a single nexthop variable
   on the stack, hence we had two options:
     1. maintain a linked-list and free it after zapi_*_route call
     2. use an array to avoid number of mallocs.
   Number of supported next-hops are finite, use of arrays should be ok. */
struct attr attr_cp[BGP_MAXIMUM_MAXPATHS];
struct attr_extra attr_extra_cp[BGP_MAXIMUM_MAXPATHS];
int    attr_index = 0;

/* Once per address-family initialization of the attribute array */
#define BGP_INFO_ATTR_BUF_INIT()\
do {\
  memset(attr_cp, 0, BGP_MAXIMUM_MAXPATHS * sizeof(struct attr));\
  memset(attr_extra_cp, 0, BGP_MAXIMUM_MAXPATHS * sizeof(struct attr_extra));\
  attr_index = 0;\
} while (0)

#define BGP_INFO_ATTR_BUF_COPY(info_src, info_dst)\
do { \
  *info_dst = *info_src; \
  assert(attr_index != BGP_MAXIMUM_MAXPATHS);\
  attr_cp[attr_index].extra = &attr_extra_cp[attr_index]; \
  bgp_attr_dup (&attr_cp[attr_index], info_src->attr); \
  bgp_attr_deep_dup (&attr_cp[attr_index], info_src->attr); \
  info_dst->attr = &attr_cp[attr_index]; \
  attr_index++;\
} while (0)

#define BGP_INFO_ATTR_BUF_FREE(info) \
do { \
  bgp_attr_deep_free(info->attr); \
} while (0)

/* Router-id update message from zebra. */
static int
bgp_router_id_update (int command, struct zclient *zclient, zebra_size_t length,
    vrf_id_t vrf_id)
{
  struct prefix router_id;
  struct listnode *node, *nnode;
  struct bgp *bgp;

  zebra_router_id_update_read(zclient->ibuf,&router_id);

  if (BGP_DEBUG(zebra, ZEBRA))
    {
      char buf[128];
      prefix2str(&router_id, buf, sizeof(buf));
      zlog_debug("Zebra rcvd: router id update %s", buf);
    }

  router_id_zebra = router_id.u.prefix4;

  for (ALL_LIST_ELEMENTS (bm->bgp, node, nnode, bgp))
    {
      if (!bgp->router_id_static.s_addr)
        bgp_router_id_set (bgp, &router_id.u.prefix4);
    }

  return 0;
}

/* Nexthop update message from zebra. */
static int
bgp_read_nexthop_update (int command, struct zclient *zclient,
			 zebra_size_t length, vrf_id_t vrf_id)
{
  bgp_parse_nexthop_update();
  return 0;
}

static void
bgp_nbr_connected_add (struct nbr_connected *ifc)
{
  struct listnode *node, *nnode, *mnode;
  struct bgp *bgp;
  struct peer *peer;

  for (ALL_LIST_ELEMENTS_RO (bm->bgp, mnode, bgp))
    {
      for (ALL_LIST_ELEMENTS (bgp->peer, node, nnode, peer))
        {
          if (peer->conf_if && (strcmp (peer->conf_if, ifc->ifp->name) == 0))
            {
              if (peer_active(peer))
                BGP_EVENT_ADD (peer, BGP_Stop);
              BGP_EVENT_ADD (peer, BGP_Start);
            }
        }
    }
}

static void
bgp_nbr_connected_delete (struct nbr_connected *ifc)
{
  struct listnode *node, *nnode, *mnode;
  struct bgp *bgp;
  struct peer *peer;

  for (ALL_LIST_ELEMENTS_RO (bm->bgp, mnode, bgp))
    {
      for (ALL_LIST_ELEMENTS (bgp->peer, node, nnode, peer))
        {
          if (peer->conf_if && (strcmp (peer->conf_if, ifc->ifp->name) == 0))
            {
              BGP_EVENT_ADD (peer, BGP_Stop);
            }
        }
    }
}

/* Inteface addition message from zebra. */
static int
bgp_interface_add (int command, struct zclient *zclient, zebra_size_t length,
    vrf_id_t vrf_id)
{
  struct interface *ifp;

  ifp = zebra_interface_add_read (zclient->ibuf, vrf_id);

  if (BGP_DEBUG(zebra, ZEBRA) && ifp)
    zlog_debug("Zebra rcvd: interface add %s", ifp->name);

  return 0;
}

static int
bgp_interface_delete (int command, struct zclient *zclient,
		      zebra_size_t length, vrf_id_t vrf_id)
{
  struct stream *s;
  struct interface *ifp;

  s = zclient->ibuf;
  ifp = zebra_interface_state_read (s, vrf_id);
  ifp->ifindex = IFINDEX_INTERNAL;

  if (BGP_DEBUG(zebra, ZEBRA))
    zlog_debug("Zebra rcvd: interface delete %s", ifp->name);

  return 0;
}

static int
bgp_interface_up (int command, struct zclient *zclient, zebra_size_t length,
    vrf_id_t vrf_id)
{
  struct stream *s;
  struct interface *ifp;
  struct connected *c;
  struct nbr_connected *nc;
  struct listnode *node, *nnode;

  s = zclient->ibuf;
  ifp = zebra_interface_state_read (s, vrf_id);

  if (! ifp)
    return 0;

  if (BGP_DEBUG(zebra, ZEBRA))
    zlog_debug("Zebra rcvd: interface %s up", ifp->name);

  for (ALL_LIST_ELEMENTS (ifp->connected, node, nnode, c))
    bgp_connected_add (c);

  for (ALL_LIST_ELEMENTS (ifp->nbr_connected, node, nnode, nc))
    bgp_nbr_connected_add (nc);

  return 0;
}

static int
bgp_interface_down (int command, struct zclient *zclient, zebra_size_t length,
    vrf_id_t vrf_id)
{
  struct stream *s;
  struct interface *ifp;
  struct connected *c;
  struct nbr_connected *nc;
  struct listnode *node, *nnode;

  s = zclient->ibuf;
  ifp = zebra_interface_state_read (s, vrf_id);
  if (! ifp)
    return 0;

  if (BGP_DEBUG(zebra, ZEBRA))
    zlog_debug("Zebra rcvd: interface %s down", ifp->name);

  for (ALL_LIST_ELEMENTS (ifp->connected, node, nnode, c))
    bgp_connected_delete (c);

  for (ALL_LIST_ELEMENTS (ifp->nbr_connected, node, nnode, nc))
    bgp_nbr_connected_delete (nc);

  /* Fast external-failover */
  {
    struct listnode *mnode;
    struct bgp *bgp;
    struct peer *peer;

    for (ALL_LIST_ELEMENTS_RO (bm->bgp, mnode, bgp))
      {
	if (CHECK_FLAG (bgp->flags, BGP_FLAG_NO_FAST_EXT_FAILOVER))
	  continue;

	for (ALL_LIST_ELEMENTS (bgp->peer, node, nnode, peer))
	  {
	    if ((peer->ttl != 1) && (peer->gtsm_hops != 1))
	      continue;

	    if (ifp == peer->nexthop.ifp)
	      BGP_EVENT_ADD (peer, BGP_Stop);
	  }
      }
  }

  return 0;
}

static int
bgp_interface_address_add (int command, struct zclient *zclient,
			   zebra_size_t length, vrf_id_t vrf_id)
{
  struct connected *ifc;

  ifc = zebra_interface_address_read (command, zclient->ibuf, vrf_id);

  if (ifc == NULL)
    return 0;

  if (BGP_DEBUG(zebra, ZEBRA))
    {
      char buf[128];
      prefix2str(ifc->address, buf, sizeof(buf));
      zlog_debug("Zebra rcvd: interface %s address add %s",
		 ifc->ifp->name, buf);
    }

  if (if_is_operative (ifc->ifp))
    bgp_connected_add (ifc);

  return 0;
}

static int
bgp_interface_address_delete (int command, struct zclient *zclient,
			      zebra_size_t length, vrf_id_t vrf_id)
{
  struct connected *ifc;

  ifc = zebra_interface_address_read (command, zclient->ibuf, vrf_id);

  if (ifc == NULL)
    return 0;

  if (BGP_DEBUG(zebra, ZEBRA))
    {
      char buf[128];
      prefix2str(ifc->address, buf, sizeof(buf));
      zlog_debug("Zebra rcvd: interface %s address delete %s",
		 ifc->ifp->name, buf);
    }

  if (if_is_operative (ifc->ifp))
    bgp_connected_delete (ifc);

  connected_free (ifc);

  return 0;
}

static int
bgp_interface_nbr_address_add (int command, struct zclient *zclient,
			       zebra_size_t length, vrf_id_t vrf_id)
{
  struct nbr_connected *ifc = NULL;

  ifc = zebra_interface_nbr_address_read (command, zclient->ibuf);

  if (ifc == NULL)
    return 0;

  if (BGP_DEBUG(zebra, ZEBRA))
    {
      char buf[128];
      prefix2str(ifc->address, buf, sizeof(buf));
      zlog_debug("Zebra rcvd: interface %s nbr address add %s",
		 ifc->ifp->name, buf);
    }

  if (if_is_operative (ifc->ifp))
    bgp_nbr_connected_add (ifc);

  return 0;
}

static int
bgp_interface_nbr_address_delete (int command, struct zclient *zclient,
				  zebra_size_t length, vrf_id_t vrf_id)
{
  struct nbr_connected *ifc = NULL;

  ifc = zebra_interface_nbr_address_read (command, zclient->ibuf);

  if (ifc == NULL)
    return 0;

  if (BGP_DEBUG(zebra, ZEBRA))
    {
      char buf[128];
      prefix2str(ifc->address, buf, sizeof(buf));
      zlog_debug("Zebra rcvd: interface %s nbr address delete %s",
		 ifc->ifp->name, buf);
    }

  if (if_is_operative (ifc->ifp))
    bgp_nbr_connected_delete (ifc);

  nbr_connected_free (ifc);

  return 0;
}

/* Zebra route add and delete treatment. */
static int
zebra_read_ipv4 (int command, struct zclient *zclient, zebra_size_t length,
    vrf_id_t vrf_id)
{
  struct stream *s;
  struct zapi_ipv4 api;
  struct in_addr nexthop;
  struct prefix_ipv4 p;
  unsigned int ifindex;

  s = zclient->ibuf;
  nexthop.s_addr = 0;

  /* Type, flags, message. */
  api.type = stream_getc (s);
  api.flags = stream_getc (s);
  api.message = stream_getc (s);

  /* IPv4 prefix. */
  memset (&p, 0, sizeof (struct prefix_ipv4));
  p.family = AF_INET;
  p.prefixlen = stream_getc (s);
  stream_get (&p.prefix, s, PSIZE (p.prefixlen));

  /* Nexthop, ifindex, distance, metric. */
  if (CHECK_FLAG (api.message, ZAPI_MESSAGE_NEXTHOP))
    {
      api.nexthop_num = stream_getc (s);
      nexthop.s_addr = stream_get_ipv4 (s);
    }
  if (CHECK_FLAG (api.message, ZAPI_MESSAGE_IFINDEX))
    {
      api.ifindex_num = stream_getc (s);
      ifindex = stream_getl (s); /* ifindex, unused */
    }
  else
    {
      ifindex = 0;
    }
  if (CHECK_FLAG (api.message, ZAPI_MESSAGE_DISTANCE))
    api.distance = stream_getc (s);
  if (CHECK_FLAG (api.message, ZAPI_MESSAGE_METRIC))
    api.metric = stream_getl (s);
  else
    api.metric = 0;

  if (command == ZEBRA_IPV4_ROUTE_ADD)
    {
      if (BGP_DEBUG(zebra, ZEBRA))
	{
	  char buf[2][INET_ADDRSTRLEN];
	  zlog_debug("Zebra rcvd: IPv4 route add %s %s/%d nexthop %s metric %u",
		     zebra_route_string(api.type),
		     inet_ntop(AF_INET, &p.prefix, buf[0], sizeof(buf[0])),
		     p.prefixlen,
		     inet_ntop(AF_INET, &nexthop, buf[1], sizeof(buf[1])),
		     api.metric);
	}
      bgp_redistribute_add((struct prefix *)&p, &nexthop, NULL, ifindex,
			   api.metric, api.type);
    }
  else
    {
      if (BGP_DEBUG(zebra, ZEBRA))
	{
	  char buf[2][INET_ADDRSTRLEN];
	  zlog_debug("Zebra rcvd: IPv4 route delete %s %s/%d "
		     "nexthop %s metric %u",
		     zebra_route_string(api.type),
		     inet_ntop(AF_INET, &p.prefix, buf[0], sizeof(buf[0])),
		     p.prefixlen,
		     inet_ntop(AF_INET, &nexthop, buf[1], sizeof(buf[1])),
		     api.metric);
	}
      bgp_redistribute_delete((struct prefix *)&p, api.type);
    }

  return 0;
}

#ifdef HAVE_IPV6
/* Zebra route add and delete treatment. */
static int
zebra_read_ipv6 (int command, struct zclient *zclient, zebra_size_t length,
    vrf_id_t vrf_id)
{
  struct stream *s;
  struct zapi_ipv6 api;
  struct in6_addr nexthop;
  struct prefix_ipv6 p;
  unsigned int ifindex;

  s = zclient->ibuf;
  memset (&nexthop, 0, sizeof (struct in6_addr));

  /* Type, flags, message. */
  api.type = stream_getc (s);
  api.flags = stream_getc (s);
  api.message = stream_getc (s);

  /* IPv6 prefix. */
  memset (&p, 0, sizeof (struct prefix_ipv6));
  p.family = AF_INET6;
  p.prefixlen = stream_getc (s);
  stream_get (&p.prefix, s, PSIZE (p.prefixlen));

  /* Nexthop, ifindex, distance, metric. */
  if (CHECK_FLAG (api.message, ZAPI_MESSAGE_NEXTHOP))
    {
      api.nexthop_num = stream_getc (s);
      stream_get (&nexthop, s, 16);
    }
  if (CHECK_FLAG (api.message, ZAPI_MESSAGE_IFINDEX))
    {
      api.ifindex_num = stream_getc (s);
      ifindex = stream_getl (s); /* ifindex, unused */
    }
  else
    {
      ifindex = 0;
    }
  if (CHECK_FLAG (api.message, ZAPI_MESSAGE_DISTANCE))
    api.distance = stream_getc (s);
  else
    api.distance = 0;
  if (CHECK_FLAG (api.message, ZAPI_MESSAGE_METRIC))
    api.metric = stream_getl (s);
  else
    api.metric = 0;

  /* Simply ignore link-local address. */
  if (IN6_IS_ADDR_LINKLOCAL (&p.prefix))
    return 0;

  if (command == ZEBRA_IPV6_ROUTE_ADD)
    {
      if (BGP_DEBUG(zebra, ZEBRA))
	{
	  char buf[2][INET6_ADDRSTRLEN];
	  zlog_debug("Zebra rcvd: IPv6 route add %s %s/%d nexthop %s metric %u",
		     zebra_route_string(api.type),
		     inet_ntop(AF_INET6, &p.prefix, buf[0], sizeof(buf[0])),
		     p.prefixlen,
		     inet_ntop(AF_INET, &nexthop, buf[1], sizeof(buf[1])),
		     api.metric);
	}
      bgp_redistribute_add ((struct prefix *)&p, NULL, &nexthop, ifindex,
			    api.metric, api.type);
    }
  else
    {
      if (BGP_DEBUG(zebra, ZEBRA))
	{
	  char buf[2][INET6_ADDRSTRLEN];
	  zlog_debug("Zebra rcvd: IPv6 route delete %s %s/%d "
		     "nexthop %s metric %u",
		     zebra_route_string(api.type),
		     inet_ntop(AF_INET6, &p.prefix, buf[0], sizeof(buf[0])),
		     p.prefixlen,
		     inet_ntop(AF_INET6, &nexthop, buf[1], sizeof(buf[1])),
		     api.metric);
	}
      bgp_redistribute_delete ((struct prefix *) &p, api.type);
    }
  
  return 0;
}
#endif /* HAVE_IPV6 */

struct interface *
if_lookup_by_ipv4 (struct in_addr *addr)
{
  struct listnode *ifnode;
  struct listnode *cnode;
  struct interface *ifp;
  struct connected *connected;
  struct prefix_ipv4 p;
  struct prefix *cp; 
  
  p.family = AF_INET;
  p.prefix = *addr;
  p.prefixlen = IPV4_MAX_BITLEN;

  for (ALL_LIST_ELEMENTS_RO (iflist, ifnode, ifp))
    {
      for (ALL_LIST_ELEMENTS_RO (ifp->connected, cnode, connected))
	{
	  cp = connected->address;
	    
	  if (cp->family == AF_INET)
	    if (prefix_match (cp, (struct prefix *)&p))
	      return ifp;
	}
    }
  return NULL;
}

struct interface *
if_lookup_by_ipv4_exact (struct in_addr *addr)
{
  struct listnode *ifnode;
  struct listnode *cnode;
  struct interface *ifp;
  struct connected *connected;
  struct prefix *cp; 
  
  for (ALL_LIST_ELEMENTS_RO (iflist, ifnode, ifp))
    {
      for (ALL_LIST_ELEMENTS_RO (ifp->connected, cnode, connected))
	{
	  cp = connected->address;
	    
	  if (cp->family == AF_INET)
	    if (IPV4_ADDR_SAME (&cp->u.prefix4, addr))
	      return ifp;
	}
    }
  return NULL;
}

#ifdef HAVE_IPV6
struct interface *
if_lookup_by_ipv6 (struct in6_addr *addr)
{
  struct listnode *ifnode;
  struct listnode *cnode;
  struct interface *ifp;
  struct connected *connected;
  struct prefix_ipv6 p;
  struct prefix *cp; 
  
  p.family = AF_INET6;
  p.prefix = *addr;
  p.prefixlen = IPV6_MAX_BITLEN;

  for (ALL_LIST_ELEMENTS_RO (iflist, ifnode, ifp))
    {
      for (ALL_LIST_ELEMENTS_RO (ifp->connected, cnode, connected))
	{
	  cp = connected->address;
	    
	  if (cp->family == AF_INET6)
	    if (prefix_match (cp, (struct prefix *)&p))
	      return ifp;
	}
    }
  return NULL;
}

struct interface *
if_lookup_by_ipv6_exact (struct in6_addr *addr)
{
  struct listnode *ifnode;
  struct listnode *cnode;
  struct interface *ifp;
  struct connected *connected;
  struct prefix *cp; 

  for (ALL_LIST_ELEMENTS_RO (iflist, ifnode, ifp))
    {
      for (ALL_LIST_ELEMENTS_RO (ifp->connected, cnode, connected))
	{
	  cp = connected->address;
	    
	  if (cp->family == AF_INET6)
	    if (IPV6_ADDR_SAME (&cp->u.prefix6, addr))
	      return ifp;
	}
    }
  return NULL;
}

static int
if_get_ipv6_global (struct interface *ifp, struct in6_addr *addr)
{
  struct listnode *cnode;
  struct connected *connected;
  struct prefix *cp; 
  
  for (ALL_LIST_ELEMENTS_RO (ifp->connected, cnode, connected))
    {
      cp = connected->address;
	    
      if (cp->family == AF_INET6)
	if (! IN6_IS_ADDR_LINKLOCAL (&cp->u.prefix6))
	  {
	    memcpy (addr, &cp->u.prefix6, IPV6_MAX_BYTELEN);
	    return 1;
	  }
    }
  return 0;
}

static int
if_get_ipv6_local (struct interface *ifp, struct in6_addr *addr)
{
  struct listnode *cnode;
  struct connected *connected;
  struct prefix *cp; 
  
  for (ALL_LIST_ELEMENTS_RO (ifp->connected, cnode, connected))
    {
      cp = connected->address;
	    
      if (cp->family == AF_INET6)
	if (IN6_IS_ADDR_LINKLOCAL (&cp->u.prefix6))
	  {
	    memcpy (addr, &cp->u.prefix6, IPV6_MAX_BYTELEN);
	    return 1;
	  }
    }
  return 0;
}
#endif /* HAVE_IPV6 */

static int
if_get_ipv4_address (struct interface *ifp, struct in_addr *addr)
{
  struct listnode *cnode;
  struct connected *connected;
  struct prefix *cp;

  for (ALL_LIST_ELEMENTS_RO (ifp->connected, cnode, connected))
    {
      cp = connected->address;
      if ((cp->family == AF_INET) && !ipv4_martian(&(cp->u.prefix4)))
	  {
	    *addr = cp->u.prefix4;
	    return 1;
	  }
    }
  return 0;
}

int
bgp_nexthop_set (union sockunion *local, union sockunion *remote, 
		 struct bgp_nexthop *nexthop, struct peer *peer)
{
  int ret = 0;
  struct interface *ifp = NULL;

  memset (nexthop, 0, sizeof (struct bgp_nexthop));

  if (!local)
    return -1;
  if (!remote)
    return -1;

  if (local->sa.sa_family == AF_INET)
    {
      nexthop->v4 = local->sin.sin_addr;
      ifp = if_lookup_by_ipv4 (&local->sin.sin_addr);
    }
#ifdef HAVE_IPV6
  if (local->sa.sa_family == AF_INET6)
    {
      if (IN6_IS_ADDR_LINKLOCAL (&local->sin6.sin6_addr))
	{
	  if (peer->conf_if || peer->ifname)
	    ifp = if_lookup_by_index (if_nametoindex (peer->conf_if ? peer->conf_if : peer->ifname));
	}
      else
	ifp = if_lookup_by_ipv6 (&local->sin6.sin6_addr);
    }
#endif /* HAVE_IPV6 */

  if (!ifp)
    return -1;

  nexthop->ifp = ifp;

  /* IPv4 connection. */
  if (local->sa.sa_family == AF_INET)
    {
#ifdef HAVE_IPV6
      /* IPv6 nexthop*/
      ret = if_get_ipv6_global (ifp, &nexthop->v6_global);

      /* There is no global nexthop. */
      if (!ret)
	if_get_ipv6_local (ifp, &nexthop->v6_global);
      else
	if_get_ipv6_local (ifp, &nexthop->v6_local);
#endif /* HAVE_IPV6 */
    }

#ifdef HAVE_IPV6
  /* IPv6 connection. */
  if (local->sa.sa_family == AF_INET6)
    {
      struct interface *direct = NULL;

      /* IPv4 nexthop. */
      ret = if_get_ipv4_address(ifp, &nexthop->v4);
      if (!ret && peer->local_id.s_addr)
	nexthop->v4 = peer->local_id;

      /* Global address*/
      if (! IN6_IS_ADDR_LINKLOCAL (&local->sin6.sin6_addr))
	{
	  memcpy (&nexthop->v6_global, &local->sin6.sin6_addr, 
		  IPV6_MAX_BYTELEN);

	  /* If directory connected set link-local address. */
	  direct = if_lookup_by_ipv6 (&remote->sin6.sin6_addr);
	  if (direct)
	    if_get_ipv6_local (ifp, &nexthop->v6_local);
	}
      else
	/* Link-local address. */
	{
	  ret = if_get_ipv6_global (ifp, &nexthop->v6_global);

	  /* If there is no global address.  Set link-local address as
             global.  I know this break RFC specification... */
	  if (!ret)
	    memcpy (&nexthop->v6_global, &local->sin6.sin6_addr, 
		    IPV6_MAX_BYTELEN);
	  else
	    memcpy (&nexthop->v6_local, &local->sin6.sin6_addr, 
		    IPV6_MAX_BYTELEN);
	}
    }

  if (IN6_IS_ADDR_LINKLOCAL (&local->sin6.sin6_addr) ||
      if_lookup_by_ipv6 (&remote->sin6.sin6_addr))
    peer->shared_network = 1;
  else
    peer->shared_network = 0;

  /* KAME stack specific treatment.  */
#ifdef KAME
  if (IN6_IS_ADDR_LINKLOCAL (&nexthop->v6_global)
      && IN6_LINKLOCAL_IFINDEX (nexthop->v6_global))
    {
      SET_IN6_LINKLOCAL_IFINDEX (nexthop->v6_global, 0);
    }
  if (IN6_IS_ADDR_LINKLOCAL (&nexthop->v6_local)
      && IN6_LINKLOCAL_IFINDEX (nexthop->v6_local))
    {
      SET_IN6_LINKLOCAL_IFINDEX (nexthop->v6_local, 0);
    }
#endif /* KAME */
#endif /* HAVE_IPV6 */
  return ret;
}

static struct in6_addr *
bgp_info_to_ipv6_nexthop (struct bgp_info *info)
{
  struct in6_addr *nexthop = NULL;

  /* Only global address nexthop exists. */
  if (info->attr->extra->mp_nexthop_len == 16)
    nexthop = &info->attr->extra->mp_nexthop_global;

  /* If both global and link-local address present. */
  if (info->attr->extra->mp_nexthop_len == 32)
    {
      /* Workaround for Cisco's nexthop bug.  */
      if (IN6_IS_ADDR_UNSPECIFIED (&info->attr->extra->mp_nexthop_global)
          && info->peer->su_remote->sa.sa_family == AF_INET6)
        nexthop = &info->peer->su_remote->sin6.sin6_addr;
      else
        nexthop = &info->attr->extra->mp_nexthop_local;
    }

  return nexthop;
}

static int
bgp_table_map_apply (struct route_map *map, struct prefix *p,
                     struct bgp_info *info)
{
  if (route_map_apply(map, p, RMAP_BGP, info) != RMAP_DENYMATCH)
    return 1;

  if (BGP_DEBUG(zebra, ZEBRA))
    {
      if (p->family == AF_INET)
        {
          char buf[2][INET_ADDRSTRLEN];
          zlog_debug("Zebra rmap deny: IPv4 route %s/%d nexthop %s",
                     inet_ntop(AF_INET, &p->u.prefix4, buf[0], sizeof(buf[0])),
                     p->prefixlen,
                     inet_ntop(AF_INET, &info->attr->nexthop, buf[1],
                               sizeof(buf[1])));
        }
      if (p->family == AF_INET6)
        {
          char buf[2][INET6_ADDRSTRLEN];
          zlog_debug("Zebra rmap deny: IPv6 route %s/%d nexthop %s",
                     inet_ntop(AF_INET6, &p->u.prefix6, buf[0], sizeof(buf[0])),
                     p->prefixlen,
                     inet_ntop(AF_INET6, bgp_info_to_ipv6_nexthop(info), buf[1],
                               sizeof(buf[1])));
        }
    }
  return 0;
}

void
bgp_zebra_announce (struct prefix *p, struct bgp_info *info, struct bgp *bgp,
                    afi_t afi, safi_t safi)
{
  int flags;
  u_char distance;
  struct peer *peer;
  struct bgp_info *mpinfo;
  size_t oldsize, newsize;
  u_int32_t nhcount, metric;
  struct bgp_info local_info;
  struct bgp_info *info_cp = &local_info;

  if (zclient->sock < 0)
    return;

  if (! vrf_bitmap_check (zclient->redist[ZEBRA_ROUTE_BGP], VRF_DEFAULT))
    return;

  if (bgp->main_zebra_update_hold)
    return;

  flags = 0;
  peer = info->peer;

  if (peer->sort == BGP_PEER_IBGP || peer->sort == BGP_PEER_CONFED)
    {
      SET_FLAG (flags, ZEBRA_FLAG_IBGP);
      SET_FLAG (flags, ZEBRA_FLAG_INTERNAL);
    }

  if ((peer->sort == BGP_PEER_EBGP && peer->ttl != 1)
      || CHECK_FLAG (peer->flags, PEER_FLAG_DISABLE_CONNECTED_CHECK))
    SET_FLAG (flags, ZEBRA_FLAG_INTERNAL);

  nhcount = 1 + bgp_info_mpath_count (info);

  if (p->family == AF_INET)
    {
      struct zapi_ipv4 api;
      struct in_addr *nexthop;
      char buf[2][INET_ADDRSTRLEN];
      int valid_nh_count = 0;

      /* resize nexthop buffer size if necessary */
      if ((oldsize = stream_get_size (bgp_nexthop_buf)) <
          (sizeof (struct in_addr *) * nhcount))
        {
          newsize = (sizeof (struct in_addr *) * nhcount);
          newsize = stream_resize (bgp_nexthop_buf, newsize);
          if (newsize == oldsize)
            {
	          zlog_err ("can't resize nexthop buffer");
	          return;
            }
        }
      stream_reset (bgp_nexthop_buf);
      nexthop = NULL;

      /* Metric is currently based on the best-path only. */
      metric = info->attr->med;

      if (bgp->table_map[afi][safi].name)
        {
          BGP_INFO_ATTR_BUF_INIT();

          /* Copy info and attributes, so the route-map apply doesn't modify the
             BGP route info. */
          BGP_INFO_ATTR_BUF_COPY(info, info_cp);
          if (bgp_table_map_apply(bgp->table_map[afi][safi].map, p, info_cp))
            {
              metric = info_cp->attr->med;
              nexthop = &info_cp->attr->nexthop;
            }
          BGP_INFO_ATTR_BUF_FREE(info_cp);
        }
      else
        {
          nexthop = &info->attr->nexthop;
        }

      if (nexthop)
        {
          stream_put (bgp_nexthop_buf, &nexthop, sizeof (struct in_addr *));
          valid_nh_count++;
        }

      api.vrf_id = VRF_DEFAULT;
      api.flags = flags;
      nexthop = &info->attr->nexthop;
      stream_put (bgp_nexthop_buf, &nexthop, sizeof (struct in_addr *));

      for (mpinfo = bgp_info_mpath_first (info); mpinfo;
           mpinfo = bgp_info_mpath_next (mpinfo))
        {
          nexthop = NULL;

          if (bgp->table_map[afi][safi].name)
            {
              /* Copy info and attributes, so the route-map apply doesn't modify the
                 BGP route info. */
              BGP_INFO_ATTR_BUF_COPY(mpinfo, info_cp);
              if (bgp_table_map_apply(bgp->table_map[afi][safi].map, p, info_cp))
                nexthop = &info_cp->attr->nexthop;
              BGP_INFO_ATTR_BUF_FREE(info_cp);
            }
          else
            {
              nexthop = &mpinfo->attr->nexthop;
            }

          if (nexthop == NULL)
            continue;

          stream_put (bgp_nexthop_buf, &nexthop, sizeof (struct in_addr *));
          valid_nh_count++;
        }

      api.flags = flags;
      api.type = ZEBRA_ROUTE_BGP;
      api.message = 0;
      api.safi = safi;
      SET_FLAG (api.message, ZAPI_MESSAGE_NEXTHOP);
      api.nexthop_num = valid_nh_count;
      api.nexthop = (struct in_addr **)STREAM_DATA (bgp_nexthop_buf);
      api.ifindex_num = 0;
      SET_FLAG (api.message, ZAPI_MESSAGE_METRIC);
      api.metric = metric;

      distance = bgp_distance_apply (p, info, bgp);

      if (distance)
	{
	  SET_FLAG (api.message, ZAPI_MESSAGE_DISTANCE);
	  api.distance = distance;
	}

      if (BGP_DEBUG(zebra, ZEBRA))
        {
          int i;
          zlog_debug("Zebra send: IPv4 route %s %s/%d  metric %u"
                     " count %d", (valid_nh_count ? "add":"delete"),
                     inet_ntop(AF_INET, &p->u.prefix4, buf[0], sizeof(buf[0])),
                     p->prefixlen, api.metric, api.nexthop_num);
          for (i = 0; i < api.nexthop_num; i++)
            zlog_debug("  IPv4 [nexthop %d] %s", i+1,
                       inet_ntop(AF_INET, api.nexthop[i], buf[1], sizeof(buf[1])));
        }

      zapi_ipv4_route (valid_nh_count ? ZEBRA_IPV4_ROUTE_ADD: ZEBRA_IPV4_ROUTE_DELETE,
                       zclient, (struct prefix_ipv4 *) p, &api);
    }
#ifdef HAVE_IPV6

  /* We have to think about a IPv6 link-local address curse. */
  if (p->family == AF_INET6)
    {
      unsigned int ifindex;
      struct in6_addr *nexthop;
      struct zapi_ipv6 api;
      int valid_nh_count = 0;
	    char buf[2][INET6_ADDRSTRLEN];

      /* resize nexthop buffer size if necessary */
      if ((oldsize = stream_get_size (bgp_nexthop_buf)) <
          (sizeof (struct in6_addr *) * nhcount))
        {
          newsize = (sizeof (struct in6_addr *) * nhcount);
          newsize = stream_resize (bgp_nexthop_buf, newsize);
          if (newsize == oldsize)
            {
              zlog_err ("can't resize nexthop buffer");
              return;
            }
        }
      stream_reset (bgp_nexthop_buf);

      /* resize ifindices buffer size if necessary */
      if ((oldsize = stream_get_size (bgp_ifindices_buf)) <
          (sizeof (unsigned int) * nhcount))
        {
          newsize = (sizeof (unsigned int) * nhcount);
          newsize = stream_resize (bgp_ifindices_buf, newsize);
          if (newsize == oldsize)
            {
              zlog_err ("can't resize nexthop buffer");
              return;
            }
        }
      stream_reset (bgp_ifindices_buf);

      ifindex = 0;
      nexthop = NULL;

      assert (info->attr->extra);

      /* Metric is currently based on the best-path only. */
      metric = info->attr->med;

      if (bgp->table_map[afi][safi].name)
        {
          BGP_INFO_ATTR_BUF_INIT();

          /* Copy info and attributes, so the route-map apply doesn't modify the
             BGP route info. */
          BGP_INFO_ATTR_BUF_COPY(info, info_cp);
          if (bgp_table_map_apply(bgp->table_map[afi][safi].map, p, info_cp))
            {
              metric = info_cp->attr->med;
              nexthop = bgp_info_to_ipv6_nexthop(info_cp);
            }
          BGP_INFO_ATTR_BUF_FREE(info_cp);
        }
      else
        {
           nexthop = bgp_info_to_ipv6_nexthop(info);
        }

      if (nexthop)
        {
          if (info->attr->extra->mp_nexthop_len == 32)
            if (info->peer->nexthop.ifp)
              ifindex = info->peer->nexthop.ifp->ifindex;

          if (!ifindex)
	    {
	      if (info->peer->conf_if || info->peer->ifname)
		ifindex = if_nametoindex (info->peer->conf_if ? info->peer->conf_if : info->peer->ifname);
	      else if (info->peer->nexthop.ifp)
		ifindex = info->peer->nexthop.ifp->ifindex;
	    }

          stream_put (bgp_nexthop_buf, &nexthop, sizeof (struct in6_addr *));
          stream_put (bgp_ifindices_buf, &ifindex, sizeof (unsigned int));
          valid_nh_count++;
        }

      for (mpinfo = bgp_info_mpath_first (info); mpinfo;
           mpinfo = bgp_info_mpath_next (mpinfo))
        {
          ifindex = 0;
          nexthop = NULL;

          if (bgp->table_map[afi][safi].name)
            {
              /* Copy info and attributes, so the route-map apply doesn't modify the
                 BGP route info. */
              BGP_INFO_ATTR_BUF_COPY(mpinfo, info_cp);
              if (bgp_table_map_apply(bgp->table_map[afi][safi].map, p, info_cp))
                nexthop = bgp_info_to_ipv6_nexthop(info_cp);
              BGP_INFO_ATTR_BUF_FREE(info_cp);
            }
          else
            {
              nexthop = bgp_info_to_ipv6_nexthop(mpinfo);
            }

          if (nexthop == NULL)
            continue;

          if (mpinfo->attr->extra->mp_nexthop_len == 32)
            if (mpinfo->peer->nexthop.ifp)
              ifindex = mpinfo->peer->nexthop.ifp->ifindex;

          if (!ifindex)
	    {
	      if (mpinfo->peer->conf_if || mpinfo->peer->ifname)
		ifindex = if_nametoindex (mpinfo->peer->conf_if ? mpinfo->peer->conf_if : mpinfo->peer->ifname);
	      else if (mpinfo->peer->nexthop.ifp)
		ifindex = mpinfo->peer->nexthop.ifp->ifindex;
	    }

          if (ifindex == 0)
            continue;

          stream_put (bgp_nexthop_buf, &nexthop, sizeof (struct in6_addr *));
          stream_put (bgp_ifindices_buf, &ifindex, sizeof (unsigned int));
          valid_nh_count++;
        }

      /* Make Zebra API structure. */
      api.vrf_id = VRF_DEFAULT;
      api.flags = flags;
      api.type = ZEBRA_ROUTE_BGP;
      api.message = 0;
      api.safi = safi;
      SET_FLAG (api.message, ZAPI_MESSAGE_NEXTHOP);
      api.nexthop_num = valid_nh_count;
      api.nexthop = (struct in6_addr **)STREAM_DATA (bgp_nexthop_buf);
      SET_FLAG (api.message, ZAPI_MESSAGE_IFINDEX);
      api.ifindex_num = valid_nh_count;
      api.ifindex = (unsigned int *)STREAM_DATA (bgp_ifindices_buf);
      SET_FLAG (api.message, ZAPI_MESSAGE_METRIC);
      api.metric = metric;

      if (BGP_DEBUG(zebra, ZEBRA))
        {
          int i;
          zlog_debug("Zebra send: IPv6 route %s %s/%d metric %u",
                   valid_nh_count ? "add" : "delete",
                   inet_ntop(AF_INET6, &p->u.prefix6, buf[0], sizeof(buf[0])),
                   p->prefixlen, api.metric);
          for (i = 0; i < api.nexthop_num; i++)
            zlog_debug("  IPv6 [nexthop %d] %s", i+1,
                       inet_ntop(AF_INET6, api.nexthop[i], buf[1], sizeof(buf[1])));
        }

      zapi_ipv6_route (valid_nh_count ? ZEBRA_IPV6_ROUTE_ADD : ZEBRA_IPV6_ROUTE_DELETE,
                       zclient, (struct prefix_ipv6 *) p, &api);
    }
#endif /* HAVE_IPV6 */
}

/* Announce all routes of a table to zebra */
void
bgp_zebra_announce_table (struct bgp *bgp, afi_t afi, safi_t safi)
{
  struct bgp_node *rn;
  struct bgp_table *table;
  struct bgp_info *ri;

  table = bgp->rib[afi][safi];
  if (!table) return;

  for (rn = bgp_table_top (table); rn; rn = bgp_route_next (rn))
    for (ri = rn->info; ri; ri = ri->next)
      if (CHECK_FLAG (ri->flags, BGP_INFO_SELECTED)
          && ri->type == ZEBRA_ROUTE_BGP
          && ri->sub_type == BGP_ROUTE_NORMAL)
        bgp_zebra_announce (&rn->p, ri, bgp, afi, safi);
}

void
bgp_zebra_withdraw (struct prefix *p, struct bgp_info *info, safi_t safi)
{
  int flags;
  struct peer *peer;

  if (zclient->sock < 0)
    return;

  if (! vrf_bitmap_check (zclient->redist[ZEBRA_ROUTE_BGP], VRF_DEFAULT))
    return;

  peer = info->peer;

  if (peer->bgp && peer->bgp->main_zebra_update_hold)
    return;

  flags = 0;

  if (peer->sort == BGP_PEER_IBGP)
    {
      SET_FLAG (flags, ZEBRA_FLAG_INTERNAL);
      SET_FLAG (flags, ZEBRA_FLAG_IBGP);
    }

  if ((peer->sort == BGP_PEER_EBGP && peer->ttl != 1)
      || CHECK_FLAG (peer->flags, PEER_FLAG_DISABLE_CONNECTED_CHECK))
    SET_FLAG (flags, ZEBRA_FLAG_INTERNAL);

  if (p->family == AF_INET)
    {
      struct zapi_ipv4 api;
      struct in_addr *nexthop;

      api.vrf_id = VRF_DEFAULT;
      api.flags = flags;
      nexthop = &info->attr->nexthop;

      api.type = ZEBRA_ROUTE_BGP;
      api.message = 0;
      api.safi = safi;
      SET_FLAG (api.message, ZAPI_MESSAGE_NEXTHOP);
      api.nexthop_num = 0;
      api.nexthop = NULL;
      api.ifindex_num = 0;
      SET_FLAG (api.message, ZAPI_MESSAGE_METRIC);
      api.metric = info->attr->med;

      if (BGP_DEBUG(zebra, ZEBRA))
	{
	  char buf[2][INET_ADDRSTRLEN];
	  zlog_debug("Zebra send: IPv4 route delete %s/%d nexthop %s metric %u",
		     inet_ntop(AF_INET, &p->u.prefix4, buf[0], sizeof(buf[0])),
		     p->prefixlen,
		     inet_ntop(AF_INET, nexthop, buf[1], sizeof(buf[1])),
		     api.metric);
	}

      zapi_ipv4_route (ZEBRA_IPV4_ROUTE_DELETE, zclient, 
                       (struct prefix_ipv4 *) p, &api);
    }
#ifdef HAVE_IPV6
  /* We have to think about a IPv6 link-local address curse. */
  if (p->family == AF_INET6)
    {
      struct zapi_ipv6 api;
      unsigned int ifindex;
      struct in6_addr *nexthop;
      
      assert (info->attr->extra);
      
      ifindex = 0;
      nexthop = NULL;

      /* Only global address nexthop exists. */
      if (info->attr->extra->mp_nexthop_len == 16)
	nexthop = &info->attr->extra->mp_nexthop_global;

      /* If both global and link-local address present. */
      if (info->attr->extra->mp_nexthop_len == 32)
	{
	  nexthop = &info->attr->extra->mp_nexthop_local;
	  if (info->peer->nexthop.ifp)
	    ifindex = info->peer->nexthop.ifp->ifindex;
	}

      if (nexthop == NULL)
	return;

      if (IN6_IS_ADDR_LINKLOCAL (nexthop) && ! ifindex)
	if (info->peer->conf_if || info->peer->ifname)
	  ifindex = if_nametoindex (info->peer->conf_if ? info->peer->conf_if : info->peer->ifname);

      api.vrf_id = VRF_DEFAULT;
      api.flags = flags;
      api.type = ZEBRA_ROUTE_BGP;
      api.message = 0;
      api.safi = safi;
      SET_FLAG (api.message, ZAPI_MESSAGE_NEXTHOP);
      api.nexthop_num = 1;
      api.nexthop = &nexthop;
      SET_FLAG (api.message, ZAPI_MESSAGE_IFINDEX);
      api.ifindex_num = 1;
      api.ifindex = &ifindex;
      SET_FLAG (api.message, ZAPI_MESSAGE_METRIC);
      api.metric = info->attr->med;

      if (BGP_DEBUG(zebra, ZEBRA))
	{
	  char buf[2][INET6_ADDRSTRLEN];
	  zlog_debug("Zebra send: IPv6 route delete %s/%d nexthop %s metric %u",
		     inet_ntop(AF_INET6, &p->u.prefix6, buf[0], sizeof(buf[0])),
		     p->prefixlen,
		     inet_ntop(AF_INET6, nexthop, buf[1], sizeof(buf[1])),
		     api.metric);
	}

      zapi_ipv6_route (ZEBRA_IPV6_ROUTE_DELETE, zclient, 
                       (struct prefix_ipv6 *) p, &api);
    }
#endif /* HAVE_IPV6 */
}

/* Other routes redistribution into BGP. */
int
bgp_redistribute_set (struct bgp *bgp, afi_t afi, int type)
{
  /* Set flag to BGP instance. */
  bgp->redist[afi][type] = 1;

  /* Return if already redistribute flag is set. */
  if (vrf_bitmap_check (zclient->redist[type], VRF_DEFAULT))
    return CMD_WARNING;

  vrf_bitmap_set (zclient->redist[type], VRF_DEFAULT);

  /* Return if zebra connection is not established. */
  if (zclient->sock < 0)
    return CMD_WARNING;

  if (BGP_DEBUG(zebra, ZEBRA))
    zlog_debug("Zebra send: redistribute add %s", zebra_route_string(type));

  /* Send distribute add message to zebra. */
  zebra_redistribute_send (ZEBRA_REDISTRIBUTE_ADD, zclient, type, VRF_DEFAULT);

  return CMD_SUCCESS;
}

int
bgp_redistribute_resend (struct bgp *bgp, afi_t afi, int type)
{
  /* Return if zebra connection is not established. */
  if (zclient->sock < 0)
    return -1;

  if (BGP_DEBUG(zebra, ZEBRA))
    zlog_debug("Zebra send: redistribute add %s", zebra_route_string(type));

  /* Send distribute add message to zebra. */
  zebra_redistribute_send (ZEBRA_REDISTRIBUTE_DELETE, zclient, type, VRF_DEFAULT);
  zebra_redistribute_send (ZEBRA_REDISTRIBUTE_ADD, zclient, type, VRF_DEFAULT);

  return 0;
}

/* Redistribute with route-map specification.  */
int
bgp_redistribute_rmap_set (struct bgp *bgp, afi_t afi, int type, 
                           const char *name)
{
  if (bgp->rmap[afi][type].name
      && (strcmp (bgp->rmap[afi][type].name, name) == 0))
    return 0;

  if (bgp->rmap[afi][type].name)
    free (bgp->rmap[afi][type].name);
  bgp->rmap[afi][type].name = strdup (name);
  bgp->rmap[afi][type].map = route_map_lookup_by_name (name);

  return 1;
}

/* Redistribute with metric specification.  */
int
bgp_redistribute_metric_set (struct bgp *bgp, afi_t afi, int type,
			     u_int32_t metric)
{
  if (bgp->redist_metric_flag[afi][type]
      && bgp->redist_metric[afi][type] == metric)
    return 0;

  bgp->redist_metric_flag[afi][type] = 1;
  bgp->redist_metric[afi][type] = metric;

  return 1;
}

/* Unset redistribution.  */
int
bgp_redistribute_unset (struct bgp *bgp, afi_t afi, int type)
{
  /* Unset flag from BGP instance. */
  bgp->redist[afi][type] = 0;

  /* Unset route-map. */
  if (bgp->rmap[afi][type].name)
    free (bgp->rmap[afi][type].name);
  bgp->rmap[afi][type].name = NULL;
  bgp->rmap[afi][type].map = NULL;

  /* Unset metric. */
  bgp->redist_metric_flag[afi][type] = 0;
  bgp->redist_metric[afi][type] = 0;

  /* Return if zebra connection is disabled. */
  if (! vrf_bitmap_check (zclient->redist[type], VRF_DEFAULT))
    return CMD_WARNING;
  vrf_bitmap_unset (zclient->redist[type], VRF_DEFAULT);

  if (bgp->redist[AFI_IP][type] == 0 
      && bgp->redist[AFI_IP6][type] == 0 
      && zclient->sock >= 0)
    {
      /* Send distribute delete message to zebra. */
      if (BGP_DEBUG(zebra, ZEBRA))
	zlog_debug("Zebra send: redistribute delete %s",
		   zebra_route_string(type));
      zebra_redistribute_send (ZEBRA_REDISTRIBUTE_DELETE, zclient, type,
                               VRF_DEFAULT);
    }
  
  /* Withdraw redistributed routes from current BGP's routing table. */
  bgp_redistribute_withdraw (bgp, afi, type);

  return CMD_SUCCESS;
}

void
bgp_zclient_reset (void)
{
  zclient_reset (zclient);
}

static void
bgp_zebra_connected (struct zclient *zclient)
{
  zclient_send_requests (zclient, VRF_DEFAULT);
}

void
bgp_zebra_init (struct thread_master *master)
{
  /* Set default values. */
  zclient = zclient_new (master);
  zclient_init (zclient, ZEBRA_ROUTE_BGP);
  zclient->zebra_connected = bgp_zebra_connected;
  zclient->router_id_update = bgp_router_id_update;
  zclient->interface_add = bgp_interface_add;
  zclient->interface_delete = bgp_interface_delete;
  zclient->interface_address_add = bgp_interface_address_add;
  zclient->interface_address_delete = bgp_interface_address_delete;
  zclient->interface_nbr_address_add = bgp_interface_nbr_address_add;
  zclient->interface_nbr_address_delete = bgp_interface_nbr_address_delete;
  zclient->ipv4_route_add = zebra_read_ipv4;
  zclient->ipv4_route_delete = zebra_read_ipv4;
  zclient->interface_up = bgp_interface_up;
  zclient->interface_down = bgp_interface_down;
#ifdef HAVE_IPV6
  zclient->ipv6_route_add = zebra_read_ipv6;
  zclient->ipv6_route_delete = zebra_read_ipv6;
#endif /* HAVE_IPV6 */
  zclient->nexthop_update = bgp_read_nexthop_update;

  bgp_nexthop_buf = stream_new(BGP_NEXTHOP_BUF_SIZE);
  bgp_ifindices_buf = stream_new(BGP_IFINDICES_BUF_SIZE);
}
