/*
 * Copyright (C) 2013 Red Hat
 *
 * Author: Nikos Mavrogiannopoulos
 *
 * This file is part of ocserv.
 *
 * ocserv is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuTLS is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <dbus/dbus.h>
#include <occtl.h>

#define DEFAULT_TIMEOUT (10*1000)

typedef void (*cmd_func) (DBusConnection * conn, const char *arg);

static void handle_help_cmd(DBusConnection * conn, const char *arg);
static void handle_exit_cmd(DBusConnection * conn, const char *arg);
static void handle_status_cmd(DBusConnection * conn, const char *arg);
static void handle_list_users_cmd(DBusConnection * conn, const char *arg);
static void handle_user_info_cmd(DBusConnection * conn, const char *arg);
static void handle_id_info_cmd(DBusConnection * conn, const char *arg);
static void handle_disconnect_user_cmd(DBusConnection * conn, const char *arg);
static void handle_disconnect_id_cmd(DBusConnection * conn, const char *arg);
static void handle_reset_cmd(DBusConnection * conn, const char *arg);
static void handle_reload_cmd(DBusConnection * conn, const char *arg);
static void handle_stop_cmd(DBusConnection * conn, const char *arg);

typedef struct {
	char *name;
	unsigned name_size;
	char *arg;
	cmd_func func;
	char *doc;
	int always_show;
} commands_st;

#define ENTRY(name, arg, func, doc, show) \
	{name, sizeof(name)-1, arg, func, doc, show}

static const commands_st commands[] = {
	ENTRY("status", NULL, handle_status_cmd,
	      "Print the status of the server", 1),
	ENTRY("stop", "now", handle_stop_cmd,
	      "Terminates the server", 1),
	ENTRY("reload", NULL, handle_reload_cmd,
	      "Reloads the server configuration", 1),
	ENTRY("list users", NULL, handle_list_users_cmd,
	      "Print the connected users", 1),
	ENTRY("info user", "[NAME]", handle_user_info_cmd,
	      "Print information on the specified user", 1),
	ENTRY("info id", "[NAME]", handle_id_info_cmd,
	      "Print information on the specified ID", 1),
	ENTRY("disconnect user", "[NAME]", handle_disconnect_user_cmd,
	      "Disconnect the specified user", 1),
	ENTRY("disconnect id", "[ID]", handle_disconnect_id_cmd,
	      "Disconnect the specified ID", 1),
	ENTRY("reset", NULL, handle_reset_cmd, "Resets the screen and terminal",
	      0),
	ENTRY("help", "or ?", handle_help_cmd, "Prints this help", 0),
	ENTRY("exit", NULL, handle_exit_cmd, "Exits this application", 0),
	ENTRY("?", NULL, handle_help_cmd, "Prints this help", -1),
	ENTRY("quit", NULL, handle_exit_cmd, "Exits this application", -1),
	{NULL, 0, NULL, NULL}
};

static void print_commands(unsigned interactive)
{
	unsigned int i;

	printf("Available Commands\n");
	for (i = 0;; i++) {
		if (commands[i].name == NULL)
			break;

		if (commands[i].always_show == -1)
			continue;

		if (commands[i].always_show == 0 && interactive == 0)
			continue;

		if (commands[i].arg)
			printf(" %12s %s\t%16s\n", commands[i].name,
			       commands[i].arg, commands[i].doc);
		else
			printf(" %16s\t%16s\n", commands[i].name,
			       commands[i].doc);
	}
}

static unsigned need_help(const char *arg)
{
	while (whitespace(*arg))
		arg++;

	if (arg[0] == 0 || (arg[0] == '?' && arg[1] == 0))
		return 1;

	return 0;
}

unsigned check_cmd_help(const char *line)
{
	unsigned int i;
	unsigned len = strlen(line);
	unsigned status = 0;

	while (len > 0 && (line[len - 1] == '?' || whitespace(line[len - 1])))
		len--;

	for (i = 0;; i++) {
		if (commands[i].name == NULL)
			break;

		if (len > commands[i].name_size)
			continue;

		if (strncasecmp(commands[i].name, line, len) == 0) {
			status = 1;
			if (commands[i].arg)
				printf(" %12s %s\t%16s\n", commands[i].name,
				       commands[i].arg, commands[i].doc);
			else
				printf(" %16s\t%16s\n", commands[i].name,
				       commands[i].doc);
		}
	}

	return status;
}

static
void usage(void)
{
	printf("occtl: [OPTIONS...] {COMMAND}\n\n");
	printf("  -h --help              Show this help\n");
	printf("\n");
	print_commands(0);
	printf("\n");
}

/* Read a string, and return a pointer to it.  Returns NULL on EOF. */
char *rl_gets(char *line_read)
{
	/* If the buffer has already been allocated, return the memory
	   to the free pool. */
	if (line_read) {
		free(line_read);
		line_read = (char *)NULL;
	}

	/* Get a line from the user. */
	line_read = readline("> ");

	/* If the line has any text in it, save it on the history. */
	if (line_read && *line_read)
		add_history(line_read);

	return (line_read);
}

