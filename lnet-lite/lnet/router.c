/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright  2008 Sun Microsystems, Inc. All rights reserved
 *
 *   This file is part of Portals
 *   http://sourceforge.net/projects/sandiaportals/
 *
 *   Portals is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Portals is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Portals; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define DEBUG_SUBSYSTEM S_LNET
#include <lnet/lib-lnet.h>

//static char *forwarding = "";
static int tiny_router_buffers = 1024;
static int small_router_buffers = 8192;
static int large_router_buffers = 512;
//static int peer_buffer_credits = 0;
//static int auto_down = 1;
//static int check_routers_before_use = 0;
//static int dead_router_check_interval = 0;
//static int live_router_check_interval = 0;
//static int router_ping_timeout = 50;

#if defined(__KERNEL__) && defined(LNET_ROUTER)

CFS_MODULE_PARM(forwarding, "s", charp, 0444,
                "Explicitly enable/disable forwarding between networks");

CFS_MODULE_PARM(tiny_router_buffers, "i", int, 0444,
                "# of 0 payload messages to buffer in the router");

CFS_MODULE_PARM(small_router_buffers, "i", int, 0444,
                "# of small (1 page) messages to buffer in the router");

CFS_MODULE_PARM(large_router_buffers, "i", int, 0444,
                "# of large messages to buffer in the router");

CFS_MODULE_PARM(peer_buffer_credits, "i", int, 0444,
                "# router buffer credits per peer");

CFS_MODULE_PARM(auto_down, "i", int, 0444,
                "Automatically mark peers down on comms error");

int
lnet_peer_buffer_credits(lnet_ni_t *ni)
{
        /* NI option overrides LNet default */
        if (ni->ni_peerrtrcredits > 0)
                return ni->ni_peerrtrcredits;
        if (peer_buffer_credits > 0)
                return peer_buffer_credits;

        /* As an approximation, allow this peer the same number of router
         * buffers as it is allowed outstanding sends */
        return ni->ni_peertxcredits;
}

/* forward ref's */
static int lnet_router_checker(void *);
#else

int
lnet_peer_buffer_credits(__unusedx lnet_ni_t *ni)
{
        return 0;
}

#endif

static int check_routers_before_use = 0;
CFS_MODULE_PARM(check_routers_before_use, "i", int, 0444,
                "Assume routers are down and ping them before use");

static int avoid_asym_router_failure = 0;
CFS_MODULE_PARM(avoid_asym_router_failure, "i", int, 0444,
                "Avoid asymmetrical failures: reserved, use at your own risk");

static int dead_router_check_interval = 0;
CFS_MODULE_PARM(dead_router_check_interval, "i", int, 0444,
                "Seconds between dead router health checks (<= 0 to disable)");

static int live_router_check_interval = 0;
CFS_MODULE_PARM(live_router_check_interval, "i", int, 0444,
                "Seconds between live router health checks (<= 0 to disable)");

static int router_ping_timeout = 50;
CFS_MODULE_PARM(router_ping_timeout, "i", int, 0444,
                "Seconds to wait for the reply to a router health query");

int
lnet_peers_start_down(void)
{
        return check_routers_before_use;
}

void
lnet_notify_locked(lnet_peer_t *lp, int notifylnd, int alive, cfs_time_t when)
{
        if (cfs_time_before(when, lp->lp_timestamp)) { /* out of date information */
                CDEBUG(D_NET, "Out of date\n");
                return;
        }

        lp->lp_timestamp = when;                /* update timestamp */
        lp->lp_ping_deadline = 0;               /* disable ping timeout */

        if (lp->lp_alive_count != 0 &&          /* got old news */
            (!lp->lp_alive) == (!alive)) {      /* new date for old news */
                CDEBUG(D_NET, "Old news\n");
                return;
        }

        /* Flag that notification is outstanding */

        lp->lp_alive_count++;
        lp->lp_alive = !(!alive);               /* 1 bit! */
        lp->lp_notify = 1;
        lp->lp_notifylnd |= notifylnd;

        CDEBUG(D_NET, "set %s %d\n", libcfs_nid2str(lp->lp_nid), alive);
}

void
lnet_do_notify (lnet_peer_t *lp)
{
        lnet_ni_t *ni = lp->lp_ni;
        int        alive;
        int        notifylnd;

        LNET_LOCK();

        /* Notify only in 1 thread at any time to ensure ordered notification.
         * NB individual events can be missed; the only guarantee is that you
         * always get the most recent news */

        if (lp->lp_notifying) {
                LNET_UNLOCK();
                return;
        }

        lp->lp_notifying = 1;

        while (lp->lp_notify) {
                alive     = lp->lp_alive;
                notifylnd = lp->lp_notifylnd;

                lp->lp_notifylnd = 0;
                lp->lp_notify    = 0;

                if (notifylnd && ni->ni_lnd->lnd_notify != NULL) {
                        LNET_UNLOCK();

                        /* A new notification could happen now; I'll handle it
                         * when control returns to me */

                        (ni->ni_lnd->lnd_notify)(ni, lp->lp_nid, alive);

                        LNET_LOCK();
                }
        }

        lp->lp_notifying = 0;

        LNET_UNLOCK();
}

static void
lnet_rtr_addref_locked(lnet_peer_t *lp)
{
        LASSERT (lp->lp_refcount > 0);
        LASSERT (lp->lp_rtr_refcount >= 0);

        lp->lp_rtr_refcount++;
        if (lp->lp_rtr_refcount == 1) {
                struct list_head *pos;

                /* a simple insertion sort */
                list_for_each_prev(pos, &the_lnet.ln_routers) {
                        lnet_peer_t *rtr = list_entry(pos, lnet_peer_t, 
                                                      lp_rtr_list);

                        if (rtr->lp_nid < lp->lp_nid)
                                break;
                }

                list_add(&lp->lp_rtr_list, pos);
                /* addref for the_lnet.ln_routers */
                lnet_peer_addref_locked(lp);
                the_lnet.ln_routers_version++;
        }
}

static void
lnet_rtr_decref_locked(lnet_peer_t *lp)
{
        LASSERT (lp->lp_refcount > 0);
        LASSERT (lp->lp_rtr_refcount > 0);

        lp->lp_rtr_refcount--;
        if (lp->lp_rtr_refcount == 0) {
                if (lp->lp_rcd != NULL) {
                        list_add(&lp->lp_rcd->rcd_list,
                                 &the_lnet.ln_zombie_rcd);
                        lp->lp_rcd = NULL;
                }

                list_del(&lp->lp_rtr_list);
                /* decref for the_lnet.ln_routers */
                lnet_peer_decref_locked(lp);
                the_lnet.ln_routers_version++;
        }
}

lnet_remotenet_t *
lnet_find_net_locked (__u32 net)
{
        lnet_remotenet_t *rnet;
        struct list_head *tmp;

        LASSERT (!the_lnet.ln_shutdown);

        list_for_each (tmp, &the_lnet.ln_remote_nets) {
                rnet = list_entry(tmp, lnet_remotenet_t, lrn_list);

                if (rnet->lrn_net == net)
                        return rnet;
        }
        return NULL;
}


lnet_remotenet_t *
lnet_find_net (__u32 net)
{
        lnet_remotenet_t *rnet = NULL;

        LNET_LOCK();
        rnet = lnet_find_net_locked(net);
        LNET_UNLOCK();

        return (rnet);
}

