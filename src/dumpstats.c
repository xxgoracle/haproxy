/*
 * Functions dedicated to statistics output and the stats socket
 *
 * Copyright 2000-2012 Willy Tarreau <w@1wt.eu>
 * Copyright 2007-2009 Krzysztof Piotr Oledzki <ole@ans.pl>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <common/cfgparse.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/memory.h>
#include <common/mini-clist.h>
#include <common/standard.h>
#include <common/ticks.h>
#include <common/time.h>
#include <common/uri_auth.h>
#include <common/version.h>
#include <common/base64.h>

#include <types/applet.h>
#include <types/global.h>
#include <types/dns.h>

#include <proto/backend.h>
#include <proto/channel.h>
#include <proto/checks.h>
#include <proto/compression.h>
#include <proto/dumpstats.h>
#include <proto/fd.h>
#include <proto/freq_ctr.h>
#include <proto/frontend.h>
#include <proto/log.h>
#include <proto/pattern.h>
#include <proto/pipe.h>
#include <proto/listener.h>
#include <proto/map.h>
#include <proto/proto_http.h>
#include <proto/proto_uxst.h>
#include <proto/proxy.h>
#include <proto/sample.h>
#include <proto/session.h>
#include <proto/stream.h>
#include <proto/server.h>
#include <proto/raw_sock.h>
#include <proto/stream_interface.h>
#include <proto/task.h>

#ifdef USE_OPENSSL
#include <proto/ssl_sock.h>
#include <types/ssl_sock.h>
#endif

/* stats socket states */
enum {
	STAT_CLI_INIT = 0,   /* initial state, must leave to zero ! */
	STAT_CLI_END,        /* final state, let's close */
	STAT_CLI_GETREQ,     /* wait for a request */
	STAT_CLI_OUTPUT,     /* all states after this one are responses */
	STAT_CLI_PROMPT,     /* display the prompt (first output, same code) */
	STAT_CLI_PRINT,      /* display message in cli->msg */
	STAT_CLI_PRINT_FREE, /* display message in cli->msg. After the display, free the pointer */
	STAT_CLI_O_INFO,     /* dump info */
	STAT_CLI_O_SESS,     /* dump streams */
	STAT_CLI_O_ERR,      /* dump errors */
	STAT_CLI_O_TAB,      /* dump tables */
	STAT_CLI_O_CLR,      /* clear tables */
	STAT_CLI_O_SET,      /* set entries in tables */
	STAT_CLI_O_STAT,     /* dump stats */
	STAT_CLI_O_PATS,     /* list all pattern reference available */
	STAT_CLI_O_PAT,      /* list all entries of a pattern */
	STAT_CLI_O_MLOOK,    /* lookup a map entry */
	STAT_CLI_O_POOLS,    /* dump memory pools */
	STAT_CLI_O_TLSK,     /* list all TLS ticket keys references */
	STAT_CLI_O_TLSK_ENT, /* list all TLS ticket keys entries for a reference */
	STAT_CLI_O_RESOLVERS,/* dump a resolver's section nameservers counters */
	STAT_CLI_O_SERVERS_STATE, /* dump server state and changing information */
	STAT_CLI_O_BACKEND,  /* dump backend list */
	STAT_CLI_O_ENV,      /* dump environment */
};

/* Actions available for the stats admin forms */
enum {
	ST_ADM_ACTION_NONE = 0,

	/* enable/disable health checks */
	ST_ADM_ACTION_DHLTH,
	ST_ADM_ACTION_EHLTH,

	/* force health check status */
	ST_ADM_ACTION_HRUNN,
	ST_ADM_ACTION_HNOLB,
	ST_ADM_ACTION_HDOWN,

	/* enable/disable agent checks */
	ST_ADM_ACTION_DAGENT,
	ST_ADM_ACTION_EAGENT,

	/* force agent check status */
	ST_ADM_ACTION_ARUNN,
	ST_ADM_ACTION_ADOWN,

	/* set admin state */
	ST_ADM_ACTION_READY,
	ST_ADM_ACTION_DRAIN,
	ST_ADM_ACTION_MAINT,
	ST_ADM_ACTION_SHUTDOWN,
	/* these are the ancient actions, still available for compatibility */
	ST_ADM_ACTION_DISABLE,
	ST_ADM_ACTION_ENABLE,
	ST_ADM_ACTION_STOP,
	ST_ADM_ACTION_START,
};


/* These are the field names for each INF_* field position. Please pay attention
 * to always use the exact same name except that the strings for new names must
 * be lower case or CamelCase while the enum entries must be upper case.
 */
const char *info_field_names[INF_TOTAL_FIELDS] = {
	[INF_NAME]                           = "Name",
	[INF_VERSION]                        = "Version",
	[INF_RELEASE_DATE]                   = "Release_date",
	[INF_NBPROC]                         = "Nbproc",
	[INF_PROCESS_NUM]                    = "Process_num",
	[INF_PID]                            = "Pid",
	[INF_UPTIME]                         = "Uptime",
	[INF_UPTIME_SEC]                     = "Uptime_sec",
	[INF_MEMMAX_MB]                      = "Memmax_MB",
	[INF_POOL_ALLOC_MB]                  = "PoolAlloc_MB",
	[INF_POOL_USED_MB]                   = "PoolUsed_MB",
	[INF_POOL_FAILED]                    = "PoolFailed",
	[INF_ULIMIT_N]                       = "Ulimit-n",
	[INF_MAXSOCK]                        = "Maxsock",
	[INF_MAXCONN]                        = "Maxconn",
	[INF_HARD_MAXCONN]                   = "Hard_maxconn",
	[INF_CURR_CONN]                      = "CurrConns",
	[INF_CUM_CONN]                       = "CumConns",
	[INF_CUM_REQ]                        = "CumReq",
	[INF_MAX_SSL_CONNS]                  = "MaxSslConns",
	[INF_CURR_SSL_CONNS]                 = "CurrSslConns",
	[INF_CUM_SSL_CONNS]                  = "CumSslConns",
	[INF_MAXPIPES]                       = "Maxpipes",
	[INF_PIPES_USED]                     = "PipesUsed",
	[INF_PIPES_FREE]                     = "PipesFree",
	[INF_CONN_RATE]                      = "ConnRate",
	[INF_CONN_RATE_LIMIT]                = "ConnRateLimit",
	[INF_MAX_CONN_RATE]                  = "MaxConnRate",
	[INF_SESS_RATE]                      = "SessRate",
	[INF_SESS_RATE_LIMIT]                = "SessRateLimit",
	[INF_MAX_SESS_RATE]                  = "MaxSessRate",
	[INF_SSL_RATE]                       = "SslRate",
	[INF_SSL_RATE_LIMIT]                 = "SslRateLimit",
	[INF_MAX_SSL_RATE]                   = "MaxSslRate",
	[INF_SSL_FRONTEND_KEY_RATE]          = "SslFrontendKeyRate",
	[INF_SSL_FRONTEND_MAX_KEY_RATE]      = "SslFrontendMaxKeyRate",
	[INF_SSL_FRONTEND_SESSION_REUSE_PCT] = "SslFrontendSessionReuse_pct",
	[INF_SSL_BACKEND_KEY_RATE]           = "SslBackendKeyRate",
	[INF_SSL_BACKEND_MAX_KEY_RATE]       = "SslBackendMaxKeyRate",
	[INF_SSL_CACHE_LOOKUPS]              = "SslCacheLookups",
	[INF_SSL_CACHE_MISSES]               = "SslCacheMisses",
	[INF_COMPRESS_BPS_IN]                = "CompressBpsIn",
	[INF_COMPRESS_BPS_OUT]               = "CompressBpsOut",
	[INF_COMPRESS_BPS_RATE_LIM]          = "CompressBpsRateLim",
	[INF_ZLIB_MEM_USAGE]                 = "ZlibMemUsage",
	[INF_MAX_ZLIB_MEM_USAGE]             = "MaxZlibMemUsage",
	[INF_TASKS]                          = "Tasks",
	[INF_RUN_QUEUE]                      = "Run_queue",
	[INF_IDLE_PCT]                       = "Idle_pct",
	[INF_NODE]                           = "node",
	[INF_DESCRIPTION]                    = "description",
};

/* one line of stats */
static struct field info[INF_TOTAL_FIELDS];

/* These are the field names for each ST_F_* field position. Please pay attention
 * to always use the exact same name except that the strings must be lower case
 * while the enum entries must be upper case.
 */
const char *stat_field_names[ST_F_TOTAL_FIELDS] = {
	[ST_F_PXNAME]         = "pxname",
	[ST_F_SVNAME]         = "svname",
	[ST_F_QCUR]           = "qcur",
	[ST_F_QMAX]           = "qmax",
	[ST_F_SCUR]           = "scur",
	[ST_F_SMAX]           = "smax",
	[ST_F_SLIM]           = "slim",
	[ST_F_STOT]           = "stot",
	[ST_F_BIN]            = "bin",
	[ST_F_BOUT]           = "bout",
	[ST_F_DREQ]           = "dreq",
	[ST_F_DRESP]          = "dresp",
	[ST_F_EREQ]           = "ereq",
	[ST_F_ECON]           = "econ",
	[ST_F_ERESP]          = "eresp",
	[ST_F_WRETR]          = "wretr",
	[ST_F_WREDIS]         = "wredis",
	[ST_F_STATUS]         = "status",
	[ST_F_WEIGHT]         = "weight",
	[ST_F_ACT]            = "act",
	[ST_F_BCK]            = "bck",
	[ST_F_CHKFAIL]        = "chkfail",
	[ST_F_CHKDOWN]        = "chkdown",
	[ST_F_LASTCHG]        = "lastchg",
	[ST_F_DOWNTIME]       = "downtime",
	[ST_F_QLIMIT]         = "qlimit",
	[ST_F_PID]            = "pid",
	[ST_F_IID]            = "iid",
	[ST_F_SID]            = "sid",
	[ST_F_THROTTLE]       = "throttle",
	[ST_F_LBTOT]          = "lbtot",
	[ST_F_TRACKED]        = "tracked",
	[ST_F_TYPE]           = "type",
	[ST_F_RATE]           = "rate",
	[ST_F_RATE_LIM]       = "rate_lim",
	[ST_F_RATE_MAX]       = "rate_max",
	[ST_F_CHECK_STATUS]   = "check_status",
	[ST_F_CHECK_CODE]     = "check_code",
	[ST_F_CHECK_DURATION] = "check_duration",
	[ST_F_HRSP_1XX]       = "hrsp_1xx",
	[ST_F_HRSP_2XX]       = "hrsp_2xx",
	[ST_F_HRSP_3XX]       = "hrsp_3xx",
	[ST_F_HRSP_4XX]       = "hrsp_4xx",
	[ST_F_HRSP_5XX]       = "hrsp_5xx",
	[ST_F_HRSP_OTHER]     = "hrsp_other",
	[ST_F_HANAFAIL]       = "hanafail",
	[ST_F_REQ_RATE]       = "req_rate",
	[ST_F_REQ_RATE_MAX]   = "req_rate_max",
	[ST_F_REQ_TOT]        = "req_tot",
	[ST_F_CLI_ABRT]       = "cli_abrt",
	[ST_F_SRV_ABRT]       = "srv_abrt",
	[ST_F_COMP_IN]        = "comp_in",
	[ST_F_COMP_OUT]       = "comp_out",
	[ST_F_COMP_BYP]       = "comp_byp",
	[ST_F_COMP_RSP]       = "comp_rsp",
	[ST_F_LASTSESS]       = "lastsess",
	[ST_F_LAST_CHK]       = "last_chk",
	[ST_F_LAST_AGT]       = "last_agt",
	[ST_F_QTIME]          = "qtime",
	[ST_F_CTIME]          = "ctime",
	[ST_F_RTIME]          = "rtime",
	[ST_F_TTIME]          = "ttime",
	[ST_F_AGENT_STATUS]   = "agent_status",
	[ST_F_AGENT_CODE]     = "agent_code",
	[ST_F_AGENT_DURATION] = "agent_duration",
	[ST_F_CHECK_DESC]     = "check_desc",
	[ST_F_AGENT_DESC]     = "agent_desc",
	[ST_F_CHECK_RISE]     = "check_rise",
	[ST_F_CHECK_FALL]     = "check_fall",
	[ST_F_CHECK_HEALTH]   = "check_health",
	[ST_F_AGENT_RISE]     = "agent_rise",
	[ST_F_AGENT_FALL]     = "agent_fall",
	[ST_F_AGENT_HEALTH]   = "agent_health",
	[ST_F_ADDR]           = "addr",
	[ST_F_COOKIE]         = "cookie",
	[ST_F_MODE]           = "mode",
	[ST_F_ALGO]           = "algo",
	[ST_F_CONN_RATE]      = "conn_rate",
	[ST_F_CONN_RATE_MAX]  = "conn_rate_max",
	[ST_F_CONN_TOT]       = "conn_tot",
	[ST_F_INTERCEPTED]    = "intercepted",
};

/* one line of stats */
static struct field stats[ST_F_TOTAL_FIELDS];

static int stats_dump_backend_to_buffer(struct stream_interface *si);
static int stats_dump_env_to_buffer(struct stream_interface *si);
static int stats_dump_info_to_buffer(struct stream_interface *si);
static int stats_dump_servers_state_to_buffer(struct stream_interface *si);
static int stats_dump_pools_to_buffer(struct stream_interface *si);
static int stats_dump_full_sess_to_buffer(struct stream_interface *si, struct stream *sess);
static int stats_dump_sess_to_buffer(struct stream_interface *si);
static int stats_dump_errors_to_buffer(struct stream_interface *si);
static int stats_table_request(struct stream_interface *si, int show);
static int stats_dump_proxy_to_buffer(struct stream_interface *si, struct proxy *px, struct uri_auth *uri);
static int stats_dump_stat_to_buffer(struct stream_interface *si, struct uri_auth *uri);
static int stats_dump_resolvers_to_buffer(struct stream_interface *si);
static int stats_pats_list(struct stream_interface *si);
static int stats_pat_list(struct stream_interface *si);
static int stats_map_lookup(struct stream_interface *si);
#if (defined SSL_CTRL_SET_TLSEXT_TICKET_KEY_CB && TLS_TICKETS_NO > 0)
static int stats_tlskeys_list(struct stream_interface *si);
#endif
static void cli_release_handler(struct appctx *appctx);

static int dump_servers_state(struct stream_interface *si, struct chunk *buf);

/*
 * cli_io_handler()
 *     -> stats_dump_sess_to_buffer()     // "show sess"
 *     -> stats_dump_errors_to_buffer()   // "show errors"
 *     -> stats_dump_info_to_buffer()     // "show info"
 *     -> stats_dump_backend_to_buffer()  // "show backend"
 *     -> stats_dump_servers_state_to_buffer() // "show servers state [<backend name>]"
 *     -> stats_dump_stat_to_buffer()     // "show stat"
 *        -> stats_dump_resolvers_to_buffer() // "show stat resolvers <id>"
 *        -> stats_dump_csv_header()
 *        -> stats_dump_proxy_to_buffer()
 *           -> stats_dump_fe_stats()
 *           -> stats_dump_li_stats()
 *           -> stats_dump_sv_stats()
 *           -> stats_dump_be_stats()
 *
 * http_stats_io_handler()
 *     -> stats_dump_stat_to_buffer()     // same as above, but used for CSV or HTML
 *        -> stats_dump_csv_header()      // emits the CSV headers (same as above)
 *        -> stats_dump_html_head()       // emits the HTML headers
 *        -> stats_dump_html_info()       // emits the equivalent of "show info" at the top
 *        -> stats_dump_proxy_to_buffer() // same as above, valid for CSV and HTML
 *           -> stats_dump_html_px_hdr()
 *           -> stats_dump_fe_stats()
 *           -> stats_dump_li_stats()
 *           -> stats_dump_sv_stats()
 *           -> stats_dump_be_stats()
 *           -> stats_dump_html_px_end()
 *        -> stats_dump_html_end()       // emits HTML trailer
 */

static struct applet cli_applet;

static const char stats_sock_usage_msg[] =
	"Unknown command. Please enter one of the following commands only :\n"
	"  clear counters : clear max statistics counters (add 'all' for all counters)\n"
	"  clear table    : remove an entry from a table\n"
	"  help           : this message\n"
	"  prompt         : toggle interactive mode with prompt\n"
	"  quit           : disconnect\n"
	"  show backend   : list backends in the current running config\n"
	"  show env [var] : dump environment variables known to the process\n"
	"  show info      : report information about the running process\n"
	"  show pools     : report information about the memory pools usage\n"
	"  show stat      : report counters for each proxy and server\n"
	"  show stat resolvers [id]: dumps counters from all resolvers section and\n"
	"                            associated name servers\n"
	"  show errors    : report last request and response errors for each proxy\n"
	"  show sess [id] : report the list of current sessions or dump this session\n"
	"  show table [id]: report table usage stats or dump this table's contents\n"
	"  show servers state [id]: dump volatile server information (for backend <id>)\n"
	"  get weight     : report a server's current weight\n"
	"  set weight     : change a server's weight\n"
	"  set server     : change a server's state, weight or address\n"
	"  set table [id] : update or create a table entry's data\n"
	"  set timeout    : change a timeout setting\n"
	"  set maxconn    : change a maxconn setting\n"
	"  set rate-limit : change a rate limiting value\n"
	"  disable        : put a server or frontend in maintenance mode\n"
	"  enable         : re-enable a server or frontend which is in maintenance mode\n"
	"  shutdown       : kill a session or a frontend (eg:to release listening ports)\n"
	"  show acl [id]  : report available acls or dump an acl's contents\n"
	"  get acl        : reports the patterns matching a sample for an ACL\n"
	"  add acl        : add acl entry\n"
	"  del acl        : delete acl entry\n"
	"  clear acl <id> : clear the content of this acl\n"
	"  show map [id]  : report available maps or dump a map's contents\n"
	"  get map        : reports the keys and values matching a sample for a map\n"
	"  set map        : modify map entry\n"
	"  add map        : add map entry\n"
	"  del map        : delete map entry\n"
	"  clear map <id> : clear the content of this map\n"
	"  set ssl <stmt> : set statement for ssl\n"
#if (defined SSL_CTRL_SET_TLSEXT_TICKET_KEY_CB && TLS_TICKETS_NO > 0)
	"  show tls-keys [id|*]: show tls keys references or dump tls ticket keys when id specified\n"
#endif
	"";

static const char stats_permission_denied_msg[] =
	"Permission denied\n"
	"";

/* data transmission states for the stats responses */
enum {
	STAT_ST_INIT = 0,
	STAT_ST_HEAD,
	STAT_ST_INFO,
	STAT_ST_LIST,
	STAT_ST_END,
	STAT_ST_FIN,
};

/* data transmission states for the stats responses inside a proxy */
enum {
	STAT_PX_ST_INIT = 0,
	STAT_PX_ST_TH,
	STAT_PX_ST_FE,
	STAT_PX_ST_LI,
	STAT_PX_ST_SV,
	STAT_PX_ST_BE,
	STAT_PX_ST_END,
	STAT_PX_ST_FIN,
};

extern const char *stat_status_codes[];

/* allocate a new stats frontend named <name>, and return it
 * (or NULL in case of lack of memory).
 */
static struct proxy *alloc_stats_fe(const char *name, const char *file, int line)
{
	struct proxy *fe;

	fe = calloc(1, sizeof(*fe));
	if (!fe)
		return NULL;

	init_new_proxy(fe);
	fe->next = proxy;
	proxy = fe;
	fe->last_change = now.tv_sec;
	fe->id = strdup("GLOBAL");
	fe->cap = PR_CAP_FE;
	fe->maxconn = 10;                 /* default to 10 concurrent connections */
	fe->timeout.client = MS_TO_TICKS(10000); /* default timeout of 10 seconds */
	fe->conf.file = strdup(file);
	fe->conf.line = line;
	fe->accept = frontend_accept;
	fe->default_target = &cli_applet.obj_type;

	/* the stats frontend is the only one able to assign ID #0 */
	fe->conf.id.key = fe->uuid = 0;
	eb32_insert(&used_proxy_id, &fe->conf.id);
	return fe;
}

/* This function parses a "stats" statement in the "global" section. It returns
 * -1 if there is any error, otherwise zero. If it returns -1, it will write an
 * error message into the <err> buffer which will be preallocated. The trailing
 * '\n' must not be written. The function must be called with <args> pointing to
 * the first word after "stats".
 */
static int stats_parse_global(char **args, int section_type, struct proxy *curpx,
                              struct proxy *defpx, const char *file, int line,
                              char **err)
{
	struct bind_conf *bind_conf;
	struct listener *l;

	if (!strcmp(args[1], "socket")) {
		int cur_arg;

		if (*args[2] == 0) {
			memprintf(err, "'%s %s' in global section expects an address or a path to a UNIX socket", args[0], args[1]);
			return -1;
		}

		if (!global.stats_fe) {
			if ((global.stats_fe = alloc_stats_fe("GLOBAL", file, line)) == NULL) {
				memprintf(err, "'%s %s' : out of memory trying to allocate a frontend", args[0], args[1]);
				return -1;
			}
		}

		bind_conf = bind_conf_alloc(&global.stats_fe->conf.bind, file, line, args[2]);
		bind_conf->level = ACCESS_LVL_OPER; /* default access level */

		if (!str2listener(args[2], global.stats_fe, bind_conf, file, line, err)) {
			memprintf(err, "parsing [%s:%d] : '%s %s' : %s\n",
			          file, line, args[0], args[1], err && *err ? *err : "error");
			return -1;
		}

		cur_arg = 3;
		while (*args[cur_arg]) {
			static int bind_dumped;
			struct bind_kw *kw;

			kw = bind_find_kw(args[cur_arg]);
			if (kw) {
				if (!kw->parse) {
					memprintf(err, "'%s %s' : '%s' option is not implemented in this version (check build options).",
						  args[0], args[1], args[cur_arg]);
					return -1;
				}

				if (kw->parse(args, cur_arg, global.stats_fe, bind_conf, err) != 0) {
					if (err && *err)
						memprintf(err, "'%s %s' : '%s'", args[0], args[1], *err);
					else
						memprintf(err, "'%s %s' : error encountered while processing '%s'",
						          args[0], args[1], args[cur_arg]);
					return -1;
				}

				cur_arg += 1 + kw->skip;
				continue;
			}

			if (!bind_dumped) {
				bind_dump_kws(err);
				indent_msg(err, 4);
				bind_dumped = 1;
			}

			memprintf(err, "'%s %s' : unknown keyword '%s'.%s%s",
			          args[0], args[1], args[cur_arg],
			          err && *err ? " Registered keywords :" : "", err && *err ? *err : "");
			return -1;
		}

		list_for_each_entry(l, &bind_conf->listeners, by_bind) {
			l->maxconn = global.stats_fe->maxconn;
			l->backlog = global.stats_fe->backlog;
			l->accept = session_accept_fd;
			l->handler = process_stream;
			l->default_target = global.stats_fe->default_target;
			l->options |= LI_O_UNLIMITED; /* don't make the peers subject to global limits */
			l->nice = -64;  /* we want to boost priority for local stats */
			global.maxsock += l->maxconn;
		}
	}
	else if (!strcmp(args[1], "timeout")) {
		unsigned timeout;
		const char *res = parse_time_err(args[2], &timeout, TIME_UNIT_MS);

		if (res) {
			memprintf(err, "'%s %s' : unexpected character '%c'", args[0], args[1], *res);
			return -1;
		}

		if (!timeout) {
			memprintf(err, "'%s %s' expects a positive value", args[0], args[1]);
			return -1;
		}
		if (!global.stats_fe) {
			if ((global.stats_fe = alloc_stats_fe("GLOBAL", file, line)) == NULL) {
				memprintf(err, "'%s %s' : out of memory trying to allocate a frontend", args[0], args[1]);
				return -1;
			}
		}
		global.stats_fe->timeout.client = MS_TO_TICKS(timeout);
	}
	else if (!strcmp(args[1], "maxconn")) {
		int maxconn = atol(args[2]);

		if (maxconn <= 0) {
			memprintf(err, "'%s %s' expects a positive value", args[0], args[1]);
			return -1;
		}

		if (!global.stats_fe) {
			if ((global.stats_fe = alloc_stats_fe("GLOBAL", file, line)) == NULL) {
				memprintf(err, "'%s %s' : out of memory trying to allocate a frontend", args[0], args[1]);
				return -1;
			}
		}
		global.stats_fe->maxconn = maxconn;
	}
	else if (!strcmp(args[1], "bind-process")) {  /* enable the socket only on some processes */
		int cur_arg = 2;
		unsigned long set = 0;

		if (!global.stats_fe) {
			if ((global.stats_fe = alloc_stats_fe("GLOBAL", file, line)) == NULL) {
				memprintf(err, "'%s %s' : out of memory trying to allocate a frontend", args[0], args[1]);
				return -1;
			}
		}

		while (*args[cur_arg]) {
			unsigned int low, high;

			if (strcmp(args[cur_arg], "all") == 0) {
				set = 0;
				break;
			}
			else if (strcmp(args[cur_arg], "odd") == 0) {
				set |= ~0UL/3UL; /* 0x555....555 */
			}
			else if (strcmp(args[cur_arg], "even") == 0) {
				set |= (~0UL/3UL) << 1; /* 0xAAA...AAA */
			}
			else if (isdigit((int)*args[cur_arg])) {
				char *dash = strchr(args[cur_arg], '-');

				low = high = str2uic(args[cur_arg]);
				if (dash)
					high = str2uic(dash + 1);

				if (high < low) {
					unsigned int swap = low;
					low = high;
					high = swap;
				}

				if (low < 1 || high > LONGBITS) {
					memprintf(err, "'%s %s' supports process numbers from 1 to %d.\n",
					          args[0], args[1], LONGBITS);
					return -1;
				}
				while (low <= high)
					set |= 1UL << (low++ - 1);
			}
			else {
				memprintf(err,
				          "'%s %s' expects 'all', 'odd', 'even', or a list of process ranges with numbers from 1 to %d.\n",
				          args[0], args[1], LONGBITS);
				return -1;
			}
			cur_arg++;
		}
		global.stats_fe->bind_proc = set;
	}
	else {
		memprintf(err, "'%s' only supports 'socket', 'maxconn', 'bind-process' and 'timeout' (got '%s')", args[0], args[1]);
		return -1;
	}
	return 0;
}

/* Dumps the stats CSV header to the trash buffer which. The caller is responsible
 * for clearing it if needed.
 * NOTE: Some tools happen to rely on the field position instead of its name,
 *       so please only append new fields at the end, never in the middle.
 */
static void stats_dump_csv_header()
{
	int field;

	chunk_appendf(&trash, "# ");
	for (field = 0; field < ST_F_TOTAL_FIELDS; field++)
		chunk_appendf(&trash, "%s,", stat_field_names[field]);

	chunk_appendf(&trash, "\n");
}

/* print a string of text buffer to <out>. The format is :
 * Non-printable chars \t, \n, \r and \e are * encoded in C format.
 * Other non-printable chars are encoded "\xHH". Space and '\' are also escaped.
 * Print stopped if null char or <bsize> is reached, or if no more place in the chunk.
 */
static int dump_text(struct chunk *out, const char *buf, int bsize)
{
	unsigned char c;
	int ptr = 0;

	while (buf[ptr] && ptr < bsize) {
		c = buf[ptr];
		if (isprint(c) && isascii(c) && c != '\\' && c != ' ') {
			if (out->len > out->size - 1)
				break;
			out->str[out->len++] = c;
		}
		else if (c == '\t' || c == '\n' || c == '\r' || c == '\e' || c == '\\' || c == ' ') {
			if (out->len > out->size - 2)
				break;
			out->str[out->len++] = '\\';
			switch (c) {
			case ' ': c = ' '; break;
			case '\t': c = 't'; break;
			case '\n': c = 'n'; break;
			case '\r': c = 'r'; break;
			case '\e': c = 'e'; break;
			case '\\': c = '\\'; break;
			}
			out->str[out->len++] = c;
		}
		else {
			if (out->len > out->size - 4)
				break;
			out->str[out->len++] = '\\';
			out->str[out->len++] = 'x';
			out->str[out->len++] = hextab[(c >> 4) & 0xF];
			out->str[out->len++] = hextab[c & 0xF];
		}
		ptr++;
	}

	return ptr;
}

/* print a buffer in hexa.
 * Print stopped if <bsize> is reached, or if no more place in the chunk.
 */
static int dump_binary(struct chunk *out, const char *buf, int bsize)
{
	unsigned char c;
	int ptr = 0;

	while (ptr < bsize) {
		c = buf[ptr];

		if (out->len > out->size - 2)
			break;
		out->str[out->len++] = hextab[(c >> 4) & 0xF];
		out->str[out->len++] = hextab[c & 0xF];

		ptr++;
	}
	return ptr;
}

/* Dump the status of a table to a stream interface's
 * read buffer. It returns 0 if the output buffer is full
 * and needs to be called again, otherwise non-zero.
 */
static int stats_dump_table_head_to_buffer(struct chunk *msg, struct stream_interface *si,
					   struct proxy *proxy, struct proxy *target)
{
	struct stream *s = si_strm(si);

	chunk_appendf(msg, "# table: %s, type: %s, size:%d, used:%d\n",
		     proxy->id, stktable_types[proxy->table.type].kw, proxy->table.size, proxy->table.current);

	/* any other information should be dumped here */

	if (target && strm_li(s)->bind_conf->level < ACCESS_LVL_OPER)
		chunk_appendf(msg, "# contents not dumped due to insufficient privileges\n");

	if (bi_putchk(si_ic(si), msg) == -1) {
		si_applet_cant_put(si);
		return 0;
	}

	return 1;
}

/* Dump the a table entry to a stream interface's
 * read buffer. It returns 0 if the output buffer is full
 * and needs to be called again, otherwise non-zero.
 */
static int stats_dump_table_entry_to_buffer(struct chunk *msg, struct stream_interface *si,
					    struct proxy *proxy, struct stksess *entry)
{
	int dt;

	chunk_appendf(msg, "%p:", entry);

	if (proxy->table.type == SMP_T_IPV4) {
		char addr[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, (const void *)&entry->key.key, addr, sizeof(addr));
		chunk_appendf(msg, " key=%s", addr);
	}
	else if (proxy->table.type == SMP_T_IPV6) {
		char addr[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, (const void *)&entry->key.key, addr, sizeof(addr));
		chunk_appendf(msg, " key=%s", addr);
	}
	else if (proxy->table.type == SMP_T_SINT) {
		chunk_appendf(msg, " key=%u", *(unsigned int *)entry->key.key);
	}
	else if (proxy->table.type == SMP_T_STR) {
		chunk_appendf(msg, " key=");
		dump_text(msg, (const char *)entry->key.key, proxy->table.key_size);
	}
	else {
		chunk_appendf(msg, " key=");
		dump_binary(msg, (const char *)entry->key.key, proxy->table.key_size);
	}

	chunk_appendf(msg, " use=%d exp=%d", entry->ref_cnt - 1, tick_remain(now_ms, entry->expire));

	for (dt = 0; dt < STKTABLE_DATA_TYPES; dt++) {
		void *ptr;

		if (proxy->table.data_ofs[dt] == 0)
			continue;
		if (stktable_data_types[dt].arg_type == ARG_T_DELAY)
			chunk_appendf(msg, " %s(%d)=", stktable_data_types[dt].name, proxy->table.data_arg[dt].u);
		else
			chunk_appendf(msg, " %s=", stktable_data_types[dt].name);

		ptr = stktable_data_ptr(&proxy->table, entry, dt);
		switch (stktable_data_types[dt].std_type) {
		case STD_T_SINT:
			chunk_appendf(msg, "%d", stktable_data_cast(ptr, std_t_sint));
			break;
		case STD_T_UINT:
			chunk_appendf(msg, "%u", stktable_data_cast(ptr, std_t_uint));
			break;
		case STD_T_ULL:
			chunk_appendf(msg, "%lld", stktable_data_cast(ptr, std_t_ull));
			break;
		case STD_T_FRQP:
			chunk_appendf(msg, "%d",
				     read_freq_ctr_period(&stktable_data_cast(ptr, std_t_frqp),
							  proxy->table.data_arg[dt].u));
			break;
		}
	}
	chunk_appendf(msg, "\n");

	if (bi_putchk(si_ic(si), msg) == -1) {
		si_applet_cant_put(si);
		return 0;
	}

	return 1;
}

static void stats_sock_table_key_request(struct stream_interface *si, char **args, int action)
{
	struct stream *s = si_strm(si);
	struct appctx *appctx = __objt_appctx(si->end);
	struct proxy *px = appctx->ctx.table.target;
	struct stksess *ts;
	uint32_t uint32_key;
	unsigned char ip6_key[sizeof(struct in6_addr)];
	long long value;
	int data_type;
	int cur_arg;
	void *ptr;
	struct freq_ctr_period *frqp;

	appctx->st0 = STAT_CLI_OUTPUT;

	if (!*args[4]) {
		appctx->ctx.cli.msg = "Key value expected\n";
		appctx->st0 = STAT_CLI_PRINT;
		return;
	}

	switch (px->table.type) {
	case SMP_T_IPV4:
		uint32_key = htonl(inetaddr_host(args[4]));
		static_table_key->key = &uint32_key;
		break;
	case SMP_T_IPV6:
		inet_pton(AF_INET6, args[4], ip6_key);
		static_table_key->key = &ip6_key;
		break;
	case SMP_T_SINT:
		{
			char *endptr;
			unsigned long val;
			errno = 0;
			val = strtoul(args[4], &endptr, 10);
			if ((errno == ERANGE && val == ULONG_MAX) ||
			    (errno != 0 && val == 0) || endptr == args[4] ||
			    val > 0xffffffff) {
				appctx->ctx.cli.msg = "Invalid key\n";
				appctx->st0 = STAT_CLI_PRINT;
				return;
			}
			uint32_key = (uint32_t) val;
			static_table_key->key = &uint32_key;
			break;
		}
		break;
	case SMP_T_STR:
		static_table_key->key = args[4];
		static_table_key->key_len = strlen(args[4]);
		break;
	default:
		switch (action) {
		case STAT_CLI_O_TAB:
			appctx->ctx.cli.msg = "Showing keys from tables of type other than ip, ipv6, string and integer is not supported\n";
			break;
		case STAT_CLI_O_CLR:
			appctx->ctx.cli.msg = "Removing keys from ip tables of type other than ip, ipv6, string and integer is not supported\n";
			break;
		default:
			appctx->ctx.cli.msg = "Unknown action\n";
			break;
		}
		appctx->st0 = STAT_CLI_PRINT;
		return;
	}

	/* check permissions */
	if (strm_li(s)->bind_conf->level < ACCESS_LVL_OPER) {
		appctx->ctx.cli.msg = stats_permission_denied_msg;
		appctx->st0 = STAT_CLI_PRINT;
		return;
	}

	ts = stktable_lookup_key(&px->table, static_table_key);

	switch (action) {
	case STAT_CLI_O_TAB:
		if (!ts)
			return;
		chunk_reset(&trash);
		if (!stats_dump_table_head_to_buffer(&trash, si, px, px))
			return;
		stats_dump_table_entry_to_buffer(&trash, si, px, ts);
		return;

	case STAT_CLI_O_CLR:
		if (!ts)
			return;
		if (ts->ref_cnt) {
			/* don't delete an entry which is currently referenced */
			appctx->ctx.cli.msg = "Entry currently in use, cannot remove\n";
			appctx->st0 = STAT_CLI_PRINT;
			return;
		}
		stksess_kill(&px->table, ts);
		break;

	case STAT_CLI_O_SET:
		if (ts)
			stktable_touch(&px->table, ts, 1);
		else {
			ts = stksess_new(&px->table, static_table_key);
			if (!ts) {
				/* don't delete an entry which is currently referenced */
				appctx->ctx.cli.msg = "Unable to allocate a new entry\n";
				appctx->st0 = STAT_CLI_PRINT;
				return;
			}
			stktable_store(&px->table, ts, 1);
		}

		for (cur_arg = 5; *args[cur_arg]; cur_arg += 2) {
			if (strncmp(args[cur_arg], "data.", 5) != 0) {
				appctx->ctx.cli.msg = "\"data.<type>\" followed by a value expected\n";
				appctx->st0 = STAT_CLI_PRINT;
				return;
			}

			data_type = stktable_get_data_type(args[cur_arg] + 5);
			if (data_type < 0) {
				appctx->ctx.cli.msg = "Unknown data type\n";
				appctx->st0 = STAT_CLI_PRINT;
				return;
			}

			if (!px->table.data_ofs[data_type]) {
				appctx->ctx.cli.msg = "Data type not stored in this table\n";
				appctx->st0 = STAT_CLI_PRINT;
				return;
			}

			if (!*args[cur_arg+1] || strl2llrc(args[cur_arg+1], strlen(args[cur_arg+1]), &value) != 0) {
				appctx->ctx.cli.msg = "Require a valid integer value to store\n";
				appctx->st0 = STAT_CLI_PRINT;
				return;
			}

			ptr = stktable_data_ptr(&px->table, ts, data_type);

			switch (stktable_data_types[data_type].std_type) {
			case STD_T_SINT:
				stktable_data_cast(ptr, std_t_sint) = value;
				break;
			case STD_T_UINT:
				stktable_data_cast(ptr, std_t_uint) = value;
				break;
			case STD_T_ULL:
				stktable_data_cast(ptr, std_t_ull) = value;
				break;
			case STD_T_FRQP:
				/* We set both the current and previous values. That way
				 * the reported frequency is stable during all the period
				 * then slowly fades out. This allows external tools to
				 * push measures without having to update them too often.
				 */
				frqp = &stktable_data_cast(ptr, std_t_frqp);
				frqp->curr_tick = now_ms;
				frqp->prev_ctr = 0;
				frqp->curr_ctr = value;
				break;
			}
		}
		break;

	default:
		appctx->ctx.cli.msg = "Unknown action\n";
		appctx->st0 = STAT_CLI_PRINT;
		break;
	}
}

