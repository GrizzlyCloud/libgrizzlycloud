/*
 *
 * GrizzlyCloud library - simplified VPN alternative for IoT
 * Copyright (C) 2017 - 2018 Filip Pancik
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <gc.h>

static struct gc_tunnel_s    *tunnels  = NULL;

static struct gc_gen_client_s *tunnel_client_find(sn port, sn fd)
{
    struct gc_tunnel_s *t;
    struct ht_s *kv;

    for(t = tunnels; t != NULL; t = t->next) {
        if(!sn_cmps(t->port_remote, port)) continue;
        if(!(t->server && t->server->clients)) continue;

        char key[16];
        snprintf(key, sizeof(key), "%.*s", sn_p(fd));
        kv = ht_get(t->server->clients, key, strlen(key));

        if(!kv) continue;

        return (struct gc_gen_client_s *)kv->s;
    }

    return NULL;
}

int gc_tunnel_response(struct gc_s *gc, struct proto_s *p, char **argv, int argc)
{
    if(argc != 3) {
        return GC_ERROR;
    }

    sn_initr(port, argv[1], strlen(argv[1]));
    sn_initr(fd,   argv[2], strlen(argv[2]));

    hm_log(LOG_TRACE, &gc->log, "Tunnel response [port:fd] [%.*s:%.*s]",
                                sn_p(port),
                                sn_p(fd));

    struct gc_gen_client_s *client = tunnel_client_find(port, fd);
    if(!client) {
        hm_log(LOG_TRACE, &gc->log, "Tunnel client not found");
        return GC_ERROR;
    }

    gc_gen_ev_send(client,
                   p->u.message_from.body.s,
                   p->u.message_from.body.n);

    return GC_OK;
}

static void client_data(struct gc_gen_client_s *client, char *buf, const int len)
{
    struct gc_tunnel_s *tunnel = client->parent->tunnel;

    assert(tunnel);

    // Payload
    sn_initr(payload, (char *)buf, len);

    // Client file descriptor
    char client_fd[8];
    snprintf(client_fd, sizeof(client_fd), "%d", client->base.fd);

    // Message header
    char header[64];
    snprintf(header, sizeof(header), "tunnel_request/%.*s/%s/%.*s",
                                     sn_p(tunnel->port_remote),
                                     client_fd,
                                     sn_p(tunnel->port_local));
    sn_initr(snheader, header, strlen(header));

    hm_log(LOG_TRACE, client->base.log, "{Tunnel}: header [%.*s]",
                                        snheader.n, snheader.s);

    struct proto_s m = { .type = MESSAGE_TO };
    sn_set(m.u.message_to.to,      tunnel->device);
    sn_set(m.u.message_to.address, tunnel->pid);
    sn_set(m.u.message_to.body,    payload);
    sn_set(m.u.message_to.tp,      snheader);

    gc_packet_send(client->base.gc, &m);
}

static int alloc_server(struct gc_s *gc, struct gc_gen_server_s **c, sn port_local)
{
    *c = hm_palloc(gc->pool, sizeof(**c));
    if(!*c) return GC_ERROR;

    memset(*c, 0, sizeof(**c));

    (*c)->loop = gc->loop;
    (*c)->log  = &gc->log;
    (*c)->pool = gc->pool;
    (*c)->callback.data = client_data;
    (*c)->host = "0.0.0.0";

    sn_to_char(port, port_local, 32);
    (*c)->port = port;

    int ret;
    ret = async_server(*c, gc);
    if(ret != GC_OK) {
        hm_pfree(gc->pool, *c);
        return ret;
    }

    return GC_OK;
}

int gc_tunnel_add(struct gc_s *gc, struct gc_device_pair_s *pair, sn type)
{
    struct gc_gen_server_s *c = NULL;

    sn_initz(forced, "forced");
    if(!sn_cmps(type, forced)) {
        int ret;
        ret = alloc_server(gc, &c, pair->port_local);
        if(ret != GC_OK) return ret;
    }

    struct gc_tunnel_s *t = hm_palloc(gc->pool, sizeof(*t));
    if(!t) return GC_ERROR;

    memset(t, 0, sizeof(*t));

    snb_cpy_ds(t->cloud,       pair->cloud);
    snb_cpy_ds(t->pid,         pair->pid);
    snb_cpy_ds(t->device,      pair->device);
    snb_cpy_ds(t->port_local,  pair->port_local);
    snb_cpy_ds(t->port_remote, pair->port_remote);
    snb_cpy_ds(t->type,        pair->type);

    // Link tunnel and server
    t->server = c;
    if(c) c->tunnel = t;

    t->next = tunnels;
    tunnels = t;

    return GC_OK;
}

void gc_tunnel_stop(struct hm_pool_s *pool, sn pid)
{
    struct gc_tunnel_s *t, *prev, *del;
    for(t = tunnels, prev = NULL; t != NULL; ) {
        if(sn_cmps(t->pid, pid)) {

            if(t->server) {
                hm_log(LOG_TRACE, t->server->log, "Tunnel stop [cloud:device:port:port_remote] [%.*s:%.*s:%.*s:%.*s]",
                                                  sn_p(t->cloud),
                                                  sn_p(t->device),
                                                  sn_p(t->port_local),
                                                  sn_p(t->port_remote));
                async_server_shutdown(t->server);
            }

            if(prev) prev->next = t->next;
            else     tunnels = t->next;

            del = t;
            t = t->next;
            hm_pfree(pool, del);
        } else {
            prev = t;
            t = t->next;
        }
    }
}

void gc_tunnel_stop_all(struct hm_pool_s *pool)
{
    struct gc_tunnel_s *t, *del;

    for(t = tunnels; t != NULL; ) {
        if(t->server) async_server_shutdown(t->server);
        del = t;
        t = t->next;
        hm_pfree(pool, del);
    }

    tunnels = NULL;
}