/* sends a message and returns the reply */
DBusMessage *send_dbus_cmd(DBusConnection * conn,
			   const char *bus_name, const char *path,
			   const char *interface, const char *method,
			   unsigned type, const void *arg)
{
	DBusMessage *msg;
	DBusMessageIter args;
	DBusPendingCall *pending = NULL;

	msg = dbus_message_new_method_call(bus_name, path, interface, method);
	if (msg == NULL) {
		goto error;
	}
	dbus_message_iter_init_append(msg, &args);
	if (arg != NULL) {
		if (!dbus_message_iter_append_basic(&args, type, arg)) {
			goto error;
		}
	}

	if (!dbus_connection_send_with_reply
	    (conn, msg, &pending, DEFAULT_TIMEOUT)) {
		goto error;
	}

	if (pending == NULL)
		goto error;

	dbus_connection_flush(conn);
	dbus_message_unref(msg);

	/* wait for reply */
	dbus_pending_call_block(pending);

	msg = dbus_pending_call_steal_reply(pending);
	if (msg == NULL)
		goto error;

	dbus_pending_call_unref(pending);

	return msg;
 error:
	if (msg != NULL)
		dbus_message_unref(msg);
	return NULL;

}

static
void handle_status_cmd(DBusConnection * conn, const char *arg)
{
	DBusMessage *msg;
	DBusMessageIter args;
	dbus_bool_t status;
	dbus_uint32_t pid;
	dbus_uint32_t sec_mod_pid;
	dbus_uint32_t clients;

	msg = send_dbus_cmd(conn, "org.infradead.ocserv",
			    "/org/infradead/ocserv",
			    "org.infradead.ocserv", "status", 0, NULL);
	if (msg == NULL) {
		goto error_send;
	}

	if (!dbus_message_iter_init(msg, &args))
		goto error;

	if (DBUS_TYPE_BOOLEAN != dbus_message_iter_get_arg_type(&args))
		goto error_status;
	dbus_message_iter_get_basic(&args, &status);

	if (!dbus_message_iter_next(&args))
		goto error_recv;

	if (DBUS_TYPE_UINT32 != dbus_message_iter_get_arg_type(&args))
		goto error_parse;
	dbus_message_iter_get_basic(&args, &pid);

	if (!dbus_message_iter_next(&args))
		goto error_recv;

	if (DBUS_TYPE_UINT32 != dbus_message_iter_get_arg_type(&args))
		goto error_parse;
	dbus_message_iter_get_basic(&args, &sec_mod_pid);

	if (!dbus_message_iter_next(&args))
		goto error_recv;

	if (DBUS_TYPE_UINT32 != dbus_message_iter_get_arg_type(&args))
		goto error_parse;
	dbus_message_iter_get_basic(&args, &clients);

	printf("OpenConnect SSL VPN server\n");
	printf("     Status: %s\n", status != 0 ? "online" : "error");
	printf("    Clients: %u\n", (unsigned)clients);
	printf("\n");
	printf(" Server PID: %u\n", (unsigned)pid);
	printf("Sec-mod PID: %u\n", (unsigned)sec_mod_pid);

	dbus_message_unref(msg);

	return;

 error_status:
	printf("OpenConnect SSL VPN server\n");
	printf("     Status: offline\n");
	goto error;

 error_parse:
	fprintf(stderr, "%s: D-BUS message parsing error\n", __func__);
	goto error;
 error_send:
	fprintf(stderr, "%s: D-BUS message creation error\n", __func__);
	goto error;
 error_recv:
	fprintf(stderr, "%s: D-BUS message receiving error\n", __func__);
 error:
	if (msg != NULL)
		dbus_message_unref(msg);
}