/* NB expects LNET_LOCK held */
void
lnet_add_route_to_rnet (lnet_remotenet_t *rnet, lnet_route_t *route)
{
        unsigned int      len = 0;
        unsigned int      offset = 0;
        struct list_head *e;
        extern __u64 lnet_create_interface_cookie(void);

        list_for_each (e, &rnet->lrn_routes) {
                len++;
        }

        /* FIXME use Lustre random function when it's moved to libcfs.
         * See bug 18751 */
        /* len+1 positions to add a new entry, also prevents division by 0 */
        offset = ((unsigned int) lnet_create_interface_cookie()) % (len + 1);
        list_for_each (e, &rnet->lrn_routes) {
                if (offset == 0)
                        break;
                offset--;
        }
        list_add(&route->lr_list, e);

        the_lnet.ln_remote_nets_version++;
        lnet_rtr_addref_locked(route->lr_gateway);
}

int
lnet_add_route (__u32 net, unsigned int hops, lnet_nid_t gateway)
{
        struct list_head    *e;
        lnet_remotenet_t    *rnet;
        lnet_remotenet_t    *rnet2;
        lnet_route_t        *route;
        lnet_ni_t           *ni;
        int                  add_route;
        int                  rc;

        CDEBUG(D_NET, "Add route: net %s hops %u gw %s\n",
               libcfs_net2str(net), hops, libcfs_nid2str(gateway));

        if (gateway == LNET_NID_ANY ||
            LNET_NETTYP(LNET_NIDNET(gateway)) == LOLND ||
            net == LNET_NIDNET(LNET_NID_ANY) ||
            LNET_NETTYP(net) == LOLND ||
            LNET_NIDNET(gateway) == net ||
            hops < 1 || hops > 255)
                return (-EINVAL);

        if (lnet_islocalnet(net))               /* it's a local network */
                return 0;                       /* ignore the route entry */

        /* Assume net, route, all new */
        LIBCFS_ALLOC(route, sizeof(*route));
        LIBCFS_ALLOC(rnet, sizeof(*rnet));
        if (route == NULL || rnet == NULL) {
                CERROR("Out of memory creating route %s %d %s\n",
                       libcfs_net2str(net), hops, libcfs_nid2str(gateway));
                if (route != NULL)
                        LIBCFS_FREE(route, sizeof(*route));
                if (rnet != NULL)
                        LIBCFS_FREE(rnet, sizeof(*rnet));
                return -ENOMEM;
        }

        CFS_INIT_LIST_HEAD(&rnet->lrn_routes);
        rnet->lrn_net = net;
        route->lr_hops = hops;
        rnet->lrn_netif = LNET_NIDNET(gateway);

        LNET_LOCK();

        rc = lnet_nid2peer_locked(&route->lr_gateway, gateway);
        if (rc != 0) {
                LNET_UNLOCK();

                LIBCFS_FREE(route, sizeof(*route));
                LIBCFS_FREE(rnet, sizeof(*rnet));

                if (rc == -EHOSTUNREACH)        /* gateway is not on a local net */
                        return 0;               /* ignore the route entry */

                CERROR("Error %d creating route %s %d %s\n", rc,
                       libcfs_net2str(net), hops, libcfs_nid2str(gateway));
                return rc;
        }

        LASSERT (!the_lnet.ln_shutdown);

        rnet2 = lnet_find_net_locked(net);
        if (rnet2 == NULL) {
                /* new network */
                list_add_tail(&rnet->lrn_list, &the_lnet.ln_remote_nets);
                rnet2 = rnet;
        }

        /* Search for a duplicate route (it's a NOOP if it is) */
        add_route = 1;
        list_for_each (e, &rnet2->lrn_routes) {
                lnet_route_t *route2 = list_entry(e, lnet_route_t, lr_list);

                if (route2->lr_gateway == route->lr_gateway) {
                        add_route = 0;
                        break;
                }

                /* our lookups must be true */
                LASSERT (route2->lr_gateway->lp_nid != gateway);
        }

        if (add_route) {
                ni = route->lr_gateway->lp_ni;
                lnet_ni_addref_locked(ni);

                lnet_add_route_to_rnet(rnet2, route);
                LNET_UNLOCK();

                /* XXX Assume alive */
                if (ni->ni_lnd->lnd_notify != NULL)
                        (ni->ni_lnd->lnd_notify)(ni, gateway, 1);

                lnet_ni_decref(ni);
        } else {
                lnet_peer_decref_locked(route->lr_gateway);
                LNET_UNLOCK();
                LIBCFS_FREE(route, sizeof(*route));
        }

        if (rnet != rnet2)
                LIBCFS_FREE(rnet, sizeof(*rnet));

        return 0;
}

int
lnet_check_routes (void)
{
        lnet_remotenet_t    *rnet;
        lnet_route_t        *route;
        lnet_route_t        *route2;
        struct list_head    *e1;
        struct list_head    *e2;

        LNET_LOCK();

        list_for_each (e1, &the_lnet.ln_remote_nets) {
                rnet = list_entry(e1, lnet_remotenet_t, lrn_list);

                route2 = NULL;
                list_for_each (e2, &rnet->lrn_routes) {
                        route = list_entry(e2, lnet_route_t, lr_list);

                        if (route2 == NULL)
                                route2 = route;
                        else if (route->lr_gateway->lp_ni !=
                                 route2->lr_gateway->lp_ni) {
                                LNET_UNLOCK();

                                CERROR("Routes to %s via %s and %s not supported\n",
                                       libcfs_net2str(rnet->lrn_net),
                                       libcfs_nid2str(route->lr_gateway->lp_nid),
                                       libcfs_nid2str(route2->lr_gateway->lp_nid));
                                return -EINVAL;
                        }
                }
        }

        LNET_UNLOCK();
        return 0;
}

int
lnet_del_route (__u32 net, lnet_nid_t gw_nid)
{
        lnet_remotenet_t    *rnet;
        lnet_route_t        *route;
        struct list_head    *e1;
        struct list_head    *e2;
        int                  rc = -ENOENT;

        CDEBUG(D_NET, "Del route: net %s : gw %s\n",
               libcfs_net2str(net), libcfs_nid2str(gw_nid));

        /* NB Caller may specify either all routes via the given gateway
         * or a specific route entry actual NIDs) */

 again:
        LNET_LOCK();

        list_for_each (e1, &the_lnet.ln_remote_nets) {
                rnet = list_entry(e1, lnet_remotenet_t, lrn_list);

                if (!(net == LNET_NIDNET(LNET_NID_ANY) ||
                      net == rnet->lrn_net))
                        continue;

                list_for_each (e2, &rnet->lrn_routes) {
                        route = list_entry(e2, lnet_route_t, lr_list);

                        if (!(gw_nid == LNET_NID_ANY ||
                              gw_nid == route->lr_gateway->lp_nid))
                                continue;

                        list_del(&route->lr_list);
                        the_lnet.ln_remote_nets_version++;

                        if (list_empty(&rnet->lrn_routes))
                                list_del(&rnet->lrn_list);
                        else
                                rnet = NULL;

                        lnet_rtr_decref_locked(route->lr_gateway);
                        lnet_peer_decref_locked(route->lr_gateway);
                        LNET_UNLOCK();

                        LIBCFS_FREE(route, sizeof (*route));

                        if (rnet != NULL)
                                LIBCFS_FREE(rnet, sizeof(*rnet));

                        rc = 0;
                        goto again;
                }
        }

        LNET_UNLOCK();
        return rc;
}

