/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "resolved-dns-query.h"
#include "resolved-dns-domain.h"

#define TRANSACTION_TIMEOUT_USEC (5 * USEC_PER_SEC)
#define QUERY_TIMEOUT_USEC (30 * USEC_PER_SEC)
#define ATTEMPTS_MAX 8

DnsQueryTransaction* dns_query_transaction_free(DnsQueryTransaction *t) {
        if (!t)
                return NULL;

        sd_event_source_unref(t->timeout_event_source);

        dns_packet_unref(t->sent);
        dns_packet_unref(t->received);

        sd_event_source_unref(t->tcp_event_source);
        safe_close(t->tcp_fd);

        if (t->query) {
                LIST_REMOVE(transactions_by_query, t->query->transactions, t);
                hashmap_remove(t->query->manager->dns_query_transactions, UINT_TO_PTR(t->id));
        }

        if (t->scope)
                LIST_REMOVE(transactions_by_scope, t->scope->transactions, t);

        free(t);
        return NULL;
}

DEFINE_TRIVIAL_CLEANUP_FUNC(DnsQueryTransaction*, dns_query_transaction_free);

static int dns_query_transaction_new(DnsQuery *q, DnsQueryTransaction **ret, DnsScope *s) {
        _cleanup_(dns_query_transaction_freep) DnsQueryTransaction *t = NULL;
        int r;

        assert(q);
        assert(s);

        r = hashmap_ensure_allocated(&q->manager->dns_query_transactions, NULL, NULL);
        if (r < 0)
                return r;

        t = new0(DnsQueryTransaction, 1);
        if (!t)
                return -ENOMEM;

        t->tcp_fd = -1;

        do
                random_bytes(&t->id, sizeof(t->id));
        while (t->id == 0 ||
               hashmap_get(q->manager->dns_query_transactions, UINT_TO_PTR(t->id)));

        r = hashmap_put(q->manager->dns_query_transactions, UINT_TO_PTR(t->id), t);
        if (r < 0) {
                t->id = 0;
                return r;
        }

        LIST_PREPEND(transactions_by_query, q->transactions, t);
        t->query = q;

        LIST_PREPEND(transactions_by_scope, s->transactions, t);
        t->scope = s;

        if (ret)
                *ret = t;

        t = NULL;

        return 0;
}

static void dns_query_transaction_stop(DnsQueryTransaction *t) {
        assert(t);

        t->timeout_event_source = sd_event_source_unref(t->timeout_event_source);
        t->tcp_event_source = sd_event_source_unref(t->tcp_event_source);
        t->tcp_fd = safe_close(t->tcp_fd);
}

static void dns_query_transaction_set_state(DnsQueryTransaction *t, DnsQueryState state) {
        assert(t);

        if (t->state == state)
                return;

        t->state = state;

        if (state != DNS_QUERY_SENT) {
                dns_query_transaction_stop(t);
                dns_query_finish(t->query);
        }
}