static
void handle_reload_cmd(DBusConnection * conn, const char *arg)
{
	DBusMessage *msg;
	DBusMessageIter args;
	dbus_bool_t status;

	msg = send_dbus_cmd(conn, "org.infradead.ocserv",
			    "/org/infradead/ocserv",
			    "org.infradead.ocserv", "reload", 0, NULL);
	if (msg == NULL) {
		goto error_send;
	}

	if (!dbus_message_iter_init(msg, &args))
		goto error;

	if (DBUS_TYPE_BOOLEAN != dbus_message_iter_get_arg_type(&args))
		goto error_status;
	dbus_message_iter_get_basic(&args, &status);

	if (status != 0)
		printf("Server scheduled to reload\n");
	else
		goto error_status;

	dbus_message_unref(msg);

	return;

 error_status:
	printf("Error scheduling reload\n");
	goto error;
 error_send:
	fprintf(stderr, "%s: D-BUS message creation error\n", __func__);
	goto error;
 error:
	if (msg != NULL)
		dbus_message_unref(msg);
}

static
void handle_stop_cmd(DBusConnection * conn, const char *arg)
{
	DBusMessage *msg;
	DBusMessageIter args;
	dbus_bool_t status;

	if (arg == NULL || need_help(arg) || strncasecmp(arg, "now", 3) != 0) {
		check_cmd_help(rl_line_buffer);
		return;
	}

	msg = send_dbus_cmd(conn, "org.infradead.ocserv",
			    "/org/infradead/ocserv",
			    "org.infradead.ocserv", "stop", 0, NULL);
	if (msg == NULL) {
		goto error_send;
	}

	if (!dbus_message_iter_init(msg, &args))
		goto error;

	if (DBUS_TYPE_BOOLEAN != dbus_message_iter_get_arg_type(&args))
		goto error_status;
	dbus_message_iter_get_basic(&args, &status);

	if (status != 0)
		printf("Server scheduled to stop\n");
	else
		goto error_status;

	dbus_message_unref(msg);

	return;

 error_status:
	printf("Error scheduling server stop\n");
	goto error;
 error_send:
	fprintf(stderr, "%s: D-BUS message creation error\n", __func__);
	goto error;
 error:
	if (msg != NULL)
		dbus_message_unref(msg);
}

static
void handle_disconnect_user_cmd(DBusConnection * conn, const char *arg)
{
	DBusMessage *msg;
	DBusMessageIter args;
	dbus_bool_t status;

	if (arg == NULL || need_help(arg)) {
		check_cmd_help(rl_line_buffer);
		return;
	}

	msg = send_dbus_cmd(conn, "org.infradead.ocserv",
			    "/org/infradead/ocserv",
			    "org.infradead.ocserv",
			    "disconnect_name", DBUS_TYPE_STRING, &arg);
	if (msg == NULL) {
		goto error;
	}

	if (!dbus_message_iter_init(msg, &args))
		goto error;

	if (DBUS_TYPE_BOOLEAN != dbus_message_iter_get_arg_type(&args))
		goto error;
	dbus_message_iter_get_basic(&args, &status);

	if (status != 0) {
		printf("user '%s' was disconnected\n", arg);
	} else {
		printf("could not disconnect user '%s'\n", arg);
	}

	dbus_message_unref(msg);

	return;

 error:
	fprintf(stderr, "could not send message; is server online?\n");
	if (msg != NULL)
		dbus_message_unref(msg);
}