void
lnet_destroy_routes (void)
{
        lnet_del_route(LNET_NIDNET(LNET_NID_ANY), LNET_NID_ANY);
}

int
lnet_get_route (int idx, __u32 *net, __u32 *hops,
               lnet_nid_t *gateway, __u32 *alive)
{
        struct list_head    *e1;
        struct list_head    *e2;
        lnet_remotenet_t    *rnet;
        lnet_route_t        *route;

        LNET_LOCK();

        list_for_each (e1, &the_lnet.ln_remote_nets) {
                rnet = list_entry(e1, lnet_remotenet_t, lrn_list);

                list_for_each (e2, &rnet->lrn_routes) {
                        route = list_entry(e2, lnet_route_t, lr_list);

                        if (idx-- == 0) {
                                *net     = rnet->lrn_net;
                                *hops    = route->lr_hops;
                                *gateway = route->lr_gateway->lp_nid;
                                *alive   = route->lr_gateway->lp_alive;
                                LNET_UNLOCK();
                                return 0;
                        }
                }
        }

        LNET_UNLOCK();
        return -ENOENT;
}

int
lnet_parse_pinginfo(lnet_ping_info_t *info, int len, int expected_nids)
{
        int rc = -EPROTO;  /* if I can't parse... */
        int i, swab, n_nids;

        if (len < 8) { /* can't check magic/version */
                CNETERR("Ping info too short %d\n", len);
                return rc;
        }

        if (info->pi_magic == LNET_PROTO_PING_MAGIC) {
                swab = 0;
        } else if (info->pi_magic == __swab32(LNET_PROTO_PING_MAGIC)) {
                swab = 1;
        } else {
                CNETERR("Unexpected magic %08x\n", info->pi_magic);
                return rc;
        }

        if (swab)
                __swab32s(&info->pi_version);

        if (info->pi_version != LNET_PROTO_PING_VERSION &&
            info->pi_version != LNET_PROTO_PING_VERSION1) {
                CNETERR("Unexpected version 0x%x\n", info->pi_version);
                return rc;
        }

        if (len < (int)lnet_pinginfo_size(0)) { /* can't check pid/nnis */
                CNETERR("Short reply %d(%d min)\n", len,
                        (int)lnet_pinginfo_size(0));
                return rc;
        }

        if (swab) {
                __swab32s(&info->pi_pid);
                __swab32s(&info->pi_nnis);
        }

        n_nids = MIN((int)info->pi_nnis, expected_nids);
        if (len < (int)lnet_pinginfo_size_v(n_nids, info->pi_version)) {
                CNETERR("Short reply %d(%d expected)\n", len,
                        (int)lnet_pinginfo_size_v(n_nids, info->pi_version));
                return rc;
        }

        if (!swab)
                return 0;

        for (i = 0; i < n_nids; i++) {
                lnet_ni_status_t *stat;

                if (info->pi_version == LNET_PROTO_PING_VERSION1) {
                        __swab64s(&info->pi_nid[i]);
                        continue;
                }

                stat = &info->pi_ni[i];
                __swab64s(&stat->ns_nid);
                __swab32s(&stat->ns_status);
        }
        return 0;
}

/* Returns # of down NIs, or negative error codes; ignore downed NIs
 * if a NI in 'net' is up */
int
lnet_router_down_ni(lnet_peer_t *rtr, __u32 net)
{
	unsigned int               i;
        int               down = 0;
        int               ptl_up = 0;
        int               ptl_down = 0;
        lnet_ping_info_t *info;
        int               rc;

        if (!avoid_asym_router_failure)
                return -ENOENT;

        if (rtr->lp_rcd == NULL)
                return -EINVAL;

        if (!rtr->lp_alive)
                return -EINVAL;  /* stale lp_rcd */

        info = rtr->lp_rcd->rcd_pinginfo;
        LASSERT (info != NULL);

        rc = lnet_parse_pinginfo(info, /* NB always racing with network! */
                                 LNET_MAX_PINGINFO_SIZE, LNET_MAX_RTR_NIS);
        if (rc != 0) {
                CNETERR("Bad ping reply from %s\n",
                        libcfs_nid2str(rtr->lp_nid));
                return -EPROTO;
        }

        if (info->pi_version == LNET_PROTO_PING_VERSION1)
                return -ENOENT;  /* v1 doesn't carry NI status info */

        for (i = 0; i < info->pi_nnis && i < LNET_MAX_RTR_NIS; i++) {
                lnet_ni_status_t *stat = &info->pi_ni[i];
                lnet_nid_t        nid = stat->ns_nid;

                if (nid == LNET_NID_ANY) {
                        CNETERR("%s: unexpected LNET_NID_ANY\n",
                                libcfs_nid2str(rtr->lp_nid));
                        return -EPROTO;
                }

                if (LNET_NETTYP(LNET_NIDNET(nid)) == LOLND)
                        continue;

                if (stat->ns_status == LNET_NI_STATUS_DOWN) {
                        if (LNET_NETTYP(LNET_NIDNET(nid)) == PTLLND)
                                ptl_down = 1;
                        else
                                down++;
                        continue;
                }

                if (stat->ns_status != LNET_NI_STATUS_UP) {
                        CNETERR("%s: Unexpected status 0x%x\n",
                                libcfs_nid2str(rtr->lp_nid), stat->ns_status);
                        return -EPROTO;
                }

                /* ignore downed NIs if there's a NI up for dest network */
                if (LNET_NIDNET(nid) == net)
                        return 0;

                if (LNET_NETTYP(LNET_NIDNET(nid)) == PTLLND)
                        ptl_up = 1;
        }

        /* ptl NIs are considered down only when they're all down */
        return down + (ptl_up ? 0 : ptl_down);
}

void
lnet_wait_known_routerstate(void)
{
        lnet_peer_t         *rtr;
        struct list_head    *entry;
        int                  all_known;

        LASSERT (the_lnet.ln_rc_state == LNET_RC_STATE_RUNNING);

        for (;;) {
                LNET_LOCK();

                all_known = 1;
                list_for_each (entry, &the_lnet.ln_routers) {
                        rtr = list_entry(entry, lnet_peer_t, lp_rtr_list);

                        if (rtr->lp_alive_count == 0) {
                                all_known = 0;
                                break;
                        }
                }

                LNET_UNLOCK();

                if (all_known)
                        return;

#ifndef __KERNEL__
                lnet_router_checker();
#endif
                cfs_pause(cfs_time_seconds(1));
        }
}

