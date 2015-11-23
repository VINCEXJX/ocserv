/*
 * Copyright (C) 2013-2015 Nikos Mavrogiannopoulos
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#include <tlslib.h>
#ifdef HAVE_LIBUTIL
# include <utmpx.h>
#endif
#include <gettime.h>

#include <vpn.h>
#include <str.h>
#include <cookies.h>
#include <tun.h>
#include <main.h>
#include <ip-lease.h>
#include <script-list.h>
#include <ccan/list/list.h>

#define OCSERV_FW_SCRIPT "/usr/bin/ocserv-fw"

#define APPEND_TO_STR(str, val) \
			ret = str_append_str(str, val); \
			if (ret < 0) { \
				mslog(s, proc, LOG_ERR, "could not append value to environment\n"); \
				exit(1); \
			}

static void export_dns_route_info(main_server_st *s, struct proc_st* proc)
{
	str_st str4;
	str_st str6;
	str_st str_common;
	unsigned i;
	int ret;

	str_init(&str4, proc);
	str_init(&str6, proc);
	str_init(&str_common, proc);

	/* We use different export strings for IPv4 and IPv6 to ease handling
	 * with legacy software such as iptables and ip6tables. */

	/* append generic routes to str */
	for (i=0;i<s->config->network.routes_size;i++) {
		APPEND_TO_STR(&str_common, s->config->network.routes[i]);
		APPEND_TO_STR(&str_common, " ");

		if (strchr(s->config->network.routes[i], ':') != 0) {
			APPEND_TO_STR(&str6, s->config->network.routes[i]);
			APPEND_TO_STR(&str6, " ");
		} else {
			APPEND_TO_STR(&str4, s->config->network.routes[i]);
			APPEND_TO_STR(&str4, " ");
		}
	}

	/* append custom routes to str */
	for (i=0;i<proc->config.routes_size;i++) {
		APPEND_TO_STR(&str_common, proc->config.routes[i]);
		APPEND_TO_STR(&str_common, " ");

		if (strchr(proc->config.routes[i], ':') != 0) {
			APPEND_TO_STR(&str6, proc->config.routes[i]);
			APPEND_TO_STR(&str6, " ");
		} else {
			APPEND_TO_STR(&str4, proc->config.routes[i]);
			APPEND_TO_STR(&str4, " ");
		}
	}

	if (str4.length > 0 && setenv("OCSERV_ROUTES4", (char*)str4.data, 1) == -1) {
		mslog(s, proc, LOG_ERR, "could not export routes\n");
		exit(1);
	}

	if (str6.length > 0 && setenv("OCSERV_ROUTES6", (char*)str6.data, 1) == -1) {
		mslog(s, proc, LOG_ERR, "could not export routes\n");
		exit(1);
	}

	if (str_common.length > 0 && setenv("OCSERV_ROUTES", (char*)str_common.data, 1) == -1) {
		mslog(s, proc, LOG_ERR, "could not export routes\n");
		exit(1);
	}

	/* export the No-routes */

	str_reset(&str4);
	str_reset(&str6);
	str_reset(&str_common);

	/* append generic no_routes to str */
	for (i=0;i<s->config->network.no_routes_size;i++) {
		APPEND_TO_STR(&str_common, s->config->network.no_routes[i]);
		APPEND_TO_STR(&str_common, " ");

		if (strchr(s->config->network.no_routes[i], ':') != 0) {
			APPEND_TO_STR(&str6, s->config->network.no_routes[i]);
			APPEND_TO_STR(&str6, " ");
		} else {
			APPEND_TO_STR(&str4, s->config->network.no_routes[i]);
			APPEND_TO_STR(&str4, " ");
		}
	}

	/* append custom no_routes to str */
	for (i=0;i<proc->config.no_routes_size;i++) {
		APPEND_TO_STR(&str_common, proc->config.no_routes[i]);
		APPEND_TO_STR(&str_common, " ");

		if (strchr(proc->config.no_routes[i], ':') != 0) {
			APPEND_TO_STR(&str6, proc->config.no_routes[i]);
			APPEND_TO_STR(&str6, " ");
		} else {
			APPEND_TO_STR(&str4, proc->config.no_routes[i]);
			APPEND_TO_STR(&str4, " ");
		}
	}

	if (str4.length > 0 && setenv("OCSERV_NO_ROUTES4", (char*)str4.data, 1) == -1) {
		mslog(s, proc, LOG_ERR, "could not export no-routes\n");
		exit(1);
	}

	if (str6.length > 0 && setenv("OCSERV_NO_ROUTES6", (char*)str6.data, 1) == -1) {
		mslog(s, proc, LOG_ERR, "could not export no-routes\n");
		exit(1);
	}

	if (str_common.length > 0 && setenv("OCSERV_NO_ROUTES", (char*)str_common.data, 1) == -1) {
		mslog(s, proc, LOG_ERR, "could not export no-routes\n");
		exit(1);
	}

	/* export the DNS servers */

	str_reset(&str4);
	str_reset(&str6);
	str_reset(&str_common);

	if (proc->config.dns_size > 0) {
		for (i=0;i<proc->config.dns_size;i++) {
			APPEND_TO_STR(&str_common, proc->config.dns[i]);
			APPEND_TO_STR(&str_common, " ");

			if (strchr(proc->config.dns[i], ':') != 0) {
				APPEND_TO_STR(&str6, proc->config.dns[i]);
				APPEND_TO_STR(&str6, " ");
			} else {
				APPEND_TO_STR(&str4, proc->config.dns[i]);
				APPEND_TO_STR(&str4, " ");
			}
		}
	} else {
		for (i=0;i<s->config->network.dns_size;i++) {
			APPEND_TO_STR(&str_common, s->config->network.dns[i]);
			APPEND_TO_STR(&str_common, " ");

			if (strchr(s->config->network.dns[i], ':') != 0) {
				APPEND_TO_STR(&str6, s->config->network.dns[i]);
				APPEND_TO_STR(&str6, " ");
			} else {
				APPEND_TO_STR(&str4, s->config->network.dns[i]);
				APPEND_TO_STR(&str4, " ");
			}
		}
	}

	if (str4.length > 0 && setenv("OCSERV_DNS4", (char*)str4.data, 1) == -1) {
		mslog(s, proc, LOG_ERR, "could not export DNS servers\n");
		exit(1);
	}

	if (str6.length > 0 && setenv("OCSERV_DNS6", (char*)str6.data, 1) == -1) {
		mslog(s, proc, LOG_ERR, "could not export DNS servers\n");
		exit(1);
	}

	if (str_common.length > 0 && setenv("OCSERV_DNS", (char*)str_common.data, 1) == -1) {
		mslog(s, proc, LOG_ERR, "could not export DNS servers\n");
		exit(1);
	}

	str_clear(&str4);
	str_clear(&str6);
	str_clear(&str_common);
}