static
void handle_disconnect_id_cmd(DBusConnection * conn, const char *arg)
{
	DBusMessage *msg;
	DBusMessageIter args;
	dbus_bool_t status;
	dbus_uint32_t id;

	if (arg != NULL)
		id = atoi(arg);

	if (arg == NULL || need_help(arg) || id == 0) {
		check_cmd_help(rl_line_buffer);
		return;
	}

	msg = send_dbus_cmd(conn, "org.infradead.ocserv",
			    "/org/infradead/ocserv",
			    "org.infradead.ocserv",
			    "disconnect_id", DBUS_TYPE_UINT32, &id);
	if (msg == NULL) {
		goto error;
	}

	if (!dbus_message_iter_init(msg, &args))
		goto error;

	if (DBUS_TYPE_BOOLEAN != dbus_message_iter_get_arg_type(&args))
		goto error;
	dbus_message_iter_get_basic(&args, &status);

	if (status != 0) {
		printf("connection ID '%s' was disconnected\n", arg);
	} else {
		printf("could not disconnect connection ID '%s'\n", arg);
	}

	dbus_message_unref(msg);

	return;

 error:
	fprintf(stderr, "could not send message; is server online?\n");
	if (msg != NULL)
		dbus_message_unref(msg);
}

void handle_list_users_cmd(DBusConnection * conn, const char *arg)
{
	DBusMessage *msg;
	DBusMessageIter args, suba, subs;
	dbus_uint32_t id = 0;
	char *username = "";
	dbus_uint32_t since = 0;
	char *groupname = "", *ip = "";
	char *vpn_ipv4 = "", *vpn_ptp_ipv4 = "";
	char *vpn_ipv6 = "", *vpn_ptp_ipv6 = "";
	char *hostname = "", *auth = "", *device = "";
	char str_since[64];
	const char *vpn_ip;
	struct tm *tm;
	time_t t;
	FILE * out;
	unsigned iteration = 0;

	out = pager_start();

	msg = send_dbus_cmd(conn, "org.infradead.ocserv",
			    "/org/infradead/ocserv",
			    "org.infradead.ocserv", "list", 0, NULL);
	if (msg == NULL) {
		goto error_send;
	}

	if (!dbus_message_iter_init(msg, &args))
		goto cleanup;

	if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY)
		goto cleanup;

	dbus_message_iter_recurse(&args, &suba);

	for (;;) {
		if (dbus_message_iter_get_arg_type(&suba) != DBUS_TYPE_STRUCT)
			goto cleanup;
		dbus_message_iter_recurse(&suba, &subs);

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_UINT32)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &id);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &username);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &groupname);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &ip);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &device);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &vpn_ipv4);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &vpn_ptp_ipv4);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &vpn_ipv6);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &vpn_ptp_ipv6);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_UINT32)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &since);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &hostname);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &auth);

		if (vpn_ipv4 != NULL && vpn_ipv4[0] != 0)
			vpn_ip = vpn_ipv4;
		else
			vpn_ip = vpn_ipv6;

		/* add header */
		if (iteration++ == 0) {
			fprintf(out, "%6s %8s %8s %15s %15s %6s %16s %10s\n",
			       "id", "user", "group", "ip", "vpn-ip", "device", "since",
			       "auth");
		}

		t = since;
		tm = localtime(&t);
		strftime(str_since, sizeof(str_since), "%Y-%m-%d %H:%M", tm);
		fprintf(out, "%6u %8s %8s %15s %15s %6s %16s %10s\n",
		       (unsigned)id, username, groupname, ip, vpn_ip,
		       device, str_since, auth);

		if (!dbus_message_iter_next(&suba))
			goto cleanup;
	}

	goto cleanup;

 error_parse:
	fprintf(stderr, "%s: D-BUS message parsing error\n", __func__);
	goto cleanup;
 error_send:
	fprintf(stderr, "%s: D-BUS message creation error\n", __func__);
	goto cleanup;
 error_recv:
	fprintf(stderr, "%s: D-BUS message receiving error\n", __func__);
 cleanup:
	pager_stop(out);
	if (msg != NULL)
		dbus_message_unref(msg);
}