static void
lnet_router_checker_event (lnet_event_t *event)
{
        /* CAVEAT EMPTOR: I'm called with LNET_LOCKed and I'm not allowed to
         * drop it (that's how come I see _every_ event, even ones that would
         * overflow my EQ) */
        lnet_rc_data_t *rcd = event->md.user_ptr;
        lnet_peer_t    *lp;
        lnet_nid_t      nid;

        if (event->unlinked) {
                if (rcd != NULL) {
                        rcd->rcd_mdh = LNET_INVALID_HANDLE;
                        return;
                }

                /* The router checker thread has unlinked the default rc_md
                 * and exited. */
                LASSERT (the_lnet.ln_rc_state == LNET_RC_STATE_UNLINKING);
                the_lnet.ln_rc_state = LNET_RC_STATE_UNLINKED;
#ifdef __KERNEL__
                mutex_up(&the_lnet.ln_rc_signal);
#endif
                return;
        }

        LASSERT (event->type == LNET_EVENT_SEND ||
                 event->type == LNET_EVENT_DROP ||
                 event->type == LNET_EVENT_REPLY);

        nid = (event->type == LNET_EVENT_SEND) ?
              event->target.nid : event->initiator.nid;

        lp = lnet_find_peer_locked(nid);
        if (lp == NULL) {
                /* router may have been removed */
                CDEBUG(D_NET, "Router %s not found\n", libcfs_nid2str(nid));
                return;
        }

        if (event->type == LNET_EVENT_SEND)     /* re-enable another ping */
                lp->lp_ping_notsent = 0;

        if (lnet_isrouter(lp) &&                /* ignore if no longer a router */
            (event->status != 0 ||
             event->type == LNET_EVENT_REPLY)) {

                /* A successful REPLY means the router is up.  If _any_ comms
                 * to the router fail I assume it's down (this will happen if
                 * we ping alive routers to try to detect router death before
                 * apps get burned). */

                lnet_notify_locked(lp, 1, (event->status == 0),
                                   cfs_time_current());

                /* The router checker will wake up very shortly and do the
                 * actual notification.  
                 * XXX If 'lp' stops being a router before then, it will still
                 * have the notification pending!!! */
        }

        /* This decref will NOT drop LNET_LOCK (it had to have 1 ref when it
         * was in the peer table and I've not dropped the lock, so no-one else
         * can have reduced the refcount) */
        LASSERT(lp->lp_refcount > 1);

        lnet_peer_decref_locked(lp);
}

void
lnet_update_ni_status(void)
{
        cfs_time_t now = cfs_time_current();
        lnet_ni_t *ni;
        unsigned int        status;
        int        timeout;

        LASSERT (the_lnet.ln_routing);

        timeout = router_ping_timeout +
                  MAX(live_router_check_interval, dead_router_check_interval);

        LNET_LOCK();

        list_for_each_entry (ni, &the_lnet.ln_nis, ni_list) {
                lnet_ni_status_t *ns = ni->ni_status;

                LASSERT (ns != NULL);

                status = LNET_NI_STATUS_UP;
                if (ni->ni_lnd->lnd_type != LOLND &&  /* @lo forever alive */
                    cfs_time_after(now, cfs_time_add(ni->ni_last_alive,
                                                     cfs_time_seconds(timeout))))
                        status = LNET_NI_STATUS_DOWN;

                if (ns->ns_status != status) {
                        ns->ns_status = status;
                        CDEBUG(D_NET, "NI(%s:%d) status changed to %s\n",
                               libcfs_nid2str(ni->ni_nid), timeout,
                               status == LNET_NI_STATUS_UP ? "up" : "down");
                }
        }

        LNET_UNLOCK();
}

void
lnet_destroy_rc_data (lnet_rc_data_t *rcd)
{
        LASSERT (list_empty(&rcd->rcd_list));
        /* detached from network */
        LASSERT (LNetHandleIsEqual(rcd->rcd_mdh, LNET_INVALID_HANDLE));

        LIBCFS_FREE(rcd->rcd_pinginfo, LNET_MAX_PINGINFO_SIZE);
        LIBCFS_FREE(rcd, sizeof(*rcd));
        return;
}

lnet_rc_data_t *
lnet_create_rc_data (void)
{
        int               i;
        int               rc;
        lnet_ping_info_t *pi;
        lnet_rc_data_t   *rcd;

        LIBCFS_ALLOC(rcd, sizeof(*rcd));
        if (rcd == NULL)
                return NULL;

        LIBCFS_ALLOC(pi, LNET_MAX_PINGINFO_SIZE);
        if (pi == NULL) {
                LIBCFS_FREE(rcd, sizeof(*rcd));
                return NULL;
        }

        memset(pi, 0, LNET_MAX_PINGINFO_SIZE);
        for (i = 0; i < LNET_MAX_RTR_NIS; i++) {
                pi->pi_ni[i].ns_nid = LNET_NID_ANY;
                pi->pi_ni[i].ns_status = LNET_NI_STATUS_INVALID;
        }
        rcd->rcd_pinginfo = pi;
        rcd->rcd_mdh = LNET_INVALID_HANDLE;
        CFS_INIT_LIST_HEAD(&rcd->rcd_list);

        LASSERT (!LNetHandleIsEqual(the_lnet.ln_rc_eqh, LNET_EQ_NONE));
        rc = LNetMDBind((lnet_md_t){.start     = pi,
                                    .user_ptr  = rcd,
                                    .length    = LNET_MAX_PINGINFO_SIZE,
                                    .threshold = LNET_MD_THRESH_INF,
                                    .options   = LNET_MD_TRUNCATE,
                                    .eq_handle = the_lnet.ln_rc_eqh},
                        LNET_UNLINK,
                        &rcd->rcd_mdh);
        if (rc < 0) {
                CERROR("Can't bind MD: %d\n", rc);
                lnet_destroy_rc_data(rcd);
                return NULL;
        }
        LASSERT (rc == 0);
        return rcd;
}

static int
lnet_router_check_interval (lnet_peer_t *rtr)
{
        int secs;

        secs = rtr->lp_alive ? live_router_check_interval :
                               dead_router_check_interval;
        if (secs < 0)
                secs = 0;

        return secs;
}

static void
lnet_ping_router_locked (lnet_peer_t *rtr)
{
        int             newrcd = 0;
        lnet_rc_data_t *rcd = NULL;
        cfs_time_t      now = cfs_time_current();
        int             secs;

        lnet_peer_addref_locked(rtr);

        if (rtr->lp_ping_deadline != 0 && /* ping timed out? */
            cfs_time_after(now, rtr->lp_ping_deadline))
                lnet_notify_locked(rtr, 1, 0, now);

        if (avoid_asym_router_failure && rtr->lp_rcd == NULL)
                newrcd = 1;

        LNET_UNLOCK();

        /* Run any outstanding notifications */
        lnet_do_notify(rtr);

        if (newrcd)
                rcd = lnet_create_rc_data();

        LNET_LOCK();

        if (!lnet_isrouter(rtr)) {
                lnet_peer_decref_locked(rtr);
                if (rcd != NULL)
                        list_add(&rcd->rcd_list, &the_lnet.ln_zombie_rcd);
                return; /* router table changed! */
        }

        if (rcd != NULL) {
                LASSERT (rtr->lp_rcd == NULL);
                rtr->lp_rcd = rcd;
        }

        secs = lnet_router_check_interval(rtr);

        CDEBUG(D_NET,
               "rtr %s %d: deadline %lu ping_notsent %d alive %d "
               "alive_count %d lp_ping_timestamp %lu\n",
               libcfs_nid2str(rtr->lp_nid), secs,
               rtr->lp_ping_deadline, rtr->lp_ping_notsent,
               rtr->lp_alive, rtr->lp_alive_count, rtr->lp_ping_timestamp);

        if (secs != 0 && !rtr->lp_ping_notsent &&
            cfs_time_after(now, cfs_time_add(rtr->lp_ping_timestamp,
                                             cfs_time_seconds(secs)))) {
                int               rc;
                lnet_process_id_t id;
                lnet_handle_md_t  mdh;

                id.nid = rtr->lp_nid;
                id.pid = LUSTRE_SRV_LNET_PID;
                CDEBUG(D_NET, "Check: %s\n", libcfs_id2str(id));

                rtr->lp_ping_notsent   = 1;
                rtr->lp_ping_timestamp = now;
                mdh = (rtr->lp_rcd == NULL) ? the_lnet.ln_rc_mdh :
                                              rtr->lp_rcd->rcd_mdh;

                if (rtr->lp_ping_deadline == 0)
                        rtr->lp_ping_deadline = cfs_time_shift(router_ping_timeout);

                LNET_UNLOCK();

                rc = LNetGet(LNET_NID_ANY, mdh, id, LNET_RESERVED_PORTAL,
                             LNET_PROTO_PING_MATCHBITS, 0);

                LNET_LOCK();
                if (rc != 0)
                        rtr->lp_ping_notsent = 0; /* no event pending */
        }

        lnet_peer_decref_locked(rtr);
        return;
}