static int on_tcp_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        DnsQueryTransaction *t = userdata;
        int r;

        assert(t);

        if (revents & EPOLLOUT) {
                struct iovec iov[2];
                be16_t sz;
                ssize_t ss;

                sz = htobe16(t->sent->size);

                iov[0].iov_base = &sz;
                iov[0].iov_len = sizeof(sz);
                iov[1].iov_base = DNS_PACKET_DATA(t->sent);
                iov[1].iov_len = t->sent->size;

                IOVEC_INCREMENT(iov, 2, t->tcp_written);

                ss = writev(fd, iov, 2);
                if (ss < 0) {
                        if (errno != EINTR && errno != EAGAIN) {
                                dns_query_transaction_set_state(t, DNS_QUERY_RESOURCES);
                                return -errno;
                        }
                } else
                        t->tcp_written += ss;

                /* Are we done? If so, disable the event source for EPOLLOUT */
                if (t->tcp_written >= sizeof(sz) + t->sent->size) {
                        r = sd_event_source_set_io_events(s, EPOLLIN);
                        if (r < 0) {
                                dns_query_transaction_set_state(t, DNS_QUERY_RESOURCES);
                                return r;
                        }
                }
        }

        if (revents & (EPOLLIN|EPOLLHUP|EPOLLRDHUP)) {

                if (t->tcp_read < sizeof(t->tcp_read_size)) {
                        ssize_t ss;

                        ss = read(fd, (uint8_t*) &t->tcp_read_size + t->tcp_read, sizeof(t->tcp_read_size) - t->tcp_read);
                        if (ss < 0) {
                                if (errno != EINTR && errno != EAGAIN) {
                                        dns_query_transaction_set_state(t, DNS_QUERY_RESOURCES);
                                        return -errno;
                                }
                        } else if (ss == 0) {
                                dns_query_transaction_set_state(t, DNS_QUERY_RESOURCES);
                                return -EIO;
                        } else
                                t->tcp_read += ss;
                }

                if (t->tcp_read >= sizeof(t->tcp_read_size)) {

                        if (be16toh(t->tcp_read_size) < DNS_PACKET_HEADER_SIZE) {
                                dns_query_transaction_set_state(t, DNS_QUERY_INVALID_REPLY);
                                return -EBADMSG;
                        }

                        if (t->tcp_read < sizeof(t->tcp_read_size) + be16toh(t->tcp_read_size)) {
                                ssize_t ss;

                                if (!t->received) {
                                        r = dns_packet_new(&t->received, be16toh(t->tcp_read_size));
                                        if (r < 0) {
                                                dns_query_transaction_set_state(t, DNS_QUERY_RESOURCES);
                                                return r;
                                        }
                                }

                                ss = read(fd,
                                          (uint8_t*) DNS_PACKET_DATA(t->received) + t->tcp_read - sizeof(t->tcp_read_size),
                                          sizeof(t->tcp_read_size) + be16toh(t->tcp_read_size) - t->tcp_read);
                                if (ss < 0) {
                                        if (errno != EINTR && errno != EAGAIN) {
                                                dns_query_transaction_set_state(t, DNS_QUERY_RESOURCES);
                                                return -errno;
                                        }
                                } else if (ss == 0) {
                                        dns_query_transaction_set_state(t, DNS_QUERY_RESOURCES);
                                        return -EIO;
                                }  else
                                        t->tcp_read += ss;
                        }

                        if (t->tcp_read >= sizeof(t->tcp_read_size) + be16toh(t->tcp_read_size)) {
                                t->received->size = be16toh(t->tcp_read_size);
                                dns_query_transaction_reply(t, t->received);
                                return 0;
                        }
                }
        }

        return 0;
}

static int dns_query_transaction_start_tcp(DnsQueryTransaction *t) {
        int r;

        assert(t);

        if (t->tcp_fd >= 0)
                return 0;

        t->tcp_written = 0;
        t->tcp_read = 0;
        t->received = dns_packet_unref(t->received);

        t->tcp_fd = dns_scope_tcp_socket(t->scope);
        if (t->tcp_fd < 0)
                return t->tcp_fd;

        r = sd_event_add_io(t->query->manager->event, &t->tcp_event_source, t->tcp_fd, EPOLLIN|EPOLLOUT, on_tcp_ready, t);
        if (r < 0) {
                t->tcp_fd = safe_close(t->tcp_fd);
                return r;
        }

        return 0;
}

void dns_query_transaction_reply(DnsQueryTransaction *t, DnsPacket *p) {
        int r;

        assert(t);
        assert(p);

        if (t->state != DNS_QUERY_SENT)
                return;

        if (t->received != p) {
                dns_packet_unref(t->received);
                t->received = dns_packet_ref(p);
        }

        if (t->tcp_fd >= 0) {
                if (DNS_PACKET_TC(p)) {
                        /* Truncated via TCP? Somebody must be fucking with us */
                        dns_query_transaction_set_state(t, DNS_QUERY_INVALID_REPLY);
                        return;
                }

                if (DNS_PACKET_ID(p) != t->id) {
                        /* Not the reply to our query? Somebody must be fucking with us */
                        dns_query_transaction_set_state(t, DNS_QUERY_INVALID_REPLY);
                        return;
                }
        }

        if (DNS_PACKET_TC(p)) {
                /* Response was truncated, let's try again with good old TCP */
                r = dns_query_transaction_start_tcp(t);
                if (r < 0) {
                        dns_query_transaction_set_state(t, DNS_QUERY_RESOURCES);
                        return;
                }
        }

        if (DNS_PACKET_RCODE(p) == DNS_RCODE_SUCCESS)
                dns_query_transaction_set_state(t, DNS_QUERY_SUCCESS);
        else
                dns_query_transaction_set_state(t, DNS_QUERY_FAILURE);
}

