/*
 * include/haproxy/http_fetch.h
 * This file contains the minimally required http sample fetch declarations.
 *
 * Copyright (C) 2000-2018 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _HAPROXY_HTTP_FETCH_H
#define _HAPROXY_HTTP_FETCH_H

#include <haproxy/arg-t.h>
#include <haproxy/base.h>
#include <haproxy/channel-t.h>
#include <haproxy/checks-t.h>
#include <haproxy/sample-t.h>

struct htx *smp_prefetch_htx(struct sample *smp, struct channel *chn, struct check *check, int vol);
int val_hdr(struct arg *arg, char **err_msg);

#endif /* _HAPROXY_HTTP_FETCH_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */