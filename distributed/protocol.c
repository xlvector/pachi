/* The functions implementing the master-slave protocol of the
 * distributed engine are grouped here. They are independent
 * of the gtp protocol. See the comments at the top of distributed.c
 * for a general introduction to the distributed engine. */

#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <ctype.h>

#define DEBUG

#include "random.h"
#include "timeinfo.h"
#include "playout.h"
#include "network.h"
#include "debug.h"
#include "distributed/distributed.h"
#include "distributed/protocol.h"

/* All gtp commands for current game separated by \n */
static char gtp_cmds[CMDS_SIZE];

/* Latest gtp command sent to slaves. */
static char *gtp_cmd = NULL;

/* Slaves send gtp_cmd when cmd_count changes. */
static int cmd_count = 0;

/* Remember at most 10 gtp ids per move: kgs-rules, boardsize, clear_board,
 * time_settings, komi, handicap, genmoves, play pass, play pass, final_status_list */
#define MAX_CMDS_PER_MOVE 10

/* History of gtp commands sent for current game, indexed by move. */
struct cmd_history {
	int gtp_id;
	char *next_cmd;
} history[MAX_GAMELEN][MAX_CMDS_PER_MOVE];

/* Number of active slave machines working for this master. */
int active_slaves = 0;

/* Number of replies to last gtp command already received. */
int reply_count = 0;

/* All replies to latest gtp command are in gtp_replies[0..reply_count-1]. */
char **gtp_replies;

/* Mutex protecting gtp_cmds, gtp_cmd, history,
 * cmd_count, active_slaves, reply_count & gtp_replies */
static pthread_mutex_t slave_lock = PTHREAD_MUTEX_INITIALIZER;

/* Condition signaled when a new gtp command is available. */
static pthread_cond_t cmd_cond = PTHREAD_COND_INITIALIZER;

/* Condition signaled when reply_count increases. */
static pthread_cond_t reply_cond = PTHREAD_COND_INITIALIZER;

/* Mutex protecting stderr. Must not be held at same time as slave_lock. */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* Absolute time when this program was started.
 * For debugging only. */
static double start_time;

/* Get exclusive access to the threads and commands state. */
void
protocol_lock(void)
{
	pthread_mutex_lock(&slave_lock);
}

/* Release exclusive access to the threads and commands state. */
void
protocol_unlock(void)
{
	pthread_mutex_unlock(&slave_lock);
}

/* Write the time, client address, prefix, and string s to stderr atomically.
 * s should end with a \n */
void
logline(struct in_addr *client, char *prefix, char *s)
{
	double now = time_now();

	char addr[INET_ADDRSTRLEN];
	if (client) {
		inet_ntop(AF_INET, client, addr, sizeof(addr));
	} else {
		addr[0] = '\0';
	}
	pthread_mutex_lock(&log_lock);
	fprintf(stderr, "%s%15s %9.3f: %s", prefix, addr, now - start_time, s);
	pthread_mutex_unlock(&log_lock);
}

/* Thread opening a connection on the given socket and copying input
 * from there to stderr. */
static void *
proxy_thread(void *arg)
{
	int proxy_sock = (long)arg;
	assert(proxy_sock >= 0);
	for (;;) {
		struct in_addr client;
		int conn = open_server_connection(proxy_sock, &client);
		FILE *f = fdopen(conn, "r");
		char buf[BSIZE];
		while (fgets(buf, BSIZE, f)) {
			logline(&client, "< ", buf);
		}
		fclose(f);
	}
}

/* Get a reply to one gtp command. Return the gtp command id,
 * or -1 if error. reply must have at least CMDS_SIZE bytes.
 * slave_lock is not held on either entry or exit of this function. */
static int
get_reply(FILE *f, struct in_addr client, char *reply)
{
	int reply_id = -1;
	*reply = '\0';
	char *line = reply;
	while (fgets(line, reply + CMDS_SIZE - line, f) && *line != '\n') {
		if (DEBUGL(3) || (DEBUGL(2) && line == reply))
			logline(&client, "<<", line);
		if (reply_id < 0 && (*line == '=' || *line == '?') && isdigit(line[1]))
			reply_id = atoi(line+1);
		line += strlen(line);
	}
	if (*line != '\n') return -1;
	return reply_id;
}

/* Send one gtp command and get a reply from the slave machine.
 * Write the reply in buf which must have at least CMDS_SIZE bytes.
 * Return the gtp command id, or -1 if error.
 * slave_lock is held on both entry and exit of this function. */