void common_info_cmd(DBusMessageIter *args)
{
	DBusMessageIter suba, subs;
	dbus_uint32_t id = 0;
	char *username = "";
	dbus_uint32_t since = 0;
	char *groupname = "", *ip = "";
	char *vpn_ipv4 = "", *vpn_ptp_ipv4 = "";
	char *vpn_ipv6 = "", *vpn_ptp_ipv6 = "";
	char *hostname = "", *auth = "", *device = "";
	char str_since[64];
	struct tm *tm;
	time_t t;
	FILE * out;
	unsigned at_least_one = 0;

	out = pager_start();

	if (dbus_message_iter_get_arg_type(args) != DBUS_TYPE_ARRAY)
		goto cleanup;

	dbus_message_iter_recurse(args, &suba);

	for (;;) {
		if (dbus_message_iter_get_arg_type(&suba) != DBUS_TYPE_STRUCT)
			goto cleanup;
		dbus_message_iter_recurse(&suba, &subs);

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_UINT32)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &id);

		fprintf(out, "ID: %u\n", (unsigned)id);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &username);


		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &groupname);


		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &ip);


		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &device);


		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &vpn_ipv4);


		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &vpn_ptp_ipv4);


		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &vpn_ipv6);


		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &vpn_ptp_ipv6);


		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_UINT32)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &since);

		t = since;
		tm = localtime(&t);
		strftime(str_since, sizeof(str_since), "%Y-%m-%d %H:%M", tm);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;

		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &hostname);

		if (!dbus_message_iter_next(&subs))
			goto error_recv;


		if (dbus_message_iter_get_arg_type(&subs) != DBUS_TYPE_STRING)
			goto error_parse;
		dbus_message_iter_get_basic(&subs, &auth);

		fprintf(out, "\tUsername: %s  ", username);
		fprintf(out, "Groupname: %s\n", groupname);
		fprintf(out, "\tAuth state: %s  ", auth);
		fprintf(out, "IP: %s\n", ip);

		if (vpn_ipv4 != NULL && vpn_ipv4[0] != 0 && 
			vpn_ptp_ipv4 != NULL && vpn_ptp_ipv4[0] != 0) {
			fprintf(out, "\tIPv4: %s  ", vpn_ipv4);
			fprintf(out, "P-t-P IPv4: %s\n", vpn_ptp_ipv4);
		}
		if (vpn_ipv6 != NULL && vpn_ipv6[0] != 0 &&
			vpn_ptp_ipv6 != NULL && vpn_ptp_ipv6[0] != 0) {
			fprintf(out, "\tIPv6: %s  ", vpn_ipv6);
			fprintf(out, "P-t-P IPv6: %s\n", vpn_ptp_ipv6);
		}
		fprintf(out, "\tDevice: %s  ", device);

		if (hostname != NULL && hostname[0] != 0)
			fprintf(out, "Hostname: %s\n", hostname);
		else
			fprintf(out, "\n");

		fprintf(out, "\tConnected since: %s\n", str_since);

		at_least_one = 1;

		if (!dbus_message_iter_next(&suba))
			goto cleanup;
	}

	goto cleanup;

 error_parse:
	fprintf(stderr, "%s: D-BUS message parsing error\n", __func__);
	goto cleanup;
 error_recv:
	fprintf(stderr, "%s: D-BUS message receiving error\n", __func__);
 cleanup:
 	if (at_least_one == 0)
 		fprintf(out, "user or ID not found\n");
	pager_stop(out);
}