static
int call_script(main_server_st *s, struct proc_st* proc, unsigned up)
{
pid_t pid;
int ret;
const char* script, *next_script = NULL;

	if (up != 0)
		script = s->config->connect_script;
	else
		script = s->config->disconnect_script;

	if (proc->config.restrict_user_to_routes) {
		next_script = script;
		script = OCSERV_FW_SCRIPT;
	}

	if (script == NULL)
		return 0;

	pid = fork();
	if (pid == 0) {
		char real[64] = "";
		char local[64] = "";
		char remote[64] = "";

		sigprocmask(SIG_SETMASK, &sig_default_set, NULL);

		snprintf(real, sizeof(real), "%u", (unsigned)proc->pid);
		setenv("ID", real, 1);

		if (proc->remote_addr_len > 0) {
			if ((ret=getnameinfo((void*)&proc->remote_addr, proc->remote_addr_len, real, sizeof(real), NULL, 0, NI_NUMERICHOST)) != 0) {
				mslog(s, proc, LOG_DEBUG, "cannot determine peer address: %s; script failed", gai_strerror(ret));
				exit(1);
			}
			setenv("IP_REAL", real, 1);
		}

		if (proc->our_addr_len > 0) {
			if ((ret=getnameinfo((void*)&proc->our_addr, proc->our_addr_len, real, sizeof(real), NULL, 0, NI_NUMERICHOST)) != 0) {
				mslog(s, proc, LOG_DEBUG, "cannot determine our address: %s", gai_strerror(ret));
			} else {
				setenv("IP_REAL_LOCAL", real, 1);
			}
		}

		if (proc->ipv4 != NULL || proc->ipv6 != NULL) {
			if (proc->ipv4 && proc->ipv4->lip_len > 0) {
				if (getnameinfo((void*)&proc->ipv4->lip, proc->ipv4->lip_len, local, sizeof(local), NULL, 0, NI_NUMERICHOST) != 0) {
					mslog(s, proc, LOG_DEBUG, "cannot determine local VPN address; script failed");
					exit(1);
				}
				setenv("IP_LOCAL", local, 1);
			}

			if (proc->ipv6 && proc->ipv6->lip_len > 0) {
				if (getnameinfo((void*)&proc->ipv6->lip, proc->ipv6->lip_len, local, sizeof(local), NULL, 0, NI_NUMERICHOST) != 0) {
					mslog(s, proc, LOG_DEBUG, "cannot determine local VPN PtP address; script failed");
					exit(1);
				}
				if (local[0] == 0)
					setenv("IP_LOCAL", local, 1);
				setenv("IPV6_LOCAL", local, 1);
			}

			if (proc->ipv4 && proc->ipv4->rip_len > 0) {
				if (getnameinfo((void*)&proc->ipv4->rip, proc->ipv4->rip_len, remote, sizeof(remote), NULL, 0, NI_NUMERICHOST) != 0) {
					mslog(s, proc, LOG_DEBUG, "cannot determine local VPN address; script failed");
					exit(1);
				}
				setenv("IP_REMOTE", remote, 1);
			}
			if (proc->ipv6 && proc->ipv6->rip_len > 0) {
				if (getnameinfo((void*)&proc->ipv6->rip, proc->ipv6->rip_len, remote, sizeof(remote), NULL, 0, NI_NUMERICHOST) != 0) {
					mslog(s, proc, LOG_DEBUG, "cannot determine local VPN PtP address; script failed");
					exit(1);
				}
				if (remote[0] == 0)
					setenv("IP_REMOTE", remote, 1);
				setenv("IPV6_REMOTE", remote, 1);

				snprintf(remote, sizeof(remote), "%u", proc->ipv6->prefix);
				setenv("IPV6_PREFIX", remote, 1);
			}
		}

		setenv("USERNAME", proc->username, 1);
		setenv("GROUPNAME", proc->groupname, 1);
		setenv("HOSTNAME", proc->hostname, 1);
		setenv("DEVICE", proc->tun_lease.name, 1);
		if (up) {
			setenv("REASON", "connect", 1);
		} else {
			/* use remote as temp buffer */
			snprintf(remote, sizeof(remote), "%lu", (unsigned long)proc->bytes_in);
			setenv("STATS_BYTES_IN", remote, 1);
			snprintf(remote, sizeof(remote), "%lu", (unsigned long)proc->bytes_out);
			setenv("STATS_BYTES_OUT", remote, 1);
			if (proc->conn_time > 0) {
				snprintf(remote, sizeof(remote), "%lu", (unsigned long)(time(0)-proc->conn_time));
				setenv("STATS_DURATION", remote, 1);
			}
			setenv("REASON", "disconnect", 1);
		}

		/* export DNS and route info */
		export_dns_route_info(s, proc);

		if (next_script) {
			setenv("OCSERV_NEXT_SCRIPT", next_script, 1);
			mslog(s, proc, LOG_DEBUG, "executing script %s %s (next: %s)", up?"up":"down", script, next_script);
		} else
			mslog(s, proc, LOG_DEBUG, "executing script %s %s", up?"up":"down", script);
		ret = execl(script, script, NULL);
		if (ret == -1) {
			mslog(s, proc, LOG_ERR, "Could not execute script %s", script);
			exit(1);
		}
			
		exit(77);
	} else if (pid == -1) {
		mslog(s, proc, LOG_ERR, "Could not fork()");
		return -1;
	}
	
	if (up) {
		add_to_script_list(s, pid, up, proc);
		return ERR_WAIT_FOR_SCRIPT;
	} else {
		return 0;
	}
}