static int
send_command(char *to_send, FILE *f, struct in_addr client, char *buf)
{
	assert(to_send && gtp_cmd);
	strncpy(buf, to_send, CMDS_SIZE);
	bool resend = to_send != gtp_cmd;

	pthread_mutex_unlock(&slave_lock);

	if (DEBUGL(1) && resend)
		logline(&client, "? ",
			to_send == gtp_cmds ? "resend all\n" : "partial resend\n");
	fputs(buf, f);
	fflush(f);
	if (DEBUGL(2)) {
		if (!DEBUGL(3)) {
			char *s = strchr(buf, '\n');
			if (s) s[1] = '\0';
		}
		logline(&client, ">>", buf);
	}

	int reply_id = get_reply(f, client, buf);

	pthread_mutex_lock(&slave_lock);
	return reply_id;
}

/* Return the command sent after that with the given gtp id,
 * or gtp_cmds if the id wasn't used in this game. If a play command
 * has overwritten a genmoves command, return the play command.
 * slave_lock is held on both entry and exit of this function. */
static char *
next_command(int cmd_id)
{
	if (cmd_id == -1) return gtp_cmds;

	int last_id = atoi(gtp_cmd);
	int reply_move = move_number(cmd_id);
	if (reply_move > move_number(last_id)) return gtp_cmds;

	int slot;
	for (slot = 0; slot < MAX_CMDS_PER_MOVE; slot++) {
		if (cmd_id == history[reply_move][slot].gtp_id) break;
	}
	if (slot == MAX_CMDS_PER_MOVE) return gtp_cmds;

	char *next = history[reply_move][slot].next_cmd;
	assert(next);
	return next;
}

/* Process the reply received from a slave machine.
 * Copy it to reply_buf and return false if ok, or return
 * true if the slave is out of sync.
 * slave_lock is held on both entry and exit of this function. */
static bool
process_reply(int reply_id, char *reply, char *reply_buf,
	      int *last_reply_id, int *reply_slot)
{
	bool resend = true;
	/* For resend everything if slave returned an error. */
	if (*reply != '=') {
		*last_reply_id = -1;
		return resend;
	}
	/* Make sure we are still in sync. cmd_count may have
	 * changed but the reply is valid as long as cmd_id didn't
	 * change (this only occurs for consecutive genmoves). */
	int cmd_id = atoi(gtp_cmd);
	if (reply_id == cmd_id) {
		strncpy(reply_buf, reply, CMDS_SIZE);
		if (reply_id != *last_reply_id)
			*reply_slot = reply_count++;
		gtp_replies[*reply_slot] = reply_buf;

		pthread_cond_signal(&reply_cond);
		resend = false;
	}
	*last_reply_id = reply_id;
	return resend;
}

/* Main loop of a slave thread.
 * Send the current command to the slave machine and wait for a reply.
 * Resend command history if the slave machine is out of sync.
 * Returns when the connection with the slave machine is cut.
 * slave_lock is held on both entry and exit of this function. */
static void
slave_loop(FILE *f, struct in_addr client, char *reply_buf, bool resend)
{
	char *to_send;
	int last_cmd_sent = 0;
	int last_reply_id = -1;
	int reply_slot = -1;
	for (;;) {
		if (resend) {
			/* Resend complete or partial history */
			to_send = next_command(last_reply_id);
		} else {
			/* Wait for a new command. */
			while (last_cmd_sent == cmd_count)
				pthread_cond_wait(&cmd_cond, &slave_lock);
			to_send = gtp_cmd;
		}

		/* Command available, send it to slave machine.
		 * If slave was out of sync, send the history. */
		char buf[CMDS_SIZE];
		last_cmd_sent = cmd_count;

		/* Send the command and get the reply, which always ends with \n\n
		 * The slave machine sends "=id reply" or "?id reply"
		 * with id == cmd_id if it is in sync. */
		int reply_id = send_command(to_send, f, client, buf);
		if (reply_id == -1) return;

		resend = process_reply(reply_id, buf, reply_buf,
				       &last_reply_id, &reply_slot);
		if (!resend)
			/* Good reply. Force waiting for a new command.
			 * The next genmoves stats we send must include those
			 * just received (this is assumed by the slave). */
			last_cmd_sent = cmd_count;
	}
}

/* Thread sending gtp commands to one slave machine, and
 * reading replies. If a slave machine dies, this thread waits
 * for a connection from another slave. */
static void *
slave_thread(void *arg)
{
	int slave_sock = (long)arg;
	assert(slave_sock >= 0);
	char reply_buf[CMDS_SIZE];
	bool resend = false;

	for (;;) {
		/* Wait for a connection from any slave. */
		struct in_addr client;
		int conn = open_server_connection(slave_sock, &client);

		FILE *f = fdopen(conn, "r+");
		if (DEBUGL(2))
			logline(&client, "= ", "new slave\n");

		/* Minimal check of the slave identity. */
		fputs("name\n", f);
		if (!fgets(reply_buf, sizeof(reply_buf), f)
		    || strncasecmp(reply_buf, "= Pachi", 7)
		    || !fgets(reply_buf, sizeof(reply_buf), f)
		    || strcmp(reply_buf, "\n")) {
			logline(&client, "? ", "bad slave\n");
			fclose(f);
			continue;
		}

		pthread_mutex_lock(&slave_lock);
		active_slaves++;
		slave_loop(f, client, reply_buf, resend);

		assert(active_slaves > 0);
		active_slaves--;
		// Unblock main thread if it was waiting for this slave.
		pthread_cond_signal(&reply_cond);
		pthread_mutex_unlock(&slave_lock);

		resend = true;
		if (DEBUGL(2))
			logline(&client, "= ", "lost slave\n");
		fclose(f);
	}
}