void handle_user_info_cmd(DBusConnection * conn, const char *arg)
{
	DBusMessage *msg;
	DBusMessageIter args;

	if (arg == NULL || need_help(arg)) {
		check_cmd_help(rl_line_buffer);
		return;
	}

	msg = send_dbus_cmd(conn, "org.infradead.ocserv",
			    "/org/infradead/ocserv",
			    "org.infradead.ocserv", "user_info",
			    DBUS_TYPE_STRING, &arg);
	if (msg == NULL) {
		goto error_send;
	}

	if (!dbus_message_iter_init(msg, &args))
		goto cleanup;

	common_info_cmd(&args);
	
	goto cleanup;

 error_send:
	fprintf(stderr, "%s: D-BUS message creation error\n", __func__);
	goto cleanup;
 cleanup:
	if (msg != NULL)
		dbus_message_unref(msg);
}

void handle_id_info_cmd(DBusConnection * conn, const char *arg)
{
	DBusMessage *msg;
	DBusMessageIter args;
	dbus_uint32_t id = 0;

	if (arg != NULL)
		id = atoi(arg);

	if (arg == NULL || need_help(arg) || id == 0) {
		check_cmd_help(rl_line_buffer);
		return;
	}

	msg = send_dbus_cmd(conn, "org.infradead.ocserv",
			    "/org/infradead/ocserv",
			    "org.infradead.ocserv", "id_info",
			    DBUS_TYPE_UINT32, &id);
	if (msg == NULL) {
		goto error_send;
	}

	if (!dbus_message_iter_init(msg, &args))
		goto cleanup;

	common_info_cmd(&args);

	goto cleanup;

 error_send:
	fprintf(stderr, "%s: D-BUS message creation error\n", __func__);
	goto cleanup;
 cleanup:
	if (msg != NULL)
		dbus_message_unref(msg);
}

static void handle_help_cmd(DBusConnection * conn, const char *arg)
{
	print_commands(1);
}

static void handle_reset_cmd(DBusConnection * conn, const char *arg)
{
	rl_reset_terminal(NULL);
	rl_reset_screen_size();
}

static void handle_exit_cmd(DBusConnection * conn, const char *arg)
{
	exit(0);
}

/* checks whether an input command of type "  list   users" maches 
 * the given cmd (e.g., "list users"). If yes it executes func() and returns true.
 */
unsigned check_cmd(const char *cmd, const char *input,
		   DBusConnection * conn, cmd_func func)
{
	char *t, *p;
	unsigned len, tlen;
	unsigned i, j, ret = 0;
	char prev;

	while (whitespace(*input))
		input++;

	len = strlen(input);

	t = malloc(len + 1);
	if (t == NULL)
		return 0;

	prev = 0;
	p = t;
	for (i = j = 0; i < len; i++) {
		if (!whitespace(prev) || !whitespace(input[i])) {
			*p = input[i];
			prev = input[i];
			p++;
		}
	}
	*p = 0;
	tlen = p - t;
	len = strlen(cmd);

	if (len == 0)
		goto cleanup;

	if (tlen >= len && strncasecmp(cmd, t, len) == 0 && cmd[len] == 0) {	/* match */
		p = t + len;
		while (whitespace(*p))
			p++;

		func(conn, p);

		ret = 1;
	}

 cleanup:
	free(t);

	return ret;
}

char *stripwhite(char *string)
{
	register char *s, *t;

	for (s = string; whitespace(*s); s++) ;

	if (*s == 0)
		return (s);

	t = s + strlen(s) - 1;
	while (t > s && whitespace(*t))
		t--;
	*++t = '\0';

	return s;
}