static int on_transaction_timeout(sd_event_source *s, usec_t usec, void *userdata) {
        DnsQueryTransaction *t = userdata;
        int r;

        assert(s);
        assert(t);

        /* Timeout reached? Try again, with a new server */
        dns_scope_next_dns_server(t->scope);

        r = dns_query_transaction_start(t);
        if (r < 0)
                dns_query_transaction_set_state(t, DNS_QUERY_RESOURCES);

        return 0;
}

static int dns_query_make_packet(DnsQueryTransaction *t) {
        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;
        unsigned n;
        int r;

        assert(t);

        if (t->sent)
                return 0;

        r = dns_packet_new_query(&p, 0);
        if (r < 0)
                return r;

        for (n = 0; n < t->query->n_keys; n++) {
                r = dns_packet_append_key(p, &t->query->keys[n], NULL);
                if (r < 0)
                        return r;
        }

        DNS_PACKET_HEADER(p)->qdcount = htobe16(t->query->n_keys);
        DNS_PACKET_HEADER(p)->id = t->id;

        t->sent = p;
        p = NULL;

        return 0;
}

int dns_query_transaction_start(DnsQueryTransaction *t) {
        int r;

        assert(t);

        dns_query_transaction_stop(t);

        if (t->n_attempts >= ATTEMPTS_MAX) {
                dns_query_transaction_set_state(t, DNS_QUERY_ATTEMPTS_MAX);
                return 0;
        }
        t->n_attempts++;

        r = dns_query_make_packet(t);
        if (r < 0)
                return r;

        /* Try via UDP, and if that fails due to large size try via TCP */
        r = dns_scope_send(t->scope, t->sent);
        if (r == -EMSGSIZE)
                r = dns_query_transaction_start_tcp(t);

        if (r == -ESRCH) {
                dns_query_transaction_set_state(t, DNS_QUERY_NO_SERVERS);
                return 0;
        }
        if (r < 0) {
                /* Couldn't send? Try immediately again, with a new server */
                dns_scope_next_dns_server(t->scope);

                return dns_query_transaction_start(t);
        }

        r = sd_event_add_time(t->query->manager->event, &t->timeout_event_source, CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + TRANSACTION_TIMEOUT_USEC, 0, on_transaction_timeout, t);
        if (r < 0)
                return r;

        dns_query_transaction_set_state(t, DNS_QUERY_SENT);
        return 1;
}

DnsQuery *dns_query_free(DnsQuery *q) {
        unsigned n;

        if (!q)
                return NULL;

        sd_bus_message_unref(q->request);
        dns_packet_unref(q->received);
        sd_event_source_unref(q->timeout_event_source);

        while (q->transactions)
                dns_query_transaction_free(q->transactions);

        if (q->manager)
                LIST_REMOVE(queries, q->manager->dns_queries, q);

        for (n = 0; n < q->n_keys; n++)
                free(q->keys[n].name);
        free(q->keys);
        free(q);

        return NULL;
}