static void stats_sock_table_data_request(struct stream_interface *si, char **args, int action)
{
	struct appctx *appctx = __objt_appctx(si->end);

	if (action != STAT_CLI_O_TAB && action != STAT_CLI_O_CLR) {
		appctx->ctx.cli.msg = "content-based lookup is only supported with the \"show\" and \"clear\" actions";
		appctx->st0 = STAT_CLI_PRINT;
		return;
	}

	/* condition on stored data value */
	appctx->ctx.table.data_type = stktable_get_data_type(args[3] + 5);
	if (appctx->ctx.table.data_type < 0) {
		appctx->ctx.cli.msg = "Unknown data type\n";
		appctx->st0 = STAT_CLI_PRINT;
		return;
	}

	if (!((struct proxy *)appctx->ctx.table.target)->table.data_ofs[appctx->ctx.table.data_type]) {
		appctx->ctx.cli.msg = "Data type not stored in this table\n";
		appctx->st0 = STAT_CLI_PRINT;
		return;
	}

	appctx->ctx.table.data_op = get_std_op(args[4]);
	if (appctx->ctx.table.data_op < 0) {
		appctx->ctx.cli.msg = "Require and operator among \"eq\", \"ne\", \"le\", \"ge\", \"lt\", \"gt\"\n";
		appctx->st0 = STAT_CLI_PRINT;
		return;
	}

	if (!*args[5] || strl2llrc(args[5], strlen(args[5]), &appctx->ctx.table.value) != 0) {
		appctx->ctx.cli.msg = "Require a valid integer value to compare against\n";
		appctx->st0 = STAT_CLI_PRINT;
		return;
	}
}

static void stats_sock_table_request(struct stream_interface *si, char **args, int action)
{
	struct appctx *appctx = __objt_appctx(si->end);

	appctx->ctx.table.data_type = -1;
	appctx->st2 = STAT_ST_INIT;
	appctx->ctx.table.target = NULL;
	appctx->ctx.table.proxy = NULL;
	appctx->ctx.table.entry = NULL;
	appctx->st0 = action;

	if (*args[2]) {
		appctx->ctx.table.target = proxy_tbl_by_name(args[2]);
		if (!appctx->ctx.table.target) {
			appctx->ctx.cli.msg = "No such table\n";
			appctx->st0 = STAT_CLI_PRINT;
			return;
		}
	}
	else {
		if (action != STAT_CLI_O_TAB)
			goto err_args;
		return;
	}

	if (strcmp(args[3], "key") == 0)
		stats_sock_table_key_request(si, args, action);
	else if (strncmp(args[3], "data.", 5) == 0)
		stats_sock_table_data_request(si, args, action);
	else if (*args[3])
		goto err_args;

	return;

err_args:
	switch (action) {
	case STAT_CLI_O_TAB:
		appctx->ctx.cli.msg = "Optional argument only supports \"data.<store_data_type>\" <operator> <value> and key <key>\n";
		break;
	case STAT_CLI_O_CLR:
		appctx->ctx.cli.msg = "Required arguments: <table> \"data.<store_data_type>\" <operator> <value> or <table> key <key>\n";
		break;
	default:
		appctx->ctx.cli.msg = "Unknown action\n";
		break;
	}
	appctx->st0 = STAT_CLI_PRINT;
}

/* Expects to find a frontend named <arg> and returns it, otherwise displays various
 * adequate error messages and returns NULL. This function also expects the stream
 * level to be admin.
 */
static struct proxy *expect_frontend_admin(struct stream *s, struct stream_interface *si, const char *arg)
{
	struct appctx *appctx = __objt_appctx(si->end);
	struct proxy *px;

	if (strm_li(s)->bind_conf->level < ACCESS_LVL_ADMIN) {
		appctx->ctx.cli.msg = stats_permission_denied_msg;
		appctx->st0 = STAT_CLI_PRINT;
		return NULL;
	}

	if (!*arg) {
		appctx->ctx.cli.msg = "A frontend name is expected.\n";
		appctx->st0 = STAT_CLI_PRINT;
		return NULL;
	}

	px = proxy_fe_by_name(arg);
	if (!px) {
		appctx->ctx.cli.msg = "No such frontend.\n";
		appctx->st0 = STAT_CLI_PRINT;
		return NULL;
	}
	return px;
}

/* Expects to find a backend and a server in <arg> under the form <backend>/<server>,
 * and returns the pointer to the server. Otherwise, display adequate error messages
 * and returns NULL. This function also expects the stream level to be admin. Note:
 * the <arg> is modified to remove the '/'.
 */
static struct server *expect_server_admin(struct stream *s, struct stream_interface *si, char *arg)
{
	struct appctx *appctx = __objt_appctx(si->end);
	struct proxy *px;
	struct server *sv;
	char *line;

	if (strm_li(s)->bind_conf->level < ACCESS_LVL_ADMIN) {
		appctx->ctx.cli.msg = stats_permission_denied_msg;
		appctx->st0 = STAT_CLI_PRINT;
		return NULL;
	}

	/* split "backend/server" and make <line> point to server */
	for (line = arg; *line; line++)
		if (*line == '/') {
			*line++ = '\0';
			break;
		}

	if (!*line || !*arg) {
		appctx->ctx.cli.msg = "Require 'backend/server'.\n";
		appctx->st0 = STAT_CLI_PRINT;
		return NULL;
	}

	if (!get_backend_server(arg, line, &px, &sv)) {
		appctx->ctx.cli.msg = px ? "No such server.\n" : "No such backend.\n";
		appctx->st0 = STAT_CLI_PRINT;
		return NULL;
	}

	if (px->state == PR_STSTOPPED) {
		appctx->ctx.cli.msg = "Proxy is disabled.\n";
		appctx->st0 = STAT_CLI_PRINT;
		return NULL;
	}

	return sv;
}

/* This function is used with TLS ticket keys management. It permits to browse
 * each reference. The variable <getnext> must contain the current node,
 * <end> point to the root node.
 */
#if (defined SSL_CTRL_SET_TLSEXT_TICKET_KEY_CB && TLS_TICKETS_NO > 0)
static inline
struct tls_keys_ref *tlskeys_list_get_next(struct tls_keys_ref *getnext, struct list *end)
{
	struct tls_keys_ref *ref = getnext;

	while (1) {

		/* Get next list entry. */
		ref = LIST_NEXT(&ref->list, struct tls_keys_ref *, list);

		/* If the entry is the last of the list, return NULL. */
		if (&ref->list == end)
			return NULL;

		return ref;
	}
}

static inline
struct tls_keys_ref *tlskeys_ref_lookup_ref(const char *reference)
{
	int id;
	char *error;

	/* If the reference starts by a '#', this is numeric id. */
	if (reference[0] == '#') {
		/* Try to convert the numeric id. If the conversion fails, the lookup fails. */
		id = strtol(reference + 1, &error, 10);
		if (*error != '\0')
			return NULL;

		/* Perform the unique id lookup. */
		return tlskeys_ref_lookupid(id);
	}

	/* Perform the string lookup. */
	return tlskeys_ref_lookup(reference);
}
#endif

/* This function is used with map and acl management. It permits to browse
 * each reference. The variable <getnext> must contain the current node,
 * <end> point to the root node and the <flags> permit to filter required
 * nodes.
 */
static inline
struct pat_ref *pat_list_get_next(struct pat_ref *getnext, struct list *end,
                                  unsigned int flags)
{
	struct pat_ref *ref = getnext;

	while (1) {

		/* Get next list entry. */
		ref = LIST_NEXT(&ref->list, struct pat_ref *, list);

		/* If the entry is the last of the list, return NULL. */
		if (&ref->list == end)
			return NULL;

		/* If the entry match the flag, return it. */
		if (ref->flags & flags)
			return ref;
	}
}

static inline
struct pat_ref *pat_ref_lookup_ref(const char *reference)
{
	int id;
	char *error;

	/* If the reference starts by a '#', this is numeric id. */
	if (reference[0] == '#') {
		/* Try to convert the numeric id. If the conversion fails, the lookup fails. */
		id = strtol(reference + 1, &error, 10);
		if (*error != '\0')
			return NULL;

		/* Perform the unique id lookup. */
		return pat_ref_lookupid(id);
	}

	/* Perform the string lookup. */
	return pat_ref_lookup(reference);
}

/* This function is used with map and acl management. It permits to browse
 * each reference.
 */
static inline
struct pattern_expr *pat_expr_get_next(struct pattern_expr *getnext, struct list *end)
{
	struct pattern_expr *expr;
	expr = LIST_NEXT(&getnext->list, struct pattern_expr *, list);
	if (&expr->list == end)
		return NULL;
	return expr;
}

/* Processes the stats interpreter on the statistics socket. This function is
 * called from an applet running in a stream interface. The function returns 1
 * if the request was understood, otherwise zero. It sets appctx->st0 to a value
 * designating the function which will have to process the request, which can
 * also be the print function to display the return message set into cli.msg.
 */