/* Create a new gtp command for all slaves. The slave lock is held
 * upon entry and upon return, so the command will actually be
 * sent when the lock is released. The last command is overwritten
 * if gtp_cmd points to a non-empty string. cmd is a single word;
 * args has all arguments and is empty or has a trailing \n */
void
update_cmd(struct board *b, char *cmd, char *args, bool new_id)
{
	assert(gtp_cmd);
	/* To make sure the slaves are in sync, we ignore the original id
	 * and use the board number plus some random bits as gtp id. */
	static int gtp_id = -1;
	int moves = is_reset(cmd) ? 0 : b->moves;
	if (new_id) {
	        /* fast_random() is 16-bit only so the multiplication can't overflow. */
		gtp_id = force_reply(moves + fast_random(65535) * DIST_GAMELEN);
		reply_count = 0;
	}
	snprintf(gtp_cmd, gtp_cmds + CMDS_SIZE - gtp_cmd, "%d %s %s",
		 gtp_id, cmd, *args ? args : "\n");
	cmd_count++;

	/* Remember history for out-of-sync slaves. */
	static int slot = 0;
	static struct cmd_history *last = NULL;
	if (new_id) {
		if (last) last->next_cmd = gtp_cmd;
		slot = (slot + 1) % MAX_CMDS_PER_MOVE;
		last = &history[moves][slot];
		last->gtp_id = gtp_id;
		last->next_cmd = NULL;
	}
	// Notify the slave threads about the new command.
	pthread_cond_broadcast(&cmd_cond);
}

/* Update the command history, then create a new gtp command
 * for all slaves. The slave lock is held upon entry and
 * upon return, so the command will actually be sent when the
 * lock is released. cmd is a single word; args has all
 * arguments and is empty or has a trailing \n */
void
new_cmd(struct board *b, char *cmd, char *args)
{
	// Clear the history when a new game starts:
	if (!gtp_cmd || is_gamestart(cmd)) {
		gtp_cmd = gtp_cmds;
		memset(history, 0, sizeof(history));
	} else {
		/* Preserve command history for new slaves.
		 * To indicate that the slave should only reply to
		 * the last command we force the id of previous
		 * commands to be just the move number. */
		int id = prevent_reply(atoi(gtp_cmd));
		int len = strspn(gtp_cmd, "0123456789");
		char buf[32];
		snprintf(buf, sizeof(buf), "%0*d", len, id);
		memcpy(gtp_cmd, buf, len);

		gtp_cmd += strlen(gtp_cmd);
	}

	// Let the slave threads send the new gtp command:
	update_cmd(b, cmd, args, true);
}

/* Wait for at least one new reply. Return when all slaves have
 * replied, or when the given absolute time is passed.
 * The replies are returned in gtp_replies[0..reply_count-1]
 * slave_lock is held on entry and on return. */
void
get_replies(double time_limit)
{
	for (;;) {
		if (reply_count > 0) {
			struct timespec ts;
			double sec;
			ts.tv_nsec = (int)(modf(time_limit, &sec)*1000000000.0);
			ts.tv_sec = (int)sec;
			pthread_cond_timedwait(&reply_cond, &slave_lock, &ts);
		} else {
			pthread_cond_wait(&reply_cond, &slave_lock);
		}
		if (reply_count == 0) continue;
		if (reply_count >= active_slaves) return;
		if (time_now() >= time_limit) break;
	}
	if (DEBUGL(1)) {
		char buf[1024];
		snprintf(buf, sizeof(buf),
			 "get_replies timeout %.3f >= %.3f, replies %d < active %d\n",
			 time_now() - start_time, time_limit - start_time,
			 reply_count, active_slaves);
		logline(NULL, "? ", buf);
	}
	assert(reply_count > 0);
}

/* Create the slave and proxy threads. */
void
protocol_init(char *slave_port, char *proxy_port, int max_slaves)
{
	start_time = time_now();

	int slave_sock = port_listen(slave_port, max_slaves);
	pthread_t thread;
	for (int id = 0; id < max_slaves; id++) {
		pthread_create(&thread, NULL, slave_thread, (void *)(long)slave_sock);
	}

	if (proxy_port) {
		int proxy_sock = port_listen(proxy_port, max_slaves);
		for (int id = 0; id < max_slaves; id++) {
			pthread_create(&thread, NULL, proxy_thread, (void *)(long)proxy_sock);
		}
	}
}