static void
add_utmp_entry(main_server_st *s, struct proc_st* proc)
{
#ifdef HAVE_LIBUTIL
	struct utmpx entry;
	struct timespec tv;
	
	if (s->config->use_utmp == 0)
		return;

	memset(&entry, 0, sizeof(entry));
	entry.ut_type = USER_PROCESS;
	entry.ut_pid = proc->pid;
	strlcpy(entry.ut_line, proc->tun_lease.name, sizeof(entry.ut_line));
	strlcpy(entry.ut_user, proc->username, sizeof(entry.ut_user));
#ifdef __linux__
	if (proc->remote_addr_len == sizeof(struct sockaddr_in))
		memcpy(entry.ut_addr_v6, SA_IN_P(&proc->remote_addr), sizeof(struct in_addr));
	else
		memcpy(entry.ut_addr_v6, SA_IN6_P(&proc->remote_addr), sizeof(struct in6_addr));
#endif

	gettime(&tv);
	entry.ut_tv.tv_sec = tv.tv_sec;
	entry.ut_tv.tv_usec = tv.tv_nsec / 1000;
	getnameinfo((void*)&proc->remote_addr, proc->remote_addr_len, entry.ut_host, sizeof(entry.ut_host), NULL, 0, NI_NUMERICHOST);

	setutxent();
	pututxline(&entry);
	endutxent();

#if defined(WTMPX_FILE)
	updwtmpx(WTMPX_FILE, &entry);
#endif   
	
	return;
#endif
}

static void remove_utmp_entry(main_server_st *s, struct proc_st* proc)
{
#ifdef HAVE_LIBUTIL
	struct utmpx entry;
	struct timespec tv;

	if (s->config->use_utmp == 0)
		return;

	memset(&entry, 0, sizeof(entry));
	entry.ut_type = DEAD_PROCESS;
	if (proc->tun_lease.name[0] != 0)
		strlcpy(entry.ut_line, proc->tun_lease.name, sizeof(entry.ut_line));
	entry.ut_pid = proc->pid;

	setutxent();
	pututxline(&entry);
	endutxent();

#if defined(WTMPX_FILE)
	gettime(&tv);
	entry.ut_tv.tv_sec = tv.tv_sec;
	entry.ut_tv.tv_usec = tv.tv_nsec / 1000;
	updwtmpx(WTMPX_FILE, &entry);
#endif   
	return;
#endif
}

int user_connected(main_server_st *s, struct proc_st* proc)
{
int ret;

	add_utmp_entry(s, proc);

	ret = call_script(s, proc, 1);
	if (ret < 0)
		return ret;

	return 0;
}

void user_disconnected(main_server_st *s, struct proc_st* proc)
{
	remove_utmp_entry(s, proc);
	call_script(s, proc, 0);
}

