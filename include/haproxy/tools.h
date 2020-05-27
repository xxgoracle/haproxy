/*
 * include/haproxy/tools.h
 * Trivial macros needed everywhere.
 *
 * Copyright (C) 2000-2020 Willy Tarreau - w@1wt.eu
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

#ifndef _HAPROXY_TOOLS_H
#define _HAPROXY_TOOLS_H

#include <sys/param.h>
#include <unistd.h>
#include <haproxy/compiler.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define SWAP(a, b) do { typeof(a) t; t = a; a = b; b = t; } while(0)

/* return an integer of type <ret> with only the highest bit set. <ret> may be
 * both a variable or a type.
 */
#define MID_RANGE(ret) ((typeof(ret))1 << (8*sizeof(ret) - 1))

/* return the largest possible integer of type <ret>, with all bits set */
#define MAX_RANGE(ret) (~(typeof(ret))0)

/* quick debugging hack, should really be removed ASAP */
#ifdef DEBUG_FULL
#define DPRINTF(x...) fprintf(x)
#else
#define DPRINTF(x...)
#endif

/* This abort is more efficient than abort() because it does not mangle the
 * stack and stops at the exact location we need.
 */
#define ABORT_NOW() (*(volatile int*)1=0)

/* BUG_ON: complains if <cond> is true when DEBUG_STRICT or DEBUG_STRICT_NOCRASH
 * are set, does nothing otherwise. With DEBUG_STRICT in addition it immediately
 * crashes using ABORT_NOW() above.
 */
#if defined(DEBUG_STRICT) || defined(DEBUG_STRICT_NOCRASH)
#if defined(DEBUG_STRICT)
#define CRASH_NOW() ABORT_NOW()
#else
#define CRASH_NOW()
#endif

#define BUG_ON(cond) _BUG_ON(cond, __FILE__, __LINE__)
#define _BUG_ON(cond, file, line) __BUG_ON(cond, file, line)
#define __BUG_ON(cond, file, line)                                             \
	do {                                                                   \
		if (unlikely(cond)) {					       \
			const char msg[] = "\nFATAL: bug condition \"" #cond "\" matched at " file ":" #line "\n"; \
			DISGUISE(write(2, msg, __builtin_strlen(msg)));        \
			CRASH_NOW();                                           \
		}                                                              \
	} while (0)
#else
#undef CRASH_NOW
#define BUG_ON(cond)
#endif

#endif /* _HAPROXY_TOOLS_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