int
lnet_router_checker_start(void)
{
        static lnet_ping_info_t pinginfo;

        lnet_md_t    md;
        int          rc;
        int          eqsz;
#ifndef __KERNEL__
        lnet_peer_t *rtr;
        __u64        version;
        int          nrtr = 0;
        int          router_checker_max_eqsize = 10240;

        LASSERT (check_routers_before_use);
        LASSERT (dead_router_check_interval > 0);

        LNET_LOCK();

        /* As an approximation, allow each router the same number of
         * outstanding events as it is allowed outstanding sends */
        eqsz = 0;
        version = the_lnet.ln_routers_version;
        list_for_each_entry(rtr, &the_lnet.ln_routers, lp_rtr_list) {
                lnet_ni_t         *ni = rtr->lp_ni;
                lnet_process_id_t  id;

                nrtr++;
                eqsz += ni->ni_peertxcredits;

                /* one async ping reply per router */
                id.nid = rtr->lp_nid;
                id.pid = LUSTRE_SRV_LNET_PID;

                LNET_UNLOCK();

                rc = LNetSetAsync(id, 1);
                if (rc != 0) {
                        CWARN("LNetSetAsync %s failed: %d\n",
                              libcfs_id2str(id), rc);
                        return rc;
                }

                LNET_LOCK();
                /* NB router list doesn't change in userspace */
                LASSERT (version == the_lnet.ln_routers_version);
        }

        LNET_UNLOCK();

        if (nrtr == 0) {
                CDEBUG(D_NET,
                       "No router found, not starting router checker\n");
                return 0;
        }

        /* at least allow a SENT and a REPLY per router */
        if (router_checker_max_eqsize < 2 * nrtr)
                router_checker_max_eqsize = 2 * nrtr;

        LASSERT (eqsz > 0);
        if (eqsz > router_checker_max_eqsize)
                eqsz = router_checker_max_eqsize;
#endif

        LASSERT (the_lnet.ln_rc_state == LNET_RC_STATE_SHUTDOWN);

        if (check_routers_before_use &&
            dead_router_check_interval <= 0) {
                LCONSOLE_ERROR_MSG(0x10a, "'dead_router_check_interval' must be"
                                   " set if 'check_routers_before_use' is set"
                                   "\n");
                return -EINVAL;
        }

        if (!the_lnet.ln_routing &&
            live_router_check_interval <= 0 &&
            dead_router_check_interval <= 0)
                return 0;

#ifdef __KERNEL__
        init_mutex_locked(&the_lnet.ln_rc_signal);
        /* EQ size doesn't matter; the callback is guaranteed to get every
         * event */
        eqsz = 1;
        rc = LNetEQAlloc(eqsz, lnet_router_checker_event,
                         &the_lnet.ln_rc_eqh);
#else
        rc = LNetEQAlloc(eqsz, LNET_EQ_HANDLER_NONE,
                         &the_lnet.ln_rc_eqh);
#endif
        if (rc != 0) {
                CERROR("Can't allocate EQ(%d): %d\n", eqsz, rc);
                return -ENOMEM;
        }

        memset(&md, 0, sizeof(md));
        md.user_ptr  = NULL;
        md.start     = &pinginfo;
        md.length    = sizeof(pinginfo);
        md.options   = LNET_MD_TRUNCATE;
        md.threshold = LNET_MD_THRESH_INF;
        md.eq_handle = the_lnet.ln_rc_eqh;
        rc = LNetMDBind(md, LNET_UNLINK, &the_lnet.ln_rc_mdh);
        if (rc < 0) {
                CERROR("Can't bind MD: %d\n", rc);
                rc = LNetEQFree(the_lnet.ln_rc_eqh);
                LASSERT (rc == 0);
                return -ENOMEM;
        }
        LASSERT (rc == 0);

        the_lnet.ln_rc_state = LNET_RC_STATE_RUNNING;
#ifdef __KERNEL__
        rc = (int)cfs_kernel_thread(lnet_router_checker, NULL, 0);
        if (rc < 0) {
                CERROR("Can't start router checker thread: %d\n", rc);
                the_lnet.ln_rc_state = LNET_RC_STATE_UNLINKING;
                rc = LNetMDUnlink(the_lnet.ln_rc_mdh);
                LASSERT (rc == 0);
                /* block until event callback signals exit */
                mutex_down(&the_lnet.ln_rc_signal);
                rc = LNetEQFree(the_lnet.ln_rc_eqh);
                LASSERT (rc == 0);
                the_lnet.ln_rc_state = LNET_RC_STATE_SHUTDOWN;
                return -ENOMEM;
        }
#endif

        if (check_routers_before_use) {
                /* Note that a helpful side-effect of pinging all known routers
                 * at startup is that it makes them drop stale connections they
                 * may have to a previous instance of me. */
                lnet_wait_known_routerstate();
        }

        return 0;
}

void
lnet_router_checker_stop (void)
{
        int rc;

        if (the_lnet.ln_rc_state == LNET_RC_STATE_SHUTDOWN)
                return;

        LASSERT (the_lnet.ln_rc_state == LNET_RC_STATE_RUNNING);
        the_lnet.ln_rc_state = LNET_RC_STATE_STOPTHREAD;

#ifdef __KERNEL__
        /* block until event callback signals exit */
        mutex_down(&the_lnet.ln_rc_signal);
#else
        while (the_lnet.ln_rc_state != LNET_RC_STATE_UNLINKED) {
                lnet_router_checker();
                cfs_pause(cfs_time_seconds(1));
        }
#endif
        LASSERT (the_lnet.ln_rc_state == LNET_RC_STATE_UNLINKED);

        rc = LNetEQFree(the_lnet.ln_rc_eqh);
        LASSERT (rc == 0);
        the_lnet.ln_rc_state = LNET_RC_STATE_SHUTDOWN;
        return;
}

#if defined(__KERNEL__) && defined(LNET_ROUTER)

static void
lnet_prune_zombie_rcd (int wait_unlink)
{
        lnet_rc_data_t   *rcd;
        lnet_rc_data_t   *tmp;
        struct list_head  free_rcd;
        int               i;
        __u64             version;

        CFS_INIT_LIST_HEAD(&free_rcd);

        LNET_LOCK();
rescan:
        version = the_lnet.ln_routers_version;
        list_for_each_entry_safe (rcd, tmp, &the_lnet.ln_zombie_rcd, rcd_list) {
                if (LNetHandleIsEqual(rcd->rcd_mdh, LNET_INVALID_HANDLE)) {
                        list_del(&rcd->rcd_list);
                        list_add(&rcd->rcd_list, &free_rcd);
                        continue;
                }

                LNET_UNLOCK();

                LNetMDUnlink(rcd->rcd_mdh);

                LNET_LOCK();
                if (version != the_lnet.ln_routers_version)
                        goto rescan;
        }

        i = 2;
        while (wait_unlink && !list_empty(&the_lnet.ln_zombie_rcd)) {
                rcd = list_entry(the_lnet.ln_zombie_rcd.next,
                                 lnet_rc_data_t, rcd_list);
                if (LNetHandleIsEqual(rcd->rcd_mdh, LNET_INVALID_HANDLE)) {
                        list_del(&rcd->rcd_list);
                        list_add(&rcd->rcd_list, &free_rcd);
                        continue;
                }

                LNET_UNLOCK();

                LNetMDUnlink(rcd->rcd_mdh);

                i++;
                CDEBUG(((i & (-i)) == i) ? D_WARNING : D_NET,
                       "Waiting for rc buffers to unlink\n");
                cfs_pause(cfs_time_seconds(1));

                LNET_LOCK();
        }

        LNET_UNLOCK();

        while (!list_empty(&free_rcd)) {
                rcd = list_entry(free_rcd.next, lnet_rc_data_t, rcd_list);
                list_del_init(&rcd->rcd_list);
                lnet_destroy_rc_data(rcd);
        }
        return;
}

static int
lnet_router_checker(void *arg)
{
        int                rc;
        lnet_peer_t       *rtr;
        struct list_head  *entry;
        lnet_process_id_t  rtr_id;

        cfs_daemonize("router_checker");
        cfs_block_allsigs();

        rtr_id.pid = LUSTRE_SRV_LNET_PID;

        LASSERT (the_lnet.ln_rc_state == LNET_RC_STATE_RUNNING);

        while (the_lnet.ln_rc_state == LNET_RC_STATE_RUNNING) {
                __u64 version;

                LNET_LOCK();
rescan:
                version = the_lnet.ln_routers_version;

                list_for_each (entry, &the_lnet.ln_routers) {
                        rtr = list_entry(entry, lnet_peer_t, lp_rtr_list);
                        lnet_ping_router_locked(rtr);

                        /* NB dropped lock */
                        if (version != the_lnet.ln_routers_version) {
                                /* the routers list has changed */
                                goto rescan;
                        }
                }

                LNET_UNLOCK();

                if (the_lnet.ln_routing)
                        lnet_update_ni_status();

                lnet_prune_zombie_rcd(0); /* don't wait for UNLINK */

                /* Call cfs_pause() here always adds 1 to load average 
                 * because kernel counts # active tasks as nr_running 
                 * + nr_uninterruptible. */
                cfs_schedule_timeout(CFS_TASK_INTERRUPTIBLE,
                                     cfs_time_seconds(1));
        }

        LNET_LOCK();

        list_for_each (entry, &the_lnet.ln_routers) {
                rtr = list_entry(entry, lnet_peer_t, lp_rtr_list);

                if (rtr->lp_rcd == NULL)
                        continue;

                LASSERT (list_empty(&rtr->lp_rcd->rcd_list));
                list_add(&rtr->lp_rcd->rcd_list, &the_lnet.ln_zombie_rcd);
                rtr->lp_rcd = NULL;
        }

        LNET_UNLOCK();

        lnet_prune_zombie_rcd(1); /* wait for UNLINK */

        LASSERT (the_lnet.ln_rc_state == LNET_RC_STATE_STOPTHREAD);
        the_lnet.ln_rc_state = LNET_RC_STATE_UNLINKING;

        rc = LNetMDUnlink(the_lnet.ln_rc_mdh);
        LASSERT (rc == 0);

        /* The unlink event callback will signal final completion */
        return 0;
}

void
lnet_destroy_rtrbuf(lnet_rtrbuf_t *rb, int npages)
{
        int sz = offsetof(lnet_rtrbuf_t, rb_ku_iov[npages]);

#ifdef __KERNEL__
        while (--npages >= 0)
                cfs_free_page(rb->rb_kiov[npages].kiov_page);
#else
        while (--npages >= 0)
                cfs_free_page(rb->rb_iov[npages].iov_base);
#endif

        LIBCFS_FREE(rb, sz);
}

lnet_rtrbuf_t *
lnet_new_rtrbuf(lnet_rtrbufpool_t *rbp)
{
        int            npages = rbp->rbp_npages;
        int            sz = offsetof(lnet_rtrbuf_t, rb_kiov[npages]);
        struct page   *page;
        lnet_rtrbuf_t *rb;
        int            i;

        LIBCFS_ALLOC(rb, sz);
        if (rb == NULL)
                return NULL;

        rb->rb_pool = rbp;

        for (i = 0; i < npages; i++) {
                page = cfs_alloc_page(CFS_ALLOC_ZERO | CFS_ALLOC_STD);
                if (page == NULL) {
                        while (--i >= 0)
                                cfs_free_page(rb->rb_kiov[i].kiov_page);

                        LIBCFS_FREE(rb, sz);
                        return NULL;
                }

                rb->rb_kiov[i].kiov_len = CFS_PAGE_SIZE;
                rb->rb_kiov[i].kiov_offset = 0;
                rb->rb_kiov[i].kiov_page = page;
        }

        return rb;
}

void
lnet_rtrpool_free_bufs(lnet_rtrbufpool_t *rbp)
{
        int            npages = rbp->rbp_npages;
        int            nbuffers = 0;
        lnet_rtrbuf_t *rb;

        LASSERT (list_empty(&rbp->rbp_msgs));
        LASSERT (rbp->rbp_credits == rbp->rbp_nbuffers);

        while (!list_empty(&rbp->rbp_bufs)) {
                LASSERT (rbp->rbp_credits > 0);

                rb = list_entry(rbp->rbp_bufs.next,
                                lnet_rtrbuf_t, rb_list);
                list_del(&rb->rb_list);
                lnet_destroy_rtrbuf(rb, npages);
                nbuffers++;
        }

        LASSERT (rbp->rbp_nbuffers == nbuffers);
        LASSERT (rbp->rbp_credits == nbuffers);

        rbp->rbp_nbuffers = rbp->rbp_credits = 0;
}

int
lnet_rtrpool_alloc_bufs(lnet_rtrbufpool_t *rbp, int nbufs)
{
        lnet_rtrbuf_t *rb;
        int            i;

        if (rbp->rbp_nbuffers != 0) {
                LASSERT (rbp->rbp_nbuffers == nbufs);
                return 0;
        }

        for (i = 0; i < nbufs; i++) {
                rb = lnet_new_rtrbuf(rbp);

                if (rb == NULL) {
                        CERROR("Failed to allocate %d router bufs of %d pages\n",
                               nbufs, rbp->rbp_npages);
                        return -ENOMEM;
                }

                rbp->rbp_nbuffers++;
                rbp->rbp_credits++;
                rbp->rbp_mincredits++;
                list_add(&rb->rb_list, &rbp->rbp_bufs);

                /* No allocation "under fire" */
                /* Otherwise we'd need code to schedule blocked msgs etc */
                LASSERT (!the_lnet.ln_routing);
        }

        LASSERT (rbp->rbp_credits == nbufs);
        return 0;
}

void
lnet_rtrpool_init(lnet_rtrbufpool_t *rbp, int npages)
{
        CFS_INIT_LIST_HEAD(&rbp->rbp_msgs);
        CFS_INIT_LIST_HEAD(&rbp->rbp_bufs);

        rbp->rbp_npages = npages;
        rbp->rbp_credits = 0;
        rbp->rbp_mincredits = 0;
}

void
lnet_free_rtrpools(void)
{
        lnet_rtrpool_free_bufs(&the_lnet.ln_rtrpools[0]);
        lnet_rtrpool_free_bufs(&the_lnet.ln_rtrpools[1]);
        lnet_rtrpool_free_bufs(&the_lnet.ln_rtrpools[2]);
}