int dns_query_new(Manager *m, DnsQuery **ret, DnsResourceKey *keys, unsigned n_keys) {
        _cleanup_(dns_query_freep) DnsQuery *q = NULL;
        DnsScope *s, *first = NULL;
        DnsScopeMatch found = DNS_SCOPE_NO;
        const char *name = NULL;
        int n, r;

        assert(m);

        if (n_keys <= 0 || n_keys >= 65535)
                return -EINVAL;

        assert(keys);

        q = new0(DnsQuery, 1);
        if (!q)
                return -ENOMEM;

        q->keys = new(DnsResourceKey, n_keys);
        if (!q->keys)
                return -ENOMEM;

        for (q->n_keys = 0; q->n_keys < n_keys; q->n_keys++) {
                q->keys[q->n_keys].class = keys[q->n_keys].class;
                q->keys[q->n_keys].type = keys[q->n_keys].type;
                q->keys[q->n_keys].name = strdup(keys[q->n_keys].name);
                if (!q->keys[q->n_keys].name)
                        return -ENOMEM;

                if (!name)
                        name = q->keys[q->n_keys].name;
                else if (!dns_name_equal(name, q->keys[q->n_keys].name))
                        return -EINVAL;
        }

        LIST_PREPEND(queries, m->dns_queries, q);
        q->manager = m;

        LIST_FOREACH(scopes, s, m->dns_scopes) {
                DnsScopeMatch match;

                match = dns_scope_test(s, name);
                if (match < 0)
                        return match;

                if (match == DNS_SCOPE_NO)
                        continue;

                found = match;

                if (match == DNS_SCOPE_YES) {
                        first = s;
                        break;
                } else {
                        assert(match == DNS_SCOPE_MAYBE);

                        if (!first)
                                first = s;
                }
        }

        if (found == DNS_SCOPE_NO)
                return -ENETDOWN;

        r = dns_query_transaction_new(q, NULL, first);
        if (r < 0)
                return r;

        n = 1;
        LIST_FOREACH(scopes, s, first->scopes_next) {
                DnsScopeMatch match;

                match = dns_scope_test(s, name);
                if (match < 0)
                        return match;

                if (match != found)
                        continue;

                r = dns_query_transaction_new(q, NULL, s);
                if (r < 0)
                        return r;

                n++;
        }

        if (ret)
                *ret = q;
        q = NULL;

        return n;
}

static void dns_query_set_state(DnsQuery *q, DnsQueryState state) {
        assert(q);

        if (q->state == state)
                return;

        q->state = state;

        if (state == DNS_QUERY_SENT)
                return;

        q->timeout_event_source = sd_event_source_unref(q->timeout_event_source);

        while (q->transactions)
                dns_query_transaction_free(q->transactions);

        if (q->complete)
                q->complete(q);
}

static int on_query_timeout(sd_event_source *s, usec_t usec, void *userdata) {
        DnsQuery *q = userdata;

        assert(s);
        assert(q);

        dns_query_set_state(q, DNS_QUERY_TIMEOUT);
        return 0;
}

int dns_query_start(DnsQuery *q) {
        DnsQueryTransaction *t;
        int r;

        assert(q);
        assert(q->state == DNS_QUERY_NULL);

        if (!q->transactions)
                return -ENETDOWN;

        r = sd_event_add_time(q->manager->event, &q->timeout_event_source, CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + QUERY_TIMEOUT_USEC, 0, on_query_timeout, q);
        if (r < 0)
                goto fail;

        dns_query_set_state(q, DNS_QUERY_SENT);

        LIST_FOREACH(transactions_by_query, t, q->transactions) {

                r = dns_query_transaction_start(t);
                if (r < 0)
                        goto fail;

                if (q->state != DNS_QUERY_SENT)
                        break;
        }

        return 0;

fail:
        while (q->transactions)
                dns_query_transaction_free(q->transactions);

        return r;
}

void dns_query_finish(DnsQuery *q) {
        DnsQueryTransaction *t;
        DnsQueryState state = DNS_QUERY_NO_SERVERS;
        DnsPacket *received = NULL;

        assert(q);

        if (q->state != DNS_QUERY_SENT)
                return;

        LIST_FOREACH(transactions_by_query, t, q->transactions) {

                /* One of the transactions is still going on, let's wait for it */
                if (t->state == DNS_QUERY_SENT || t->state == DNS_QUERY_NULL)
                        return;

                /* One of the transactions is successful, let's use it */
                if (t->state == DNS_QUERY_SUCCESS) {
                        q->received = dns_packet_ref(t->received);
                        dns_query_set_state(q, DNS_QUERY_SUCCESS);
                        return;
                }

                /* One of the transactions has failed, let's see
                 * whether we find anything better, but if not, return
                 * its response packet */
                if (t->state == DNS_QUERY_FAILURE) {
                        received = t->received;
                        state = DNS_QUERY_FAILURE;
                        continue;
                }

                if (state == DNS_QUERY_NO_SERVERS && t->state != DNS_QUERY_NO_SERVERS)
                        state = t->state;
        }

        if (state == DNS_QUERY_FAILURE)
                q->received = dns_packet_ref(received);

        dns_query_set_state(q, state);
}