void handle_cmd(DBusConnection * conn, char *line)
{
	char *cline;
	unsigned int i;

	cline = stripwhite(line);

	if (strlen(cline) == 0)
		return;

	for (i = 0;; i++) {
		if (commands[i].name == NULL)
			goto error;

		if (check_cmd
		    (commands[i].name, cline, conn, commands[i].func) != 0)
			break;
	}

	return;

 error:
	if (check_cmd_help(line) == 0) {
		fprintf(stderr, "unknown command: %s\n", line);
		fprintf(stderr,
			"use help or '?' to get a list of the available commands\n");
	}
	return;
}

static DBusConnection *init_dbus(void)
{
	DBusError err;
	DBusConnection *conn;

	dbus_error_init(&err);

	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (dbus_error_is_set(&err)) {
		fprintf(stderr, "D-BUS connection error (%s)\n", err.message);
		dbus_error_free(&err);
	}

	if (conn == NULL)
		exit(1);

	return conn;
}

char *merge_args(int argc, char **argv)
{
	unsigned size = 0;
	char *data, *p;
	unsigned i, len;

	for (i = 1; i < argc; i++) {
		size += strlen(argv[i]) + 1;
	}
	size++;
	data = malloc(size);
	if (data == NULL) {
		fprintf(stderr, "memory error\n");
		exit(1);
	}

	p = data;
	for (i = 1; i < argc; i++) {
		len = strlen(argv[i]);
		memcpy(p, argv[i], len);
		p += len;
		*p = ' ';
		p++;
	}

	*p = 0;

	return data;
}

static unsigned int cmd_start = 0;
static char *command_generator(const char *text, int state)
{
	static int list_index, len;
	unsigned name_size;
	char *name;

	/* If this is a new word to complete, initialize now.  This includes
	   saving the length of TEXT for efficiency, and initializing the index
	   variable to 0. */
	if (!state) {
		list_index = 0;
		len = strlen(text);
	}

	/* Return the next name which partially matches from the command list. */
	while ((name = commands[list_index].name)) {
		name_size = commands[list_index].name_size;
		list_index++;

		if (name_size < cmd_start)
			continue;

		if (cmd_start > 0 && name[cmd_start - 1] != ' ')
			continue;

		if (rl_line_buffer != NULL
		    && strncmp(rl_line_buffer, name, cmd_start) != 0)
			continue;

		name += cmd_start;

		if (strncmp(name, text, len) == 0) {
			return (strdup(name));
		}
	}

	return NULL;
}

static char *occtl_completion(char *text, int start, int end)
{
	cmd_start = start;
	return (void *)rl_completion_matches(text, command_generator);
}

void handle_sigint(int signo)
{
	rl_reset_line_state();
	rl_replace_line("", 0);
	rl_crlf();
	rl_redisplay();
	return;
}

void initialize_readline(void)
{
	rl_readline_name = "occtl";
	rl_attempted_completion_function = (CPPFunction *) occtl_completion;
	rl_completion_entry_function = command_generator;
	rl_completion_query_items = 20;
	rl_clear_signals();
	signal(SIGINT, handle_sigint);
}

int main(int argc, char **argv)
{
	char *line = NULL;
	DBusConnection *conn;

	conn = init_dbus();

	if (argc > 1) {
		if (argv[1][0] == '-') {
			usage();
			exit(0);
		}

		line = merge_args(argc, argv);
		handle_cmd(conn, line);

		free(line);
		return 0;
	}

	initialize_readline();

	fprintf(stderr,
		"OpenConnect server control (occtl) version %s\n\n", VERSION);

	for (;;) {
		line = rl_gets(line);
		if (line == NULL)
			return 0;

		handle_cmd(conn, line);
	}

	dbus_connection_close(conn);

	return 0;
}