void
lnet_init_rtrpools(void)
{
        int small_pages = 1;
        int large_pages = (LNET_MTU + CFS_PAGE_SIZE - 1) >> CFS_PAGE_SHIFT;

        lnet_rtrpool_init(&the_lnet.ln_rtrpools[0], 0);
        lnet_rtrpool_init(&the_lnet.ln_rtrpools[1], small_pages);
        lnet_rtrpool_init(&the_lnet.ln_rtrpools[2], large_pages);
}

int
lnet_alloc_rtrpools(int im_a_router)
{
        int       rc;

        if (!strcmp(forwarding, "")) {
                /* not set either way */
                if (!im_a_router)
                        return 0;
        } else if (!strcmp(forwarding, "disabled")) {
                /* explicitly disabled */
                return 0;
        } else if (!strcmp(forwarding, "enabled")) {
                /* explicitly enabled */
        } else {
                LCONSOLE_ERROR_MSG(0x10b, "'forwarding' not set to either "
                                   "'enabled' or 'disabled'\n");
                return -EINVAL;
        }

        if (tiny_router_buffers <= 0) {
                LCONSOLE_ERROR_MSG(0x10c, "tiny_router_buffers=%d invalid when "
                                   "routing enabled\n", tiny_router_buffers);
                rc = -EINVAL;
                goto failed;
        }

        rc = lnet_rtrpool_alloc_bufs(&the_lnet.ln_rtrpools[0],
                                     tiny_router_buffers);
        if (rc != 0)
                goto failed;

        if (small_router_buffers <= 0) {
                LCONSOLE_ERROR_MSG(0x10d, "small_router_buffers=%d invalid when"
                                   " routing enabled\n", small_router_buffers);
                rc = -EINVAL;
                goto failed;
        }

        rc = lnet_rtrpool_alloc_bufs(&the_lnet.ln_rtrpools[1],
                                     small_router_buffers);
        if (rc != 0)
                goto failed;

        if (large_router_buffers <= 0) {
                LCONSOLE_ERROR_MSG(0x10e, "large_router_buffers=%d invalid when"
                                   " routing enabled\n", large_router_buffers);
                rc = -EINVAL;
                goto failed;
        }

        rc = lnet_rtrpool_alloc_bufs(&the_lnet.ln_rtrpools[2],
                                     large_router_buffers);
        if (rc != 0)
                goto failed;

        LNET_LOCK();
        the_lnet.ln_routing = 1;
        LNET_UNLOCK();

        return 0;

 failed:
        lnet_free_rtrpools();
        return rc;
}

int
lnet_notify (lnet_ni_t *ni, lnet_nid_t nid, int alive, cfs_time_t when)
{
        lnet_peer_t *lp = NULL;
        cfs_time_t   now = cfs_time_current();

        LASSERT (!in_interrupt ());

        CDEBUG (D_NET, "%s notifying %s: %s\n",
                (ni == NULL) ? "userspace" : libcfs_nid2str(ni->ni_nid),
                libcfs_nid2str(nid),
                alive ? "up" : "down");

        if (ni != NULL &&
            LNET_NIDNET(ni->ni_nid) != LNET_NIDNET(nid)) {
                CWARN ("Ignoring notification of %s %s by %s (different net)\n",
                        libcfs_nid2str(nid), alive ? "birth" : "death",
                        libcfs_nid2str(ni->ni_nid));
                return -EINVAL;
        }

        /* can't do predictions... */
        if (cfs_time_after(when, now)) {
                CWARN ("Ignoring prediction from %s of %s %s "
                       "%ld seconds in the future\n",
                       (ni == NULL) ? "userspace" : libcfs_nid2str(ni->ni_nid),
                       libcfs_nid2str(nid), alive ? "up" : "down",
                       cfs_duration_sec(cfs_time_sub(when, now)));
                return -EINVAL;
        }

        if (ni != NULL && !alive &&             /* LND telling me she's down */
            !auto_down) {                       /* auto-down disabled */
                CDEBUG(D_NET, "Auto-down disabled\n");
                return 0;
        }

        LNET_LOCK();

        lp = lnet_find_peer_locked(nid);
        if (lp == NULL) {
                /* nid not found */
                LNET_UNLOCK();
                CDEBUG(D_NET, "%s not found\n", libcfs_nid2str(nid));
                return 0;
        }

        /* We can't fully trust LND on reporting exact peer last_alive
         * if he notifies us about dead peer. For example ksocklnd can
         * call us with when == _time_when_the_node_was_booted_ if
         * no connections were successfully established */
        if (ni != NULL && !alive && when < lp->lp_last_alive)
                when = lp->lp_last_alive;

        lnet_notify_locked(lp, ni == NULL, alive, when);

        LNET_UNLOCK();

        lnet_do_notify(lp);

        LNET_LOCK();

        lnet_peer_decref_locked(lp);

        LNET_UNLOCK();
        return 0;
}
EXPORT_SYMBOL(lnet_notify);

void
lnet_get_tunables (void)
{
        return;
}

#else

int
lnet_notify (__unusedx lnet_ni_t *ni, __unusedx lnet_nid_t nid,
    __unusedx int alive, __unusedx cfs_time_t when)
{
        return -EOPNOTSUPP;
}

void
lnet_router_checker (void)
{
        static time_t last = 0;
        static int    running = 0;

        time_t            now = cfs_time_current_sec();
        int               interval = now - last;
        int               rc;
        __u64             version;
        lnet_peer_t      *rtr;

        /* It's no use to call me again within a sec - all intervals and
         * timeouts are measured in seconds */
        if (last != 0 && interval < 2)
                return;

        if (last != 0 && live_router_check_interval &&
            interval > MAX(live_router_check_interval,
                           dead_router_check_interval))
                CNETERR("Checker(%d/%d) not called for %d seconds\n",
                        live_router_check_interval, dead_router_check_interval,
                        interval);

        LNET_LOCK();
        LASSERT (!running); /* recursion check */
        running = 1;
        LNET_UNLOCK();

        last = now;

        if (the_lnet.ln_rc_state == LNET_RC_STATE_STOPTHREAD) {
                the_lnet.ln_rc_state = LNET_RC_STATE_UNLINKING;
                rc = LNetMDUnlink(the_lnet.ln_rc_mdh);
                LASSERT (rc == 0);
        }

        /* consume all pending events */
        while (1) {
                int          i;
                lnet_event_t ev;

                /* NB ln_rc_eqh must be the 1st in 'eventqs' otherwise the
                 * recursion breaker in LNetEQPoll would fail */
                rc = LNetEQPoll(&the_lnet.ln_rc_eqh, 1, 0, &ev, &i);
                if (rc == 0)   /* no event pending */
                        break;

                /* NB a lost SENT prevents me from pinging a router again */
                if (rc == -EOVERFLOW) {
                        CERROR("Dropped an event!!!\n");
                        abort();
                }

                LASSERT (rc == 1);

                LNET_LOCK();
                lnet_router_checker_event(&ev);
                LNET_UNLOCK();
        }

        if (the_lnet.ln_rc_state == LNET_RC_STATE_UNLINKED ||
            the_lnet.ln_rc_state == LNET_RC_STATE_UNLINKING) {
                running = 0;
                return;
        }

        LASSERT (the_lnet.ln_rc_state == LNET_RC_STATE_RUNNING);

        LNET_LOCK();

        version = the_lnet.ln_routers_version;
        list_for_each_entry (rtr, &the_lnet.ln_routers, lp_rtr_list) {
                lnet_ping_router_locked(rtr);
                LASSERT (version == the_lnet.ln_routers_version);
        }

        LNET_UNLOCK();

        running = 0; /* lock only needed for the recursion check */
        return;
}