static int stats_sock_parse_request(struct stream_interface *si, char *line)
{
	struct stream *s = si_strm(si);
	struct appctx *appctx = __objt_appctx(si->end);
	char *args[MAX_STATS_ARGS + 1];
	int arg;
	int i, j;

	while (isspace((unsigned char)*line))
		line++;

	arg = 0;
	args[arg] = line;

	while (*line && arg < MAX_STATS_ARGS) {
		if (*line == '\\') {
			line++;
			if (*line == '\0')
				break;
		}
		else if (isspace((unsigned char)*line)) {
			*line++ = '\0';

			while (isspace((unsigned char)*line))
				line++;

			args[++arg] = line;
			continue;
		}

		line++;
	}

	while (++arg <= MAX_STATS_ARGS)
		args[arg] = line;

	/* remove \ */
	arg = 0;
	while (*args[arg] != '\0') {
		j = 0;
		for (i=0; args[arg][i] != '\0'; i++) {
			if (args[arg][i] == '\\')
				continue;
			args[arg][j] = args[arg][i];
			j++;
		}
		args[arg][j] = '\0';
		arg++;
	}

	appctx->ctx.stats.scope_str = 0;
	appctx->ctx.stats.scope_len = 0;
	appctx->ctx.stats.flags = 0;
	if (strcmp(args[0], "show") == 0) {
		if (strcmp(args[1], "backend") == 0) {
			appctx->ctx.be.px = NULL;
			appctx->st2 = STAT_ST_INIT;
			appctx->st0 = STAT_CLI_O_BACKEND;
		}
		else if (strcmp(args[1], "env") == 0) {
			extern char **environ;

			if (strm_li(s)->bind_conf->level < ACCESS_LVL_OPER) {
				appctx->ctx.cli.msg = stats_permission_denied_msg;
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}
			appctx->ctx.env.var = environ;
			appctx->st2 = STAT_ST_INIT;
			appctx->st0 = STAT_CLI_O_ENV; // stats_dump_env_to_buffer

			if (*args[2]) {
				int len = strlen(args[2]);

				for (; *appctx->ctx.env.var; appctx->ctx.env.var++) {
					if (strncmp(*appctx->ctx.env.var, args[2], len) == 0 &&
					    (*appctx->ctx.env.var)[len] == '=')
						break;
				}
				if (!*appctx->ctx.env.var) {
					appctx->ctx.cli.msg = "Variable not found\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
				appctx->st2 = STAT_ST_END;
			}
		}
		else if (strcmp(args[1], "stat") == 0) {
			if (strcmp(args[2], "resolvers") == 0) {
				struct dns_resolvers *presolvers;

				if (*args[3]) {
					appctx->ctx.resolvers.ptr = NULL;
					list_for_each_entry(presolvers, &dns_resolvers, list) {
						if (strcmp(presolvers->id, args[3]) == 0) {
							appctx->ctx.resolvers.ptr = presolvers;
							break;
						}
					}
					if (appctx->ctx.resolvers.ptr == NULL) {
						appctx->ctx.cli.msg = "Can't find that resolvers section\n";
						appctx->st0 = STAT_CLI_PRINT;
						return 1;
					}
				}

				appctx->st2 = STAT_ST_INIT;
				appctx->st0 = STAT_CLI_O_RESOLVERS;
				return 1;
			}
			else if (*args[2] && *args[3] && *args[4]) {
				appctx->ctx.stats.flags |= STAT_BOUND;
				appctx->ctx.stats.iid = atoi(args[2]);
				appctx->ctx.stats.type = atoi(args[3]);
				appctx->ctx.stats.sid = atoi(args[4]);
				if (strcmp(args[5], "typed") == 0)
					appctx->ctx.stats.flags |= STAT_FMT_TYPED;
			}
			else if (strcmp(args[2], "typed") == 0)
				appctx->ctx.stats.flags |= STAT_FMT_TYPED;

			appctx->st2 = STAT_ST_INIT;
			appctx->st0 = STAT_CLI_O_STAT; // stats_dump_stat_to_buffer
		}
		else if (strcmp(args[1], "info") == 0) {
			if (strcmp(args[2], "typed") == 0)
				appctx->ctx.stats.flags |= STAT_FMT_TYPED;
			appctx->st2 = STAT_ST_INIT;
			appctx->st0 = STAT_CLI_O_INFO; // stats_dump_info_to_buffer
		}
		else if (strcmp(args[1], "servers") == 0 && strcmp(args[2], "state") == 0) {
			appctx->ctx.server_state.iid = 0;
			appctx->ctx.server_state.px = NULL;
			appctx->ctx.server_state.sv = NULL;

			/* check if a backend name has been provided */
			if (*args[3]) {
				/* read server state from local file */
				appctx->ctx.server_state.px = proxy_be_by_name(args[3]);

				if (!appctx->ctx.server_state.px) {
					appctx->ctx.cli.msg = "Can't find backend.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
				appctx->ctx.server_state.iid = appctx->ctx.server_state.px->uuid;
			}
			appctx->st2 = STAT_ST_INIT;
			appctx->st0 = STAT_CLI_O_SERVERS_STATE; // stats_dump_servers_state_to_buffer
			return 1;
		}
		else if (strcmp(args[1], "pools") == 0) {
			appctx->st2 = STAT_ST_INIT;
			appctx->st0 = STAT_CLI_O_POOLS; // stats_dump_pools_to_buffer
		}
		else if (strcmp(args[1], "sess") == 0) {
			appctx->st2 = STAT_ST_INIT;
			if (strm_li(s)->bind_conf->level < ACCESS_LVL_OPER) {
				appctx->ctx.cli.msg = stats_permission_denied_msg;
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}
			if (*args[2] && strcmp(args[2], "all") == 0)
				appctx->ctx.sess.target = (void *)-1;
			else if (*args[2])
				appctx->ctx.sess.target = (void *)strtoul(args[2], NULL, 0);
			else
				appctx->ctx.sess.target = NULL;
			appctx->ctx.sess.section = 0; /* start with stream status */
			appctx->ctx.sess.pos = 0;
			appctx->st0 = STAT_CLI_O_SESS; // stats_dump_sess_to_buffer
		}
		else if (strcmp(args[1], "errors") == 0) {
			if (strm_li(s)->bind_conf->level < ACCESS_LVL_OPER) {
				appctx->ctx.cli.msg = stats_permission_denied_msg;
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}
			if (*args[2])
				appctx->ctx.errors.iid	= atoi(args[2]);
			else
				appctx->ctx.errors.iid	= -1;
			appctx->ctx.errors.px = NULL;
			appctx->st2 = STAT_ST_INIT;
			appctx->st0 = STAT_CLI_O_ERR; // stats_dump_errors_to_buffer
		}
		else if (strcmp(args[1], "table") == 0) {
			stats_sock_table_request(si, args, STAT_CLI_O_TAB);
		}
		else if (strcmp(args[1], "tls-keys") == 0) {
#if (defined SSL_CTRL_SET_TLSEXT_TICKET_KEY_CB && TLS_TICKETS_NO > 0)
			/* no parameter, shows only file list */
			if (!*args[2]) {
				appctx->st2 = STAT_ST_INIT;
				appctx->st0 = STAT_CLI_O_TLSK;
				return 1;
			}

			if (args[2][0] == '*') {
				/* list every TLS ticket keys */
				appctx->ctx.tlskeys.ref = NULL;
			} else {
				appctx->ctx.tlskeys.ref = tlskeys_ref_lookup_ref(args[2]);
				if(!appctx->ctx.tlskeys.ref) {
					appctx->ctx.cli.msg = "'show tls-keys' unable to locate referenced filename\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
			}
			appctx->st2 = STAT_ST_INIT;
			appctx->st0 = STAT_CLI_O_TLSK_ENT;

#else
			appctx->ctx.cli.msg = "HAProxy was compiled against a version of OpenSSL "
						"that doesn't support specifying TLS ticket keys\n";
			appctx->st0 = STAT_CLI_PRINT;
#endif
			return 1;
		}
		else if (strcmp(args[1], "map") == 0 ||
		         strcmp(args[1], "acl") == 0) {

			/* Set ACL or MAP flags. */
			if (args[1][0] == 'm')
				appctx->ctx.map.display_flags = PAT_REF_MAP;
			else
				appctx->ctx.map.display_flags = PAT_REF_ACL;

			/* no parameter: display all map available */
			if (!*args[2]) {
				appctx->st2 = STAT_ST_INIT;
				appctx->st0 = STAT_CLI_O_PATS;
				return 1;
			}

			/* lookup into the refs and check the map flag */
			appctx->ctx.map.ref = pat_ref_lookup_ref(args[2]);
			if (!appctx->ctx.map.ref ||
			    !(appctx->ctx.map.ref->flags & appctx->ctx.map.display_flags)) {
				if (appctx->ctx.map.display_flags == PAT_REF_MAP)
					appctx->ctx.cli.msg = "Unknown map identifier. Please use #<id> or <file>.\n";
				else
					appctx->ctx.cli.msg = "Unknown ACL identifier. Please use #<id> or <file>.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}
			appctx->st2 = STAT_ST_INIT;
			appctx->st0 = STAT_CLI_O_PAT;
		}
		else { /* neither "stat" nor "info" nor "sess" nor "errors" nor "table" */
			return 0;
		}
	}
	else if (strcmp(args[0], "clear") == 0) {
		if (strcmp(args[1], "counters") == 0) {
			struct proxy *px;
			struct server *sv;
			struct listener *li;
			int clrall = 0;

			if (strcmp(args[2], "all") == 0)
				clrall = 1;

			/* check permissions */
			if (strm_li(s)->bind_conf->level < ACCESS_LVL_OPER ||
			    (clrall && strm_li(s)->bind_conf->level < ACCESS_LVL_ADMIN)) {
				appctx->ctx.cli.msg = stats_permission_denied_msg;
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			for (px = proxy; px; px = px->next) {
				if (clrall) {
					memset(&px->be_counters, 0, sizeof(px->be_counters));
					memset(&px->fe_counters, 0, sizeof(px->fe_counters));
				}
				else {
					px->be_counters.conn_max = 0;
					px->be_counters.p.http.rps_max = 0;
					px->be_counters.sps_max = 0;
					px->be_counters.cps_max = 0;
					px->be_counters.nbpend_max = 0;

					px->fe_counters.conn_max = 0;
					px->fe_counters.p.http.rps_max = 0;
					px->fe_counters.sps_max = 0;
					px->fe_counters.cps_max = 0;
					px->fe_counters.nbpend_max = 0;
				}

				for (sv = px->srv; sv; sv = sv->next)
					if (clrall)
						memset(&sv->counters, 0, sizeof(sv->counters));
					else {
						sv->counters.cur_sess_max = 0;
						sv->counters.nbpend_max = 0;
						sv->counters.sps_max = 0;
					}

				list_for_each_entry(li, &px->conf.listeners, by_fe)
					if (li->counters) {
						if (clrall)
							memset(li->counters, 0, sizeof(*li->counters));
						else
							li->counters->conn_max = 0;
					}
			}

			global.cps_max = 0;
			global.sps_max = 0;
			return 1;
		}
		else if (strcmp(args[1], "table") == 0) {
			stats_sock_table_request(si, args, STAT_CLI_O_CLR);
			/* end of processing */
			return 1;
		}
		else if (strcmp(args[1], "map") == 0 || strcmp(args[1], "acl") == 0) {
			/* Set ACL or MAP flags. */
			if (args[1][0] == 'm')
				appctx->ctx.map.display_flags = PAT_REF_MAP;
			else
				appctx->ctx.map.display_flags = PAT_REF_ACL;

			/* no parameter */
			if (!*args[2]) {
				if (appctx->ctx.map.display_flags == PAT_REF_MAP)
					appctx->ctx.cli.msg = "Missing map identifier.\n";
				else
					appctx->ctx.cli.msg = "Missing ACL identifier.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			/* lookup into the refs and check the map flag */
			appctx->ctx.map.ref = pat_ref_lookup_ref(args[2]);
			if (!appctx->ctx.map.ref ||
			    !(appctx->ctx.map.ref->flags & appctx->ctx.map.display_flags)) {
				if (appctx->ctx.map.display_flags == PAT_REF_MAP)
					appctx->ctx.cli.msg = "Unknown map identifier. Please use #<id> or <file>.\n";
				else
					appctx->ctx.cli.msg = "Unknown ACL identifier. Please use #<id> or <file>.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			/* Clear all. */
			pat_ref_prune(appctx->ctx.map.ref);

			/* return response */
			appctx->st0 = STAT_CLI_PROMPT;
			return 1;
		}
		else {
			/* unknown "clear" argument */
			return 0;
		}
	}
	else if (strcmp(args[0], "get") == 0) {
		if (strcmp(args[1], "weight") == 0) {
			struct proxy *px;
			struct server *sv;

			/* split "backend/server" and make <line> point to server */
			for (line = args[2]; *line; line++)
				if (*line == '/') {
					*line++ = '\0';
					break;
				}

			if (!*line) {
				appctx->ctx.cli.msg = "Require 'backend/server'.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			if (!get_backend_server(args[2], line, &px, &sv)) {
				appctx->ctx.cli.msg = px ? "No such server.\n" : "No such backend.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			/* return server's effective weight at the moment */
			snprintf(trash.str, trash.size, "%d (initial %d)\n", sv->uweight, sv->iweight);
			if (bi_putstr(si_ic(si), trash.str) == -1)
				si_applet_cant_put(si);

			return 1;
		}
		else if (strcmp(args[1], "map") == 0 || strcmp(args[1], "acl") == 0) {
			/* Set flags. */
			if (args[1][0] == 'm')
				appctx->ctx.map.display_flags = PAT_REF_MAP;
			else
				appctx->ctx.map.display_flags = PAT_REF_ACL;

			/* No parameter. */
			if (!*args[2] || !*args[3]) {
				if (appctx->ctx.map.display_flags == PAT_REF_MAP)
					appctx->ctx.cli.msg = "Missing map identifier and/or key.\n";
				else
					appctx->ctx.cli.msg = "Missing ACL identifier and/or key.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			/* lookup into the maps */
			appctx->ctx.map.ref = pat_ref_lookup_ref(args[2]);
			if (!appctx->ctx.map.ref) {
				if (appctx->ctx.map.display_flags == PAT_REF_MAP)
					appctx->ctx.cli.msg = "Unknown map identifier. Please use #<id> or <file>.\n";
				else
					appctx->ctx.cli.msg = "Unknown ACL identifier. Please use #<id> or <file>.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			/* copy input string. The string must be allocated because
			 * it may be used over multiple iterations. It's released
			 * at the end and upon abort anyway.
			 */
			appctx->ctx.map.chunk.len = strlen(args[3]);
			appctx->ctx.map.chunk.size = appctx->ctx.map.chunk.len + 1;
			appctx->ctx.map.chunk.str = strdup(args[3]);
			if (!appctx->ctx.map.chunk.str) {
				appctx->ctx.cli.msg = "Out of memory error.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			/* prepare response */
			appctx->st2 = STAT_ST_INIT;
			appctx->st0 = STAT_CLI_O_MLOOK;
		}
		else { /* not "get weight" */
			return 0;
		}
	}
	else if (strcmp(args[0], "set") == 0) {
		if (strcmp(args[1], "weight") == 0) {
			struct server *sv;
			const char *warning;

			sv = expect_server_admin(s, si, args[2]);
			if (!sv)
				return 1;

			warning = server_parse_weight_change_request(sv, args[3]);
			if (warning) {
				appctx->ctx.cli.msg = warning;
				appctx->st0 = STAT_CLI_PRINT;
			}
			return 1;
		}
		else if (strcmp(args[1], "server") == 0) {
			struct server *sv;
			const char *warning;

			sv = expect_server_admin(s, si, args[2]);
			if (!sv)
				return 1;

			if (strcmp(args[3], "weight") == 0) {
				warning = server_parse_weight_change_request(sv, args[4]);
				if (warning) {
					appctx->ctx.cli.msg = warning;
					appctx->st0 = STAT_CLI_PRINT;
				}
			}
			else if (strcmp(args[3], "state") == 0) {
				if (strcmp(args[4], "ready") == 0)
					srv_adm_set_ready(sv);
				else if (strcmp(args[4], "drain") == 0)
					srv_adm_set_drain(sv);
				else if (strcmp(args[4], "maint") == 0)
					srv_adm_set_maint(sv);
				else {
					appctx->ctx.cli.msg = "'set server <srv> state' expects 'ready', 'drain' and 'maint'.\n";
					appctx->st0 = STAT_CLI_PRINT;
				}
			}
			else if (strcmp(args[3], "health") == 0) {
				if (sv->track) {
					appctx->ctx.cli.msg = "cannot change health on a tracking server.\n";
					appctx->st0 = STAT_CLI_PRINT;
				}
				else if (strcmp(args[4], "up") == 0) {
					sv->check.health = sv->check.rise + sv->check.fall - 1;
					srv_set_running(sv, "changed from CLI");
				}
				else if (strcmp(args[4], "stopping") == 0) {
					sv->check.health = sv->check.rise + sv->check.fall - 1;
					srv_set_stopping(sv, "changed from CLI");
				}
				else if (strcmp(args[4], "down") == 0) {
					sv->check.health = 0;
					srv_set_stopped(sv, "changed from CLI");
				}
				else {
					appctx->ctx.cli.msg = "'set server <srv> health' expects 'up', 'stopping', or 'down'.\n";
					appctx->st0 = STAT_CLI_PRINT;
				}
			}
			else if (strcmp(args[3], "agent") == 0) {
				if (!(sv->agent.state & CHK_ST_ENABLED)) {
					appctx->ctx.cli.msg = "agent checks are not enabled on this server.\n";
					appctx->st0 = STAT_CLI_PRINT;
				}
				else if (strcmp(args[4], "up") == 0) {
					sv->agent.health = sv->agent.rise + sv->agent.fall - 1;
					srv_set_running(sv, "changed from CLI");
				}
				else if (strcmp(args[4], "down") == 0) {
					sv->agent.health = 0;
					srv_set_stopped(sv, "changed from CLI");
				}
				else {
					appctx->ctx.cli.msg = "'set server <srv> agent' expects 'up' or 'down'.\n";
					appctx->st0 = STAT_CLI_PRINT;
				}
			}
			else if (strcmp(args[3], "addr") == 0) {
				warning = server_parse_addr_change_request(sv, args[4], "stats command");
				if (warning) {
					appctx->ctx.cli.msg = warning;
					appctx->st0 = STAT_CLI_PRINT;
				}
			}
			else {
				appctx->ctx.cli.msg = "'set server <srv>' only supports 'agent', 'health', 'state', 'weight' and 'addr'.\n";
				appctx->st0 = STAT_CLI_PRINT;
			}
			return 1;
		}
		else if (strcmp(args[1], "timeout") == 0) {
			if (strcmp(args[2], "cli") == 0) {
				unsigned timeout;
				const char *res;

				if (!*args[3]) {
					appctx->ctx.cli.msg = "Expects an integer value.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				res = parse_time_err(args[3], &timeout, TIME_UNIT_S);
				if (res || timeout < 1) {
					appctx->ctx.cli.msg = "Invalid timeout value.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				s->req.rto = s->res.wto = 1 + MS_TO_TICKS(timeout*1000);
				return 1;
			}
			else {
				appctx->ctx.cli.msg = "'set timeout' only supports 'cli'.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}
		}
		else if (strcmp(args[1], "maxconn") == 0) {
			if (strcmp(args[2], "frontend") == 0) {
				struct proxy *px;
				struct listener *l;
				int v;

				px = expect_frontend_admin(s, si, args[3]);
				if (!px)
					return 1;

				if (!*args[4]) {
					appctx->ctx.cli.msg = "Integer value expected.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				v = atoi(args[4]);
				if (v < 0) {
					appctx->ctx.cli.msg = "Value out of range.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				/* OK, the value is fine, so we assign it to the proxy and to all of
				 * its listeners. The blocked ones will be dequeued.
				 */
				px->maxconn = v;
				list_for_each_entry(l, &px->conf.listeners, by_fe) {
					l->maxconn = v;
					if (l->state == LI_FULL)
						resume_listener(l);
				}

				if (px->maxconn > px->feconn && !LIST_ISEMPTY(&strm_fe(s)->listener_queue))
					dequeue_all_listeners(&strm_fe(s)->listener_queue);

				return 1;
			}
			else if (strcmp(args[2], "server") == 0) {
				struct server *sv;
				const char *warning;

				sv = expect_server_admin(s, si, args[3]);
				if (!sv)
					return 1;

				warning = server_parse_maxconn_change_request(sv, args[4]);
				if (warning) {
					appctx->ctx.cli.msg = warning;
					appctx->st0 = STAT_CLI_PRINT;
				}

				return 1;
			}
			else if (strcmp(args[2], "global") == 0) {
				int v;

				if (strm_li(s)->bind_conf->level < ACCESS_LVL_ADMIN) {
					appctx->ctx.cli.msg = stats_permission_denied_msg;
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				if (!*args[3]) {
					appctx->ctx.cli.msg = "Expects an integer value.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				v = atoi(args[3]);
				if (v > global.hardmaxconn) {
					appctx->ctx.cli.msg = "Value out of range.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				/* check for unlimited values */
				if (v <= 0)
					v = global.hardmaxconn;

				global.maxconn = v;

				/* Dequeues all of the listeners waiting for a resource */
				if (!LIST_ISEMPTY(&global_listener_queue))
					dequeue_all_listeners(&global_listener_queue);

				return 1;
			}
			else {
				appctx->ctx.cli.msg = "'set maxconn' only supports 'frontend', 'server', and 'global'.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}
		}
		else if (strcmp(args[1], "rate-limit") == 0) {
			if (strcmp(args[2], "connections") == 0) {
				if (strcmp(args[3], "global") == 0) {
					int v;

					if (strm_li(s)->bind_conf->level < ACCESS_LVL_ADMIN) {
						appctx->ctx.cli.msg = stats_permission_denied_msg;
						appctx->st0 = STAT_CLI_PRINT;
						return 1;
					}

					if (!*args[4]) {
						appctx->ctx.cli.msg = "Expects an integer value.\n";
						appctx->st0 = STAT_CLI_PRINT;
						return 1;
					}

					v = atoi(args[4]);
					if (v < 0) {
						appctx->ctx.cli.msg = "Value out of range.\n";
						appctx->st0 = STAT_CLI_PRINT;
						return 1;
					}

					global.cps_lim = v;

					/* Dequeues all of the listeners waiting for a resource */
					if (!LIST_ISEMPTY(&global_listener_queue))
						dequeue_all_listeners(&global_listener_queue);

					return 1;
				}
				else {
					appctx->ctx.cli.msg = "'set rate-limit connections' only supports 'global'.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
			}
			else if (strcmp(args[2], "sessions") == 0) {
				if (strcmp(args[3], "global") == 0) {
					int v;

					if (strm_li(s)->bind_conf->level < ACCESS_LVL_ADMIN) {
						appctx->ctx.cli.msg = stats_permission_denied_msg;
						appctx->st0 = STAT_CLI_PRINT;
						return 1;
					}

					if (!*args[4]) {
						appctx->ctx.cli.msg = "Expects an integer value.\n";
						appctx->st0 = STAT_CLI_PRINT;
						return 1;
					}

					v = atoi(args[4]);
					if (v < 0) {
						appctx->ctx.cli.msg = "Value out of range.\n";
						appctx->st0 = STAT_CLI_PRINT;
						return 1;
					}

					global.sps_lim = v;

					/* Dequeues all of the listeners waiting for a resource */
					if (!LIST_ISEMPTY(&global_listener_queue))
						dequeue_all_listeners(&global_listener_queue);

					return 1;
				}
				else {
					appctx->ctx.cli.msg = "'set rate-limit sessions' only supports 'global'.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
			}
#ifdef USE_OPENSSL
			else if (strcmp(args[2], "ssl-sessions") == 0) {
				if (strcmp(args[3], "global") == 0) {
					int v;

					if (strm_li(s)->bind_conf->level < ACCESS_LVL_ADMIN) {
						appctx->ctx.cli.msg = stats_permission_denied_msg;
						appctx->st0 = STAT_CLI_PRINT;
						return 1;
					}

					if (!*args[4]) {
						appctx->ctx.cli.msg = "Expects an integer value.\n";
						appctx->st0 = STAT_CLI_PRINT;
						return 1;
					}

					v = atoi(args[4]);
					if (v < 0) {
						appctx->ctx.cli.msg = "Value out of range.\n";
						appctx->st0 = STAT_CLI_PRINT;
						return 1;
					}

					global.ssl_lim = v;

					/* Dequeues all of the listeners waiting for a resource */
					if (!LIST_ISEMPTY(&global_listener_queue))
						dequeue_all_listeners(&global_listener_queue);

					return 1;
				}
				else {
					appctx->ctx.cli.msg = "'set rate-limit ssl-sessions' only supports 'global'.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
			}
#endif
			else if (strcmp(args[2], "http-compression") == 0) {
				if (strcmp(args[3], "global") == 0) {
					int v;

					if (strm_li(s)->bind_conf->level < ACCESS_LVL_ADMIN) {
						appctx->ctx.cli.msg = stats_permission_denied_msg;
						appctx->st0 = STAT_CLI_PRINT;
						return 1;
					}

					if (!*args[4]) {
						appctx->ctx.cli.msg = "Expects a maximum input byte rate in kB/s.\n";
						appctx->st0 = STAT_CLI_PRINT;
						return 1;
					}

					v = atoi(args[4]);
					global.comp_rate_lim = v * 1024; /* Kilo to bytes. */
				}
				else {
					appctx->ctx.cli.msg = "'set rate-limit http-compression' only supports 'global'.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
			}
			else {
				appctx->ctx.cli.msg = "'set rate-limit' supports 'connections', 'sessions', 'ssl-sessions', and 'http-compression'.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}
		}
		else if (strcmp(args[1], "table") == 0) {
			stats_sock_table_request(si, args, STAT_CLI_O_SET);
		}
		else if (strcmp(args[1], "map") == 0) {
			char *err;

			/* Set flags. */
			appctx->ctx.map.display_flags = PAT_REF_MAP;

			/* Expect three parameters: map name, key and new value. */
			if (!*args[2] || !*args[3] || !*args[4]) {
				appctx->ctx.cli.msg = "'set map' expects three parameters: map identifier, key and value.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			/* Lookup the reference in the maps. */
			appctx->ctx.map.ref = pat_ref_lookup_ref(args[2]);
			if (!appctx->ctx.map.ref) {
				appctx->ctx.cli.msg = "Unknown map identifier. Please use #<id> or <file>.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			/* If the entry identifier start with a '#', it is considered as
			 * pointer id
			 */
			if (args[3][0] == '#' && args[3][1] == '0' && args[3][2] == 'x') {
				struct pat_ref_elt *ref;
				long long int conv;
				char *error;

				/* Convert argument to integer value. */
				conv = strtoll(&args[3][1], &error, 16);
				if (*error != '\0') {
					appctx->ctx.cli.msg = "Malformed identifier. Please use #<id> or <file>.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				/* Convert and check integer to pointer. */
				ref = (struct pat_ref_elt *)(long)conv;
				if ((long long int)(long)ref != conv) {
					appctx->ctx.cli.msg = "Malformed identifier. Please use #<id> or <file>.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				/* Try to delete the entry. */
				err = NULL;
				if (!pat_ref_set_by_id(appctx->ctx.map.ref, ref, args[4], &err)) {
					if (err)
						memprintf(&err, "%s.\n", err);
					appctx->ctx.cli.err = err;
					appctx->st0 = STAT_CLI_PRINT_FREE;
					return 1;
				}
			}
			else {
				/* Else, use the entry identifier as pattern
				 * string, and update the value.
				 */
				err = NULL;
				if (!pat_ref_set(appctx->ctx.map.ref, args[3], args[4], &err)) {
					if (err)
						memprintf(&err, "%s.\n", err);
					appctx->ctx.cli.err = err;
					appctx->st0 = STAT_CLI_PRINT_FREE;
					return 1;
				}
			}

			/* The set is done, send message. */
			appctx->st0 = STAT_CLI_PROMPT;
			return 1;
		}
#ifdef USE_OPENSSL
		else if (strcmp(args[1], "ssl") == 0) {
			if (strcmp(args[2], "ocsp-response") == 0) {
#if (defined SSL_CTRL_SET_TLSEXT_STATUS_REQ_CB && !defined OPENSSL_NO_OCSP)
				char *err = NULL;

				/* Expect one parameter: the new response in base64 encoding */
				if (!*args[3]) {
					appctx->ctx.cli.msg = "'set ssl ocsp-response' expects response in base64 encoding.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				trash.len = base64dec(args[3], strlen(args[3]), trash.str, trash.size);
				if (trash.len < 0) {
					appctx->ctx.cli.msg = "'set ssl ocsp-response' received invalid base64 encoded response.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				if (ssl_sock_update_ocsp_response(&trash, &err)) {
					if (err) {
						memprintf(&err, "%s.\n", err);
						appctx->ctx.cli.err = err;
						appctx->st0 = STAT_CLI_PRINT_FREE;
					}
					return 1;
				}
				appctx->ctx.cli.msg = "OCSP Response updated!";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
#else
				appctx->ctx.cli.msg = "HAProxy was compiled against a version of OpenSSL that doesn't support OCSP stapling.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
#endif
			}
			else if (strcmp(args[2], "tls-key") == 0) {
#if (defined SSL_CTRL_SET_TLSEXT_TICKET_KEY_CB && TLS_TICKETS_NO > 0)
				/* Expect two parameters: the filename and the new new TLS key in encoding */
				if (!*args[3] || !*args[4]) {
					appctx->ctx.cli.msg = "'set ssl tls-key' expects a filename and the new TLS key in base64 encoding.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				appctx->ctx.tlskeys.ref = tlskeys_ref_lookup_ref(args[3]);
				if(!appctx->ctx.tlskeys.ref) {
					appctx->ctx.cli.msg = "'set ssl tls-key' unable to locate referenced filename\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				trash.len = base64dec(args[4], strlen(args[4]), trash.str, trash.size);
				if (trash.len != sizeof(struct tls_sess_key)) {
					appctx->ctx.cli.msg = "'set ssl tls-key' received invalid base64 encoded TLS key.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				memcpy(appctx->ctx.tlskeys.ref->tlskeys + ((appctx->ctx.tlskeys.ref->tls_ticket_enc_index + 2) % TLS_TICKETS_NO), trash.str, trash.len);
				appctx->ctx.tlskeys.ref->tls_ticket_enc_index = (appctx->ctx.tlskeys.ref->tls_ticket_enc_index + 1) % TLS_TICKETS_NO;

				appctx->ctx.cli.msg = "TLS ticket key updated!";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
#else
				appctx->ctx.cli.msg = "HAProxy was compiled against a version of OpenSSL "
							"that doesn't support specifying TLS ticket keys\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
#endif
			}
			else {
				appctx->ctx.cli.msg = "'set ssl' only supports 'ocsp-response'.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}
		}
#endif
		else { /* unknown "set" parameter */
			return 0;
		}
	}
	else if (strcmp(args[0], "enable") == 0) {
		if (strcmp(args[1], "agent") == 0) {
			struct server *sv;

			sv = expect_server_admin(s, si, args[2]);
			if (!sv)
				return 1;

			if (!(sv->agent.state & CHK_ST_CONFIGURED)) {
				appctx->ctx.cli.msg = "Agent was not configured on this server, cannot enable.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			sv->agent.state |= CHK_ST_ENABLED;
			return 1;
		}
		else if (strcmp(args[1], "health") == 0) {
			struct server *sv;

			sv = expect_server_admin(s, si, args[2]);
			if (!sv)
				return 1;

			if (!(sv->check.state & CHK_ST_CONFIGURED)) {
				appctx->ctx.cli.msg = "Health checks are not configured on this server, cannot enable.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			sv->check.state |= CHK_ST_ENABLED;
			return 1;
		}
		else if (strcmp(args[1], "server") == 0) {
			struct server *sv;

			sv = expect_server_admin(s, si, args[2]);
			if (!sv)
				return 1;

			srv_adm_set_ready(sv);
			return 1;
		}
		else if (strcmp(args[1], "frontend") == 0) {
			struct proxy *px;

			px = expect_frontend_admin(s, si, args[2]);
			if (!px)
				return 1;

			if (px->state == PR_STSTOPPED) {
				appctx->ctx.cli.msg = "Frontend was previously shut down, cannot enable.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			if (px->state != PR_STPAUSED) {
				appctx->ctx.cli.msg = "Frontend is already enabled.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			if (!resume_proxy(px)) {
				appctx->ctx.cli.msg = "Failed to resume frontend, check logs for precise cause (port conflict?).\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}
			return 1;
		}
		else { /* unknown "enable" parameter */
			appctx->ctx.cli.msg = "'enable' only supports 'agent', 'frontend', 'health', and 'server'.\n";
			appctx->st0 = STAT_CLI_PRINT;
			return 1;
		}
	}
	else if (strcmp(args[0], "disable") == 0) {
		if (strcmp(args[1], "agent") == 0) {
			struct server *sv;

			sv = expect_server_admin(s, si, args[2]);
			if (!sv)
				return 1;

			sv->agent.state &= ~CHK_ST_ENABLED;
			return 1;
		}
		else if (strcmp(args[1], "health") == 0) {
			struct server *sv;

			sv = expect_server_admin(s, si, args[2]);
			if (!sv)
				return 1;

			sv->check.state &= ~CHK_ST_ENABLED;
			return 1;
		}
		else if (strcmp(args[1], "server") == 0) {
			struct server *sv;

			sv = expect_server_admin(s, si, args[2]);
			if (!sv)
				return 1;

			srv_adm_set_maint(sv);
			return 1;
		}
		else if (strcmp(args[1], "frontend") == 0) {
			struct proxy *px;

			px = expect_frontend_admin(s, si, args[2]);
			if (!px)
				return 1;

			if (px->state == PR_STSTOPPED) {
				appctx->ctx.cli.msg = "Frontend was previously shut down, cannot disable.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			if (px->state == PR_STPAUSED) {
				appctx->ctx.cli.msg = "Frontend is already disabled.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			if (!pause_proxy(px)) {
				appctx->ctx.cli.msg = "Failed to pause frontend, check logs for precise cause.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}
			return 1;
		}
		else { /* unknown "disable" parameter */
			appctx->ctx.cli.msg = "'disable' only supports 'agent', 'frontend', 'health', and 'server'.\n";
			appctx->st0 = STAT_CLI_PRINT;
			return 1;
		}
	}
	else if (strcmp(args[0], "shutdown") == 0) {
		if (strcmp(args[1], "frontend") == 0) {
			struct proxy *px;

			px = expect_frontend_admin(s, si, args[2]);
			if (!px)
				return 1;

			if (px->state == PR_STSTOPPED) {
				appctx->ctx.cli.msg = "Frontend was already shut down.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			Warning("Proxy %s stopped (FE: %lld conns, BE: %lld conns).\n",
				px->id, px->fe_counters.cum_conn, px->be_counters.cum_conn);
			send_log(px, LOG_WARNING, "Proxy %s stopped (FE: %lld conns, BE: %lld conns).\n",
				 px->id, px->fe_counters.cum_conn, px->be_counters.cum_conn);
			stop_proxy(px);
			return 1;
		}
		else if (strcmp(args[1], "session") == 0) {
			struct stream *sess, *ptr;

			if (strm_li(s)->bind_conf->level < ACCESS_LVL_ADMIN) {
				appctx->ctx.cli.msg = stats_permission_denied_msg;
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			if (!*args[2]) {
				appctx->ctx.cli.msg = "Session pointer expected (use 'show sess').\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			ptr = (void *)strtoul(args[2], NULL, 0);

			/* first, look for the requested stream in the stream table */
			list_for_each_entry(sess, &streams, list) {
				if (sess == ptr)
					break;
			}

			/* do we have the stream ? */
			if (sess != ptr) {
				appctx->ctx.cli.msg = "No such session (use 'show sess').\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			stream_shutdown(sess, SF_ERR_KILLED);
			return 1;
		}
		else if (strcmp(args[1], "sessions") == 0) {
			if (strcmp(args[2], "server") == 0) {
				struct server *sv;
				struct stream *sess, *sess_bck;

				sv = expect_server_admin(s, si, args[3]);
				if (!sv)
					return 1;

				/* kill all the stream that are on this server */
				list_for_each_entry_safe(sess, sess_bck, &sv->actconns, by_srv)
					if (sess->srv_conn == sv)
						stream_shutdown(sess, SF_ERR_KILLED);

				return 1;
			}
			else {
				appctx->ctx.cli.msg = "'shutdown sessions' only supports 'server'.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}
		}
		else { /* unknown "disable" parameter */
			appctx->ctx.cli.msg = "'shutdown' only supports 'frontend', 'session' and 'sessions'.\n";
			appctx->st0 = STAT_CLI_PRINT;
			return 1;
		}
	}
	else if (strcmp(args[0], "del") == 0) {
		if (strcmp(args[1], "map") == 0 || strcmp(args[1], "acl") == 0) {
			if (args[1][0] == 'm')
				appctx->ctx.map.display_flags = PAT_REF_MAP;
			else
				appctx->ctx.map.display_flags = PAT_REF_ACL;

			/* Expect two parameters: map name and key. */
			if (appctx->ctx.map.display_flags == PAT_REF_MAP) {
				if (!*args[2] || !*args[3]) {
					appctx->ctx.cli.msg = "This command expects two parameters: map identifier and key.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
			}

			else {
				if (!*args[2] || !*args[3]) {
					appctx->ctx.cli.msg = "This command expects two parameters: ACL identifier and key.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
			}

			/* Lookup the reference in the maps. */
			appctx->ctx.map.ref = pat_ref_lookup_ref(args[2]);
			if (!appctx->ctx.map.ref ||
			    !(appctx->ctx.map.ref->flags & appctx->ctx.map.display_flags)) {
				appctx->ctx.cli.msg = "Unknown map identifier. Please use #<id> or <file>.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			/* If the entry identifier start with a '#', it is considered as
			 * pointer id
			 */
			if (args[3][0] == '#' && args[3][1] == '0' && args[3][2] == 'x') {
				struct pat_ref_elt *ref;
				long long int conv;
				char *error;

				/* Convert argument to integer value. */
				conv = strtoll(&args[3][1], &error, 16);
				if (*error != '\0') {
					appctx->ctx.cli.msg = "Malformed identifier. Please use #<id> or <file>.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				/* Convert and check integer to pointer. */
				ref = (struct pat_ref_elt *)(long)conv;
				if ((long long int)(long)ref != conv) {
					appctx->ctx.cli.msg = "Malformed identifier. Please use #<id> or <file>.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}

				/* Try to delete the entry. */
				if (!pat_ref_delete_by_id(appctx->ctx.map.ref, ref)) {
					/* The entry is not found, send message. */
					appctx->ctx.cli.msg = "Key not found.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
			}
			else {
				/* Else, use the entry identifier as pattern
				 * string and try to delete the entry.
				 */
				if (!pat_ref_delete(appctx->ctx.map.ref, args[3])) {
					/* The entry is not found, send message. */
					appctx->ctx.cli.msg = "Key not found.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
			}

			/* The deletion is done, send message. */
			appctx->st0 = STAT_CLI_PROMPT;
			return 1;
		}
		else { /* unknown "del" parameter */
			appctx->ctx.cli.msg = "'del' only supports 'map' or 'acl'.\n";
			appctx->st0 = STAT_CLI_PRINT;
			return 1;
		}
	}
	else if (strcmp(args[0], "add") == 0) {
		if (strcmp(args[1], "map") == 0 ||
		    strcmp(args[1], "acl") == 0) {
			int ret;
			char *err;

			/* Set flags. */
			if (args[1][0] == 'm')
				appctx->ctx.map.display_flags = PAT_REF_MAP;
			else
				appctx->ctx.map.display_flags = PAT_REF_ACL;

			/* If the keywork is "map", we expect three parameters, if it
			 * is "acl", we expect only two parameters
			 */
			if (appctx->ctx.map.display_flags == PAT_REF_MAP) {
				if (!*args[2] || !*args[3] || !*args[4]) {
					appctx->ctx.cli.msg = "'add map' expects three parameters: map identifier, key and value.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
			}
			else {
				if (!*args[2] || !*args[3]) {
					appctx->ctx.cli.msg = "'add acl' expects two parameters: ACL identifier and pattern.\n";
					appctx->st0 = STAT_CLI_PRINT;
					return 1;
				}
			}

			/* Lookup for the reference. */
			appctx->ctx.map.ref = pat_ref_lookup_ref(args[2]);
			if (!appctx->ctx.map.ref) {
				if (appctx->ctx.map.display_flags == PAT_REF_MAP)
					appctx->ctx.cli.msg = "Unknown map identifier. Please use #<id> or <file>.\n";
				else
					appctx->ctx.cli.msg = "Unknown ACL identifier. Please use #<id> or <file>.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			/* The command "add acl" is prohibited if the reference
			 * use samples.
			 */
			if ((appctx->ctx.map.display_flags & PAT_REF_ACL) &&
			    (appctx->ctx.map.ref->flags & PAT_REF_SMP)) {
				appctx->ctx.cli.msg = "This ACL is shared with a map containing samples. "
				                      "You must use the command 'add map' to add values.\n";
				appctx->st0 = STAT_CLI_PRINT;
				return 1;
			}

			/* Add value. */
			err = NULL;
			if (appctx->ctx.map.display_flags == PAT_REF_MAP)
				ret = pat_ref_add(appctx->ctx.map.ref, args[3], args[4], &err);
			else
				ret = pat_ref_add(appctx->ctx.map.ref, args[3], NULL, &err);
			if (!ret) {
				if (err)
					memprintf(&err, "%s.\n", err);
				appctx->ctx.cli.err = err;
				appctx->st0 = STAT_CLI_PRINT_FREE;
				return 1;
			}

			/* The add is done, send message. */
			appctx->st0 = STAT_CLI_PROMPT;
			return 1;
		}
		else { /* unknown "del" parameter */
			appctx->ctx.cli.msg = "'add' only supports 'map'.\n";
			appctx->st0 = STAT_CLI_PRINT;
			return 1;
		}
	}
	else { /* not "show" nor "clear" nor "get" nor "set" nor "enable" nor "disable" */
		return 0;
	}
	return 1;
}

/* This I/O handler runs as an applet embedded in a stream interface. It is
 * used to processes I/O from/to the stats unix socket. The system relies on a
 * state machine handling requests and various responses. We read a request,
 * then we process it and send the response, and we possibly display a prompt.
 * Then we can read again. The state is stored in appctx->st0 and is one of the
 * STAT_CLI_* constants. appctx->st1 is used to indicate whether prompt is enabled
 * or not.
 */
static void cli_io_handler(struct appctx *appctx)
{
	struct stream_interface *si = appctx->owner;
	struct channel *req = si_oc(si);
	struct channel *res = si_ic(si);
	int reql;
	int len;

	if (unlikely(si->state == SI_ST_DIS || si->state == SI_ST_CLO))
		goto out;

	while (1) {
		if (appctx->st0 == STAT_CLI_INIT) {
			/* Stats output not initialized yet */
			memset(&appctx->ctx.stats, 0, sizeof(appctx->ctx.stats));
			appctx->st0 = STAT_CLI_GETREQ;
		}
		else if (appctx->st0 == STAT_CLI_END) {
			/* Let's close for real now. We just close the request
			 * side, the conditions below will complete if needed.
			 */
			si_shutw(si);
			break;
		}
		else if (appctx->st0 == STAT_CLI_GETREQ) {
			/* ensure we have some output room left in the event we
			 * would want to return some info right after parsing.
			 */
			if (buffer_almost_full(si_ib(si))) {
				si_applet_cant_put(si);
				break;
			}

			reql = bo_getline(si_oc(si), trash.str, trash.size);
			if (reql <= 0) { /* closed or EOL not found */
				if (reql == 0)
					break;
				appctx->st0 = STAT_CLI_END;
				continue;
			}

			/* seek for a possible semi-colon. If we find one, we
			 * replace it with an LF and skip only this part.
			 */
			for (len = 0; len < reql; len++)
				if (trash.str[len] == ';') {
					trash.str[len] = '\n';
					reql = len + 1;
					break;
				}

			/* now it is time to check that we have a full line,
			 * remove the trailing \n and possibly \r, then cut the
			 * line.
			 */
			len = reql - 1;
			if (trash.str[len] != '\n') {
				appctx->st0 = STAT_CLI_END;
				continue;
			}

			if (len && trash.str[len-1] == '\r')
				len--;

			trash.str[len] = '\0';

			appctx->st0 = STAT_CLI_PROMPT;
			if (len) {
				if (strcmp(trash.str, "quit") == 0) {
					appctx->st0 = STAT_CLI_END;
					continue;
				}
				else if (strcmp(trash.str, "prompt") == 0)
					appctx->st1 = !appctx->st1;
				else if (strcmp(trash.str, "help") == 0 ||
					 !stats_sock_parse_request(si, trash.str)) {
					appctx->ctx.cli.msg = stats_sock_usage_msg;
					appctx->st0 = STAT_CLI_PRINT;
				}
				/* NB: stats_sock_parse_request() may have put
				 * another STAT_CLI_O_* into appctx->st0.
				 */
			}
			else if (!appctx->st1) {
				/* if prompt is disabled, print help on empty lines,
				 * so that the user at least knows how to enable
				 * prompt and find help.
				 */
				appctx->ctx.cli.msg = stats_sock_usage_msg;
				appctx->st0 = STAT_CLI_PRINT;
			}

			/* re-adjust req buffer */
			bo_skip(si_oc(si), reql);
			req->flags |= CF_READ_DONTWAIT; /* we plan to read small requests */
		}
		else {	/* output functions */
			switch (appctx->st0) {
			case STAT_CLI_PROMPT:
				break;
			case STAT_CLI_PRINT:
				if (bi_putstr(si_ic(si), appctx->ctx.cli.msg) != -1)
					appctx->st0 = STAT_CLI_PROMPT;
				else
					si_applet_cant_put(si);
				break;
			case STAT_CLI_PRINT_FREE:
				if (bi_putstr(si_ic(si), appctx->ctx.cli.err) != -1) {
					free(appctx->ctx.cli.err);
					appctx->st0 = STAT_CLI_PROMPT;
				}
				else
					si_applet_cant_put(si);
				break;
			case STAT_CLI_O_BACKEND:
				if (stats_dump_backend_to_buffer(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			case STAT_CLI_O_INFO:
				if (stats_dump_info_to_buffer(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			case STAT_CLI_O_SERVERS_STATE:
				if (stats_dump_servers_state_to_buffer(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			case STAT_CLI_O_STAT:
				if (stats_dump_stat_to_buffer(si, NULL))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			case STAT_CLI_O_RESOLVERS:
				if (stats_dump_resolvers_to_buffer(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			case STAT_CLI_O_SESS:
				if (stats_dump_sess_to_buffer(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			case STAT_CLI_O_ERR:	/* errors dump */
				if (stats_dump_errors_to_buffer(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			case STAT_CLI_O_TAB:
			case STAT_CLI_O_CLR:
				if (stats_table_request(si, appctx->st0))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			case STAT_CLI_O_PATS:
				if (stats_pats_list(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			case STAT_CLI_O_PAT:
				if (stats_pat_list(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			case STAT_CLI_O_MLOOK:
				if (stats_map_lookup(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			case STAT_CLI_O_POOLS:
				if (stats_dump_pools_to_buffer(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
#if (defined SSL_CTRL_SET_TLSEXT_TICKET_KEY_CB && TLS_TICKETS_NO > 0)
			case STAT_CLI_O_TLSK:
				if (stats_tlskeys_list(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			case STAT_CLI_O_TLSK_ENT:
				if (stats_tlskeys_list(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
#endif
			case STAT_CLI_O_ENV:	/* environment dump */
				if (stats_dump_env_to_buffer(si))
					appctx->st0 = STAT_CLI_PROMPT;
				break;
			default: /* abnormal state */
				si->flags |= SI_FL_ERR;
				break;
			}

			/* The post-command prompt is either LF alone or LF + '> ' in interactive mode */
			if (appctx->st0 == STAT_CLI_PROMPT) {
				if (bi_putstr(si_ic(si), appctx->st1 ? "\n> " : "\n") != -1)
					appctx->st0 = STAT_CLI_GETREQ;
				else
					si_applet_cant_put(si);
			}

			/* If the output functions are still there, it means they require more room. */
			if (appctx->st0 >= STAT_CLI_OUTPUT)
				break;

			/* Now we close the output if one of the writers did so,
			 * or if we're not in interactive mode and the request
			 * buffer is empty. This still allows pipelined requests
			 * to be sent in non-interactive mode.
			 */
			if ((res->flags & (CF_SHUTW|CF_SHUTW_NOW)) || (!appctx->st1 && !req->buf->o)) {
				appctx->st0 = STAT_CLI_END;
				continue;
			}

			/* switch state back to GETREQ to read next requests */
			appctx->st0 = STAT_CLI_GETREQ;
		}
	}

	if ((res->flags & CF_SHUTR) && (si->state == SI_ST_EST)) {
		DPRINTF(stderr, "%s@%d: si to buf closed. req=%08x, res=%08x, st=%d\n",
			__FUNCTION__, __LINE__, req->flags, res->flags, si->state);
		/* Other side has closed, let's abort if we have no more processing to do
		 * and nothing more to consume. This is comparable to a broken pipe, so
		 * we forward the close to the request side so that it flows upstream to
		 * the client.
		 */
		si_shutw(si);
	}

	if ((req->flags & CF_SHUTW) && (si->state == SI_ST_EST) && (appctx->st0 < STAT_CLI_OUTPUT)) {
		DPRINTF(stderr, "%s@%d: buf to si closed. req=%08x, res=%08x, st=%d\n",
			__FUNCTION__, __LINE__, req->flags, res->flags, si->state);
		/* We have no more processing to do, and nothing more to send, and
		 * the client side has closed. So we'll forward this state downstream
		 * on the response buffer.
		 */
		si_shutr(si);
		res->flags |= CF_READ_NULL;
	}

 out:
	DPRINTF(stderr, "%s@%d: st=%d, rqf=%x, rpf=%x, rqh=%d, rqs=%d, rh=%d, rs=%d\n",
		__FUNCTION__, __LINE__,
		si->state, req->flags, res->flags, req->buf->i, req->buf->o, res->buf->i, res->buf->o);
}

/* Emits a stats field without any surrounding element and properly encoded to
 * resist CSV output. Returns non-zero on success, 0 if the buffer is full.
 */
int stats_emit_raw_data_field(struct chunk *out, const struct field *f)
{
	switch (field_format(f, 0)) {
	case FF_EMPTY: return 1;
	case FF_S32:   return chunk_appendf(out, "%d", f->u.s32);
	case FF_U32:   return chunk_appendf(out, "%u", f->u.u32);
	case FF_S64:   return chunk_appendf(out, "%lld", (long long)f->u.s64);
	case FF_U64:   return chunk_appendf(out, "%llu", (unsigned long long)f->u.u64);
	case FF_STR:   return csv_enc_append(field_str(f, 0), 1, out) != NULL;
	default:       return chunk_appendf(out, "[INCORRECT_FIELD_TYPE_%08x]", f->type);
	}
}

/* Emits a stats field prefixed with its type. No CSV encoding is prepared, the
 * output is supposed to be used on its own line. Returns non-zero on success, 0
 * if the buffer is full.
 */
int stats_emit_typed_data_field(struct chunk *out, const struct field *f)
{
	switch (field_format(f, 0)) {
	case FF_EMPTY: return 1;
	case FF_S32:   return chunk_appendf(out, "s32:%d", f->u.s32);
	case FF_U32:   return chunk_appendf(out, "u32:%u", f->u.u32);
	case FF_S64:   return chunk_appendf(out, "s64:%lld", (long long)f->u.s64);
	case FF_U64:   return chunk_appendf(out, "u64:%llu", (unsigned long long)f->u.u64);
	case FF_STR:   return chunk_appendf(out, "str:%s", field_str(f, 0));
	default:       return chunk_appendf(out, "%08x:?", f->type);
	}
}

/* Emits an encoding of the field type on 3 characters followed by a delimiter.
 * Returns non-zero on success, 0 if the buffer is full.
 */
int stats_emit_field_tags(struct chunk *out, const struct field *f, char delim)
{
	char origin, nature, scope;

	switch (field_origin(f, 0)) {
	case FO_METRIC:  origin = 'M'; break;
	case FO_STATUS:  origin = 'S'; break;
	case FO_KEY:     origin = 'K'; break;
	case FO_CONFIG:  origin = 'C'; break;
	case FO_PRODUCT: origin = 'P'; break;
	default:         origin = '?'; break;
	}

	switch (field_nature(f, 0)) {
	case FN_GAUGE:    nature = 'G'; break;
	case FN_LIMIT:    nature = 'L'; break;
	case FN_MIN:      nature = 'm'; break;
	case FN_MAX:      nature = 'M'; break;
	case FN_RATE:     nature = 'R'; break;
	case FN_COUNTER:  nature = 'C'; break;
	case FN_DURATION: nature = 'D'; break;
	case FN_AGE:      nature = 'A'; break;
	case FN_TIME:     nature = 'T'; break;
	case FN_NAME:     nature = 'N'; break;
	case FN_OUTPUT:   nature = 'O'; break;
	case FN_AVG:      nature = 'a'; break;
	default:          nature = '?'; break;
	}

	switch (field_scope(f, 0)) {
	case FS_PROCESS: scope = 'P'; break;
	case FS_SERVICE: scope = 'S'; break;
	case FS_SYSTEM:  scope = 's'; break;
	case FS_CLUSTER: scope = 'C'; break;
	default:         scope = '?'; break;
	}

	return chunk_appendf(out, "%c%c%c%c", origin, nature, scope, delim);
}

/* Dump all fields from <info> into <out> using the "show info" format (name: value) */
static int stats_dump_info_fields(struct chunk *out, const struct field *info)
{
	int field;

	for (field = 0; field < INF_TOTAL_FIELDS; field++) {
		if (!field_format(info, field))
			continue;

		if (!chunk_appendf(out, "%s: ", info_field_names[field]))
			return 0;
		if (!stats_emit_raw_data_field(out, &info[field]))
			return 0;
		if (!chunk_strcat(out, "\n"))
			return 0;
	}
	return 1;
}

/* Dump all fields from <info> into <out> using the "show info typed" format */
static int stats_dump_typed_info_fields(struct chunk *out, const struct field *info)
{
	int field;

	for (field = 0; field < INF_TOTAL_FIELDS; field++) {
		if (!field_format(info, field))
			continue;

		if (!chunk_appendf(out, "%d.%s.%u:", field, info_field_names[field], info[INF_PROCESS_NUM].u.u32))
			return 0;
		if (!stats_emit_field_tags(out, &info[field], ':'))
			return 0;
		if (!stats_emit_typed_data_field(out, &info[field]))
			return 0;
		if (!chunk_strcat(out, "\n"))
			return 0;
	}
	return 1;
}

/* Fill <info> with HAProxy global info. <info> is preallocated
 * array of length <len>. The length of the aray must be
 * INF_TOTAL_FIELDS. If this length is less then this value, the
 * function returns 0, otherwise, it returns 1.
 */
int stats_fill_info(struct field *info, int len)
{
	unsigned int up = (now.tv_sec - start_date.tv_sec);
	struct chunk *out = get_trash_chunk();

#ifdef USE_OPENSSL
	int ssl_sess_rate = read_freq_ctr(&global.ssl_per_sec);
	int ssl_key_rate = read_freq_ctr(&global.ssl_fe_keys_per_sec);
	int ssl_reuse = 0;

	if (ssl_key_rate < ssl_sess_rate) {
		/* count the ssl reuse ratio and avoid overflows in both directions */
		ssl_reuse = 100 - (100 * ssl_key_rate + (ssl_sess_rate - 1) / 2) / ssl_sess_rate;
	}
#endif

	if (len < INF_TOTAL_FIELDS)
		return 0;

	chunk_reset(out);
	memset(info, 0, sizeof(*info) * len);

	info[INF_NAME]                           = mkf_str(FO_PRODUCT|FN_OUTPUT|FS_SERVICE, PRODUCT_NAME);
	info[INF_VERSION]                        = mkf_str(FO_PRODUCT|FN_OUTPUT|FS_SERVICE, HAPROXY_VERSION);
	info[INF_RELEASE_DATE]                   = mkf_str(FO_PRODUCT|FN_OUTPUT|FS_SERVICE, HAPROXY_DATE);

	info[INF_NBPROC]                         = mkf_u32(FO_CONFIG|FS_SERVICE, global.nbproc);
	info[INF_PROCESS_NUM]                    = mkf_u32(FO_KEY, relative_pid);
	info[INF_PID]                            = mkf_u32(FO_STATUS, pid);

	info[INF_UPTIME]                         = mkf_str(FN_DURATION, chunk_newstr(out));
	chunk_appendf(out, "%ud %uh%02um%02us", up / 86400, (up % 86400) / 3600, (up % 3600) / 60, (up % 60));

	info[INF_UPTIME_SEC]                     = mkf_u32(FN_DURATION, up);
	info[INF_MEMMAX_MB]                      = mkf_u32(FO_CONFIG|FN_LIMIT, global.rlimit_memmax);
	info[INF_POOL_ALLOC_MB]                  = mkf_u32(0, (unsigned)(pool_total_allocated() / 1048576L));
	info[INF_POOL_USED_MB]                   = mkf_u32(0, (unsigned)(pool_total_used() / 1048576L));
	info[INF_POOL_FAILED]                    = mkf_u32(FN_COUNTER, pool_total_failures());
	info[INF_ULIMIT_N]                       = mkf_u32(FO_CONFIG|FN_LIMIT, global.rlimit_nofile);
	info[INF_MAXSOCK]                        = mkf_u32(FO_CONFIG|FN_LIMIT, global.maxsock);
	info[INF_MAXCONN]                        = mkf_u32(FO_CONFIG|FN_LIMIT, global.maxconn);
	info[INF_HARD_MAXCONN]                   = mkf_u32(FO_CONFIG|FN_LIMIT, global.hardmaxconn);
	info[INF_CURR_CONN]                      = mkf_u32(0, actconn);
	info[INF_CUM_CONN]                       = mkf_u32(FN_COUNTER, totalconn);
	info[INF_CUM_REQ]                        = mkf_u32(FN_COUNTER, global.req_count);
#ifdef USE_OPENSSL
	info[INF_MAX_SSL_CONNS]                  = mkf_u32(FN_MAX, global.maxsslconn);
	info[INF_CURR_SSL_CONNS]                 = mkf_u32(0, sslconns);
	info[INF_CUM_SSL_CONNS]                  = mkf_u32(FN_COUNTER, totalsslconns);
#endif
	info[INF_MAXPIPES]                       = mkf_u32(FO_CONFIG|FN_LIMIT, global.maxpipes);
	info[INF_PIPES_USED]                     = mkf_u32(0, pipes_used);
	info[INF_PIPES_FREE]                     = mkf_u32(0, pipes_free);
	info[INF_CONN_RATE]                      = mkf_u32(FN_RATE, read_freq_ctr(&global.conn_per_sec));
	info[INF_CONN_RATE_LIMIT]                = mkf_u32(FO_CONFIG|FN_LIMIT, global.cps_lim);
	info[INF_MAX_CONN_RATE]                  = mkf_u32(FN_MAX, global.cps_max);
	info[INF_SESS_RATE]                      = mkf_u32(FN_RATE, read_freq_ctr(&global.sess_per_sec));
	info[INF_SESS_RATE_LIMIT]                = mkf_u32(FO_CONFIG|FN_LIMIT, global.sps_lim);
	info[INF_MAX_SESS_RATE]                  = mkf_u32(FN_RATE, global.sps_max);

#ifdef USE_OPENSSL
	info[INF_SSL_RATE]                       = mkf_u32(FN_RATE, ssl_sess_rate);
	info[INF_SSL_RATE_LIMIT]                 = mkf_u32(FO_CONFIG|FN_LIMIT, global.ssl_lim);
	info[INF_MAX_SSL_RATE]                   = mkf_u32(FN_MAX, global.ssl_max);
	info[INF_SSL_FRONTEND_KEY_RATE]          = mkf_u32(0, ssl_key_rate);
	info[INF_SSL_FRONTEND_MAX_KEY_RATE]      = mkf_u32(FN_MAX, global.ssl_fe_keys_max);
	info[INF_SSL_FRONTEND_SESSION_REUSE_PCT] = mkf_u32(0, ssl_reuse);
	info[INF_SSL_BACKEND_KEY_RATE]           = mkf_u32(FN_RATE, read_freq_ctr(&global.ssl_be_keys_per_sec));
	info[INF_SSL_BACKEND_MAX_KEY_RATE]       = mkf_u32(FN_MAX, global.ssl_be_keys_max);
	info[INF_SSL_CACHE_LOOKUPS]              = mkf_u32(FN_COUNTER, global.shctx_lookups);
	info[INF_SSL_CACHE_MISSES]               = mkf_u32(FN_COUNTER, global.shctx_misses);
#endif
	info[INF_COMPRESS_BPS_IN]                = mkf_u32(FN_RATE, read_freq_ctr(&global.comp_bps_in));
	info[INF_COMPRESS_BPS_OUT]               = mkf_u32(FN_RATE, read_freq_ctr(&global.comp_bps_out));
	info[INF_COMPRESS_BPS_RATE_LIM]          = mkf_u32(FO_CONFIG|FN_LIMIT, global.comp_rate_lim);
#ifdef USE_ZLIB
	info[INF_ZLIB_MEM_USAGE]                 = mkf_u32(0, zlib_used_memory);
	info[INF_MAX_ZLIB_MEM_USAGE]             = mkf_u32(FO_CONFIG|FN_LIMIT, global.maxzlibmem);
#endif
	info[INF_TASKS]                          = mkf_u32(0, nb_tasks_cur);
	info[INF_RUN_QUEUE]                      = mkf_u32(0, run_queue_cur);
	info[INF_IDLE_PCT]                       = mkf_u32(FN_AVG, idle_pct);
	info[INF_NODE]                           = mkf_str(FO_CONFIG|FN_OUTPUT|FS_SERVICE, global.node);
	if (global.desc)
		info[INF_DESCRIPTION]            = mkf_str(FO_CONFIG|FN_OUTPUT|FS_SERVICE, global.desc);

	return 1;
}

/* This function dumps information onto the stream interface's read buffer.
 * It returns 0 as long as it does not complete, non-zero upon completion.
 * No state is used.
 */
static int stats_dump_info_to_buffer(struct stream_interface *si)
{
	struct appctx *appctx = __objt_appctx(si->end);

	if (!stats_fill_info(info, INF_TOTAL_FIELDS))
		return 0;

	chunk_reset(&trash);

	if (appctx->ctx.stats.flags & STAT_FMT_TYPED)
		stats_dump_typed_info_fields(&trash, info);
	else
		stats_dump_info_fields(&trash, info);

	if (bi_putchk(si_ic(si), &trash) == -1) {
		si_applet_cant_put(si);
		return 0;
	}

	return 1;
}

/* dumps server state information into <buf> for all the servers found in <backend>
 * These information are all the parameters which may change during HAProxy runtime.
 * By default, we only export to the last known server state file format.
 * These information can be used at next startup to recover same level of server state.
 */
static int dump_servers_state(struct stream_interface *si, struct chunk *buf)
{
	struct appctx *appctx = __objt_appctx(si->end);
	struct server *srv;
	char srv_addr[INET6_ADDRSTRLEN + 1];
	time_t srv_time_since_last_change;
	int bk_f_forced_id, srv_f_forced_id;


	/* we don't want to report any state if the backend is not enabled on this process */
	if (appctx->ctx.server_state.px->bind_proc && !(appctx->ctx.server_state.px->bind_proc & (1UL << (relative_pid - 1))))
		return 1;

	if (!appctx->ctx.server_state.sv)
		appctx->ctx.server_state.sv = appctx->ctx.server_state.px->srv;

	for (; appctx->ctx.server_state.sv != NULL; appctx->ctx.server_state.sv = srv->next) {
		srv = appctx->ctx.server_state.sv;
		srv_addr[0] = '\0';
		srv_time_since_last_change = 0;
		bk_f_forced_id = 0;
		srv_f_forced_id = 0;

		switch (srv->addr.ss_family) {
			case AF_INET:
				inet_ntop(srv->addr.ss_family, &((struct sockaddr_in *)&srv->addr)->sin_addr,
					  srv_addr, INET_ADDRSTRLEN + 1);
				break;
			case AF_INET6:
				inet_ntop(srv->addr.ss_family, &((struct sockaddr_in6 *)&srv->addr)->sin6_addr,
					  srv_addr, INET6_ADDRSTRLEN + 1);
				break;
		}
		srv_time_since_last_change = now.tv_sec - srv->last_change;
		bk_f_forced_id = appctx->ctx.server_state.px->options & PR_O_FORCED_ID ? 1 : 0;
		srv_f_forced_id = srv->flags & SRV_F_FORCED_ID ? 1 : 0;

		chunk_appendf(buf,
				"%d %s "
				"%d %s %s "
				"%d %d %d %d %ld "
				"%d %d %d %d %d "
				"%d %d"
				"\n",
				appctx->ctx.server_state.px->uuid, appctx->ctx.server_state.px->id,
				srv->puid, srv->id, srv_addr,
				srv->state, srv->admin, srv->uweight, srv->iweight, (long int)srv_time_since_last_change,
				srv->check.status, srv->check.result, srv->check.health, srv->check.state, srv->agent.state,
				bk_f_forced_id, srv_f_forced_id);
		if (bi_putchk(si_ic(si), &trash) == -1) {
			si_applet_cant_put(si);
			return 0;
		}
	}
	return 1;
}

/* Parses backend list and simply report backend names */
static int stats_dump_backend_to_buffer(struct stream_interface *si)
{
	struct appctx *appctx = __objt_appctx(si->end);
	extern struct proxy *proxy;
	struct proxy *curproxy;

	chunk_reset(&trash);

	if (!appctx->ctx.be.px) {
		chunk_printf(&trash, "# name\n");
		if (bi_putchk(si_ic(si), &trash) == -1) {
			si_applet_cant_put(si);
			return 0;
		}
		appctx->ctx.be.px = proxy;
	}

	for (; appctx->ctx.be.px != NULL; appctx->ctx.be.px = curproxy->next) {
		curproxy = appctx->ctx.be.px;

		/* looking for backends only */
		if (!(curproxy->cap & PR_CAP_BE))
			continue;

		/* we don't want to list a backend which is bound to this process */
		if (curproxy->bind_proc && !(curproxy->bind_proc & (1UL << (relative_pid - 1))))
			continue;

		chunk_appendf(&trash, "%s\n", curproxy->id);
		if (bi_putchk(si_ic(si), &trash) == -1) {
			si_applet_cant_put(si);
			return 0;
		}
	}

	return 1;
}

/* Parses backend list or simply use backend name provided by the user to return
 * states of servers to stdout.
 */
static int stats_dump_servers_state_to_buffer(struct stream_interface *si)
{
	struct appctx *appctx = __objt_appctx(si->end);
	extern struct proxy *proxy;
	struct proxy *curproxy;

	chunk_reset(&trash);

	if (!appctx->ctx.server_state.px) {
		chunk_printf(&trash, "%d\n# %s\n", SRV_STATE_FILE_VERSION, SRV_STATE_FILE_FIELD_NAMES);
		if (bi_putchk(si_ic(si), &trash) == -1) {
			si_applet_cant_put(si);
			return 0;
		}
		appctx->ctx.server_state.px = proxy;
	}

	for (; appctx->ctx.server_state.px != NULL; appctx->ctx.server_state.px = curproxy->next) {
		curproxy = appctx->ctx.server_state.px;
		/* servers are only in backends */
		if (curproxy->cap & PR_CAP_BE) {
			if (!dump_servers_state(si, &trash))
				return 0;

			if (bi_putchk(si_ic(si), &trash) == -1) {
				si_applet_cant_put(si);
				return 0;
			}
		}
		/* only the selected proxy is dumped */
		if (appctx->ctx.server_state.iid)
			break;
	}

	return 1;
}

/* This function dumps memory usage information onto the stream interface's
 * read buffer. It returns 0 as long as it does not complete, non-zero upon
 * completion. No state is used.
 */
static int stats_dump_pools_to_buffer(struct stream_interface *si)
{
	dump_pools_to_trash();
	if (bi_putchk(si_ic(si), &trash) == -1) {
		si_applet_cant_put(si);
		return 0;
	}
	return 1;
}

/* Dump all fields from <stats> into <out> using CSV format */
static int stats_dump_fields_csv(struct chunk *out, const struct field *stats)
{
	int field;

	for (field = 0; field < ST_F_TOTAL_FIELDS; field++) {
		if (!stats_emit_raw_data_field(out, &stats[field]))
			return 0;
		if (!chunk_strcat(out, ","))
			return 0;
	}
	chunk_strcat(out, "\n");
	return 1;
}

/* Dump all fields from <stats> into <out> using a typed "field:desc:type:value" format */
static int stats_dump_fields_typed(struct chunk *out, const struct field *stats)
{
	int field;

	for (field = 0; field < ST_F_TOTAL_FIELDS; field++) {
		if (!stats[field].type)
			continue;

		chunk_appendf(out, "%c.%u.%u.%d.%s.%u:",
		              stats[ST_F_TYPE].u.u32 == STATS_TYPE_FE ? 'F' :
		              stats[ST_F_TYPE].u.u32 == STATS_TYPE_BE ? 'B' :
		              stats[ST_F_TYPE].u.u32 == STATS_TYPE_SO ? 'L' :
		              stats[ST_F_TYPE].u.u32 == STATS_TYPE_SV ? 'S' :
		              '?',
		              stats[ST_F_IID].u.u32, stats[ST_F_SID].u.u32,
		              field, stat_field_names[field], stats[ST_F_PID].u.u32);

		if (!stats_emit_field_tags(out, &stats[field], ':'))
			return 0;
		if (!stats_emit_typed_data_field(out, &stats[field]))
			return 0;
		if (!chunk_strcat(out, "\n"))
			return 0;
	}
	return 1;
}

/* Dump all fields from <stats> into <out> using the HTML format. A column is
 * reserved for the checkbox is ST_SHOWADMIN is set in <flags>. Some extra info
 * are provided if ST_SHLGNDS is present in <flags>.
 */
static int stats_dump_fields_html(struct chunk *out, const struct field *stats, unsigned int flags)
{
	struct chunk src;

	if (stats[ST_F_TYPE].u.u32 == STATS_TYPE_FE) {
		chunk_appendf(out,
		              /* name, queue */
		              "<tr class=\"frontend\">");

		if (flags & ST_SHOWADMIN) {
			/* Column sub-heading for Enable or Disable server */
			chunk_appendf(out, "<td></td>");
		}

		chunk_appendf(out,
		              "<td class=ac>"
		              "<a name=\"%s/Frontend\"></a>"
		              "<a class=lfsb href=\"#%s/Frontend\">Frontend</a></td>"
		              "<td colspan=3></td>"
		              "",
		              field_str(stats, ST_F_PXNAME), field_str(stats, ST_F_PXNAME));

		chunk_appendf(out,
		              /* sessions rate : current */
		              "<td><u>%s<div class=tips><table class=det>"
		              "<tr><th>Current connection rate:</th><td>%s/s</td></tr>"
		              "<tr><th>Current session rate:</th><td>%s/s</td></tr>"
		              "",
		              U2H(stats[ST_F_RATE].u.u32),
		              U2H(stats[ST_F_CONN_RATE].u.u32),
		              U2H(stats[ST_F_RATE].u.u32));

		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0)
			chunk_appendf(out,
			              "<tr><th>Current request rate:</th><td>%s/s</td></tr>",
			              U2H(stats[ST_F_REQ_RATE].u.u32));

		chunk_appendf(out,
		              "</table></div></u></td>"
		              /* sessions rate : max */
		              "<td><u>%s<div class=tips><table class=det>"
		              "<tr><th>Max connection rate:</th><td>%s/s</td></tr>"
		              "<tr><th>Max session rate:</th><td>%s/s</td></tr>"
		              "",
		              U2H(stats[ST_F_RATE_MAX].u.u32),
		              U2H(stats[ST_F_CONN_RATE_MAX].u.u32),
		              U2H(stats[ST_F_RATE_MAX].u.u32));

		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0)
			chunk_appendf(out,
			              "<tr><th>Max request rate:</th><td>%s/s</td></tr>",
			              U2H(stats[ST_F_REQ_RATE_MAX].u.u32));

		chunk_appendf(out,
		              "</table></div></u></td>"
		              /* sessions rate : limit */
		              "<td>%s</td>",
		              LIM2A(stats[ST_F_RATE_LIM].u.u32, "-"));

		chunk_appendf(out,
		              /* sessions: current, max, limit, total */
		              "<td>%s</td><td>%s</td><td>%s</td>"
		              "<td><u>%s<div class=tips><table class=det>"
		              "<tr><th>Cum. connections:</th><td>%s</td></tr>"
		              "<tr><th>Cum. sessions:</th><td>%s</td></tr>"
		              "",
		              U2H(stats[ST_F_SCUR].u.u32), U2H(stats[ST_F_SMAX].u.u32), U2H(stats[ST_F_SLIM].u.u32),
		              U2H(stats[ST_F_STOT].u.u64),
		              U2H(stats[ST_F_CONN_TOT].u.u64),
		              U2H(stats[ST_F_STOT].u.u64));

		/* http response (via hover): 1xx, 2xx, 3xx, 4xx, 5xx, other */
		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0) {
			chunk_appendf(out,
			              "<tr><th>Cum. HTTP requests:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 1xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 2xx responses:</th><td>%s</td></tr>"
			              "<tr><th>&nbsp;&nbsp;Compressed 2xx:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- HTTP 3xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 4xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 5xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- other responses:</th><td>%s</td></tr>"
			              "<tr><th>Intercepted requests:</th><td>%s</td></tr>"
			              "",
			              U2H(stats[ST_F_REQ_TOT].u.u64),
			              U2H(stats[ST_F_HRSP_1XX].u.u64),
			              U2H(stats[ST_F_HRSP_2XX].u.u64),
			              U2H(stats[ST_F_COMP_RSP].u.u64),
			              stats[ST_F_HRSP_2XX].u.u64 ?
			              (int)(100 * stats[ST_F_COMP_RSP].u.u64 / stats[ST_F_HRSP_2XX].u.u64) : 0,
			              U2H(stats[ST_F_HRSP_3XX].u.u64),
			              U2H(stats[ST_F_HRSP_4XX].u.u64),
			              U2H(stats[ST_F_HRSP_5XX].u.u64),
			              U2H(stats[ST_F_HRSP_OTHER].u.u64),
			              U2H(stats[ST_F_INTERCEPTED].u.u64));
		}

		chunk_appendf(out,
		              "</table></div></u></td>"
		              /* sessions: lbtot, lastsess */
		              "<td></td><td></td>"
		              /* bytes : in */
		              "<td>%s</td>"
		              "",
		              U2H(stats[ST_F_BIN].u.u64));

		chunk_appendf(out,
			      /* bytes:out + compression stats (via hover): comp_in, comp_out, comp_byp */
		              "<td>%s%s<div class=tips><table class=det>"
			      "<tr><th>Response bytes in:</th><td>%s</td></tr>"
			      "<tr><th>Compression in:</th><td>%s</td></tr>"
			      "<tr><th>Compression out:</th><td>%s</td><td>(%d%%)</td></tr>"
			      "<tr><th>Compression bypass:</th><td>%s</td></tr>"
			      "<tr><th>Total bytes saved:</th><td>%s</td><td>(%d%%)</td></tr>"
			      "</table></div>%s</td>",
		              (stats[ST_F_COMP_IN].u.u64 || stats[ST_F_COMP_BYP].u.u64) ? "<u>":"",
		              U2H(stats[ST_F_BOUT].u.u64),
		              U2H(stats[ST_F_BOUT].u.u64),
		              U2H(stats[ST_F_COMP_IN].u.u64),
			      U2H(stats[ST_F_COMP_OUT].u.u64),
			      stats[ST_F_COMP_IN].u.u64 ? (int)(stats[ST_F_COMP_OUT].u.u64 * 100 / stats[ST_F_COMP_IN].u.u64) : 0,
			      U2H(stats[ST_F_COMP_BYP].u.u64),
			      U2H(stats[ST_F_COMP_IN].u.u64 - stats[ST_F_COMP_OUT].u.u64),
			      stats[ST_F_BOUT].u.u64 ? (int)((stats[ST_F_COMP_IN].u.u64 - stats[ST_F_COMP_OUT].u.u64) * 100 / stats[ST_F_BOUT].u.u64) : 0,
		              (stats[ST_F_COMP_IN].u.u64 || stats[ST_F_COMP_BYP].u.u64) ? "</u>":"");

		chunk_appendf(out,
		              /* denied: req, resp */
		              "<td>%s</td><td>%s</td>"
		              /* errors : request, connect, response */
		              "<td>%s</td><td></td><td></td>"
		              /* warnings: retries, redispatches */
		              "<td></td><td></td>"
		              /* server status : reflect frontend status */
		              "<td class=ac>%s</td>"
		              /* rest of server: nothing */
		              "<td class=ac colspan=8></td></tr>"
		              "",
		              U2H(stats[ST_F_DREQ].u.u64), U2H(stats[ST_F_DRESP].u.u64),
		              U2H(stats[ST_F_EREQ].u.u64),
		              field_str(stats, ST_F_STATUS));
	}
	else if (stats[ST_F_TYPE].u.u32 == STATS_TYPE_SO) {
		chunk_appendf(out, "<tr class=socket>");
		if (flags & ST_SHOWADMIN) {
			/* Column sub-heading for Enable or Disable server */
			chunk_appendf(out, "<td></td>");
		}

		chunk_appendf(out,
		              /* frontend name, listener name */
		              "<td class=ac><a name=\"%s/+%s\"></a>%s"
		              "<a class=lfsb href=\"#%s/+%s\">%s</a>"
		              "",
		              field_str(stats, ST_F_PXNAME), field_str(stats, ST_F_SVNAME),
		              (flags & ST_SHLGNDS)?"<u>":"",
		              field_str(stats, ST_F_PXNAME), field_str(stats, ST_F_SVNAME), field_str(stats, ST_F_SVNAME));

		if (flags & ST_SHLGNDS) {
			chunk_appendf(out, "<div class=tips>");

			if (isdigit(*field_str(stats, ST_F_ADDR)))
				chunk_appendf(out, "IPv4: %s, ", field_str(stats, ST_F_ADDR));
			else if (*field_str(stats, ST_F_ADDR) == '[')
				chunk_appendf(out, "IPv6: %s, ", field_str(stats, ST_F_ADDR));
			else if (*field_str(stats, ST_F_ADDR))
				chunk_appendf(out, "%s, ", field_str(stats, ST_F_ADDR));

			/* id */
			chunk_appendf(out, "id: %d</div>", stats[ST_F_SID].u.u32);
		}

		chunk_appendf(out,
			      /* queue */
		              "%s</td><td colspan=3></td>"
		              /* sessions rate: current, max, limit */
		              "<td colspan=3>&nbsp;</td>"
		              /* sessions: current, max, limit, total, lbtot, lastsess */
		              "<td>%s</td><td>%s</td><td>%s</td>"
		              "<td>%s</td><td>&nbsp;</td><td>&nbsp;</td>"
		              /* bytes: in, out */
		              "<td>%s</td><td>%s</td>"
		              "",
		              (flags & ST_SHLGNDS)?"</u>":"",
		              U2H(stats[ST_F_SCUR].u.u32), U2H(stats[ST_F_SMAX].u.u32), U2H(stats[ST_F_SLIM].u.u32),
		              U2H(stats[ST_F_STOT].u.u64), U2H(stats[ST_F_BIN].u.u64), U2H(stats[ST_F_BOUT].u.u64));

		chunk_appendf(out,
		              /* denied: req, resp */
		              "<td>%s</td><td>%s</td>"
		              /* errors: request, connect, response */
		              "<td>%s</td><td></td><td></td>"
		              /* warnings: retries, redispatches */
		              "<td></td><td></td>"
		              /* server status: reflect listener status */
		              "<td class=ac>%s</td>"
		              /* rest of server: nothing */
		              "<td class=ac colspan=8></td></tr>"
		              "",
		              U2H(stats[ST_F_DREQ].u.u64), U2H(stats[ST_F_DRESP].u.u64),
		              U2H(stats[ST_F_EREQ].u.u64),
		              field_str(stats, ST_F_STATUS));
	}
	else if (stats[ST_F_TYPE].u.u32 == STATS_TYPE_SV) {
		const char *style;

		/* determine the style to use depending on the server's state,
		 * its health and weight. There isn't a 1-to-1 mapping between
		 * state and styles for the cases where the server is (still)
		 * up. The reason is that we don't want to report nolb and
		 * drain with the same color.
		 */

		if (strcmp(field_str(stats, ST_F_STATUS), "DOWN") == 0 ||
		    strcmp(field_str(stats, ST_F_STATUS), "DOWN (agent)") == 0) {
			style = "down";
		}
		else if (strcmp(field_str(stats, ST_F_STATUS), "DOWN ") == 0) {
			style = "going_up";
		}
		else if (strcmp(field_str(stats, ST_F_STATUS), "NOLB ") == 0) {
			style = "going_down";
		}
		else if (strcmp(field_str(stats, ST_F_STATUS), "NOLB") == 0) {
			style = "nolb";
		}
		else if (strcmp(field_str(stats, ST_F_STATUS), "no check") == 0) {
			style = "no_check";
		}
		else if (!stats[ST_F_CHKFAIL].type ||
			 stats[ST_F_CHECK_HEALTH].u.u32 == stats[ST_F_CHECK_RISE].u.u32 + stats[ST_F_CHECK_FALL].u.u32 - 1) {
			/* no check or max health = UP */
			if (stats[ST_F_WEIGHT].u.u32)
				style = "up";
			else
				style = "draining";
		}
		else {
			style = "going_down";
		}

		if (memcmp(field_str(stats, ST_F_STATUS), "MAINT", 5) == 0)
			chunk_appendf(out, "<tr class=\"maintain\">");
		else
			chunk_appendf(out,
			              "<tr class=\"%s_%s\">",
			              (stats[ST_F_BCK].u.u32) ? "backup" : "active", style);


		if (flags & ST_SHOWADMIN)
			chunk_appendf(out,
			              "<td><input type=\"checkbox\" name=\"s\" value=\"%s\"></td>",
			              field_str(stats, ST_F_SVNAME));

		chunk_appendf(out,
		              "<td class=ac><a name=\"%s/%s\"></a>%s"
		              "<a class=lfsb href=\"#%s/%s\">%s</a>"
		              "",
		              field_str(stats, ST_F_PXNAME), field_str(stats, ST_F_SVNAME),
		              (flags & ST_SHLGNDS) ? "<u>" : "",
		              field_str(stats, ST_F_PXNAME), field_str(stats, ST_F_SVNAME), field_str(stats, ST_F_SVNAME));

		if (flags & ST_SHLGNDS) {
			chunk_appendf(out, "<div class=tips>");

			if (isdigit(*field_str(stats, ST_F_ADDR)))
				chunk_appendf(out, "IPv4: %s, ", field_str(stats, ST_F_ADDR));
			else if (*field_str(stats, ST_F_ADDR) == '[')
				chunk_appendf(out, "IPv6: %s, ", field_str(stats, ST_F_ADDR));
			else if (*field_str(stats, ST_F_ADDR))
				chunk_appendf(out, "%s, ", field_str(stats, ST_F_ADDR));

			/* id */
			chunk_appendf(out, "id: %d", stats[ST_F_SID].u.u32);

			/* cookie */
			if (stats[ST_F_COOKIE].type) {
				chunk_appendf(out, ", cookie: '");
				chunk_initstr(&src, field_str(stats, ST_F_COOKIE));
				chunk_htmlencode(out, &src);
				chunk_appendf(out, "'");
			}

			chunk_appendf(out, "</div>");
		}

		chunk_appendf(out,
		              /* queue : current, max, limit */
		              "%s</td><td>%s</td><td>%s</td><td>%s</td>"
		              /* sessions rate : current, max, limit */
		              "<td>%s</td><td>%s</td><td></td>"
		              "",
		              (flags & ST_SHLGNDS) ? "</u>" : "",
		              U2H(stats[ST_F_QCUR].u.u32), U2H(stats[ST_F_QMAX].u.u32), LIM2A(stats[ST_F_QLIMIT].u.u32, "-"),
		              U2H(stats[ST_F_RATE].u.u32), U2H(stats[ST_F_RATE_MAX].u.u32));

		chunk_appendf(out,
		              /* sessions: current, max, limit, total */
		              "<td>%s</td><td>%s</td><td>%s</td>"
		              "<td><u>%s<div class=tips><table class=det>"
		              "<tr><th>Cum. sessions:</th><td>%s</td></tr>"
		              "",
		              U2H(stats[ST_F_SCUR].u.u32), U2H(stats[ST_F_SMAX].u.u32), LIM2A(stats[ST_F_SLIM].u.u32, "-"),
		              U2H(stats[ST_F_STOT].u.u64),
		              U2H(stats[ST_F_STOT].u.u64));

		/* http response (via hover): 1xx, 2xx, 3xx, 4xx, 5xx, other */
		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0) {
			unsigned long long tot;

			tot  = stats[ST_F_HRSP_OTHER].u.u64;
			tot += stats[ST_F_HRSP_1XX].u.u64;
			tot += stats[ST_F_HRSP_2XX].u.u64;
			tot += stats[ST_F_HRSP_3XX].u.u64;
			tot += stats[ST_F_HRSP_4XX].u.u64;
			tot += stats[ST_F_HRSP_5XX].u.u64;

			chunk_appendf(out,
			              "<tr><th>Cum. HTTP responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 1xx responses:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- HTTP 2xx responses:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- HTTP 3xx responses:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- HTTP 4xx responses:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- HTTP 5xx responses:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- other responses:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "",
			              U2H(tot),
			              U2H(stats[ST_F_HRSP_1XX].u.u64), tot ? (int)(100 * stats[ST_F_HRSP_1XX].u.u64 / tot) : 0,
			              U2H(stats[ST_F_HRSP_2XX].u.u64), tot ? (int)(100 * stats[ST_F_HRSP_2XX].u.u64 / tot) : 0,
			              U2H(stats[ST_F_HRSP_3XX].u.u64), tot ? (int)(100 * stats[ST_F_HRSP_3XX].u.u64 / tot) : 0,
			              U2H(stats[ST_F_HRSP_4XX].u.u64), tot ? (int)(100 * stats[ST_F_HRSP_4XX].u.u64 / tot) : 0,
			              U2H(stats[ST_F_HRSP_5XX].u.u64), tot ? (int)(100 * stats[ST_F_HRSP_5XX].u.u64 / tot) : 0,
			              U2H(stats[ST_F_HRSP_OTHER].u.u64), tot ? (int)(100 * stats[ST_F_HRSP_OTHER].u.u64 / tot) : 0);
		}

		chunk_appendf(out, "<tr><th colspan=3>Avg over last 1024 success. conn.</th></tr>");
		chunk_appendf(out, "<tr><th>- Queue time:</th><td>%s</td><td>ms</td></tr>",   U2H(stats[ST_F_QTIME].u.u32));
		chunk_appendf(out, "<tr><th>- Connect time:</th><td>%s</td><td>ms</td></tr>", U2H(stats[ST_F_CTIME].u.u32));
		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0)
			chunk_appendf(out, "<tr><th>- Response time:</th><td>%s</td><td>ms</td></tr>", U2H(stats[ST_F_RTIME].u.u32));
		chunk_appendf(out, "<tr><th>- Total time:</th><td>%s</td><td>ms</td></tr>",   U2H(stats[ST_F_TTIME].u.u32));

		chunk_appendf(out,
		              "</table></div></u></td>"
		              /* sessions: lbtot, last */
		              "<td>%s</td><td>%s</td>",
		              U2H(stats[ST_F_LBTOT].u.u64),
		              human_time(stats[ST_F_LASTSESS].u.s32, 1));

		chunk_appendf(out,
		              /* bytes : in, out */
		              "<td>%s</td><td>%s</td>"
		              /* denied: req, resp */
		              "<td></td><td>%s</td>"
		              /* errors : request, connect */
		              "<td></td><td>%s</td>"
		              /* errors : response */
		              "<td><u>%s<div class=tips>Connection resets during transfers: %lld client, %lld server</div></u></td>"
		              /* warnings: retries, redispatches */
		              "<td>%lld</td><td>%lld</td>"
		              "",
		              U2H(stats[ST_F_BIN].u.u64), U2H(stats[ST_F_BOUT].u.u64),
		              U2H(stats[ST_F_DRESP].u.u64),
		              U2H(stats[ST_F_ECON].u.u64),
		              U2H(stats[ST_F_ERESP].u.u64),
		              (long long)stats[ST_F_CLI_ABRT].u.u64,
		              (long long)stats[ST_F_SRV_ABRT].u.u64,
		              (long long)stats[ST_F_WRETR].u.u64,
			      (long long)stats[ST_F_WREDIS].u.u64);

		/* status, last change */
		chunk_appendf(out, "<td class=ac>");

		/* FIXME!!!!
		 *   LASTCHG should contain the last change for *this* server and must be computed
		 * properly above, as was done below, ie: this server if maint, otherwise ref server
		 * if tracking. Note that ref is either local or remote depending on tracking.
		 */


		if (memcmp(field_str(stats, ST_F_STATUS), "MAINT", 5) == 0) {
			chunk_appendf(out, "%s MAINT", human_time(stats[ST_F_LASTCHG].u.u32, 1));
		}
		else if (memcmp(field_str(stats, ST_F_STATUS), "no check", 5) == 0) {
			chunk_strcat(out, "<i>no check</i>");
		}
		else {
			chunk_appendf(out, "%s %s", human_time(stats[ST_F_LASTCHG].u.u32, 1), field_str(stats, ST_F_STATUS));
			if (memcmp(field_str(stats, ST_F_STATUS), "DOWN", 4) == 0) {
				if (stats[ST_F_CHECK_HEALTH].u.u32)
					chunk_strcat(out, " &uarr;");
			}
			else if (stats[ST_F_CHECK_HEALTH].u.u32 < stats[ST_F_CHECK_RISE].u.u32 + stats[ST_F_CHECK_FALL].u.u32 - 1)
				chunk_strcat(out, " &darr;");
		}

		if (memcmp(field_str(stats, ST_F_STATUS), "DOWN", 4) == 0 &&
		    stats[ST_F_AGENT_STATUS].type && !stats[ST_F_AGENT_HEALTH].u.u32) {
			chunk_appendf(out,
			              "</td><td class=ac><u> %s",
			              field_str(stats, ST_F_AGENT_STATUS));

			if (stats[ST_F_AGENT_CODE].type)
				chunk_appendf(out, "/%d", stats[ST_F_AGENT_CODE].u.u32);

			if (stats[ST_F_AGENT_DURATION].type && stats[ST_F_AGENT_DURATION].u.u64 >= 0)
				chunk_appendf(out, " in %lums", (long)stats[ST_F_AGENT_DURATION].u.u64);

			chunk_appendf(out, "<div class=tips>%s", field_str(stats, ST_F_AGENT_DESC));

			if (*field_str(stats, ST_F_LAST_AGT)) {
				chunk_appendf(out, ": ");
				chunk_initstr(&src, field_str(stats, ST_F_LAST_AGT));
				chunk_htmlencode(out, &src);
			}
			chunk_appendf(out, "</div></u>");
		}
		else if (stats[ST_F_CHECK_STATUS].type) {
			chunk_appendf(out,
			              "</td><td class=ac><u> %s",
			              field_str(stats, ST_F_CHECK_STATUS));

			if (stats[ST_F_CHECK_CODE].type)
				chunk_appendf(out, "/%d", stats[ST_F_CHECK_CODE].u.u32);

			if (stats[ST_F_CHECK_DURATION].type && stats[ST_F_CHECK_DURATION].u.u64 >= 0)
				chunk_appendf(out, " in %lums", (long)stats[ST_F_CHECK_DURATION].u.u64);

			chunk_appendf(out, "<div class=tips>%s", field_str(stats, ST_F_CHECK_DESC));

			if (*field_str(stats, ST_F_LAST_CHK)) {
				chunk_appendf(out, ": ");
				chunk_initstr(&src, field_str(stats, ST_F_LAST_CHK));
				chunk_htmlencode(out, &src);
			}
			chunk_appendf(out, "</div></u>");
		}
		else
			chunk_appendf(out, "</td><td>");

		chunk_appendf(out,
		              /* weight */
		              "</td><td class=ac>%d</td>"
		              /* act, bck */
		              "<td class=ac>%s</td><td class=ac>%s</td>"
		              "",
		              stats[ST_F_WEIGHT].u.u32,
		              stats[ST_F_BCK].u.u32 ? "-" : "Y",
		              stats[ST_F_BCK].u.u32 ? "Y" : "-");

		/* check failures: unique, fatal, down time */
		if (stats[ST_F_CHKFAIL].type) {
			chunk_appendf(out, "<td><u>%lld", (long long)stats[ST_F_CHKFAIL].u.u64);

			if (stats[ST_F_HANAFAIL].type)
				chunk_appendf(out, "/%lld", (long long)stats[ST_F_HANAFAIL].u.u64);

			chunk_appendf(out,
			              "<div class=tips>Failed Health Checks%s</div></u></td>"
			              "<td>%lld</td><td>%s</td>"
			              "",
			              stats[ST_F_HANAFAIL].type ? "/Health Analyses" : "",
			              (long long)stats[ST_F_CHKDOWN].u.u64, human_time(stats[ST_F_DOWNTIME].u.u32, 1));
		}
		else if (strcmp(field_str(stats, ST_F_STATUS), "MAINT") != 0 && field_format(stats, ST_F_TRACKED) == FF_STR) {
			/* tracking a server (hence inherited maint would appear as "MAINT (via...)" */
			chunk_appendf(out,
			              "<td class=ac colspan=3><a class=lfsb href=\"#%s\">via %s</a></td>",
			              field_str(stats, ST_F_TRACKED), field_str(stats, ST_F_TRACKED));
		}
		else
			chunk_appendf(out, "<td colspan=3></td>");

		/* throttle */
		if (stats[ST_F_THROTTLE].type)
			chunk_appendf(out, "<td class=ac>%d %%</td></tr>\n", stats[ST_F_THROTTLE].u.u32);
		else
			chunk_appendf(out, "<td class=ac>-</td></tr>\n");
	}
	else if (stats[ST_F_TYPE].u.u32 == STATS_TYPE_BE) {
		chunk_appendf(out, "<tr class=\"backend\">");
		if (flags & ST_SHOWADMIN) {
			/* Column sub-heading for Enable or Disable server */
			chunk_appendf(out, "<td></td>");
		}
		chunk_appendf(out,
		              "<td class=ac>"
		              /* name */
		              "%s<a name=\"%s/Backend\"></a>"
		              "<a class=lfsb href=\"#%s/Backend\">Backend</a>"
		              "",
		              (flags & ST_SHLGNDS)?"<u>":"",
		              field_str(stats, ST_F_PXNAME), field_str(stats, ST_F_PXNAME));

		if (flags & ST_SHLGNDS) {
			/* balancing */
			chunk_appendf(out, "<div class=tips>balancing: %s",
			              field_str(stats, ST_F_ALGO));

			/* cookie */
			if (stats[ST_F_COOKIE].type) {
				chunk_appendf(out, ", cookie: '");
				chunk_initstr(&src, field_str(stats, ST_F_COOKIE));
				chunk_htmlencode(out, &src);
				chunk_appendf(out, "'");
			}
			chunk_appendf(out, "</div>");
		}

		chunk_appendf(out,
		              "%s</td>"
		              /* queue : current, max */
		              "<td>%s</td><td>%s</td><td></td>"
		              /* sessions rate : current, max, limit */
		              "<td>%s</td><td>%s</td><td></td>"
		              "",
		              (flags & ST_SHLGNDS)?"</u>":"",
		              U2H(stats[ST_F_QCUR].u.u32), U2H(stats[ST_F_QMAX].u.u32),
		              U2H(stats[ST_F_RATE].u.u32), U2H(stats[ST_F_RATE_MAX].u.u32));

		chunk_appendf(out,
		              /* sessions: current, max, limit, total */
		              "<td>%s</td><td>%s</td><td>%s</td>"
		              "<td><u>%s<div class=tips><table class=det>"
		              "<tr><th>Cum. sessions:</th><td>%s</td></tr>"
		              "",
		              U2H(stats[ST_F_SCUR].u.u32), U2H(stats[ST_F_SCUR].u.u32), U2H(stats[ST_F_SLIM].u.u32),
		              U2H(stats[ST_F_STOT].u.u64),
		              U2H(stats[ST_F_STOT].u.u64));

		/* http response (via hover): 1xx, 2xx, 3xx, 4xx, 5xx, other */
		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0) {
			chunk_appendf(out,
			              "<tr><th>Cum. HTTP requests:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 1xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 2xx responses:</th><td>%s</td></tr>"
			              "<tr><th>&nbsp;&nbsp;Compressed 2xx:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- HTTP 3xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 4xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 5xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- other responses:</th><td>%s</td></tr>"
			              "<tr><th>Intercepted requests:</th><td>%s</td></tr>"
				      "<tr><th colspan=3>Avg over last 1024 success. conn.</th></tr>"
			              "",
			              U2H(stats[ST_F_REQ_TOT].u.u64),
			              U2H(stats[ST_F_HRSP_1XX].u.u64),
			              U2H(stats[ST_F_HRSP_2XX].u.u64),
			              U2H(stats[ST_F_COMP_RSP].u.u64),
			              stats[ST_F_HRSP_2XX].u.u64 ?
			              (int)(100 * stats[ST_F_COMP_RSP].u.u64 / stats[ST_F_HRSP_2XX].u.u64) : 0,
			              U2H(stats[ST_F_HRSP_3XX].u.u64),
			              U2H(stats[ST_F_HRSP_4XX].u.u64),
			              U2H(stats[ST_F_HRSP_5XX].u.u64),
			              U2H(stats[ST_F_HRSP_OTHER].u.u64),
			              U2H(stats[ST_F_INTERCEPTED].u.u64));
		}

		chunk_appendf(out, "<tr><th>- Queue time:</th><td>%s</td><td>ms</td></tr>",   U2H(stats[ST_F_QTIME].u.u32));
		chunk_appendf(out, "<tr><th>- Connect time:</th><td>%s</td><td>ms</td></tr>", U2H(stats[ST_F_QTIME].u.u32));
		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0)
			chunk_appendf(out, "<tr><th>- Response time:</th><td>%s</td><td>ms</td></tr>", U2H(stats[ST_F_RTIME].u.u32));
		chunk_appendf(out, "<tr><th>- Total time:</th><td>%s</td><td>ms</td></tr>",   U2H(stats[ST_F_TTIME].u.u32));

		chunk_appendf(out,
		              "</table></div></u></td>"
		              /* sessions: lbtot, last */
		              "<td>%s</td><td>%s</td>"
		              /* bytes: in */
		              "<td>%s</td>"
		              "",
		              U2H(stats[ST_F_LBTOT].u.u64),
		              human_time(stats[ST_F_LASTSESS].u.s32, 1),
		              U2H(stats[ST_F_BIN].u.u64));

		chunk_appendf(out,
			      /* bytes:out + compression stats (via hover): comp_in, comp_out, comp_byp */
		              "<td>%s%s<div class=tips><table class=det>"
			      "<tr><th>Response bytes in:</th><td>%s</td></tr>"
			      "<tr><th>Compression in:</th><td>%s</td></tr>"
			      "<tr><th>Compression out:</th><td>%s</td><td>(%d%%)</td></tr>"
			      "<tr><th>Compression bypass:</th><td>%s</td></tr>"
			      "<tr><th>Total bytes saved:</th><td>%s</td><td>(%d%%)</td></tr>"
			      "</table></div>%s</td>",
		              (stats[ST_F_COMP_IN].u.u64 || stats[ST_F_COMP_BYP].u.u64) ? "<u>":"",
		              U2H(stats[ST_F_BOUT].u.u64),
		              U2H(stats[ST_F_BOUT].u.u64),
		              U2H(stats[ST_F_COMP_IN].u.u64),
			      U2H(stats[ST_F_COMP_OUT].u.u64),
			      stats[ST_F_COMP_IN].u.u64 ? (int)(stats[ST_F_COMP_OUT].u.u64 * 100 / stats[ST_F_COMP_IN].u.u64) : 0,
			      U2H(stats[ST_F_COMP_BYP].u.u64),
			      U2H(stats[ST_F_COMP_IN].u.u64 - stats[ST_F_COMP_OUT].u.u64),
			      stats[ST_F_BOUT].u.u64 ? (int)((stats[ST_F_COMP_IN].u.u64 - stats[ST_F_COMP_OUT].u.u64) * 100 / stats[ST_F_BOUT].u.u64) : 0,
		              (stats[ST_F_COMP_IN].u.u64 || stats[ST_F_COMP_BYP].u.u64) ? "</u>":"");

		chunk_appendf(out,
		              /* denied: req, resp */
		              "<td>%s</td><td>%s</td>"
		              /* errors : request, connect */
		              "<td></td><td>%s</td>"
		              /* errors : response */
		              "<td><u>%s<div class=tips>Connection resets during transfers: %lld client, %lld server</div></u></td>"
		              /* warnings: retries, redispatches */
		              "<td>%lld</td><td>%lld</td>"
		              /* backend status: reflect backend status (up/down): we display UP
		               * if the backend has known working servers or if it has no server at
		               * all (eg: for stats). Then we display the total weight, number of
		               * active and backups. */
		              "<td class=ac>%s %s</td><td class=ac>&nbsp;</td><td class=ac>%d</td>"
		              "<td class=ac>%d</td><td class=ac>%d</td>"
		              "",
		              U2H(stats[ST_F_DREQ].u.u64), U2H(stats[ST_F_DRESP].u.u64),
		              U2H(stats[ST_F_ECON].u.u64),
		              U2H(stats[ST_F_ERESP].u.u64),
		              (long long)stats[ST_F_CLI_ABRT].u.u64,
		              (long long)stats[ST_F_SRV_ABRT].u.u64,
		              (long long)stats[ST_F_WRETR].u.u64, (long long)stats[ST_F_WREDIS].u.u64,
		              human_time(stats[ST_F_LASTCHG].u.u32, 1),
		              strcmp(field_str(stats, ST_F_STATUS), "DOWN") ? field_str(stats, ST_F_STATUS) : "<font color=\"red\"><b>DOWN</b></font>",
		              stats[ST_F_WEIGHT].u.u32,
		              stats[ST_F_ACT].u.u32, stats[ST_F_BCK].u.u32);

		chunk_appendf(out,
		              /* rest of backend: nothing, down transitions, total downtime, throttle */
		              "<td class=ac>&nbsp;</td><td>%d</td>"
		              "<td>%s</td>"
		              "<td></td>"
		              "</tr>",
		              stats[ST_F_CHKDOWN].u.u32,
		              stats[ST_F_DOWNTIME].type ? human_time(stats[ST_F_DOWNTIME].u.u32, 1) : "&nbsp;");
	}
	return 1;
}

static int stats_dump_one_line(const struct field *stats, unsigned int flags, struct proxy *px, struct appctx *appctx)
{
	if ((px->cap & PR_CAP_BE) && px->srv && (appctx->ctx.stats.flags & STAT_ADMIN))
		flags |= ST_SHOWADMIN;

	if (appctx->ctx.stats.flags & STAT_FMT_HTML)
		return stats_dump_fields_html(&trash, stats, flags);
	else if (appctx->ctx.stats.flags & STAT_FMT_TYPED)
		return stats_dump_fields_typed(&trash, stats);
	else
		return stats_dump_fields_csv(&trash, stats);
}

/* Fill <stats> with the frontend statistics. <stats> is
 * preallocated array of length <len>. The length of the array
 * must be at least ST_F_TOTAL_FIELDS. If this length is less then
 * this value, the function returns 0, otherwise, it returns 1.
 */
int stats_fill_fe_stats(struct proxy *px, struct field *stats, int len)
{
	if (len < ST_F_TOTAL_FIELDS)
		return 0;

	memset(stats, 0, sizeof(*stats) * len);

	stats[ST_F_PXNAME]   = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, px->id);
	stats[ST_F_SVNAME]   = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, "FRONTEND");
	stats[ST_F_MODE]     = mkf_str(FO_CONFIG|FS_SERVICE, proxy_mode_str(px->mode));
	stats[ST_F_SCUR]     = mkf_u32(0, px->feconn);
	stats[ST_F_SMAX]     = mkf_u32(FN_MAX, px->fe_counters.conn_max);
	stats[ST_F_SLIM]     = mkf_u32(FO_CONFIG|FN_LIMIT, px->maxconn);
	stats[ST_F_STOT]     = mkf_u64(FN_COUNTER, px->fe_counters.cum_sess);
	stats[ST_F_BIN]      = mkf_u64(FN_COUNTER, px->fe_counters.bytes_in);
	stats[ST_F_BOUT]     = mkf_u64(FN_COUNTER, px->fe_counters.bytes_out);
	stats[ST_F_DREQ]     = mkf_u64(FN_COUNTER, px->fe_counters.denied_req);
	stats[ST_F_DRESP]    = mkf_u64(FN_COUNTER, px->fe_counters.denied_resp);
	stats[ST_F_EREQ]     = mkf_u64(FN_COUNTER, px->fe_counters.failed_req);
	stats[ST_F_STATUS]   = mkf_str(FO_STATUS, px->state == PR_STREADY ? "OPEN" : px->state == PR_STFULL ? "FULL" : "STOP");
	stats[ST_F_PID]      = mkf_u32(FO_KEY, relative_pid);
	stats[ST_F_IID]      = mkf_u32(FO_KEY|FS_SERVICE, px->uuid);
	stats[ST_F_SID]      = mkf_u32(FO_KEY|FS_SERVICE, 0);
	stats[ST_F_TYPE]     = mkf_u32(FO_CONFIG|FS_SERVICE, STATS_TYPE_FE);
	stats[ST_F_RATE]     = mkf_u32(FN_RATE, read_freq_ctr(&px->fe_sess_per_sec));
	stats[ST_F_RATE_LIM] = mkf_u32(FO_CONFIG|FN_LIMIT, px->fe_sps_lim);
	stats[ST_F_RATE_MAX] = mkf_u32(FN_MAX, px->fe_counters.sps_max);

	/* http response: 1xx, 2xx, 3xx, 4xx, 5xx, other */
	if (px->mode == PR_MODE_HTTP) {
		stats[ST_F_HRSP_1XX]    = mkf_u64(FN_COUNTER, px->fe_counters.p.http.rsp[1]);
		stats[ST_F_HRSP_2XX]    = mkf_u64(FN_COUNTER, px->fe_counters.p.http.rsp[2]);
		stats[ST_F_HRSP_3XX]    = mkf_u64(FN_COUNTER, px->fe_counters.p.http.rsp[3]);
		stats[ST_F_HRSP_4XX]    = mkf_u64(FN_COUNTER, px->fe_counters.p.http.rsp[4]);
		stats[ST_F_HRSP_5XX]    = mkf_u64(FN_COUNTER, px->fe_counters.p.http.rsp[5]);
		stats[ST_F_HRSP_OTHER]  = mkf_u64(FN_COUNTER, px->fe_counters.p.http.rsp[0]);
		stats[ST_F_INTERCEPTED] = mkf_u64(FN_COUNTER, px->fe_counters.intercepted_req);
	}

	/* requests : req_rate, req_rate_max, req_tot, */
	stats[ST_F_REQ_RATE]     = mkf_u32(FN_RATE, read_freq_ctr(&px->fe_req_per_sec));
	stats[ST_F_REQ_RATE_MAX] = mkf_u32(FN_MAX, px->fe_counters.p.http.rps_max);
	stats[ST_F_REQ_TOT]      = mkf_u64(FN_COUNTER, px->fe_counters.p.http.cum_req);

	/* compression: in, out, bypassed, responses */
	stats[ST_F_COMP_IN]      = mkf_u64(FN_COUNTER, px->fe_counters.comp_in);
	stats[ST_F_COMP_OUT]     = mkf_u64(FN_COUNTER, px->fe_counters.comp_out);
	stats[ST_F_COMP_BYP]     = mkf_u64(FN_COUNTER, px->fe_counters.comp_byp);
	stats[ST_F_COMP_RSP]     = mkf_u64(FN_COUNTER, px->fe_counters.p.http.comp_rsp);

	/* connections : conn_rate, conn_rate_max, conn_tot, conn_max */
	stats[ST_F_CONN_RATE]     = mkf_u32(FN_RATE, read_freq_ctr(&px->fe_conn_per_sec));
	stats[ST_F_CONN_RATE_MAX] = mkf_u32(FN_MAX, px->fe_counters.cps_max);
	stats[ST_F_CONN_TOT]      = mkf_u64(FN_COUNTER, px->fe_counters.cum_conn);

	return 1;
}

/* Dumps a frontend's line to the trash for the current proxy <px> and uses
 * the state from stream interface <si>. The caller is responsible for clearing
 * the trash if needed. Returns non-zero if it emits anything, zero otherwise.
 */
static int stats_dump_fe_stats(struct stream_interface *si, struct proxy *px)
{
	struct appctx *appctx = __objt_appctx(si->end);

	if (!(px->cap & PR_CAP_FE))
		return 0;

	if ((appctx->ctx.stats.flags & STAT_BOUND) && !(appctx->ctx.stats.type & (1 << STATS_TYPE_FE)))
		return 0;

	if (!stats_fill_fe_stats(px, stats, ST_F_TOTAL_FIELDS))
		return 0;

	return stats_dump_one_line(stats, 0, px, appctx);
}

/* Fill <stats> with the listener statistics. <stats> is
 * preallocated array of length <len>. The length of the array
 * must be at least ST_F_TOTAL_FIELDS. If this length is less
 * then this value, the function returns 0, otherwise, it
 * returns 1. <flags> can take the value ST_SHLGNDS.
 */
int stats_fill_li_stats(struct proxy *px, struct listener *l, int flags,
                        struct field *stats, int len)
{
	struct chunk *out = get_trash_chunk();

	if (len < ST_F_TOTAL_FIELDS)
		return 0;

	if (!l->counters)
		return 0;

	chunk_reset(out);
	memset(stats, 0, sizeof(*stats) * len);

	stats[ST_F_PXNAME]   = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, px->id);
	stats[ST_F_SVNAME]   = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, l->name);
	stats[ST_F_MODE]     = mkf_str(FO_CONFIG|FS_SERVICE, proxy_mode_str(px->mode));
	stats[ST_F_SCUR]     = mkf_u32(0, l->nbconn);
	stats[ST_F_SMAX]     = mkf_u32(FN_MAX, l->counters->conn_max);
	stats[ST_F_SLIM]     = mkf_u32(FO_CONFIG|FN_LIMIT, l->maxconn);
	stats[ST_F_STOT]     = mkf_u64(FN_COUNTER, l->counters->cum_conn);
	stats[ST_F_BIN]      = mkf_u64(FN_COUNTER, l->counters->bytes_in);
	stats[ST_F_BOUT]     = mkf_u64(FN_COUNTER, l->counters->bytes_out);
	stats[ST_F_DREQ]     = mkf_u64(FN_COUNTER, l->counters->denied_req);
	stats[ST_F_DRESP]    = mkf_u64(FN_COUNTER, l->counters->denied_resp);
	stats[ST_F_EREQ]     = mkf_u64(FN_COUNTER, l->counters->failed_req);
	stats[ST_F_STATUS]   = mkf_str(FO_STATUS, (l->nbconn < l->maxconn) ? (l->state == LI_LIMITED) ? "WAITING" : "OPEN" : "FULL");
	stats[ST_F_PID]      = mkf_u32(FO_KEY, relative_pid);
	stats[ST_F_IID]      = mkf_u32(FO_KEY|FS_SERVICE, px->uuid);
	stats[ST_F_SID]      = mkf_u32(FO_KEY|FS_SERVICE, l->luid);
	stats[ST_F_TYPE]     = mkf_u32(FO_CONFIG|FS_SERVICE, STATS_TYPE_SO);

	if (flags & ST_SHLGNDS) {
		char str[INET6_ADDRSTRLEN];
		int port;

		port = get_host_port(&l->addr);
		switch (addr_to_str(&l->addr, str, sizeof(str))) {
		case AF_INET:
			stats[ST_F_ADDR] = mkf_str(FO_CONFIG|FS_SERVICE, chunk_newstr(out));
			chunk_appendf(out, "%s:%d", str, port);
			break;
		case AF_INET6:
			stats[ST_F_ADDR] = mkf_str(FO_CONFIG|FS_SERVICE, chunk_newstr(out));
			chunk_appendf(out, "[%s]:%d", str, port);
			break;
		case AF_UNIX:
			stats[ST_F_ADDR] = mkf_str(FO_CONFIG|FS_SERVICE, "unix");
			break;
		case -1:
			stats[ST_F_ADDR] = mkf_str(FO_CONFIG|FS_SERVICE, chunk_newstr(out));
			chunk_strcat(out, strerror(errno));
			break;
		default: /* address family not supported */
			break;
		}
	}

	return 1;
}

/* Dumps a line for listener <l> and proxy <px> to the trash and uses the state
 * from stream interface <si>, and stats flags <flags>. The caller is responsible
 * for clearing the trash if needed. Returns non-zero if it emits anything, zero
 * otherwise.
 */
static int stats_dump_li_stats(struct stream_interface *si, struct proxy *px, struct listener *l, int flags)
{
	struct appctx *appctx = __objt_appctx(si->end);

	if (!stats_fill_li_stats(px, l, flags, stats, ST_F_TOTAL_FIELDS))
		return 0;

	return stats_dump_one_line(stats, flags, px, appctx);
}

enum srv_stats_state {
	SRV_STATS_STATE_DOWN = 0,
	SRV_STATS_STATE_DOWN_AGENT,
	SRV_STATS_STATE_GOING_UP,
	SRV_STATS_STATE_UP_GOING_DOWN,
	SRV_STATS_STATE_UP,
	SRV_STATS_STATE_NOLB_GOING_DOWN,
	SRV_STATS_STATE_NOLB,
	SRV_STATS_STATE_DRAIN_GOING_DOWN,
	SRV_STATS_STATE_DRAIN,
	SRV_STATS_STATE_DRAIN_AGENT,
	SRV_STATS_STATE_NO_CHECK,

	SRV_STATS_STATE_COUNT, /* Must be last */
};

static const char *srv_hlt_st[SRV_STATS_STATE_COUNT] = {
	[SRV_STATS_STATE_DOWN]			= "DOWN",
	[SRV_STATS_STATE_DOWN_AGENT]		= "DOWN (agent)",
	[SRV_STATS_STATE_GOING_UP]		= "DOWN %d/%d",
	[SRV_STATS_STATE_UP_GOING_DOWN]		= "UP %d/%d",
	[SRV_STATS_STATE_UP]			= "UP",
	[SRV_STATS_STATE_NOLB_GOING_DOWN]	= "NOLB %d/%d",
	[SRV_STATS_STATE_NOLB]			= "NOLB",
	[SRV_STATS_STATE_DRAIN_GOING_DOWN]	= "DRAIN %d/%d",
	[SRV_STATS_STATE_DRAIN]			= "DRAIN",
	[SRV_STATS_STATE_DRAIN_AGENT]		= "DRAIN (agent)",
	[SRV_STATS_STATE_NO_CHECK]		= "no check"
};

/* Fill <stats> with the server statistics. <stats> is
 * preallocated array of length <len>. The length of the array
 * must be at least ST_F_TOTAL_FIELDS. If this length is less
 * then this value, the function returns 0, otherwise, it
 * returns 1. <flags> can take the value ST_SHLGNDS.
 */
int stats_fill_sv_stats(struct proxy *px, struct server *sv, int flags,
                        struct field *stats, int len)
{
	struct server *via, *ref;
	char str[INET6_ADDRSTRLEN];
	struct chunk *out = get_trash_chunk();
	enum srv_stats_state state;
	char *fld_status;

	if (len < ST_F_TOTAL_FIELDS)
		return 0;

	memset(stats, 0, sizeof(*stats) * len);

	/* we have "via" which is the tracked server as described in the configuration,
	 * and "ref" which is the checked server and the end of the chain.
	 */
	via = sv->track ? sv->track : sv;
	ref = via;
	while (ref->track)
		ref = ref->track;

	if (sv->state == SRV_ST_RUNNING || sv->state == SRV_ST_STARTING) {
		if ((ref->check.state & CHK_ST_ENABLED) &&
		    (ref->check.health < ref->check.rise + ref->check.fall - 1)) {
			state = SRV_STATS_STATE_UP_GOING_DOWN;
		} else {
			state = SRV_STATS_STATE_UP;
		}

		if (sv->admin & SRV_ADMF_DRAIN) {
			if (ref->agent.state & CHK_ST_ENABLED)
				state = SRV_STATS_STATE_DRAIN_AGENT;
			else if (state == SRV_STATS_STATE_UP_GOING_DOWN)
				state = SRV_STATS_STATE_DRAIN_GOING_DOWN;
			else
				state = SRV_STATS_STATE_DRAIN;
		}

		if (state == SRV_STATS_STATE_UP && !(ref->check.state & CHK_ST_ENABLED)) {
			state = SRV_STATS_STATE_NO_CHECK;
		}
	}
	else if (sv->state == SRV_ST_STOPPING) {
		if ((!(sv->check.state & CHK_ST_ENABLED) && !sv->track) ||
		    (ref->check.health == ref->check.rise + ref->check.fall - 1)) {
			state = SRV_STATS_STATE_NOLB;
		} else {
			state = SRV_STATS_STATE_NOLB_GOING_DOWN;
		}
	}
	else {	/* stopped */
		if ((ref->agent.state & CHK_ST_ENABLED) && !ref->agent.health) {
			state = SRV_STATS_STATE_DOWN_AGENT;
		} else if ((ref->check.state & CHK_ST_ENABLED) && !ref->check.health) {
			state = SRV_STATS_STATE_DOWN; /* DOWN */
		} else if ((ref->agent.state & CHK_ST_ENABLED) || (ref->check.state & CHK_ST_ENABLED)) {
			state = SRV_STATS_STATE_GOING_UP;
		} else {
			state = SRV_STATS_STATE_DOWN; /* DOWN, unchecked */
		}
	}

	chunk_reset(out);

	stats[ST_F_PXNAME]   = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, px->id);
	stats[ST_F_SVNAME]   = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, sv->id);
	stats[ST_F_MODE]     = mkf_str(FO_CONFIG|FS_SERVICE, proxy_mode_str(px->mode));
	stats[ST_F_QCUR]     = mkf_u32(0, sv->nbpend);
	stats[ST_F_QMAX]     = mkf_u32(FN_MAX, sv->counters.nbpend_max);
	stats[ST_F_SCUR]     = mkf_u32(0, sv->cur_sess);
	stats[ST_F_SMAX]     = mkf_u32(FN_MAX, sv->counters.cur_sess_max);

	if (sv->maxconn)
		stats[ST_F_SLIM] = mkf_u32(FO_CONFIG|FN_LIMIT, sv->maxconn);

	stats[ST_F_STOT]     = mkf_u64(FN_COUNTER, sv->counters.cum_sess);
	stats[ST_F_BIN]      = mkf_u64(FN_COUNTER, sv->counters.bytes_in);
	stats[ST_F_BOUT]     = mkf_u64(FN_COUNTER, sv->counters.bytes_out);
	stats[ST_F_DRESP]    = mkf_u64(FN_COUNTER, sv->counters.failed_secu);
	stats[ST_F_ECON]     = mkf_u64(FN_COUNTER, sv->counters.failed_conns);
	stats[ST_F_ERESP]    = mkf_u64(FN_COUNTER, sv->counters.failed_resp);
	stats[ST_F_WRETR]    = mkf_u64(FN_COUNTER, sv->counters.retries);
	stats[ST_F_WREDIS]   = mkf_u64(FN_COUNTER, sv->counters.redispatches);

	/* status */
	fld_status = chunk_newstr(out);
	if (sv->admin & SRV_ADMF_IMAINT)
		chunk_appendf(out, "MAINT (via %s/%s)", via->proxy->id, via->id);
	else if (sv->admin & SRV_ADMF_MAINT)
		chunk_appendf(out, "MAINT");
	else
		chunk_appendf(out,
			      srv_hlt_st[state],
			      (ref->state != SRV_ST_STOPPED) ? (ref->check.health - ref->check.rise + 1) : (ref->check.health),
			      (ref->state != SRV_ST_STOPPED) ? (ref->check.fall) : (ref->check.rise));

	stats[ST_F_STATUS]   = mkf_str(FO_STATUS, fld_status);
	stats[ST_F_LASTCHG]  = mkf_u32(FN_AGE, now.tv_sec - sv->last_change);
	stats[ST_F_WEIGHT]   = mkf_u32(FN_AVG, (sv->eweight * px->lbprm.wmult + px->lbprm.wdiv - 1) / px->lbprm.wdiv);
	stats[ST_F_ACT]      = mkf_u32(FO_STATUS, (sv->flags & SRV_F_BACKUP) ? 0 : 1);
	stats[ST_F_BCK]      = mkf_u32(FO_STATUS, (sv->flags & SRV_F_BACKUP) ? 1 : 0);

	/* check failures: unique, fatal; last change, total downtime */
	if (sv->check.state & CHK_ST_ENABLED) {
		stats[ST_F_CHKFAIL]  = mkf_u64(FN_COUNTER, sv->counters.failed_checks);
		stats[ST_F_CHKDOWN]  = mkf_u64(FN_COUNTER, sv->counters.down_trans);
		stats[ST_F_DOWNTIME] = mkf_u32(FN_COUNTER, srv_downtime(sv));
	}

	if (sv->maxqueue)
		stats[ST_F_QLIMIT]   = mkf_u32(FO_CONFIG|FS_SERVICE, sv->maxqueue);

	stats[ST_F_PID]      = mkf_u32(FO_KEY, relative_pid);
	stats[ST_F_IID]      = mkf_u32(FO_KEY|FS_SERVICE, px->uuid);
	stats[ST_F_SID]      = mkf_u32(FO_KEY|FS_SERVICE, sv->puid);

	if (sv->state == SRV_ST_STARTING && !server_is_draining(sv))
		stats[ST_F_THROTTLE] = mkf_u32(FN_AVG, server_throttle_rate(sv));

	stats[ST_F_LBTOT]    = mkf_u64(FN_COUNTER, sv->counters.cum_lbconn);

	if (sv->track) {
		char *fld_track = chunk_newstr(out);

		chunk_appendf(out, "%s/%s", sv->track->proxy->id, sv->track->id);
		stats[ST_F_TRACKED] = mkf_str(FO_CONFIG|FN_NAME|FS_SERVICE, fld_track);
	}

	stats[ST_F_TYPE]     = mkf_u32(FO_CONFIG|FS_SERVICE, STATS_TYPE_SV);
	stats[ST_F_RATE]     = mkf_u32(FN_RATE, read_freq_ctr(&sv->sess_per_sec));
	stats[ST_F_RATE_MAX] = mkf_u32(FN_MAX, sv->counters.sps_max);

	if ((sv->check.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED) {
		const char *fld_chksts;

		fld_chksts = chunk_newstr(out);
		chunk_strcat(out, "* "); // for check in progress
		chunk_strcat(out, get_check_status_info(sv->check.status));
		if (!(sv->check.state & CHK_ST_INPROGRESS))
			fld_chksts += 2; // skip "* "
		stats[ST_F_CHECK_STATUS] = mkf_str(FN_OUTPUT, fld_chksts);

		if (sv->check.status >= HCHK_STATUS_L57DATA)
			stats[ST_F_CHECK_CODE] = mkf_u32(FN_OUTPUT, sv->check.code);

		if (sv->check.status >= HCHK_STATUS_CHECKED)
			stats[ST_F_CHECK_DURATION] = mkf_u64(FN_DURATION, sv->check.duration);

		stats[ST_F_CHECK_DESC] = mkf_str(FN_OUTPUT, get_check_status_description(sv->check.status));
		stats[ST_F_LAST_CHK] = mkf_str(FN_OUTPUT, sv->check.desc);
		stats[ST_F_CHECK_RISE]   = mkf_u32(FO_CONFIG|FS_SERVICE, ref->check.rise);
		stats[ST_F_CHECK_FALL]   = mkf_u32(FO_CONFIG|FS_SERVICE, ref->check.fall);
		stats[ST_F_CHECK_HEALTH] = mkf_u32(FO_CONFIG|FS_SERVICE, ref->check.health);
	}

	if ((sv->agent.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED) {
		const char *fld_chksts;

		fld_chksts = chunk_newstr(out);
		chunk_strcat(out, "* "); // for check in progress
		chunk_strcat(out, get_check_status_info(sv->agent.status));
		if (!(sv->agent.state & CHK_ST_INPROGRESS))
			fld_chksts += 2; // skip "* "
		stats[ST_F_AGENT_STATUS] = mkf_str(FN_OUTPUT, fld_chksts);

		if (sv->agent.status >= HCHK_STATUS_L57DATA)
			stats[ST_F_AGENT_CODE] = mkf_u32(FN_OUTPUT, sv->agent.code);

		if (sv->agent.status >= HCHK_STATUS_CHECKED)
			stats[ST_F_AGENT_DURATION] = mkf_u64(FN_DURATION, sv->agent.duration);

		stats[ST_F_AGENT_DESC] = mkf_str(FN_OUTPUT, get_check_status_description(sv->agent.status));
		stats[ST_F_LAST_AGT] = mkf_str(FN_OUTPUT, sv->agent.desc);
		stats[ST_F_AGENT_RISE]   = mkf_u32(FO_CONFIG|FS_SERVICE, sv->agent.rise);
		stats[ST_F_AGENT_FALL]   = mkf_u32(FO_CONFIG|FS_SERVICE, sv->agent.fall);
		stats[ST_F_AGENT_HEALTH] = mkf_u32(FO_CONFIG|FS_SERVICE, sv->agent.health);
	}

	/* http response: 1xx, 2xx, 3xx, 4xx, 5xx, other */
	if (px->mode == PR_MODE_HTTP) {
		stats[ST_F_HRSP_1XX]   = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[1]);
		stats[ST_F_HRSP_2XX]   = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[2]);
		stats[ST_F_HRSP_3XX]   = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[3]);
		stats[ST_F_HRSP_4XX]   = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[4]);
		stats[ST_F_HRSP_5XX]   = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[5]);
		stats[ST_F_HRSP_OTHER] = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[0]);
	}

	if (ref->observe)
		stats[ST_F_HANAFAIL] = mkf_u64(FN_COUNTER, sv->counters.failed_hana);

	stats[ST_F_CLI_ABRT] = mkf_u64(FN_COUNTER, sv->counters.cli_aborts);
	stats[ST_F_SRV_ABRT] = mkf_u64(FN_COUNTER, sv->counters.srv_aborts);
	stats[ST_F_LASTSESS] = mkf_s32(FN_AGE, srv_lastsession(sv));

	stats[ST_F_QTIME] = mkf_u32(FN_AVG, swrate_avg(sv->counters.q_time, TIME_STATS_SAMPLES));
	stats[ST_F_CTIME] = mkf_u32(FN_AVG, swrate_avg(sv->counters.c_time, TIME_STATS_SAMPLES));
	stats[ST_F_RTIME] = mkf_u32(FN_AVG, swrate_avg(sv->counters.d_time, TIME_STATS_SAMPLES));
	stats[ST_F_TTIME] = mkf_u32(FN_AVG, swrate_avg(sv->counters.t_time, TIME_STATS_SAMPLES));

	if (flags & ST_SHLGNDS) {
		switch (addr_to_str(&sv->addr, str, sizeof(str))) {
		case AF_INET:
			stats[ST_F_ADDR] = mkf_str(FO_CONFIG|FS_SERVICE, chunk_newstr(out));
			chunk_appendf(out, "%s:%d", str, get_host_port(&sv->addr));
			break;
		case AF_INET6:
			stats[ST_F_ADDR] = mkf_str(FO_CONFIG|FS_SERVICE, chunk_newstr(out));
			chunk_appendf(out, "[%s]:%d", str, get_host_port(&sv->addr));
			break;
		case AF_UNIX:
			stats[ST_F_ADDR] = mkf_str(FO_CONFIG|FS_SERVICE, "unix");
			break;
		case -1:
			stats[ST_F_ADDR] = mkf_str(FO_CONFIG|FS_SERVICE, chunk_newstr(out));
			chunk_strcat(out, strerror(errno));
			break;
		default: /* address family not supported */
			break;
		}

		if (sv->cookie)
			stats[ST_F_COOKIE] = mkf_str(FO_CONFIG|FN_NAME|FS_SERVICE, sv->cookie);
	}

	return 1;
}

/* Dumps a line for server <sv> and proxy <px> to the trash and uses the state
 * from stream interface <si>, stats flags <flags>, and server state <state>.
 * The caller is responsible for clearing the trash if needed. Returns non-zero
 * if it emits anything, zero otherwise.
 */
static int stats_dump_sv_stats(struct stream_interface *si, struct proxy *px, int flags, struct server *sv)
{
	struct appctx *appctx = __objt_appctx(si->end);

	if (!stats_fill_sv_stats(px, sv, flags, stats, ST_F_TOTAL_FIELDS))
		return 0;

	return stats_dump_one_line(stats, flags, px, appctx);
}

/* Fill <stats> with the backend statistics. <stats> is
 * preallocated array of length <len>. The length of the array
 * must be at least ST_F_TOTAL_FIELDS. If this length is less
 * then this value, the function returns 0, otherwise, it
 * returns 1. <flags> can take the value ST_SHLGNDS.
 */
int stats_fill_be_stats(struct proxy *px, int flags, struct field *stats, int len)
{
	if (len < ST_F_TOTAL_FIELDS)
		return 0;

	memset(stats, 0, sizeof(*stats) * len);

	stats[ST_F_PXNAME]   = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, px->id);
	stats[ST_F_SVNAME]   = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, "BACKEND");
	stats[ST_F_MODE]     = mkf_str(FO_CONFIG|FS_SERVICE, proxy_mode_str(px->mode));
	stats[ST_F_QCUR]     = mkf_u32(0, px->nbpend);
	stats[ST_F_QMAX]     = mkf_u32(FN_MAX, px->be_counters.nbpend_max);
	stats[ST_F_SCUR]     = mkf_u32(FO_CONFIG|FN_LIMIT, px->beconn);
	stats[ST_F_SMAX]     = mkf_u32(FN_MAX, px->be_counters.conn_max);
	stats[ST_F_SLIM]     = mkf_u32(FO_CONFIG|FN_LIMIT, px->fullconn);
	stats[ST_F_STOT]     = mkf_u64(FN_COUNTER, px->be_counters.cum_conn);
	stats[ST_F_BIN]      = mkf_u64(FN_COUNTER, px->be_counters.bytes_in);
	stats[ST_F_BOUT]     = mkf_u64(FN_COUNTER, px->be_counters.bytes_out);
	stats[ST_F_DREQ]     = mkf_u64(FN_COUNTER, px->be_counters.denied_req);
	stats[ST_F_DRESP]    = mkf_u64(FN_COUNTER, px->be_counters.denied_resp);
	stats[ST_F_ECON]     = mkf_u64(FN_COUNTER, px->be_counters.failed_conns);
	stats[ST_F_ERESP]    = mkf_u64(FN_COUNTER, px->be_counters.failed_resp);
	stats[ST_F_WRETR]    = mkf_u64(FN_COUNTER, px->be_counters.retries);
	stats[ST_F_WREDIS]   = mkf_u64(FN_COUNTER, px->be_counters.redispatches);
	stats[ST_F_STATUS]   = mkf_str(FO_STATUS, (px->lbprm.tot_weight > 0 || !px->srv) ? "UP" : "DOWN");
	stats[ST_F_WEIGHT]   = mkf_u32(FN_AVG, (px->lbprm.tot_weight * px->lbprm.wmult + px->lbprm.wdiv - 1) / px->lbprm.wdiv);
	stats[ST_F_ACT]      = mkf_u32(0, px->srv_act);
	stats[ST_F_BCK]      = mkf_u32(0, px->srv_bck);
	stats[ST_F_CHKDOWN]  = mkf_u64(FN_COUNTER, px->down_trans);
	stats[ST_F_LASTCHG]  = mkf_u32(FN_AGE, now.tv_sec - px->last_change);
	if (px->srv)
		stats[ST_F_DOWNTIME] = mkf_u32(FN_COUNTER, be_downtime(px));

	stats[ST_F_PID]      = mkf_u32(FO_KEY, relative_pid);
	stats[ST_F_IID]      = mkf_u32(FO_KEY|FS_SERVICE, px->uuid);
	stats[ST_F_SID]      = mkf_u32(FO_KEY|FS_SERVICE, 0);
	stats[ST_F_LBTOT]    = mkf_u64(FN_COUNTER, px->be_counters.cum_lbconn);
	stats[ST_F_TYPE]     = mkf_u32(FO_CONFIG|FS_SERVICE, STATS_TYPE_BE);
	stats[ST_F_RATE]     = mkf_u32(0, read_freq_ctr(&px->be_sess_per_sec));
	stats[ST_F_RATE_MAX] = mkf_u32(0, px->be_counters.sps_max);

	if (flags & ST_SHLGNDS) {
		if (px->cookie_name)
			stats[ST_F_COOKIE] = mkf_str(FO_CONFIG|FN_NAME|FS_SERVICE, px->cookie_name);
		stats[ST_F_ALGO] = mkf_str(FO_CONFIG|FS_SERVICE, backend_lb_algo_str(px->lbprm.algo & BE_LB_ALGO));
	}

	/* http response: 1xx, 2xx, 3xx, 4xx, 5xx, other */
	if (px->mode == PR_MODE_HTTP) {
		stats[ST_F_REQ_TOT]     = mkf_u64(FN_COUNTER, px->be_counters.p.http.cum_req);
		stats[ST_F_HRSP_1XX]    = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[1]);
		stats[ST_F_HRSP_2XX]    = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[2]);
		stats[ST_F_HRSP_3XX]    = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[3]);
		stats[ST_F_HRSP_4XX]    = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[4]);
		stats[ST_F_HRSP_5XX]    = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[5]);
		stats[ST_F_HRSP_OTHER]  = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[0]);
		stats[ST_F_INTERCEPTED] = mkf_u64(FN_COUNTER, px->be_counters.intercepted_req);
	}

	stats[ST_F_CLI_ABRT]     = mkf_u64(FN_COUNTER, px->be_counters.cli_aborts);
	stats[ST_F_SRV_ABRT]     = mkf_u64(FN_COUNTER, px->be_counters.srv_aborts);

	/* compression: in, out, bypassed, responses */
	stats[ST_F_COMP_IN]      = mkf_u64(FN_COUNTER, px->be_counters.comp_in);
	stats[ST_F_COMP_OUT]     = mkf_u64(FN_COUNTER, px->be_counters.comp_out);
	stats[ST_F_COMP_BYP]     = mkf_u64(FN_COUNTER, px->be_counters.comp_byp);
	stats[ST_F_COMP_RSP]     = mkf_u64(FN_COUNTER, px->be_counters.p.http.comp_rsp);
	stats[ST_F_LASTSESS]     = mkf_s32(FN_AGE, be_lastsession(px));

	stats[ST_F_QTIME]        = mkf_u32(FN_AVG, swrate_avg(px->be_counters.q_time, TIME_STATS_SAMPLES));
	stats[ST_F_CTIME]        = mkf_u32(FN_AVG, swrate_avg(px->be_counters.c_time, TIME_STATS_SAMPLES));
	stats[ST_F_RTIME]        = mkf_u32(FN_AVG, swrate_avg(px->be_counters.d_time, TIME_STATS_SAMPLES));
	stats[ST_F_TTIME]        = mkf_u32(FN_AVG, swrate_avg(px->be_counters.t_time, TIME_STATS_SAMPLES));

	return 1;
}

/* Dumps a line for backend <px> to the trash for and uses the state from stream
 * interface <si> and stats flags <flags>. The caller is responsible for clearing
 * the trash if needed. Returns non-zero if it emits anything, zero otherwise.
 */
static int stats_dump_be_stats(struct stream_interface *si, struct proxy *px, int flags)
{
	struct appctx *appctx = __objt_appctx(si->end);

	if (!(px->cap & PR_CAP_BE))
		return 0;

	if ((appctx->ctx.stats.flags & STAT_BOUND) && !(appctx->ctx.stats.type & (1 << STATS_TYPE_BE)))
		return 0;

	if (!stats_fill_be_stats(px, flags, stats, ST_F_TOTAL_FIELDS))
		return 0;

	return stats_dump_one_line(stats, flags, px, appctx);
}

/* Dumps the HTML table header for proxy <px> to the trash for and uses the state from
 * stream interface <si> and per-uri parameters <uri>. The caller is responsible
 * for clearing the trash if needed.
 */
static void stats_dump_html_px_hdr(struct stream_interface *si, struct proxy *px, struct uri_auth *uri)
{
	struct appctx *appctx = __objt_appctx(si->end);
	char scope_txt[STAT_SCOPE_TXT_MAXLEN + sizeof STAT_SCOPE_PATTERN];

	if (px->cap & PR_CAP_BE && px->srv && (appctx->ctx.stats.flags & STAT_ADMIN)) {
		/* A form to enable/disable this proxy servers */

		/* scope_txt = search pattern + search query, appctx->ctx.stats.scope_len is always <= STAT_SCOPE_TXT_MAXLEN */
		scope_txt[0] = 0;
		if (appctx->ctx.stats.scope_len) {
			strcpy(scope_txt, STAT_SCOPE_PATTERN);
			memcpy(scope_txt + strlen(STAT_SCOPE_PATTERN), bo_ptr(si_ob(si)) + appctx->ctx.stats.scope_str, appctx->ctx.stats.scope_len);
			scope_txt[strlen(STAT_SCOPE_PATTERN) + appctx->ctx.stats.scope_len] = 0;
		}

		chunk_appendf(&trash,
			      "<form method=\"post\">");
	}

	/* print a new table */
	chunk_appendf(&trash,
		      "<table class=\"tbl\" width=\"100%%\">\n"
		      "<tr class=\"titre\">"
		      "<th class=\"pxname\" width=\"10%%\">");

	chunk_appendf(&trash,
	              "<a name=\"%s\"></a>%s"
	              "<a class=px href=\"#%s\">%s</a>",
	              px->id,
	              (uri->flags & ST_SHLGNDS) ? "<u>":"",
	              px->id, px->id);

	if (uri->flags & ST_SHLGNDS) {
		/* cap, mode, id */
		chunk_appendf(&trash, "<div class=tips>cap: %s, mode: %s, id: %d",
		              proxy_cap_str(px->cap), proxy_mode_str(px->mode),
		              px->uuid);
		chunk_appendf(&trash, "</div>");
	}

	chunk_appendf(&trash,
	              "%s</th>"
	              "<th class=\"%s\" width=\"90%%\">%s</th>"
	              "</tr>\n"
	              "</table>\n"
	              "<table class=\"tbl\" width=\"100%%\">\n"
	              "<tr class=\"titre\">",
	              (uri->flags & ST_SHLGNDS) ? "</u>":"",
	              px->desc ? "desc" : "empty", px->desc ? px->desc : "");

	if ((px->cap & PR_CAP_BE) && px->srv && (appctx->ctx.stats.flags & STAT_ADMIN)) {
		/* Column heading for Enable or Disable server */
		chunk_appendf(&trash, "<th rowspan=2 width=1></th>");
	}

	chunk_appendf(&trash,
	              "<th rowspan=2></th>"
	              "<th colspan=3>Queue</th>"
	              "<th colspan=3>Session rate</th><th colspan=6>Sessions</th>"
	              "<th colspan=2>Bytes</th><th colspan=2>Denied</th>"
	              "<th colspan=3>Errors</th><th colspan=2>Warnings</th>"
	              "<th colspan=9>Server</th>"
	              "</tr>\n"
	              "<tr class=\"titre\">"
	              "<th>Cur</th><th>Max</th><th>Limit</th>"
	              "<th>Cur</th><th>Max</th><th>Limit</th><th>Cur</th><th>Max</th>"
	              "<th>Limit</th><th>Total</th><th>LbTot</th><th>Last</th><th>In</th><th>Out</th>"
	              "<th>Req</th><th>Resp</th><th>Req</th><th>Conn</th>"
	              "<th>Resp</th><th>Retr</th><th>Redis</th>"
	              "<th>Status</th><th>LastChk</th><th>Wght</th><th>Act</th>"
	              "<th>Bck</th><th>Chk</th><th>Dwn</th><th>Dwntme</th>"
	              "<th>Thrtle</th>\n"
	              "</tr>");
}

/* Dumps the HTML table trailer for proxy <px> to the trash for and uses the state from
 * stream interface <si>. The caller is responsible for clearing the trash if needed.
 */
static void stats_dump_html_px_end(struct stream_interface *si, struct proxy *px)
{
	struct appctx *appctx = __objt_appctx(si->end);
	chunk_appendf(&trash, "</table>");

	if ((px->cap & PR_CAP_BE) && px->srv && (appctx->ctx.stats.flags & STAT_ADMIN)) {
		/* close the form used to enable/disable this proxy servers */
		chunk_appendf(&trash,
			      "Choose the action to perform on the checked servers : "
			      "<select name=action>"
			      "<option value=\"\"></option>"
			      "<option value=\"ready\">Set state to READY</option>"
			      "<option value=\"drain\">Set state to DRAIN</option>"
			      "<option value=\"maint\">Set state to MAINT</option>"
			      "<option value=\"dhlth\">Health: disable checks</option>"
			      "<option value=\"ehlth\">Health: enable checks</option>"
			      "<option value=\"hrunn\">Health: force UP</option>"
			      "<option value=\"hnolb\">Health: force NOLB</option>"
			      "<option value=\"hdown\">Health: force DOWN</option>"
			      "<option value=\"dagent\">Agent: disable checks</option>"
			      "<option value=\"eagent\">Agent: enable checks</option>"
			      "<option value=\"arunn\">Agent: force UP</option>"
			      "<option value=\"adown\">Agent: force DOWN</option>"
			      "<option value=\"shutdown\">Kill Sessions</option>"
			      "</select>"
			      "<input type=\"hidden\" name=\"b\" value=\"#%d\">"
			      "&nbsp;<input type=\"submit\" value=\"Apply\">"
			      "</form>",
			      px->uuid);
	}

	chunk_appendf(&trash, "<p>\n");
}

/*
 * Dumps statistics for a proxy. The output is sent to the stream interface's
 * input buffer. Returns 0 if it had to stop dumping data because of lack of
 * buffer space, or non-zero if everything completed. This function is used
 * both by the CLI and the HTTP entry points, and is able to dump the output
 * in HTML or CSV formats. If the later, <uri> must be NULL.
 */
static int stats_dump_proxy_to_buffer(struct stream_interface *si, struct proxy *px, struct uri_auth *uri)
{
	struct appctx *appctx = __objt_appctx(si->end);
	struct stream *s = si_strm(si);
	struct channel *rep = si_ic(si);
	struct server *sv, *svs;	/* server and server-state, server-state=server or server->track */
	struct listener *l;
	unsigned int flags;

	if (uri)
		flags = uri->flags;
	else if (strm_li(s)->bind_conf->level >= ACCESS_LVL_OPER)
		flags = ST_SHLGNDS | ST_SHNODE | ST_SHDESC;
	else
		flags = ST_SHNODE | ST_SHDESC;

	chunk_reset(&trash);

	switch (appctx->ctx.stats.px_st) {
	case STAT_PX_ST_INIT:
		/* we are on a new proxy */
		if (uri && uri->scope) {
			/* we have a limited scope, we have to check the proxy name */
			struct stat_scope *scope;
			int len;

			len = strlen(px->id);
			scope = uri->scope;

			while (scope) {
				/* match exact proxy name */
				if (scope->px_len == len && !memcmp(px->id, scope->px_id, len))
					break;

				/* match '.' which means 'self' proxy */
				if (!strcmp(scope->px_id, ".") && px == s->be)
					break;
				scope = scope->next;
			}

			/* proxy name not found : don't dump anything */
			if (scope == NULL)
				return 1;
		}

		/* if the user has requested a limited output and the proxy
		 * name does not match, skip it.
		 */
		if (appctx->ctx.stats.scope_len &&
		    strnistr(px->id, strlen(px->id), bo_ptr(si_ob(si)) + appctx->ctx.stats.scope_str, appctx->ctx.stats.scope_len) == NULL)
			return 1;

		if ((appctx->ctx.stats.flags & STAT_BOUND) &&
		    (appctx->ctx.stats.iid != -1) &&
		    (px->uuid != appctx->ctx.stats.iid))
			return 1;

		appctx->ctx.stats.px_st = STAT_PX_ST_TH;
		/* fall through */

	case STAT_PX_ST_TH:
		if (appctx->ctx.stats.flags & STAT_FMT_HTML) {
			stats_dump_html_px_hdr(si, px, uri);
			if (bi_putchk(rep, &trash) == -1) {
				si_applet_cant_put(si);
				return 0;
			}
		}

		appctx->ctx.stats.px_st = STAT_PX_ST_FE;
		/* fall through */

	case STAT_PX_ST_FE:
		/* print the frontend */
		if (stats_dump_fe_stats(si, px)) {
			if (bi_putchk(rep, &trash) == -1) {
				si_applet_cant_put(si);
				return 0;
			}
		}

		appctx->ctx.stats.l = px->conf.listeners.n;
		appctx->ctx.stats.px_st = STAT_PX_ST_LI;
		/* fall through */

	case STAT_PX_ST_LI:
		/* stats.l has been initialized above */
		for (; appctx->ctx.stats.l != &px->conf.listeners; appctx->ctx.stats.l = l->by_fe.n) {
			if (buffer_almost_full(rep->buf)) {
				si_applet_cant_put(si);
				return 0;
			}

			l = LIST_ELEM(appctx->ctx.stats.l, struct listener *, by_fe);
			if (!l->counters)
				continue;

			if (appctx->ctx.stats.flags & STAT_BOUND) {
				if (!(appctx->ctx.stats.type & (1 << STATS_TYPE_SO)))
					break;

				if (appctx->ctx.stats.sid != -1 && l->luid != appctx->ctx.stats.sid)
					continue;
			}

			/* print the frontend */
			if (stats_dump_li_stats(si, px, l, flags)) {
				if (bi_putchk(rep, &trash) == -1) {
					si_applet_cant_put(si);
					return 0;
				}
			}
		}

		appctx->ctx.stats.sv = px->srv; /* may be NULL */
		appctx->ctx.stats.px_st = STAT_PX_ST_SV;
		/* fall through */

	case STAT_PX_ST_SV:
		/* stats.sv has been initialized above */
		for (; appctx->ctx.stats.sv != NULL; appctx->ctx.stats.sv = sv->next) {
			if (buffer_almost_full(rep->buf)) {
				si_applet_cant_put(si);
				return 0;
			}

			sv = appctx->ctx.stats.sv;

			if (appctx->ctx.stats.flags & STAT_BOUND) {
				if (!(appctx->ctx.stats.type & (1 << STATS_TYPE_SV)))
					break;

				if (appctx->ctx.stats.sid != -1 && sv->puid != appctx->ctx.stats.sid)
					continue;
			}

			svs = sv;
			while (svs->track)
				svs = svs->track;

			/* do not report servers which are DOWN and not changing state */
			if ((appctx->ctx.stats.flags & STAT_HIDE_DOWN) &&
			    ((sv->admin & SRV_ADMF_MAINT) || /* server is in maintenance */
			     (sv->state == SRV_ST_STOPPED && /* server is down */
			      (!((svs->agent.state | svs->check.state) & CHK_ST_ENABLED) ||
			       ((svs->agent.state & CHK_ST_ENABLED) && !svs->agent.health) ||
			       ((svs->check.state & CHK_ST_ENABLED) && !svs->check.health))))) {
				continue;
			}

			if (stats_dump_sv_stats(si, px, flags, sv)) {
				if (bi_putchk(rep, &trash) == -1) {
					si_applet_cant_put(si);
					return 0;
				}
			}
		} /* for sv */

		appctx->ctx.stats.px_st = STAT_PX_ST_BE;
		/* fall through */

	case STAT_PX_ST_BE:
		/* print the backend */
		if (stats_dump_be_stats(si, px, flags)) {
			if (bi_putchk(rep, &trash) == -1) {
				si_applet_cant_put(si);
				return 0;
			}
		}

		appctx->ctx.stats.px_st = STAT_PX_ST_END;
		/* fall through */

	case STAT_PX_ST_END:
		if (appctx->ctx.stats.flags & STAT_FMT_HTML) {
			stats_dump_html_px_end(si, px);
			if (bi_putchk(rep, &trash) == -1) {
				si_applet_cant_put(si);
				return 0;
			}
		}

		appctx->ctx.stats.px_st = STAT_PX_ST_FIN;
		/* fall through */

	case STAT_PX_ST_FIN:
		return 1;

	default:
		/* unknown state, we should put an abort() here ! */
		return 1;
	}
}

/* Dumps the HTTP stats head block to the trash for and uses the per-uri
 * parameters <uri>. The caller is responsible for clearing the trash if needed.
 */
static void stats_dump_html_head(struct uri_auth *uri)
{
	/* WARNING! This must fit in the first buffer !!! */
	chunk_appendf(&trash,
	              "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\"\n"
	              "\"http://www.w3.org/TR/html4/loose.dtd\">\n"
	              "<html><head><title>Statistics Report for " PRODUCT_NAME "%s%s</title>\n"
	              "<meta http-equiv=\"content-type\" content=\"text/html; charset=iso-8859-1\">\n"
	              "<style type=\"text/css\"><!--\n"
	              "body {"
	              " font-family: arial, helvetica, sans-serif;"
	              " font-size: 12px;"
	              " font-weight: normal;"
	              " color: black;"
	              " background: white;"
	              "}\n"
	              "th,td {"
	              " font-size: 10px;"
	              "}\n"
	              "h1 {"
	              " font-size: x-large;"
	              " margin-bottom: 0.5em;"
	              "}\n"
	              "h2 {"
	              " font-family: helvetica, arial;"
	              " font-size: x-large;"
	              " font-weight: bold;"
	              " font-style: italic;"
	              " color: #6020a0;"
	              " margin-top: 0em;"
	              " margin-bottom: 0em;"
	              "}\n"
	              "h3 {"
	              " font-family: helvetica, arial;"
	              " font-size: 16px;"
	              " font-weight: bold;"
	              " color: #b00040;"
	              " background: #e8e8d0;"
	              " margin-top: 0em;"
	              " margin-bottom: 0em;"
	              "}\n"
	              "li {"
	              " margin-top: 0.25em;"
	              " margin-right: 2em;"
	              "}\n"
	              ".hr {margin-top: 0.25em;"
	              " border-color: black;"
	              " border-bottom-style: solid;"
	              "}\n"
	              ".titre	{background: #20D0D0;color: #000000; font-weight: bold; text-align: center;}\n"
	              ".total	{background: #20D0D0;color: #ffff80;}\n"
	              ".frontend	{background: #e8e8d0;}\n"
	              ".socket	{background: #d0d0d0;}\n"
	              ".backend	{background: #e8e8d0;}\n"
	              ".active_down		{background: #ff9090;}\n"
	              ".active_going_up		{background: #ffd020;}\n"
	              ".active_going_down	{background: #ffffa0;}\n"
	              ".active_up		{background: #c0ffc0;}\n"
	              ".active_nolb		{background: #20a0ff;}\n"
	              ".active_draining		{background: #20a0FF;}\n"
	              ".active_no_check		{background: #e0e0e0;}\n"
	              ".backup_down		{background: #ff9090;}\n"
	              ".backup_going_up		{background: #ff80ff;}\n"
	              ".backup_going_down	{background: #c060ff;}\n"
	              ".backup_up		{background: #b0d0ff;}\n"
	              ".backup_nolb		{background: #90b0e0;}\n"
	              ".backup_draining		{background: #cc9900;}\n"
	              ".backup_no_check		{background: #e0e0e0;}\n"
	              ".maintain	{background: #c07820;}\n"
	              ".rls      {letter-spacing: 0.2em; margin-right: 1px;}\n" /* right letter spacing (used for grouping digits) */
	              "\n"
	              "a.px:link {color: #ffff40; text-decoration: none;}"
	              "a.px:visited {color: #ffff40; text-decoration: none;}"
	              "a.px:hover {color: #ffffff; text-decoration: none;}"
	              "a.lfsb:link {color: #000000; text-decoration: none;}"
	              "a.lfsb:visited {color: #000000; text-decoration: none;}"
	              "a.lfsb:hover {color: #505050; text-decoration: none;}"
	              "\n"
	              "table.tbl { border-collapse: collapse; border-style: none;}\n"
	              "table.tbl td { text-align: right; border-width: 1px 1px 1px 1px; border-style: solid solid solid solid; padding: 2px 3px; border-color: gray; white-space: nowrap;}\n"
	              "table.tbl td.ac { text-align: center;}\n"
	              "table.tbl th { border-width: 1px; border-style: solid solid solid solid; border-color: gray;}\n"
	              "table.tbl th.pxname { background: #b00040; color: #ffff40; font-weight: bold; border-style: solid solid none solid; padding: 2px 3px; white-space: nowrap;}\n"
	              "table.tbl th.empty { border-style: none; empty-cells: hide; background: white;}\n"
	              "table.tbl th.desc { background: white; border-style: solid solid none solid; text-align: left; padding: 2px 3px;}\n"
	              "\n"
	              "table.lgd { border-collapse: collapse; border-width: 1px; border-style: none none none solid; border-color: black;}\n"
	              "table.lgd td { border-width: 1px; border-style: solid solid solid solid; border-color: gray; padding: 2px;}\n"
	              "table.lgd td.noborder { border-style: none; padding: 2px; white-space: nowrap;}\n"
	              "table.det { border-collapse: collapse; border-style: none; }\n"
	              "table.det th { text-align: left; border-width: 0px; padding: 0px 1px 0px 0px; font-style:normal;font-size:11px;font-weight:bold;font-family: sans-serif;}\n"
	              "table.det td { text-align: right; border-width: 0px; padding: 0px 0px 0px 4px; white-space: nowrap; font-style:normal;font-size:11px;font-weight:normal;}\n"
	              "u {text-decoration:none; border-bottom: 1px dotted black;}\n"
		      "div.tips {\n"
		      " display:block;\n"
		      " visibility:hidden;\n"
		      " z-index:2147483647;\n"
		      " position:absolute;\n"
		      " padding:2px 4px 3px;\n"
		      " background:#f0f060; color:#000000;\n"
		      " border:1px solid #7040c0;\n"
		      " white-space:nowrap;\n"
		      " font-style:normal;font-size:11px;font-weight:normal;\n"
		      " -moz-border-radius:3px;-webkit-border-radius:3px;border-radius:3px;\n"
		      " -moz-box-shadow:gray 2px 2px 3px;-webkit-box-shadow:gray 2px 2px 3px;box-shadow:gray 2px 2px 3px;\n"
		      "}\n"
		      "u:hover div.tips {visibility:visible;}\n"
	              "-->\n"
	              "</style></head>\n",
	              (uri->flags & ST_SHNODE) ? " on " : "",
	              (uri->flags & ST_SHNODE) ? (uri->node ? uri->node : global.node) : ""
	              );
}

/* Dumps the HTML stats information block to the trash for and uses the state from
 * stream interface <si> and per-uri parameters <uri>. The caller is responsible
 * for clearing the trash if needed.
 */
static void stats_dump_html_info(struct stream_interface *si, struct uri_auth *uri)
{
	struct appctx *appctx = __objt_appctx(si->end);
	unsigned int up = (now.tv_sec - start_date.tv_sec);
	char scope_txt[STAT_SCOPE_TXT_MAXLEN + sizeof STAT_SCOPE_PATTERN];

	/* WARNING! this has to fit the first packet too.
	 * We are around 3.5 kB, add adding entries will
	 * become tricky if we want to support 4kB buffers !
	 */
	chunk_appendf(&trash,
	              "<body><h1><a href=\"" PRODUCT_URL "\" style=\"text-decoration: none;\">"
	              PRODUCT_NAME "%s</a></h1>\n"
	              "<h2>Statistics Report for pid %d%s%s%s%s</h2>\n"
	              "<hr width=\"100%%\" class=\"hr\">\n"
	              "<h3>&gt; General process information</h3>\n"
	              "<table border=0><tr><td align=\"left\" nowrap width=\"1%%\">\n"
	              "<p><b>pid = </b> %d (process #%d, nbproc = %d)<br>\n"
	              "<b>uptime = </b> %dd %dh%02dm%02ds<br>\n"
	              "<b>system limits:</b> memmax = %s%s; ulimit-n = %d<br>\n"
	              "<b>maxsock = </b> %d; <b>maxconn = </b> %d; <b>maxpipes = </b> %d<br>\n"
	              "current conns = %d; current pipes = %d/%d; conn rate = %d/sec<br>\n"
	              "Running tasks: %d/%d; idle = %d %%<br>\n"
	              "</td><td align=\"center\" nowrap>\n"
	              "<table class=\"lgd\"><tr>\n"
	              "<td class=\"active_up\">&nbsp;</td><td class=\"noborder\">active UP </td>"
	              "<td class=\"backup_up\">&nbsp;</td><td class=\"noborder\">backup UP </td>"
	              "</tr><tr>\n"
	              "<td class=\"active_going_down\"></td><td class=\"noborder\">active UP, going down </td>"
	              "<td class=\"backup_going_down\"></td><td class=\"noborder\">backup UP, going down </td>"
	              "</tr><tr>\n"
	              "<td class=\"active_going_up\"></td><td class=\"noborder\">active DOWN, going up </td>"
	              "<td class=\"backup_going_up\"></td><td class=\"noborder\">backup DOWN, going up </td>"
	              "</tr><tr>\n"
	              "<td class=\"active_down\"></td><td class=\"noborder\">active or backup DOWN &nbsp;</td>"
	              "<td class=\"active_no_check\"></td><td class=\"noborder\">not checked </td>"
	              "</tr><tr>\n"
	              "<td class=\"maintain\"></td><td class=\"noborder\" colspan=\"3\">active or backup DOWN for maintenance (MAINT) &nbsp;</td>"
	              "</tr><tr>\n"
	              "<td class=\"active_draining\"></td><td class=\"noborder\" colspan=\"3\">active or backup SOFT STOPPED for maintenance &nbsp;</td>"
	              "</tr></table>\n"
	              "Note: \"NOLB\"/\"DRAIN\" = UP with load-balancing disabled."
	              "</td>"
	              "<td align=\"left\" valign=\"top\" nowrap width=\"1%%\">"
	              "<b>Display option:</b><ul style=\"margin-top: 0.25em;\">"
	              "",
	              (uri->flags & ST_HIDEVER) ? "" : (STATS_VERSION_STRING),
	              pid, (uri->flags & ST_SHNODE) ? " on " : "",
		      (uri->flags & ST_SHNODE) ? (uri->node ? uri->node : global.node) : "",
	              (uri->flags & ST_SHDESC) ? ": " : "",
		      (uri->flags & ST_SHDESC) ? (uri->desc ? uri->desc : global.desc) : "",
	              pid, relative_pid, global.nbproc,
	              up / 86400, (up % 86400) / 3600,
	              (up % 3600) / 60, (up % 60),
	              global.rlimit_memmax ? ultoa(global.rlimit_memmax) : "unlimited",
	              global.rlimit_memmax ? " MB" : "",
	              global.rlimit_nofile,
	              global.maxsock, global.maxconn, global.maxpipes,
	              actconn, pipes_used, pipes_used+pipes_free, read_freq_ctr(&global.conn_per_sec),
	              run_queue_cur, nb_tasks_cur, idle_pct
	              );

	/* scope_txt = search query, appctx->ctx.stats.scope_len is always <= STAT_SCOPE_TXT_MAXLEN */
	memcpy(scope_txt, bo_ptr(si_ob(si)) + appctx->ctx.stats.scope_str, appctx->ctx.stats.scope_len);
	scope_txt[appctx->ctx.stats.scope_len] = '\0';

	chunk_appendf(&trash,
		      "<li><form method=\"GET\">Scope : <input value=\"%s\" name=\"" STAT_SCOPE_INPUT_NAME "\" size=\"8\" maxlength=\"%d\" tabindex=\"1\"/></form>\n",
		      (appctx->ctx.stats.scope_len > 0) ? scope_txt : "",
		      STAT_SCOPE_TXT_MAXLEN);

	/* scope_txt = search pattern + search query, appctx->ctx.stats.scope_len is always <= STAT_SCOPE_TXT_MAXLEN */
	scope_txt[0] = 0;
	if (appctx->ctx.stats.scope_len) {
		strcpy(scope_txt, STAT_SCOPE_PATTERN);
		memcpy(scope_txt + strlen(STAT_SCOPE_PATTERN), bo_ptr(si_ob(si)) + appctx->ctx.stats.scope_str, appctx->ctx.stats.scope_len);
		scope_txt[strlen(STAT_SCOPE_PATTERN) + appctx->ctx.stats.scope_len] = 0;
	}

	if (appctx->ctx.stats.flags & STAT_HIDE_DOWN)
		chunk_appendf(&trash,
		              "<li><a href=\"%s%s%s%s\">Show all servers</a><br>\n",
		              uri->uri_prefix,
		              "",
		              (appctx->ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			      scope_txt);
	else
		chunk_appendf(&trash,
		              "<li><a href=\"%s%s%s%s\">Hide 'DOWN' servers</a><br>\n",
		              uri->uri_prefix,
		              ";up",
		              (appctx->ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			      scope_txt);

	if (uri->refresh > 0) {
		if (appctx->ctx.stats.flags & STAT_NO_REFRESH)
			chunk_appendf(&trash,
			              "<li><a href=\"%s%s%s%s\">Enable refresh</a><br>\n",
			              uri->uri_prefix,
			              (appctx->ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
			              "",
				      scope_txt);
		else
			chunk_appendf(&trash,
			              "<li><a href=\"%s%s%s%s\">Disable refresh</a><br>\n",
			              uri->uri_prefix,
			              (appctx->ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
			              ";norefresh",
				      scope_txt);
	}

	chunk_appendf(&trash,
	              "<li><a href=\"%s%s%s%s\">Refresh now</a><br>\n",
	              uri->uri_prefix,
	              (appctx->ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
	              (appctx->ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "",
		      scope_txt);

	chunk_appendf(&trash,
	              "<li><a href=\"%s;csv%s%s\">CSV export</a><br>\n",
	              uri->uri_prefix,
	              (uri->refresh > 0) ? ";norefresh" : "",
		      scope_txt);

	chunk_appendf(&trash,
	              "</ul></td>"
	              "<td align=\"left\" valign=\"top\" nowrap width=\"1%%\">"
	              "<b>External resources:</b><ul style=\"margin-top: 0.25em;\">\n"
	              "<li><a href=\"" PRODUCT_URL "\">Primary site</a><br>\n"
	              "<li><a href=\"" PRODUCT_URL_UPD "\">Updates (v" PRODUCT_BRANCH ")</a><br>\n"
	              "<li><a href=\"" PRODUCT_URL_DOC "\">Online manual</a><br>\n"
	              "</ul>"
	              "</td>"
	              "</tr></table>\n"
	              ""
	              );

	if (appctx->ctx.stats.st_code) {
		switch (appctx->ctx.stats.st_code) {
		case STAT_STATUS_DONE:
			chunk_appendf(&trash,
			              "<p><div class=active_up>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "Action processed successfully."
			              "</div>\n", uri->uri_prefix,
			              (appctx->ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (appctx->ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		case STAT_STATUS_NONE:
			chunk_appendf(&trash,
			              "<p><div class=active_going_down>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "Nothing has changed."
			              "</div>\n", uri->uri_prefix,
			              (appctx->ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (appctx->ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		case STAT_STATUS_PART:
			chunk_appendf(&trash,
			              "<p><div class=active_going_down>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "Action partially processed.<br>"
			              "Some server names are probably unknown or ambiguous (duplicated names in the backend)."
			              "</div>\n", uri->uri_prefix,
			              (appctx->ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (appctx->ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		case STAT_STATUS_ERRP:
			chunk_appendf(&trash,
			              "<p><div class=active_down>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "Action not processed because of invalid parameters."
			              "<ul>"
			              "<li>The action is maybe unknown.</li>"
			              "<li>The backend name is probably unknown or ambiguous (duplicated names).</li>"
			              "<li>Some server names are probably unknown or ambiguous (duplicated names in the backend).</li>"
			              "</ul>"
			              "</div>\n", uri->uri_prefix,
			              (appctx->ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (appctx->ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		case STAT_STATUS_EXCD:
			chunk_appendf(&trash,
			              "<p><div class=active_down>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "<b>Action not processed : the buffer couldn't store all the data.<br>"
			              "You should retry with less servers at a time.</b>"
			              "</div>\n", uri->uri_prefix,
			              (appctx->ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (appctx->ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		case STAT_STATUS_DENY:
			chunk_appendf(&trash,
			              "<p><div class=active_down>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "<b>Action denied.</b>"
			              "</div>\n", uri->uri_prefix,
			              (appctx->ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (appctx->ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		default:
			chunk_appendf(&trash,
			              "<p><div class=active_no_check>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "Unexpected result."
			              "</div>\n", uri->uri_prefix,
			              (appctx->ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (appctx->ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
		}
		chunk_appendf(&trash, "<p>\n");
	}
}

/* Dumps the HTML stats trailer block to the trash. The caller is responsible
 * for clearing the trash if needed.
 */
static void stats_dump_html_end()
{
	chunk_appendf(&trash, "</body></html>\n");
}

/* This function dumps statistics onto the stream interface's read buffer in
 * either CSV or HTML format. <uri> contains some HTML-specific parameters that
 * are ignored for CSV format (hence <uri> may be NULL there). It returns 0 if
 * it had to stop writing data and an I/O is needed, 1 if the dump is finished
 * and the stream must be closed, or -1 in case of any error. This function is
 * used by both the CLI and the HTTP handlers.
 */
static int stats_dump_stat_to_buffer(struct stream_interface *si, struct uri_auth *uri)
{
	struct appctx *appctx = __objt_appctx(si->end);
	struct channel *rep = si_ic(si);
	struct proxy *px;

	chunk_reset(&trash);

	switch (appctx->st2) {
	case STAT_ST_INIT:
		appctx->st2 = STAT_ST_HEAD; /* let's start producing data */
		/* fall through */

	case STAT_ST_HEAD:
		if (appctx->ctx.stats.flags & STAT_FMT_HTML)
			stats_dump_html_head(uri);
		else if (!(appctx->ctx.stats.flags & STAT_FMT_TYPED))
			stats_dump_csv_header();

		if (bi_putchk(rep, &trash) == -1) {
			si_applet_cant_put(si);
			return 0;
		}

		appctx->st2 = STAT_ST_INFO;
		/* fall through */

	case STAT_ST_INFO:
		if (appctx->ctx.stats.flags & STAT_FMT_HTML) {
			stats_dump_html_info(si, uri);
			if (bi_putchk(rep, &trash) == -1) {
				si_applet_cant_put(si);
				return 0;
			}
		}

		appctx->ctx.stats.px = proxy;
		appctx->ctx.stats.px_st = STAT_PX_ST_INIT;
		appctx->st2 = STAT_ST_LIST;
		/* fall through */

	case STAT_ST_LIST:
		/* dump proxies */
		while (appctx->ctx.stats.px) {
			if (buffer_almost_full(rep->buf)) {
				si_applet_cant_put(si);
				return 0;
			}

			px = appctx->ctx.stats.px;
			/* skip the disabled proxies, global frontend and non-networked ones */
			if (px->state != PR_STSTOPPED && px->uuid > 0 && (px->cap & (PR_CAP_FE | PR_CAP_BE)))
				if (stats_dump_proxy_to_buffer(si, px, uri) == 0)
					return 0;

			appctx->ctx.stats.px = px->next;
			appctx->ctx.stats.px_st = STAT_PX_ST_INIT;
		}
		/* here, we just have reached the last proxy */

		appctx->st2 = STAT_ST_END;
		/* fall through */

	case STAT_ST_END:
		if (appctx->ctx.stats.flags & STAT_FMT_HTML) {
			stats_dump_html_end();
			if (bi_putchk(rep, &trash) == -1) {
				si_applet_cant_put(si);
				return 0;
			}
		}

		appctx->st2 = STAT_ST_FIN;
		/* fall through */

	case STAT_ST_FIN:
		return 1;

	default:
		/* unknown state ! */
		appctx->st2 = STAT_ST_FIN;
		return -1;
	}
}

/* We reached the stats page through a POST request. The appctx is
 * expected to have already been allocated by the caller.
 * Parse the posted data and enable/disable servers if necessary.
 * Returns 1 if request was parsed or zero if it needs more data.
 */
static int stats_process_http_post(struct stream_interface *si)
{
	struct stream *s = si_strm(si);
	struct appctx *appctx = objt_appctx(si->end);

	struct proxy *px = NULL;
	struct server *sv = NULL;

	char key[LINESIZE];
	int action = ST_ADM_ACTION_NONE;
	int reprocess = 0;

	int total_servers = 0;
	int altered_servers = 0;

	char *first_param, *cur_param, *next_param, *end_params;
	char *st_cur_param = NULL;
	char *st_next_param = NULL;

	struct chunk *temp;
	int reql;

	temp = get_trash_chunk();
	if (temp->size < s->txn->req.body_len) {
		/* too large request */
		appctx->ctx.stats.st_code = STAT_STATUS_EXCD;
		goto out;
	}

	reql = bo_getblk(si_oc(si), temp->str, s->txn->req.body_len, s->txn->req.eoh + 2);
	if (reql <= 0) {
		/* we need more data */
		appctx->ctx.stats.st_code = STAT_STATUS_NONE;
		return 0;
	}

	first_param = temp->str;
	end_params  = temp->str + reql;
	cur_param = next_param = end_params;
	*end_params = '\0';

	appctx->ctx.stats.st_code = STAT_STATUS_NONE;

	/*
	 * Parse the parameters in reverse order to only store the last value.
	 * From the html form, the backend and the action are at the end.
	 */
	while (cur_param > first_param) {
		char *value;
		int poffset, plen;

		cur_param--;

		if ((*cur_param == '&') || (cur_param == first_param)) {
		reprocess_servers:
			/* Parse the key */
			poffset = (cur_param != first_param ? 1 : 0);
			plen = next_param - cur_param + (cur_param == first_param ? 1 : 0);
			if ((plen > 0) && (plen <= sizeof(key))) {
				strncpy(key, cur_param + poffset, plen);
				key[plen - 1] = '\0';
			} else {
				appctx->ctx.stats.st_code = STAT_STATUS_EXCD;
				goto out;
			}

			/* Parse the value */
			value = key;
			while (*value != '\0' && *value != '=') {
				value++;
			}
			if (*value == '=') {
				/* Ok, a value is found, we can mark the end of the key */
				*value++ = '\0';
			}
			if (url_decode(key) < 0 || url_decode(value) < 0)
				break;

			/* Now we can check the key to see what to do */
			if (!px && (strcmp(key, "b") == 0)) {
				if ((px = proxy_be_by_name(value)) == NULL) {
					/* the backend name is unknown or ambiguous (duplicate names) */
					appctx->ctx.stats.st_code = STAT_STATUS_ERRP;
					goto out;
				}
			}
			else if (!action && (strcmp(key, "action") == 0)) {
				if (strcmp(value, "ready") == 0) {
					action = ST_ADM_ACTION_READY;
				}
				else if (strcmp(value, "drain") == 0) {
					action = ST_ADM_ACTION_DRAIN;
				}
				else if (strcmp(value, "maint") == 0) {
					action = ST_ADM_ACTION_MAINT;
				}
				else if (strcmp(value, "shutdown") == 0) {
					action = ST_ADM_ACTION_SHUTDOWN;
				}
				else if (strcmp(value, "dhlth") == 0) {
					action = ST_ADM_ACTION_DHLTH;
				}
				else if (strcmp(value, "ehlth") == 0) {
					action = ST_ADM_ACTION_EHLTH;
				}
				else if (strcmp(value, "hrunn") == 0) {
					action = ST_ADM_ACTION_HRUNN;
				}
				else if (strcmp(value, "hnolb") == 0) {
					action = ST_ADM_ACTION_HNOLB;
				}
				else if (strcmp(value, "hdown") == 0) {
					action = ST_ADM_ACTION_HDOWN;
				}
				else if (strcmp(value, "dagent") == 0) {
					action = ST_ADM_ACTION_DAGENT;
				}
				else if (strcmp(value, "eagent") == 0) {
					action = ST_ADM_ACTION_EAGENT;
				}
				else if (strcmp(value, "arunn") == 0) {
					action = ST_ADM_ACTION_ARUNN;
				}
				else if (strcmp(value, "adown") == 0) {
					action = ST_ADM_ACTION_ADOWN;
				}
				/* else these are the old supported methods */
				else if (strcmp(value, "disable") == 0) {
					action = ST_ADM_ACTION_DISABLE;
				}
				else if (strcmp(value, "enable") == 0) {
					action = ST_ADM_ACTION_ENABLE;
				}
				else if (strcmp(value, "stop") == 0) {
					action = ST_ADM_ACTION_STOP;
				}
				else if (strcmp(value, "start") == 0) {
					action = ST_ADM_ACTION_START;
				}
				else {
					appctx->ctx.stats.st_code = STAT_STATUS_ERRP;
					goto out;
				}
			}
			else if (strcmp(key, "s") == 0) {
				if (!(px && action)) {
					/*
					 * Indicates that we'll need to reprocess the parameters
					 * as soon as backend and action are known
					 */
					if (!reprocess) {
						st_cur_param  = cur_param;
						st_next_param = next_param;
					}
					reprocess = 1;
				}
				else if ((sv = findserver(px, value)) != NULL) {
					switch (action) {
					case ST_ADM_ACTION_DISABLE:
						if (!(sv->admin & SRV_ADMF_FMAINT)) {
							altered_servers++;
							total_servers++;
							srv_set_admin_flag(sv, SRV_ADMF_FMAINT);
						}
						break;
					case ST_ADM_ACTION_ENABLE:
						if (sv->admin & SRV_ADMF_FMAINT) {
							altered_servers++;
							total_servers++;
							srv_clr_admin_flag(sv, SRV_ADMF_FMAINT);
						}
						break;
					case ST_ADM_ACTION_STOP:
						if (!(sv->admin & SRV_ADMF_FDRAIN)) {
							srv_set_admin_flag(sv, SRV_ADMF_FDRAIN);
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_START:
						if (sv->admin & SRV_ADMF_FDRAIN) {
							srv_clr_admin_flag(sv, SRV_ADMF_FDRAIN);
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_DHLTH:
						if (sv->check.state & CHK_ST_CONFIGURED) {
							sv->check.state &= ~CHK_ST_ENABLED;
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_EHLTH:
						if (sv->check.state & CHK_ST_CONFIGURED) {
							sv->check.state |= CHK_ST_ENABLED;
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_HRUNN:
						if (!(sv->track)) {
							sv->check.health = sv->check.rise + sv->check.fall - 1;
							srv_set_running(sv, "changed from Web interface");
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_HNOLB:
						if (!(sv->track)) {
							sv->check.health = sv->check.rise + sv->check.fall - 1;
							srv_set_stopping(sv, "changed from Web interface");
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_HDOWN:
						if (!(sv->track)) {
							sv->check.health = 0;
							srv_set_stopped(sv, "changed from Web interface");
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_DAGENT:
						if (sv->agent.state & CHK_ST_CONFIGURED) {
							sv->agent.state &= ~CHK_ST_ENABLED;
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_EAGENT:
						if (sv->agent.state & CHK_ST_CONFIGURED) {
							sv->agent.state |= CHK_ST_ENABLED;
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_ARUNN:
						if (sv->agent.state & CHK_ST_ENABLED) {
							sv->agent.health = sv->agent.rise + sv->agent.fall - 1;
							srv_set_running(sv, "changed from Web interface");
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_ADOWN:
						if (sv->agent.state & CHK_ST_ENABLED) {
							sv->agent.health = 0;
							srv_set_stopped(sv, "changed from Web interface");
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_READY:
						srv_adm_set_ready(sv);
						altered_servers++;
						total_servers++;
						break;
					case ST_ADM_ACTION_DRAIN:
						srv_adm_set_drain(sv);
						altered_servers++;
						total_servers++;
						break;
					case ST_ADM_ACTION_MAINT:
						srv_adm_set_maint(sv);
						altered_servers++;
						total_servers++;
						break;
					case ST_ADM_ACTION_SHUTDOWN:
						if (px->state != PR_STSTOPPED) {
							struct stream *sess, *sess_bck;

							list_for_each_entry_safe(sess, sess_bck, &sv->actconns, by_srv)
								if (sess->srv_conn == sv)
									stream_shutdown(sess, SF_ERR_KILLED);

							altered_servers++;
							total_servers++;
						}
						break;
					}
				} else {
					/* the server name is unknown or ambiguous (duplicate names) */
					total_servers++;
				}
			}
			if (reprocess && px && action) {
				/* Now, we know the backend and the action chosen by the user.
				 * We can safely restart from the first server parameter
				 * to reprocess them
				 */
				cur_param  = st_cur_param;
				next_param = st_next_param;
				reprocess = 0;
				goto reprocess_servers;
			}

			next_param = cur_param;
		}
	}

	if (total_servers == 0) {
		appctx->ctx.stats.st_code = STAT_STATUS_NONE;
	}
	else if (altered_servers == 0) {
		appctx->ctx.stats.st_code = STAT_STATUS_ERRP;
	}
	else if (altered_servers == total_servers) {
		appctx->ctx.stats.st_code = STAT_STATUS_DONE;
	}
	else {
		appctx->ctx.stats.st_code = STAT_STATUS_PART;
	}
 out:
	return 1;
}


static int stats_send_http_headers(struct stream_interface *si)
{
	struct stream *s = si_strm(si);
	struct uri_auth *uri = s->be->uri_auth;
	struct appctx *appctx = objt_appctx(si->end);

	chunk_printf(&trash,
		     "HTTP/1.1 200 OK\r\n"
		     "Cache-Control: no-cache\r\n"
		     "Connection: close\r\n"
		     "Content-Type: %s\r\n",
		     (appctx->ctx.stats.flags & STAT_FMT_HTML) ? "text/html" : "text/plain");

	if (uri->refresh > 0 && !(appctx->ctx.stats.flags & STAT_NO_REFRESH))
		chunk_appendf(&trash, "Refresh: %d\r\n",
			      uri->refresh);

	/* we don't send the CRLF in chunked mode, it will be sent with the first chunk's size */

	if (appctx->ctx.stats.flags & STAT_CHUNKED)
		chunk_appendf(&trash, "Transfer-Encoding: chunked\r\n");
	else
		chunk_appendf(&trash, "\r\n");

	s->txn->status = 200;
	s->logs.tv_request = now;

	if (bi_putchk(si_ic(si), &trash) == -1) {
		si_applet_cant_put(si);
		return 0;
	}

	return 1;
}

static int stats_send_http_redirect(struct stream_interface *si)
{
	char scope_txt[STAT_SCOPE_TXT_MAXLEN + sizeof STAT_SCOPE_PATTERN];
	struct stream *s = si_strm(si);
	struct uri_auth *uri = s->be->uri_auth;
	struct appctx *appctx = objt_appctx(si->end);

	/* scope_txt = search pattern + search query, appctx->ctx.stats.scope_len is always <= STAT_SCOPE_TXT_MAXLEN */
	scope_txt[0] = 0;
	if (appctx->ctx.stats.scope_len) {
		strcpy(scope_txt, STAT_SCOPE_PATTERN);
		memcpy(scope_txt + strlen(STAT_SCOPE_PATTERN), bo_ptr(si_ob(si)) + appctx->ctx.stats.scope_str, appctx->ctx.stats.scope_len);
		scope_txt[strlen(STAT_SCOPE_PATTERN) + appctx->ctx.stats.scope_len] = 0;
	}

	/* We don't want to land on the posted stats page because a refresh will
	 * repost the data. We don't want this to happen on accident so we redirect
	 * the browse to the stats page with a GET.
	 */
	chunk_printf(&trash,
		     "HTTP/1.1 303 See Other\r\n"
		     "Cache-Control: no-cache\r\n"
		     "Content-Type: text/plain\r\n"
		     "Connection: close\r\n"
		     "Location: %s;st=%s%s%s%s\r\n"
		     "Content-length: 0\r\n"
		     "\r\n",
		     uri->uri_prefix,
		     ((appctx->ctx.stats.st_code > STAT_STATUS_INIT) &&
		      (appctx->ctx.stats.st_code < STAT_STATUS_SIZE) &&
		      stat_status_codes[appctx->ctx.stats.st_code]) ?
		     stat_status_codes[appctx->ctx.stats.st_code] :
		     stat_status_codes[STAT_STATUS_UNKN],
		     (appctx->ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
		     (appctx->ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "",
		     scope_txt);

	s->txn->status = 303;
	s->logs.tv_request = now;

	if (bi_putchk(si_ic(si), &trash) == -1) {
		si_applet_cant_put(si);
		return 0;
	}

	return 1;
}

/* This I/O handler runs as an applet embedded in a stream interface. It is
 * used to send HTTP stats over a TCP socket. The mechanism is very simple.
 * appctx->st0 contains the operation in progress (dump, done). The handler
 * automatically unregisters itself once transfer is complete.
 */
static void http_stats_io_handler(struct appctx *appctx)
{
	struct stream_interface *si = appctx->owner;
	struct stream *s = si_strm(si);
	struct channel *req = si_oc(si);
	struct channel *res = si_ic(si);

	if (unlikely(si->state == SI_ST_DIS || si->state == SI_ST_CLO))
		goto out;

	/* check that the output is not closed */
	if (res->flags & (CF_SHUTW|CF_SHUTW_NOW))
		appctx->st0 = STAT_HTTP_DONE;

	/* all states are processed in sequence */
	if (appctx->st0 == STAT_HTTP_HEAD) {
		if (stats_send_http_headers(si)) {
			if (s->txn->meth == HTTP_METH_HEAD)
				appctx->st0 = STAT_HTTP_DONE;
			else
				appctx->st0 = STAT_HTTP_DUMP;
		}
	}

	if (appctx->st0 == STAT_HTTP_DUMP) {
		unsigned int prev_len = si_ib(si)->i;
		unsigned int data_len;
		unsigned int last_len;
		unsigned int last_fwd = 0;

		if (appctx->ctx.stats.flags & STAT_CHUNKED) {
			/* One difficulty we're facing is that we must prevent
			 * the input data from being automatically forwarded to
			 * the output area. For this, we temporarily disable
			 * forwarding on the channel.
			 */
			last_fwd = si_ic(si)->to_forward;
			si_ic(si)->to_forward = 0;
			chunk_printf(&trash, "\r\n000000\r\n");
			if (bi_putchk(si_ic(si), &trash) == -1) {
				si_applet_cant_put(si);
				si_ic(si)->to_forward = last_fwd;
				goto out;
			}
		}

		data_len = si_ib(si)->i;
		if (stats_dump_stat_to_buffer(si, s->be->uri_auth))
			appctx->st0 = STAT_HTTP_DONE;

		last_len = si_ib(si)->i;

		/* Now we must either adjust or remove the chunk size. This is
		 * not easy because the chunk size might wrap at the end of the
		 * buffer, so we pretend we have nothing in the buffer, we write
		 * the size, then restore the buffer's contents. Note that we can
		 * only do that because no forwarding is scheduled on the stats
		 * applet.
		 */
		if (appctx->ctx.stats.flags & STAT_CHUNKED) {
			si_ic(si)->total -= (last_len - prev_len);
			si_ib(si)->i     -= (last_len - prev_len);

			if (last_len != data_len) {
				chunk_printf(&trash, "\r\n%06x\r\n", (last_len - data_len));
				if (bi_putchk(si_ic(si), &trash) == -1)
					si_applet_cant_put(si);

				si_ic(si)->total += (last_len - data_len);
				si_ib(si)->i     += (last_len - data_len);
			}
			/* now re-enable forwarding */
			channel_forward(si_ic(si), last_fwd);
		}
	}

	if (appctx->st0 == STAT_HTTP_POST) {
		if (stats_process_http_post(si))
			appctx->st0 = STAT_HTTP_LAST;
		else if (si_oc(si)->flags & CF_SHUTR)
			appctx->st0 = STAT_HTTP_DONE;
	}

	if (appctx->st0 == STAT_HTTP_LAST) {
		if (stats_send_http_redirect(si))
			appctx->st0 = STAT_HTTP_DONE;
	}

	if (appctx->st0 == STAT_HTTP_DONE) {
		if (appctx->ctx.stats.flags & STAT_CHUNKED) {
			chunk_printf(&trash, "\r\n0\r\n\r\n");
			if (bi_putchk(si_ic(si), &trash) == -1) {
				si_applet_cant_put(si);
				goto out;
			}
		}
		/* eat the whole request */
		bo_skip(si_oc(si), si_ob(si)->o);
		res->flags |= CF_READ_NULL;
		si_shutr(si);
	}

	if ((res->flags & CF_SHUTR) && (si->state == SI_ST_EST))
		si_shutw(si);

	if (appctx->st0 == STAT_HTTP_DONE) {
		if ((req->flags & CF_SHUTW) && (si->state == SI_ST_EST)) {
			si_shutr(si);
			res->flags |= CF_READ_NULL;
		}
	}
 out:
	/* just to make gcc happy */ ;
}


static inline const char *get_conn_ctrl_name(const struct connection *conn)
{
	if (!conn_ctrl_ready(conn))
		return "NONE";
	return conn->ctrl->name;
}

static inline const char *get_conn_xprt_name(const struct connection *conn)
{
	static char ptr[17];

	if (!conn_xprt_ready(conn))
		return "NONE";

	if (conn->xprt == &raw_sock)
		return "RAW";

#ifdef USE_OPENSSL
	if (conn->xprt == &ssl_sock)
		return "SSL";
#endif
	snprintf(ptr, sizeof(ptr), "%p", conn->xprt);
	return ptr;
}

static inline const char *get_conn_data_name(const struct connection *conn)
{
	static char ptr[17];

	if (!conn->data)
		return "NONE";

	if (conn->data == &sess_conn_cb)
		return "SESS";

	if (conn->data == &si_conn_cb)
		return "STRM";

	if (conn->data == &check_conn_cb)
		return "CHCK";

	snprintf(ptr, sizeof(ptr), "%p", conn->data);
	return ptr;
}

/* This function dumps a complete stream state onto the stream interface's
 * read buffer. The stream has to be set in sess->target. It returns
 * 0 if the output buffer is full and it needs to be called again, otherwise
 * non-zero. It is designed to be called from stats_dump_sess_to_buffer() below.
 */
static int stats_dump_full_sess_to_buffer(struct stream_interface *si, struct stream *sess)
{
	struct appctx *appctx = __objt_appctx(si->end);
	struct tm tm;
	extern const char *monthname[12];
	char pn[INET6_ADDRSTRLEN];
	struct connection *conn;
	struct appctx *tmpctx;

	chunk_reset(&trash);

	if (appctx->ctx.sess.section > 0 && appctx->ctx.sess.uid != sess->uniq_id) {
		/* stream changed, no need to go any further */
		chunk_appendf(&trash, "  *** session terminated while we were watching it ***\n");
		if (bi_putchk(si_ic(si), &trash) == -1) {
			si_applet_cant_put(si);
			return 0;
		}
		appctx->ctx.sess.uid = 0;
		appctx->ctx.sess.section = 0;
		return 1;
	}

	switch (appctx->ctx.sess.section) {
	case 0: /* main status of the stream */
		appctx->ctx.sess.uid = sess->uniq_id;
		appctx->ctx.sess.section = 1;
		/* fall through */

	case 1:
		get_localtime(sess->logs.accept_date.tv_sec, &tm);
		chunk_appendf(&trash,
			     "%p: [%02d/%s/%04d:%02d:%02d:%02d.%06d] id=%u proto=%s",
			     sess,
			     tm.tm_mday, monthname[tm.tm_mon], tm.tm_year+1900,
			     tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(sess->logs.accept_date.tv_usec),
			     sess->uniq_id,
			     strm_li(sess) ? strm_li(sess)->proto->name : "?");

		conn = objt_conn(strm_orig(sess));
		switch (conn ? addr_to_str(&conn->addr.from, pn, sizeof(pn)) : AF_UNSPEC) {
		case AF_INET:
		case AF_INET6:
			chunk_appendf(&trash, " source=%s:%d\n",
			              pn, get_host_port(&conn->addr.from));
			break;
		case AF_UNIX:
			chunk_appendf(&trash, " source=unix:%d\n", strm_li(sess)->luid);
			break;
		default:
			/* no more information to print right now */
			chunk_appendf(&trash, "\n");
			break;
		}

		chunk_appendf(&trash,
			     "  flags=0x%x, conn_retries=%d, srv_conn=%p, pend_pos=%p\n",
			     sess->flags, sess->si[1].conn_retries, sess->srv_conn, sess->pend_pos);

		chunk_appendf(&trash,
			     "  frontend=%s (id=%u mode=%s), listener=%s (id=%u)",
			     strm_fe(sess)->id, strm_fe(sess)->uuid, strm_fe(sess)->mode ? "http" : "tcp",
			     strm_li(sess) ? strm_li(sess)->name ? strm_li(sess)->name : "?" : "?",
			     strm_li(sess) ? strm_li(sess)->luid : 0);

		if (conn)
			conn_get_to_addr(conn);

		switch (conn ? addr_to_str(&conn->addr.to, pn, sizeof(pn)) : AF_UNSPEC) {
		case AF_INET:
		case AF_INET6:
			chunk_appendf(&trash, " addr=%s:%d\n",
				     pn, get_host_port(&conn->addr.to));
			break;
		case AF_UNIX:
			chunk_appendf(&trash, " addr=unix:%d\n", strm_li(sess)->luid);
			break;
		default:
			/* no more information to print right now */
			chunk_appendf(&trash, "\n");
			break;
		}

		if (sess->be->cap & PR_CAP_BE)
			chunk_appendf(&trash,
				     "  backend=%s (id=%u mode=%s)",
				     sess->be->id,
				     sess->be->uuid, sess->be->mode ? "http" : "tcp");
		else
			chunk_appendf(&trash, "  backend=<NONE> (id=-1 mode=-)");

		conn = objt_conn(sess->si[1].end);
		if (conn)
			conn_get_from_addr(conn);

		switch (conn ? addr_to_str(&conn->addr.from, pn, sizeof(pn)) : AF_UNSPEC) {
		case AF_INET:
		case AF_INET6:
			chunk_appendf(&trash, " addr=%s:%d\n",
				     pn, get_host_port(&conn->addr.from));
			break;
		case AF_UNIX:
			chunk_appendf(&trash, " addr=unix\n");
			break;
		default:
			/* no more information to print right now */
			chunk_appendf(&trash, "\n");
			break;
		}

		if (sess->be->cap & PR_CAP_BE)
			chunk_appendf(&trash,
				     "  server=%s (id=%u)",
				     objt_server(sess->target) ? objt_server(sess->target)->id : "<none>",
				     objt_server(sess->target) ? objt_server(sess->target)->puid : 0);
		else
			chunk_appendf(&trash, "  server=<NONE> (id=-1)");

		if (conn)
			conn_get_to_addr(conn);

		switch (conn ? addr_to_str(&conn->addr.to, pn, sizeof(pn)) : AF_UNSPEC) {
		case AF_INET:
		case AF_INET6:
			chunk_appendf(&trash, " addr=%s:%d\n",
				     pn, get_host_port(&conn->addr.to));
			break;
		case AF_UNIX:
			chunk_appendf(&trash, " addr=unix\n");
			break;
		default:
			/* no more information to print right now */
			chunk_appendf(&trash, "\n");
			break;
		}

		chunk_appendf(&trash,
			     "  task=%p (state=0x%02x nice=%d calls=%d exp=%s%s",
			     sess->task,
			     sess->task->state,
			     sess->task->nice, sess->task->calls,
			     sess->task->expire ?
			             tick_is_expired(sess->task->expire, now_ms) ? "<PAST>" :
			                     human_time(TICKS_TO_MS(sess->task->expire - now_ms),
			                     TICKS_TO_MS(1000)) : "<NEVER>",
			     task_in_rq(sess->task) ? ", running" : "");

		chunk_appendf(&trash,
			     " age=%s)\n",
			     human_time(now.tv_sec - sess->logs.accept_date.tv_sec, 1));

		if (sess->txn)
			chunk_appendf(&trash,
			     "  txn=%p flags=0x%x meth=%d status=%d req.st=%s rsp.st=%s waiting=%d\n",
			      sess->txn, sess->txn->flags, sess->txn->meth, sess->txn->status,
			      http_msg_state_str(sess->txn->req.msg_state), http_msg_state_str(sess->txn->rsp.msg_state), !LIST_ISEMPTY(&sess->buffer_wait));

		chunk_appendf(&trash,
			     "  si[0]=%p (state=%s flags=0x%02x endp0=%s:%p exp=%s, et=0x%03x)\n",
			     &sess->si[0],
			     si_state_str(sess->si[0].state),
			     sess->si[0].flags,
			     obj_type_name(sess->si[0].end),
			     obj_base_ptr(sess->si[0].end),
			     sess->si[0].exp ?
			             tick_is_expired(sess->si[0].exp, now_ms) ? "<PAST>" :
			                     human_time(TICKS_TO_MS(sess->si[0].exp - now_ms),
			                     TICKS_TO_MS(1000)) : "<NEVER>",
			     sess->si[0].err_type);

		chunk_appendf(&trash,
			     "  si[1]=%p (state=%s flags=0x%02x endp1=%s:%p exp=%s, et=0x%03x)\n",
			     &sess->si[1],
			     si_state_str(sess->si[1].state),
			     sess->si[1].flags,
			     obj_type_name(sess->si[1].end),
			     obj_base_ptr(sess->si[1].end),
			     sess->si[1].exp ?
			             tick_is_expired(sess->si[1].exp, now_ms) ? "<PAST>" :
			                     human_time(TICKS_TO_MS(sess->si[1].exp - now_ms),
			                     TICKS_TO_MS(1000)) : "<NEVER>",
			     sess->si[1].err_type);

		if ((conn = objt_conn(sess->si[0].end)) != NULL) {
			chunk_appendf(&trash,
			              "  co0=%p ctrl=%s xprt=%s data=%s target=%s:%p\n",
				      conn,
				      get_conn_ctrl_name(conn),
				      get_conn_xprt_name(conn),
				      get_conn_data_name(conn),
			              obj_type_name(conn->target),
			              obj_base_ptr(conn->target));

			chunk_appendf(&trash,
			              "      flags=0x%08x fd=%d fd.state=%02x fd.cache=%d updt=%d\n",
			              conn->flags,
			              conn->t.sock.fd,
			              conn->t.sock.fd >= 0 ? fdtab[conn->t.sock.fd].state : 0,
			              conn->t.sock.fd >= 0 ? fdtab[conn->t.sock.fd].cache : 0,
			              conn->t.sock.fd >= 0 ? fdtab[conn->t.sock.fd].updated : 0);
		}
		else if ((tmpctx = objt_appctx(sess->si[0].end)) != NULL) {
			chunk_appendf(&trash,
			              "  app0=%p st0=%d st1=%d st2=%d applet=%s\n",
				      tmpctx,
				      tmpctx->st0,
				      tmpctx->st1,
				      tmpctx->st2,
			              tmpctx->applet->name);
		}

		if ((conn = objt_conn(sess->si[1].end)) != NULL) {
			chunk_appendf(&trash,
			              "  co1=%p ctrl=%s xprt=%s data=%s target=%s:%p\n",
				      conn,
				      get_conn_ctrl_name(conn),
				      get_conn_xprt_name(conn),
				      get_conn_data_name(conn),
			              obj_type_name(conn->target),
			              obj_base_ptr(conn->target));

			chunk_appendf(&trash,
			              "      flags=0x%08x fd=%d fd.state=%02x fd.cache=%d updt=%d\n",
			              conn->flags,
			              conn->t.sock.fd,
			              conn->t.sock.fd >= 0 ? fdtab[conn->t.sock.fd].state : 0,
			              conn->t.sock.fd >= 0 ? fdtab[conn->t.sock.fd].cache : 0,
			              conn->t.sock.fd >= 0 ? fdtab[conn->t.sock.fd].updated : 0);
		}
		else if ((tmpctx = objt_appctx(sess->si[1].end)) != NULL) {
			chunk_appendf(&trash,
			              "  app1=%p st0=%d st1=%d st2=%d applet=%s\n",
				      tmpctx,
				      tmpctx->st0,
				      tmpctx->st1,
				      tmpctx->st2,
			              tmpctx->applet->name);
		}

		chunk_appendf(&trash,
			     "  req=%p (f=0x%06x an=0x%x pipe=%d tofwd=%d total=%lld)\n"
			     "      an_exp=%s",
			     &sess->req,
			     sess->req.flags, sess->req.analysers,
			     sess->req.pipe ? sess->req.pipe->data : 0,
			     sess->req.to_forward, sess->req.total,
			     sess->req.analyse_exp ?
			     human_time(TICKS_TO_MS(sess->req.analyse_exp - now_ms),
					TICKS_TO_MS(1000)) : "<NEVER>");

		chunk_appendf(&trash,
			     " rex=%s",
			     sess->req.rex ?
			     human_time(TICKS_TO_MS(sess->req.rex - now_ms),
					TICKS_TO_MS(1000)) : "<NEVER>");

		chunk_appendf(&trash,
			     " wex=%s\n"
			     "      buf=%p data=%p o=%d p=%d req.next=%d i=%d size=%d\n",
			     sess->req.wex ?
			     human_time(TICKS_TO_MS(sess->req.wex - now_ms),
					TICKS_TO_MS(1000)) : "<NEVER>",
			     sess->req.buf,
			     sess->req.buf->data, sess->req.buf->o,
			     (int)(sess->req.buf->p - sess->req.buf->data),
			     sess->txn ? sess->txn->req.next : 0, sess->req.buf->i,
			     sess->req.buf->size);

		chunk_appendf(&trash,
			     "  res=%p (f=0x%06x an=0x%x pipe=%d tofwd=%d total=%lld)\n"
			     "      an_exp=%s",
			     &sess->res,
			     sess->res.flags, sess->res.analysers,
			     sess->res.pipe ? sess->res.pipe->data : 0,
			     sess->res.to_forward, sess->res.total,
			     sess->res.analyse_exp ?
			     human_time(TICKS_TO_MS(sess->res.analyse_exp - now_ms),
					TICKS_TO_MS(1000)) : "<NEVER>");

		chunk_appendf(&trash,
			     " rex=%s",
			     sess->res.rex ?
			     human_time(TICKS_TO_MS(sess->res.rex - now_ms),
					TICKS_TO_MS(1000)) : "<NEVER>");

		chunk_appendf(&trash,
			     " wex=%s\n"
			     "      buf=%p data=%p o=%d p=%d rsp.next=%d i=%d size=%d\n",
			     sess->res.wex ?
			     human_time(TICKS_TO_MS(sess->res.wex - now_ms),
					TICKS_TO_MS(1000)) : "<NEVER>",
			     sess->res.buf,
			     sess->res.buf->data, sess->res.buf->o,
			     (int)(sess->res.buf->p - sess->res.buf->data),
			     sess->txn ? sess->txn->rsp.next : 0, sess->res.buf->i,
			     sess->res.buf->size);

		if (bi_putchk(si_ic(si), &trash) == -1) {
			si_applet_cant_put(si);
			return 0;
		}

		/* use other states to dump the contents */
	}
	/* end of dump */
	appctx->ctx.sess.uid = 0;
	appctx->ctx.sess.section = 0;
	return 1;
}

#if (defined SSL_CTRL_SET_TLSEXT_TICKET_KEY_CB && TLS_TICKETS_NO > 0)
static int stats_tlskeys_list(struct stream_interface *si) {
	struct appctx *appctx = __objt_appctx(si->end);
	struct tls_keys_ref *ref;

	switch (appctx->st2) {
	case STAT_ST_INIT:
		/* Display the column headers. If the message cannot be sent,
		 * quit the fucntion with returning 0. The function is called
		 * later and restart at the state "STAT_ST_INIT".
		 */
		chunk_reset(&trash);

		if (appctx->st0 == STAT_CLI_O_TLSK_ENT)
			chunk_appendf(&trash, "# id secret\n");
		else
			chunk_appendf(&trash, "# id (file)\n");

		if (bi_putchk(si_ic(si), &trash) == -1) {
			si_applet_cant_put(si);
			return 0;
		}

		ref = appctx->ctx.tlskeys.ref;

		/* Now, we start the browsing of the references lists.
		 * Note that the following call to LIST_ELEM return bad pointer. The only
		 * available field of this pointer is <list>. It is used with the function
		 * tlskeys_list_get_next() for retruning the first available entry
		 */
		if (ref == NULL) {
			ref = LIST_ELEM(&tlskeys_reference, struct tls_keys_ref *, list);
			ref = tlskeys_list_get_next(ref, &tlskeys_reference);
		}

		appctx->st2 = STAT_ST_LIST;
		/* fall through */

	case STAT_ST_LIST:
		while (ref) {
			int i;
			int head = ref->tls_ticket_enc_index;

			chunk_reset(&trash);
			if (appctx->st0 == STAT_CLI_O_TLSK_ENT)
				chunk_appendf(&trash, "# ");
			chunk_appendf(&trash, "%d (%s)\n", ref->unique_id,
			              ref->filename);

			if (appctx->st0 == STAT_CLI_O_TLSK_ENT) {
				for (i = 0; i < TLS_TICKETS_NO; i++) {
					struct chunk *t2 = get_trash_chunk();
					int b64_len;

					chunk_reset(t2);
					b64_len = a2base64((char *)(ref->tlskeys + (head + 2 + i) % TLS_TICKETS_NO),
					                   sizeof(struct tls_sess_key), t2->str, t2->size);
					if (b64_len < 0)
						return 0;
					t2->len = b64_len;
					chunk_appendf(&trash, "%d.%d %s\n", ref->unique_id, i, t2->str);
				}
			}
			if (bi_putchk(si_ic(si), &trash) == -1) {
				/* let's try again later from this stream. We add ourselves into
				 * this stream's users so that it can remove us upon termination.
				 */
				si_applet_cant_put(si);
				return 0;
			}

			if (appctx->ctx.tlskeys.ref) /* don't display everything if don't null */
				break;

			/* get next list entry and check the end of the list */
			ref = tlskeys_list_get_next(ref, &tlskeys_reference);

		}

		appctx->st2 = STAT_ST_FIN;
		/* fall through */

	default:
		appctx->st2 = STAT_ST_FIN;
		return 1;
	}
	return 0;
}
#endif

static int stats_pats_list(struct stream_interface *si)
{
	struct appctx *appctx = __objt_appctx(si->end);

	switch (appctx->st2) {
	case STAT_ST_INIT:
		/* Display the column headers. If the message cannot be sent,
		 * quit the fucntion with returning 0. The function is called
		 * later and restart at the state "STAT_ST_INIT".
		 */
		chunk_reset(&trash);
		chunk_appendf(&trash, "# id (file) description\n");
		if (bi_putchk(si_ic(si), &trash) == -1) {
			si_applet_cant_put(si);
			return 0;
		}

		/* Now, we start the browsing of the references lists.
		 * Note that the following call to LIST_ELEM return bad pointer. The only
		 * available field of this pointer is <list>. It is used with the function
		 * pat_list_get_next() for retruning the first available entry
		 */
		appctx->ctx.map.ref = LIST_ELEM(&pattern_reference, struct pat_ref *, list);
		appctx->ctx.map.ref = pat_list_get_next(appctx->ctx.map.ref, &pattern_reference,
		                                        appctx->ctx.map.display_flags);
		appctx->st2 = STAT_ST_LIST;
		/* fall through */

	case STAT_ST_LIST:
		while (appctx->ctx.map.ref) {
			chunk_reset(&trash);

			/* Build messages. If the reference is used by another category than
			 * the listed categorie, display the information in the massage.
			 */
			chunk_appendf(&trash, "%d (%s) %s\n", appctx->ctx.map.ref->unique_id,
			              appctx->ctx.map.ref->reference ? appctx->ctx.map.ref->reference : "",
			              appctx->ctx.map.ref->display);

			if (bi_putchk(si_ic(si), &trash) == -1) {
				/* let's try again later from this stream. We add ourselves into
				 * this stream's users so that it can remove us upon termination.
				 */
				si_applet_cant_put(si);
				return 0;
			}

			/* get next list entry and check the end of the list */
			appctx->ctx.map.ref = pat_list_get_next(appctx->ctx.map.ref, &pattern_reference,
			                                        appctx->ctx.map.display_flags);
		}

		appctx->st2 = STAT_ST_FIN;
		/* fall through */

	default:
		appctx->st2 = STAT_ST_FIN;
		return 1;
	}
	return 0;
}

static int stats_map_lookup(struct stream_interface *si)
{
	struct appctx *appctx = __objt_appctx(si->end);
	struct sample sample;
	struct pattern *pat;
	int match_method;

	switch (appctx->st2) {
	case STAT_ST_INIT:
		/* Init to the first entry. The list cannot be change */
		appctx->ctx.map.expr = LIST_ELEM(&appctx->ctx.map.ref->pat, struct pattern_expr *, list);
		appctx->ctx.map.expr = pat_expr_get_next(appctx->ctx.map.expr, &appctx->ctx.map.ref->pat);
		appctx->st2 = STAT_ST_LIST;
		/* fall through */

	case STAT_ST_LIST:
		/* for each lookup type */
		while (appctx->ctx.map.expr) {
			/* initialise chunk to build new message */
			chunk_reset(&trash);

			/* execute pattern matching */
			sample.data.type = SMP_T_STR;
			sample.flags = SMP_F_CONST;
			sample.data.u.str.len = appctx->ctx.map.chunk.len;
			sample.data.u.str.str = appctx->ctx.map.chunk.str;
			if (appctx->ctx.map.expr->pat_head->match &&
			    sample_convert(&sample, appctx->ctx.map.expr->pat_head->expect_type))
				pat = appctx->ctx.map.expr->pat_head->match(&sample, appctx->ctx.map.expr, 1);
			else
				pat = NULL;

			/* build return message: set type of match */
			for (match_method=0; match_method<PAT_MATCH_NUM; match_method++)
				if (appctx->ctx.map.expr->pat_head->match == pat_match_fcts[match_method])
					break;
			if (match_method >= PAT_MATCH_NUM)
				chunk_appendf(&trash, "type=unknown(%p)", appctx->ctx.map.expr->pat_head->match);
			else
				chunk_appendf(&trash, "type=%s", pat_match_names[match_method]);

			/* case sensitive */
			if (appctx->ctx.map.expr->mflags & PAT_MF_IGNORE_CASE)
				chunk_appendf(&trash, ", case=insensitive");
			else
				chunk_appendf(&trash, ", case=sensitive");

			/* Display no match, and set default value */
			if (!pat) {
				if (appctx->ctx.map.display_flags == PAT_REF_MAP)
					chunk_appendf(&trash, ", found=no");
				else
					chunk_appendf(&trash, ", match=no");
			}

			/* Display match and match info */
			else {
				/* display match */
				if (appctx->ctx.map.display_flags == PAT_REF_MAP)
					chunk_appendf(&trash, ", found=yes");
				else
					chunk_appendf(&trash, ", match=yes");

				/* display index mode */
				if (pat->sflags & PAT_SF_TREE)
					chunk_appendf(&trash, ", idx=tree");
				else
					chunk_appendf(&trash, ", idx=list");

				/* display pattern */
				if (appctx->ctx.map.display_flags == PAT_REF_MAP) {
					if (pat->ref && pat->ref->pattern)
						chunk_appendf(&trash, ", key=\"%s\"", pat->ref->pattern);
					else
						chunk_appendf(&trash, ", key=unknown");
				}
				else {
					if (pat->ref && pat->ref->pattern)
						chunk_appendf(&trash, ", pattern=\"%s\"", pat->ref->pattern);
					else
						chunk_appendf(&trash, ", pattern=unknown");
				}

				/* display return value */
				if (appctx->ctx.map.display_flags == PAT_REF_MAP) {
					if (pat->data && pat->ref && pat->ref->sample)
						chunk_appendf(&trash, ", value=\"%s\", type=\"%s\"", pat->ref->sample,
						              smp_to_type[pat->data->type]);
					else
						chunk_appendf(&trash, ", value=none");
				}
			}

			chunk_appendf(&trash, "\n");

			/* display response */
			if (bi_putchk(si_ic(si), &trash) == -1) {
				/* let's try again later from this stream. We add ourselves into
				 * this stream's users so that it can remove us upon termination.
				 */
				si_applet_cant_put(si);
				return 0;
			}

			/* get next entry */
			appctx->ctx.map.expr = pat_expr_get_next(appctx->ctx.map.expr,
			                                         &appctx->ctx.map.ref->pat);
		}

		appctx->st2 = STAT_ST_FIN;
		/* fall through */

	default:
		appctx->st2 = STAT_ST_FIN;
		free(appctx->ctx.map.chunk.str);
		return 1;
	}
}

static int stats_pat_list(struct stream_interface *si)
{
	struct appctx *appctx = __objt_appctx(si->end);

	switch (appctx->st2) {

	case STAT_ST_INIT:
		/* Init to the first entry. The list cannot be change */
		appctx->ctx.map.elt = LIST_NEXT(&appctx->ctx.map.ref->head,
		                                struct pat_ref_elt *, list);
		if (&appctx->ctx.map.elt->list == &appctx->ctx.map.ref->head)
			appctx->ctx.map.elt = NULL;
		appctx->st2 = STAT_ST_LIST;
		/* fall through */

	case STAT_ST_LIST:
		while (appctx->ctx.map.elt) {
			chunk_reset(&trash);

			/* build messages */
			if (appctx->ctx.map.elt->sample)
				chunk_appendf(&trash, "%p %s %s\n",
				              appctx->ctx.map.elt, appctx->ctx.map.elt->pattern,
				              appctx->ctx.map.elt->sample);
			else
				chunk_appendf(&trash, "%p %s\n",
				              appctx->ctx.map.elt, appctx->ctx.map.elt->pattern);

			if (bi_putchk(si_ic(si), &trash) == -1) {
				/* let's try again later from this stream. We add ourselves into
				 * this stream's users so that it can remove us upon termination.
				 */
				si_applet_cant_put(si);
				return 0;
			}

			/* get next list entry and check the end of the list */
			appctx->ctx.map.elt = LIST_NEXT(&appctx->ctx.map.elt->list,
			                                struct pat_ref_elt *, list);
			if (&appctx->ctx.map.elt->list == &appctx->ctx.map.ref->head)
				break;
		}

		appctx->st2 = STAT_ST_FIN;
		/* fall through */

	default:
		appctx->st2 = STAT_ST_FIN;
		return 1;
	}
}

/* This function dumps all streams' states onto the stream interface's
 * read buffer. It returns 0 if the output buffer is full and it needs
 * to be called again, otherwise non-zero. It is designed to be called
 * from stats_dump_sess_to_buffer() below.
 */
static int stats_dump_sess_to_buffer(struct stream_interface *si)
{
	struct appctx *appctx = __objt_appctx(si->end);
	struct connection *conn;

	if (unlikely(si_ic(si)->flags & (CF_WRITE_ERROR|CF_SHUTW))) {
		/* If we're forced to shut down, we might have to remove our
		 * reference to the last stream being dumped.
		 */
		if (appctx->st2 == STAT_ST_LIST) {
			if (!LIST_ISEMPTY(&appctx->ctx.sess.bref.users)) {
				LIST_DEL(&appctx->ctx.sess.bref.users);
				LIST_INIT(&appctx->ctx.sess.bref.users);
			}
		}
		return 1;
	}

	chunk_reset(&trash);

	switch (appctx->st2) {
	case STAT_ST_INIT:
		/* the function had not been called yet, let's prepare the
		 * buffer for a response. We initialize the current stream
		 * pointer to the first in the global list. When a target
		 * stream is being destroyed, it is responsible for updating
		 * this pointer. We know we have reached the end when this
		 * pointer points back to the head of the streams list.
		 */
		LIST_INIT(&appctx->ctx.sess.bref.users);
		appctx->ctx.sess.bref.ref = streams.n;
		appctx->st2 = STAT_ST_LIST;
		/* fall through */

	case STAT_ST_LIST:
		/* first, let's detach the back-ref from a possible previous stream */
		if (!LIST_ISEMPTY(&appctx->ctx.sess.bref.users)) {
			LIST_DEL(&appctx->ctx.sess.bref.users);
			LIST_INIT(&appctx->ctx.sess.bref.users);
		}

		/* and start from where we stopped */
		while (appctx->ctx.sess.bref.ref != &streams) {
			char pn[INET6_ADDRSTRLEN];
			struct stream *curr_sess;

			curr_sess = LIST_ELEM(appctx->ctx.sess.bref.ref, struct stream *, list);

			if (appctx->ctx.sess.target) {
				if (appctx->ctx.sess.target != (void *)-1 && appctx->ctx.sess.target != curr_sess)
					goto next_sess;

				LIST_ADDQ(&curr_sess->back_refs, &appctx->ctx.sess.bref.users);
				/* call the proper dump() function and return if we're missing space */
				if (!stats_dump_full_sess_to_buffer(si, curr_sess))
					return 0;

				/* stream dump complete */
				LIST_DEL(&appctx->ctx.sess.bref.users);
				LIST_INIT(&appctx->ctx.sess.bref.users);
				if (appctx->ctx.sess.target != (void *)-1) {
					appctx->ctx.sess.target = NULL;
					break;
				}
				else
					goto next_sess;
			}

			chunk_appendf(&trash,
				     "%p: proto=%s",
				     curr_sess,
				     strm_li(curr_sess) ? strm_li(curr_sess)->proto->name : "?");

			conn = objt_conn(strm_orig(curr_sess));
			switch (conn ? addr_to_str(&conn->addr.from, pn, sizeof(pn)) : AF_UNSPEC) {
			case AF_INET:
			case AF_INET6:
				chunk_appendf(&trash,
					     " src=%s:%d fe=%s be=%s srv=%s",
					     pn,
					     get_host_port(&conn->addr.from),
					     strm_fe(curr_sess)->id,
					     (curr_sess->be->cap & PR_CAP_BE) ? curr_sess->be->id : "<NONE>",
					     objt_server(curr_sess->target) ? objt_server(curr_sess->target)->id : "<none>"
					     );
				break;
			case AF_UNIX:
				chunk_appendf(&trash,
					     " src=unix:%d fe=%s be=%s srv=%s",
					     strm_li(curr_sess)->luid,
					     strm_fe(curr_sess)->id,
					     (curr_sess->be->cap & PR_CAP_BE) ? curr_sess->be->id : "<NONE>",
					     objt_server(curr_sess->target) ? objt_server(curr_sess->target)->id : "<none>"
					     );
				break;
			}

			chunk_appendf(&trash,
				     " ts=%02x age=%s calls=%d",
				     curr_sess->task->state,
				     human_time(now.tv_sec - curr_sess->logs.tv_accept.tv_sec, 1),
				     curr_sess->task->calls);

			chunk_appendf(&trash,
				     " rq[f=%06xh,i=%d,an=%02xh,rx=%s",
				     curr_sess->req.flags,
				     curr_sess->req.buf->i,
				     curr_sess->req.analysers,
				     curr_sess->req.rex ?
				     human_time(TICKS_TO_MS(curr_sess->req.rex - now_ms),
						TICKS_TO_MS(1000)) : "");

			chunk_appendf(&trash,
				     ",wx=%s",
				     curr_sess->req.wex ?
				     human_time(TICKS_TO_MS(curr_sess->req.wex - now_ms),
						TICKS_TO_MS(1000)) : "");

			chunk_appendf(&trash,
				     ",ax=%s]",
				     curr_sess->req.analyse_exp ?
				     human_time(TICKS_TO_MS(curr_sess->req.analyse_exp - now_ms),
						TICKS_TO_MS(1000)) : "");

			chunk_appendf(&trash,
				     " rp[f=%06xh,i=%d,an=%02xh,rx=%s",
				     curr_sess->res.flags,
				     curr_sess->res.buf->i,
				     curr_sess->res.analysers,
				     curr_sess->res.rex ?
				     human_time(TICKS_TO_MS(curr_sess->res.rex - now_ms),
						TICKS_TO_MS(1000)) : "");

			chunk_appendf(&trash,
				     ",wx=%s",
				     curr_sess->res.wex ?
				     human_time(TICKS_TO_MS(curr_sess->res.wex - now_ms),
						TICKS_TO_MS(1000)) : "");

			chunk_appendf(&trash,
				     ",ax=%s]",
				     curr_sess->res.analyse_exp ?
				     human_time(TICKS_TO_MS(curr_sess->res.analyse_exp - now_ms),
						TICKS_TO_MS(1000)) : "");

			conn = objt_conn(curr_sess->si[0].end);
			chunk_appendf(&trash,
				     " s0=[%d,%1xh,fd=%d,ex=%s]",
				     curr_sess->si[0].state,
				     curr_sess->si[0].flags,
				     conn ? conn->t.sock.fd : -1,
				     curr_sess->si[0].exp ?
				     human_time(TICKS_TO_MS(curr_sess->si[0].exp - now_ms),
						TICKS_TO_MS(1000)) : "");

			conn = objt_conn(curr_sess->si[1].end);
			chunk_appendf(&trash,
				     " s1=[%d,%1xh,fd=%d,ex=%s]",
				     curr_sess->si[1].state,
				     curr_sess->si[1].flags,
				     conn ? conn->t.sock.fd : -1,
				     curr_sess->si[1].exp ?
				     human_time(TICKS_TO_MS(curr_sess->si[1].exp - now_ms),
						TICKS_TO_MS(1000)) : "");

			chunk_appendf(&trash,
				     " exp=%s",
				     curr_sess->task->expire ?
				     human_time(TICKS_TO_MS(curr_sess->task->expire - now_ms),
						TICKS_TO_MS(1000)) : "");
			if (task_in_rq(curr_sess->task))
				chunk_appendf(&trash, " run(nice=%d)", curr_sess->task->nice);

			chunk_appendf(&trash, "\n");

			if (bi_putchk(si_ic(si), &trash) == -1) {
				/* let's try again later from this stream. We add ourselves into
				 * this stream's users so that it can remove us upon termination.
				 */
				si_applet_cant_put(si);
				LIST_ADDQ(&curr_sess->back_refs, &appctx->ctx.sess.bref.users);
				return 0;
			}

		next_sess:
			appctx->ctx.sess.bref.ref = curr_sess->list.n;
		}

		if (appctx->ctx.sess.target && appctx->ctx.sess.target != (void *)-1) {
			/* specified stream not found */
			if (appctx->ctx.sess.section > 0)
				chunk_appendf(&trash, "  *** session terminated while we were watching it ***\n");
			else
				chunk_appendf(&trash, "Session not found.\n");

			if (bi_putchk(si_ic(si), &trash) == -1) {
				si_applet_cant_put(si);
				return 0;
			}

			appctx->ctx.sess.target = NULL;
			appctx->ctx.sess.uid = 0;
			return 1;
		}

		appctx->st2 = STAT_ST_FIN;
		/* fall through */

	default:
		appctx->st2 = STAT_ST_FIN;
		return 1;
	}
}

/* This is called when the stream interface is closed. For instance, upon an
 * external abort, we won't call the i/o handler anymore so we may need to
 * remove back references to the stream currently being dumped.
 */
static void cli_release_handler(struct appctx *appctx)
{
	if (appctx->st0 == STAT_CLI_O_SESS && appctx->st2 == STAT_ST_LIST) {
		if (!LIST_ISEMPTY(&appctx->ctx.sess.bref.users))
			LIST_DEL(&appctx->ctx.sess.bref.users);
	}
	else if (appctx->st0 == STAT_CLI_PRINT_FREE) {
		free(appctx->ctx.cli.err);
		appctx->ctx.cli.err = NULL;
	}
	else if (appctx->st0 == STAT_CLI_O_MLOOK) {
		free(appctx->ctx.map.chunk.str);
		appctx->ctx.map.chunk.str = NULL;
	}
}

/* This function is used to either dump tables states (when action is set
 * to STAT_CLI_O_TAB) or clear tables (when action is STAT_CLI_O_CLR).
 * It returns 0 if the output buffer is full and it needs to be called
 * again, otherwise non-zero.
 */
static int stats_table_request(struct stream_interface *si, int action)
{
	struct appctx *appctx = __objt_appctx(si->end);
	struct stream *s = si_strm(si);
	struct ebmb_node *eb;
	int dt;
	int skip_entry;
	int show = action == STAT_CLI_O_TAB;

	/*
	 * We have 3 possible states in appctx->st2 :
	 *   - STAT_ST_INIT : the first call
	 *   - STAT_ST_INFO : the proxy pointer points to the next table to
	 *     dump, the entry pointer is NULL ;
	 *   - STAT_ST_LIST : the proxy pointer points to the current table
	 *     and the entry pointer points to the next entry to be dumped,
	 *     and the refcount on the next entry is held ;
	 *   - STAT_ST_END : nothing left to dump, the buffer may contain some
	 *     data though.
	 */

	if (unlikely(si_ic(si)->flags & (CF_WRITE_ERROR|CF_SHUTW))) {
		/* in case of abort, remove any refcount we might have set on an entry */
		if (appctx->st2 == STAT_ST_LIST) {
			appctx->ctx.table.entry->ref_cnt--;
			stksess_kill_if_expired(&appctx->ctx.table.proxy->table, appctx->ctx.table.entry);
		}
		return 1;
	}

	chunk_reset(&trash);

	while (appctx->st2 != STAT_ST_FIN) {
		switch (appctx->st2) {
		case STAT_ST_INIT:
			appctx->ctx.table.proxy = appctx->ctx.table.target;
			if (!appctx->ctx.table.proxy)
				appctx->ctx.table.proxy = proxy;

			appctx->ctx.table.entry = NULL;
			appctx->st2 = STAT_ST_INFO;
			break;

		case STAT_ST_INFO:
			if (!appctx->ctx.table.proxy ||
			    (appctx->ctx.table.target &&
			     appctx->ctx.table.proxy != appctx->ctx.table.target)) {
				appctx->st2 = STAT_ST_END;
				break;
			}

			if (appctx->ctx.table.proxy->table.size) {
				if (show && !stats_dump_table_head_to_buffer(&trash, si, appctx->ctx.table.proxy,
									     appctx->ctx.table.target))
					return 0;

				if (appctx->ctx.table.target &&
				    strm_li(s)->bind_conf->level >= ACCESS_LVL_OPER) {
					/* dump entries only if table explicitly requested */
					eb = ebmb_first(&appctx->ctx.table.proxy->table.keys);
					if (eb) {
						appctx->ctx.table.entry = ebmb_entry(eb, struct stksess, key);
						appctx->ctx.table.entry->ref_cnt++;
						appctx->st2 = STAT_ST_LIST;
						break;
					}
				}
			}
			appctx->ctx.table.proxy = appctx->ctx.table.proxy->next;
			break;

		case STAT_ST_LIST:
			skip_entry = 0;

			if (appctx->ctx.table.data_type >= 0) {
				/* we're filtering on some data contents */
				void *ptr;
				long long data;

				dt = appctx->ctx.table.data_type;
				ptr = stktable_data_ptr(&appctx->ctx.table.proxy->table,
							appctx->ctx.table.entry,
							dt);

				data = 0;
				switch (stktable_data_types[dt].std_type) {
				case STD_T_SINT:
					data = stktable_data_cast(ptr, std_t_sint);
					break;
				case STD_T_UINT:
					data = stktable_data_cast(ptr, std_t_uint);
					break;
				case STD_T_ULL:
					data = stktable_data_cast(ptr, std_t_ull);
					break;
				case STD_T_FRQP:
					data = read_freq_ctr_period(&stktable_data_cast(ptr, std_t_frqp),
								    appctx->ctx.table.proxy->table.data_arg[dt].u);
					break;
				}

				/* skip the entry if the data does not match the test and the value */
				if ((data < appctx->ctx.table.value &&
				     (appctx->ctx.table.data_op == STD_OP_EQ ||
				      appctx->ctx.table.data_op == STD_OP_GT ||
				      appctx->ctx.table.data_op == STD_OP_GE)) ||
				    (data == appctx->ctx.table.value &&
				     (appctx->ctx.table.data_op == STD_OP_NE ||
				      appctx->ctx.table.data_op == STD_OP_GT ||
				      appctx->ctx.table.data_op == STD_OP_LT)) ||
				    (data > appctx->ctx.table.value &&
				     (appctx->ctx.table.data_op == STD_OP_EQ ||
				      appctx->ctx.table.data_op == STD_OP_LT ||
				      appctx->ctx.table.data_op == STD_OP_LE)))
					skip_entry = 1;
			}

			if (show && !skip_entry &&
			    !stats_dump_table_entry_to_buffer(&trash, si, appctx->ctx.table.proxy,
							      appctx->ctx.table.entry))
			    return 0;

			appctx->ctx.table.entry->ref_cnt--;

			eb = ebmb_next(&appctx->ctx.table.entry->key);
			if (eb) {
				struct stksess *old = appctx->ctx.table.entry;
				appctx->ctx.table.entry = ebmb_entry(eb, struct stksess, key);
				if (show)
					stksess_kill_if_expired(&appctx->ctx.table.proxy->table, old);
				else if (!skip_entry && !appctx->ctx.table.entry->ref_cnt)
					stksess_kill(&appctx->ctx.table.proxy->table, old);
				appctx->ctx.table.entry->ref_cnt++;
				break;
			}


			if (show)
				stksess_kill_if_expired(&appctx->ctx.table.proxy->table, appctx->ctx.table.entry);
			else if (!skip_entry && !appctx->ctx.table.entry->ref_cnt)
				stksess_kill(&appctx->ctx.table.proxy->table, appctx->ctx.table.entry);

			appctx->ctx.table.proxy = appctx->ctx.table.proxy->next;
			appctx->st2 = STAT_ST_INFO;
			break;

		case STAT_ST_END:
			appctx->st2 = STAT_ST_FIN;
			break;
		}
	}
	return 1;
}

/* print a line of text buffer (limited to 70 bytes) to <out>. The format is :
 * <2 spaces> <offset=5 digits> <space or plus> <space> <70 chars max> <\n>
 * which is 60 chars per line. Non-printable chars \t, \n, \r and \e are
 * encoded in C format. Other non-printable chars are encoded "\xHH". Original
 * lines are respected within the limit of 70 output chars. Lines that are
 * continuation of a previous truncated line begin with "+" instead of " "
 * after the offset. The new pointer is returned.
 */
static int dump_text_line(struct chunk *out, const char *buf, int bsize, int len,
			  int *line, int ptr)
{
	int end;
	unsigned char c;

	end = out->len + 80;
	if (end > out->size)
		return ptr;

	chunk_appendf(out, "  %05d%c ", ptr, (ptr == *line) ? ' ' : '+');

	while (ptr < len && ptr < bsize) {
		c = buf[ptr];
		if (isprint(c) && isascii(c) && c != '\\') {
			if (out->len > end - 2)
				break;
			out->str[out->len++] = c;
		} else if (c == '\t' || c == '\n' || c == '\r' || c == '\e' || c == '\\') {
			if (out->len > end - 3)
				break;
			out->str[out->len++] = '\\';
			switch (c) {
			case '\t': c = 't'; break;
			case '\n': c = 'n'; break;
			case '\r': c = 'r'; break;
			case '\e': c = 'e'; break;
			case '\\': c = '\\'; break;
			}
			out->str[out->len++] = c;
		} else {
			if (out->len > end - 5)
				break;
			out->str[out->len++] = '\\';
			out->str[out->len++] = 'x';
			out->str[out->len++] = hextab[(c >> 4) & 0xF];
			out->str[out->len++] = hextab[c & 0xF];
		}
		if (buf[ptr++] == '\n') {
			/* we had a line break, let's return now */
			out->str[out->len++] = '\n';
			*line = ptr;
			return ptr;
		}
	}
	/* we have an incomplete line, we return it as-is */
	out->str[out->len++] = '\n';
	return ptr;
}

/* This function dumps counters from all resolvers section and associated name servers.
 * It returns 0 if the output buffer is full and it needs
 * to be called again, otherwise non-zero.
 */
static int stats_dump_resolvers_to_buffer(struct stream_interface *si)
{
	struct appctx *appctx = __objt_appctx(si->end);
	struct dns_resolvers *presolvers;
	struct dns_nameserver *pnameserver;

	chunk_reset(&trash);

	switch (appctx->st2) {
	case STAT_ST_INIT:
		appctx->st2 = STAT_ST_LIST; /* let's start producing data */
		/* fall through */

	case STAT_ST_LIST:
		if (LIST_ISEMPTY(&dns_resolvers)) {
			chunk_appendf(&trash, "No resolvers found\n");
		}
		else {
			list_for_each_entry(presolvers, &dns_resolvers, list) {
				if (appctx->ctx.resolvers.ptr != NULL && appctx->ctx.resolvers.ptr != presolvers)
					continue;

				chunk_appendf(&trash, "Resolvers section %s\n", presolvers->id);
				list_for_each_entry(pnameserver, &presolvers->nameserver_list, list) {
					chunk_appendf(&trash, " nameserver %s:\n", pnameserver->id);
					chunk_appendf(&trash, "  sent: %ld\n", pnameserver->counters.sent);
					chunk_appendf(&trash, "  valid: %ld\n", pnameserver->counters.valid);
					chunk_appendf(&trash, "  update: %ld\n", pnameserver->counters.update);
					chunk_appendf(&trash, "  cname: %ld\n", pnameserver->counters.cname);
					chunk_appendf(&trash, "  cname_error: %ld\n", pnameserver->counters.cname_error);
					chunk_appendf(&trash, "  any_err: %ld\n", pnameserver->counters.any_err);
					chunk_appendf(&trash, "  nx: %ld\n", pnameserver->counters.nx);
					chunk_appendf(&trash, "  timeout: %ld\n", pnameserver->counters.timeout);
					chunk_appendf(&trash, "  refused: %ld\n", pnameserver->counters.refused);
					chunk_appendf(&trash, "  other: %ld\n", pnameserver->counters.other);
					chunk_appendf(&trash, "  invalid: %ld\n", pnameserver->counters.invalid);
					chunk_appendf(&trash, "  too_big: %ld\n", pnameserver->counters.too_big);
					chunk_appendf(&trash, "  truncated: %ld\n", pnameserver->counters.truncated);
					chunk_appendf(&trash, "  outdated: %ld\n", pnameserver->counters.outdated);
				}
			}
		}

		/* display response */
		if (bi_putchk(si_ic(si), &trash) == -1) {
			/* let's try again later from this session. We add ourselves into
			 * this session's users so that it can remove us upon termination.
			 */
			si->flags |= SI_FL_WAIT_ROOM;
			return 0;
		}

		appctx->st2 = STAT_ST_FIN;
		/* fall through */

	default:
		appctx->st2 = STAT_ST_FIN;
		return 1;
	}
}

/* This function dumps all captured errors onto the stream interface's
 * read buffer. It returns 0 if the output buffer is full and it needs
 * to be called again, otherwise non-zero.
 */
static int stats_dump_errors_to_buffer(struct stream_interface *si)
{
	struct appctx *appctx = __objt_appctx(si->end);
	extern const char *monthname[12];

	if (unlikely(si_ic(si)->flags & (CF_WRITE_ERROR|CF_SHUTW)))
		return 1;

	chunk_reset(&trash);

	if (!appctx->ctx.errors.px) {
		/* the function had not been called yet, let's prepare the
		 * buffer for a response.
		 */
		struct tm tm;

		get_localtime(date.tv_sec, &tm);
		chunk_appendf(&trash, "Total events captured on [%02d/%s/%04d:%02d:%02d:%02d.%03d] : %u\n",
			     tm.tm_mday, monthname[tm.tm_mon], tm.tm_year+1900,
			     tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(date.tv_usec/1000),
			     error_snapshot_id);

		if (bi_putchk(si_ic(si), &trash) == -1) {
			/* Socket buffer full. Let's try again later from the same point */
			si_applet_cant_put(si);
			return 0;
		}

		appctx->ctx.errors.px = proxy;
		appctx->ctx.errors.buf = 0;
		appctx->ctx.errors.bol = 0;
		appctx->ctx.errors.ptr = -1;
	}

	/* we have two inner loops here, one for the proxy, the other one for
	 * the buffer.
	 */
	while (appctx->ctx.errors.px) {
		struct error_snapshot *es;

		if (appctx->ctx.errors.buf == 0)
			es = &appctx->ctx.errors.px->invalid_req;
		else
			es = &appctx->ctx.errors.px->invalid_rep;

		if (!es->when.tv_sec)
			goto next;

		if (appctx->ctx.errors.iid >= 0 &&
		    appctx->ctx.errors.px->uuid != appctx->ctx.errors.iid &&
		    es->oe->uuid != appctx->ctx.errors.iid)
			goto next;

		if (appctx->ctx.errors.ptr < 0) {
			/* just print headers now */

			char pn[INET6_ADDRSTRLEN];
			struct tm tm;
			int port;

			get_localtime(es->when.tv_sec, &tm);
			chunk_appendf(&trash, " \n[%02d/%s/%04d:%02d:%02d:%02d.%03d]",
				     tm.tm_mday, monthname[tm.tm_mon], tm.tm_year+1900,
				     tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(es->when.tv_usec/1000));

			switch (addr_to_str(&es->src, pn, sizeof(pn))) {
			case AF_INET:
			case AF_INET6:
				port = get_host_port(&es->src);
				break;
			default:
				port = 0;
			}

			switch (appctx->ctx.errors.buf) {
			case 0:
				chunk_appendf(&trash,
					     " frontend %s (#%d): invalid request\n"
					     "  backend %s (#%d)",
					     appctx->ctx.errors.px->id, appctx->ctx.errors.px->uuid,
					     (es->oe->cap & PR_CAP_BE) ? es->oe->id : "<NONE>",
					     (es->oe->cap & PR_CAP_BE) ? es->oe->uuid : -1);
				break;
			case 1:
				chunk_appendf(&trash,
					     " backend %s (#%d): invalid response\n"
					     "  frontend %s (#%d)",
					     appctx->ctx.errors.px->id, appctx->ctx.errors.px->uuid,
					     es->oe->id, es->oe->uuid);
				break;
			}

			chunk_appendf(&trash,
				     ", server %s (#%d), event #%u\n"
				     "  src %s:%d, session #%d, session flags 0x%08x\n"
				     "  HTTP msg state %d, msg flags 0x%08x, tx flags 0x%08x\n"
				     "  HTTP chunk len %lld bytes, HTTP body len %lld bytes\n"
				     "  buffer flags 0x%08x, out %d bytes, total %lld bytes\n"
				     "  pending %d bytes, wrapping at %d, error at position %d:\n \n",
				     es->srv ? es->srv->id : "<NONE>", es->srv ? es->srv->puid : -1,
				     es->ev_id,
				     pn, port, es->sid, es->s_flags,
				     es->state, es->m_flags, es->t_flags,
				     es->m_clen, es->m_blen,
				     es->b_flags, es->b_out, es->b_tot,
				     es->len, es->b_wrap, es->pos);

			if (bi_putchk(si_ic(si), &trash) == -1) {
				/* Socket buffer full. Let's try again later from the same point */
				si_applet_cant_put(si);
				return 0;
			}
			appctx->ctx.errors.ptr = 0;
			appctx->ctx.errors.sid = es->sid;
		}

		if (appctx->ctx.errors.sid != es->sid) {
			/* the snapshot changed while we were dumping it */
			chunk_appendf(&trash,
				     "  WARNING! update detected on this snapshot, dump interrupted. Please re-check!\n");
			if (bi_putchk(si_ic(si), &trash) == -1) {
				si_applet_cant_put(si);
				return 0;
			}
			goto next;
		}

		/* OK, ptr >= 0, so we have to dump the current line */
		while (es->buf && appctx->ctx.errors.ptr < es->len && appctx->ctx.errors.ptr < global.tune.bufsize) {
			int newptr;
			int newline;

			newline = appctx->ctx.errors.bol;
			newptr = dump_text_line(&trash, es->buf, global.tune.bufsize, es->len, &newline, appctx->ctx.errors.ptr);
			if (newptr == appctx->ctx.errors.ptr)
				return 0;

			if (bi_putchk(si_ic(si), &trash) == -1) {
				/* Socket buffer full. Let's try again later from the same point */
				si_applet_cant_put(si);
				return 0;
			}
			appctx->ctx.errors.ptr = newptr;
			appctx->ctx.errors.bol = newline;
		};
	next:
		appctx->ctx.errors.bol = 0;
		appctx->ctx.errors.ptr = -1;
		appctx->ctx.errors.buf++;
		if (appctx->ctx.errors.buf > 1) {
			appctx->ctx.errors.buf = 0;
			appctx->ctx.errors.px = appctx->ctx.errors.px->next;
		}
	}

	/* dump complete */
	return 1;
}

/* This function dumps all environmnent variables to the buffer. It returns 0
 * if the output buffer is full and it needs to be called again, otherwise
 * non-zero. Dumps only one entry if st2 == STAT_ST_END.
 */
static int stats_dump_env_to_buffer(struct stream_interface *si)
{
	struct appctx *appctx = __objt_appctx(si->end);

	if (unlikely(si_ic(si)->flags & (CF_WRITE_ERROR|CF_SHUTW)))
		return 1;

	chunk_reset(&trash);

	/* we have two inner loops here, one for the proxy, the other one for
	 * the buffer.
	 */
	while (*appctx->ctx.env.var) {
		chunk_printf(&trash, "%s\n", *appctx->ctx.env.var);

		if (bi_putchk(si_ic(si), &trash) == -1) {
			si_applet_cant_put(si);
			return 0;
		}
		if (appctx->st2 == STAT_ST_END)
			break;
		appctx->ctx.env.var++;
	}

	/* dump complete */
	return 1;
}

/* parse the "level" argument on the bind lines */
static int bind_parse_level(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing level", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if (!strcmp(args[cur_arg+1], "user"))
		conf->level = ACCESS_LVL_USER;
	else if (!strcmp(args[cur_arg+1], "operator"))
		conf->level = ACCESS_LVL_OPER;
	else if (!strcmp(args[cur_arg+1], "admin"))
		conf->level = ACCESS_LVL_ADMIN;
	else {
		memprintf(err, "'%s' only supports 'user', 'operator', and 'admin' (got '%s')",
			  args[cur_arg], args[cur_arg+1]);
		return ERR_ALERT | ERR_FATAL;
	}

	return 0;
}

struct applet http_stats_applet = {
	.obj_type = OBJ_TYPE_APPLET,
	.name = "<STATS>", /* used for logging */
	.fct = http_stats_io_handler,
	.release = NULL,
};

static struct applet cli_applet = {
	.obj_type = OBJ_TYPE_APPLET,
	.name = "<CLI>", /* used for logging */
	.fct = cli_io_handler,
	.release = cli_release_handler,
};

static struct cfg_kw_list cfg_kws = {ILH, {
	{ CFG_GLOBAL, "stats", stats_parse_global },
	{ 0, NULL, NULL },
}};

static struct bind_kw_list bind_kws = { "STAT", { }, {
	{ "level",    bind_parse_level,    1 }, /* set the unix socket admin level */
	{ NULL, NULL, 0 },
}};

__attribute__((constructor))
static void __dumpstats_module_init(void)
{
	cfg_register_keywords(&cfg_kws);
	bind_register_keywords(&bind_kws);
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