/* NB lnet_peers_start_down depends on me,
 * so must be called before any peer creation */
void
lnet_get_tunables (void)
{
        char *s;

        s = getenv("LNET_ROUTER_PING_TIMEOUT");
        if (s != NULL) router_ping_timeout = atoi(s);

        s = getenv("LNET_LIVE_ROUTER_CHECK_INTERVAL");
        if (s != NULL) live_router_check_interval = atoi(s);

        s = getenv("LNET_DEAD_ROUTER_CHECK_INTERVAL");
        if (s != NULL) dead_router_check_interval = atoi(s);

        /* This replaces old lnd_notify mechanism */
        check_routers_before_use = 1;
        if (dead_router_check_interval <= 0)
                dead_router_check_interval = 30;
}

void
lnet_destroy_rtrbuf(lnet_rtrbuf_t *rb, int npages)
{
        __unusedx int sz = offsetof(lnet_rtrbuf_t, rb_ku_iov[npages]);

#ifdef __KERNEL__
        while (--npages >= 0)
                cfs_free_page(rb->rb_kiov[npages].kiov_page);
#else
        while (--npages >= 0)
                cfs_free_page(rb->rb_iov[npages].iov_base);
#endif

        LIBCFS_FREE(rb, sz);
}

lnet_rtrbuf_t *
lnet_new_rtrbuf(lnet_rtrbufpool_t *rbp)
{
        int            npages = rbp->rbp_npages;
        int            sz = offsetof(lnet_rtrbuf_t, rb_ku_iov[npages]);
        struct page   *page;
        lnet_rtrbuf_t *rb;
        int            i;

        LIBCFS_ALLOC(rb, sz);
        if (rb == NULL)
                return NULL;

        rb->rb_pool = rbp;

        for (i = 0; i < npages; i++) {
                page = cfs_alloc_page(CFS_ALLOC_ZERO | CFS_ALLOC_STD);
                if (page == NULL) {
#ifdef __KERNEL__
                        while (--i >= 0)
                                cfs_free_page(rb->rb_kiov[i].kiov_page);
#else
                        while (--i >= 0)
                                cfs_free_page(rb->rb_iov[i].iov_base);
#endif

                        LIBCFS_FREE(rb, sz);
                        return NULL;
                }

#ifdef __KERNEL__
                rb->rb_kiov[i].kiov_len = CFS_PAGE_SIZE;
                rb->rb_kiov[i].kiov_offset = 0;
                rb->rb_kiov[i].kiov_page = page;
#else
                rb->rb_iov[i].iov_base = page;
                rb->rb_iov[i].iov_len = CFS_PAGE_SIZE;
#endif
        }

        return rb;
}

void
lnet_rtrpool_free_bufs(lnet_rtrbufpool_t *rbp)
{
        int            npages = rbp->rbp_npages;
        int            nbuffers = 0;
        lnet_rtrbuf_t *rb;

        LASSERT (list_empty(&rbp->rbp_msgs));
        LASSERT (rbp->rbp_credits == rbp->rbp_nbuffers);

        while (!list_empty(&rbp->rbp_bufs)) {
                LASSERT (rbp->rbp_credits > 0);

                rb = list_entry(rbp->rbp_bufs.next,
                                lnet_rtrbuf_t, rb_list);
                list_del(&rb->rb_list);
                lnet_destroy_rtrbuf(rb, npages);
                nbuffers++;
        }

        LASSERT (rbp->rbp_nbuffers == nbuffers);
        LASSERT (rbp->rbp_credits == nbuffers);

        rbp->rbp_nbuffers = rbp->rbp_credits = 0;
}

int
lnet_rtrpool_alloc_bufs(lnet_rtrbufpool_t *rbp, int nbufs)
{
        lnet_rtrbuf_t *rb;
        int            i;

        if (rbp->rbp_nbuffers != 0) {
                LASSERT (rbp->rbp_nbuffers == nbufs);
                return 0;
        }

        for (i = 0; i < nbufs; i++) {
                rb = lnet_new_rtrbuf(rbp);

                if (rb == NULL) {
                        CERROR("Failed to allocate %d router bufs of %d pages\n",
                               nbufs, rbp->rbp_npages);
                        return -ENOMEM;
                }

                rbp->rbp_nbuffers++;
                rbp->rbp_credits++;
                rbp->rbp_mincredits++;
                list_add(&rb->rb_list, &rbp->rbp_bufs);

                /* No allocation "under fire" */
                /* Otherwise we'd need code to schedule blocked msgs etc */
                LASSERT (!the_lnet.ln_routing);
        }

        LASSERT (rbp->rbp_credits == nbufs);
        return 0;
}

void
lnet_rtrpool_init(lnet_rtrbufpool_t *rbp, int npages)
{
        CFS_INIT_LIST_HEAD(&rbp->rbp_msgs);
        CFS_INIT_LIST_HEAD(&rbp->rbp_bufs);

        rbp->rbp_npages = npages;
        rbp->rbp_credits = 0;
        rbp->rbp_mincredits = 0;
}

void
lnet_free_rtrpools(void)
{
        lnet_rtrpool_free_bufs(&the_lnet.ln_rtrpools[0]);
        lnet_rtrpool_free_bufs(&the_lnet.ln_rtrpools[1]);
        lnet_rtrpool_free_bufs(&the_lnet.ln_rtrpools[2]);
}

void
lnet_init_rtrpools(void)
{
        int small_pages = 1;
        int large_pages = (LNET_MTU + CFS_PAGE_SIZE - 1) >> CFS_PAGE_SHIFT;

        lnet_rtrpool_init(&the_lnet.ln_rtrpools[0], 0);
        lnet_rtrpool_init(&the_lnet.ln_rtrpools[1], small_pages);
        lnet_rtrpool_init(&the_lnet.ln_rtrpools[2], large_pages);
}

int
lnet_alloc_rtrpools(int im_a_router)
{
        int       rc;

        if (!im_a_router)
                return (0);

        rc = lnet_rtrpool_alloc_bufs(&the_lnet.ln_rtrpools[0],
                                     tiny_router_buffers);
        if (rc != 0)
                goto failed;

        if (small_router_buffers <= 0) {
                LCONSOLE_ERROR_MSG(0x10d, "small_router_buffers=%d invalid when"
                                   " routing enabled\n", small_router_buffers);
                rc = -EINVAL;
                goto failed;
        }

        rc = lnet_rtrpool_alloc_bufs(&the_lnet.ln_rtrpools[1],
                                     small_router_buffers);
        if (rc != 0)
                goto failed;

        if (large_router_buffers <= 0) {
                LCONSOLE_ERROR_MSG(0x10e, "large_router_buffers=%d invalid when"
                                   " routing enabled\n", large_router_buffers);
                rc = -EINVAL;
                goto failed;
        }

        rc = lnet_rtrpool_alloc_bufs(&the_lnet.ln_rtrpools[2],
                                     large_router_buffers);
        if (rc != 0)
                goto failed;

        LNET_LOCK();
        the_lnet.ln_routing = 1;
        LNET_UNLOCK();

        return 0;

 failed:
        lnet_free_rtrpools();
        return rc;
}

#endif
