/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */

#define HELPS	"Usage: aprsc [-t <chrootdir>] [-u <setuid user>] [-c <cfgfile>] [-f (fork)]\n" \
	" [-n <logname>] [-e <loglevel>] [-o <logdest>] [-r <logdir>] [-h (help)]\n"

#include <pthread.h>
#include <semaphore.h>

#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <time.h>
#include <sys/time.h>
#ifdef __linux__ /* Very Linux-specific code.. */
#include <sys/syscall.h>
#endif
#include <sys/resource.h>
#include <locale.h>
#include <ctype.h>

#include "hmalloc.h"
#include "hlog.h"
#include "config.h"
#include "accept.h"
#include "uplink.h"
#include "worker.h"
#include "status.h"
#include "http.h"
#include "version.h"

#include "dupecheck.h"
#include "filter.h"
#include "historydb.h"
#include "client_heard.h"
#include "keyhash.h"

struct itimerval itv; // Linux profiling timer does not pass over to pthreads..

int shutting_down;		// are we shutting down now?
int reopen_logs;		// should we reopen log files now?
int reconfiguring;		// should we reconfigure now?
int fileno_limit;
int dbdump_at_exit;
int want_dbdump;

/* random instance ID, alphanumeric low-case */
#define INSTANCE_ID_LEN 8
char instance_id[INSTANCE_ID_LEN+1];

pthread_attr_t pthr_attrs;

/*
 *	Parse arguments
 */

void parse_cmdline(int argc, char *argv[])
{
	int s, i;
	int failed = 0;
	
	while ((s = getopt(argc, argv, "c:ft:u:n:r:d:De:o:?h")) != -1) {
	switch (s) {
		case 'c':
			cfgfile = hstrdup(optarg);
			break;
		case 'f':
			fork_a_daemon = 1;
			break;
		case 't':
			chrootdir = hstrdup(optarg);
			break;
		case 'u':
			setuid_s = hstrdup(optarg);
			break;
		case 'n':
			logname = hstrdup(optarg);
			break;
		case 'r':
			log_dir = hstrdup(optarg);
			break;
		case 'd':
			if (!strcasecmp(optarg, "requests")) {
				//dump_requests = 1;
			} else if (!strcasecmp(optarg, "splay")) {
				dump_splay = 1;
			} else {
				fprintf(stderr, "Unknown -d parameter: %s\n", optarg);
				failed = 1;
			}
			break;
		case 'D':
			dbdump_at_exit = 1;
			break;
		case 'e':
			i = pick_loglevel(optarg, log_levelnames);
			if (i > -1)
				log_level = i;
			else {
				fprintf(stderr, "Log level unknown: \"%s\"\n", optarg);
				failed = 1;
			}
			break;
		case 'o':
			i = pick_loglevel(optarg, log_destnames);
			if (i > -1)
				log_dest = i;
			else {
				fprintf(stderr, "Log destination unknown: \"%s\"\n", optarg);
				failed = 1;
			}
			break;
		case '?':
		case 'h':
			fprintf(stderr, "%s\n", verstr);
			failed = 1;
	}
	}
	
	if ((log_dest == L_FILE) && (!log_dir)) {
		fprintf(stderr, "Log destination set to 'file' but no log directory specified!\n");
		failed = 1;
	}
	
	if (failed) {
		fputs(HELPS, stderr);
		exit(failed);
	}
}

/*
 *	signal handler
 */
 
int sighandler(int signum)
{
	switch (signum) {

	case SIGINT:
	case SIGTERM:
	case SIGQUIT:
		hlog(LOG_NOTICE, "Shutting down on signal %d", signum);
		shutting_down = 1;
		return 0;
		
	case SIGHUP:
		hlog(LOG_INFO, "SIGHUP received: reopening logs");
		reopen_logs = 1;
		break;
		
	case SIGUSR1:
		hlog(LOG_INFO, "SIGUSR1 received: reconfiguring");
		reconfiguring = 1;
		break;
		
	case SIGUSR2:
		hlog(LOG_INFO, "SIGUSR2 received: database dumping");
		want_dbdump = 1;
		break;
		
	default:
		hlog(LOG_WARNING, "* SIG %d ignored", signum);
		break;
	}
	
	signal(signum, (void *)sighandler);	/* restore handler */
	return 0;
		
}

/*
 *	A very Linux specific thing, as there the pthreads are a special variation
 *	of fork(), and per POSIX the profiling timers are not kept over fork()...
 */
void pthreads_profiling_reset(const char *name)
{
#ifdef __linux__ /* Very Linux-specific code.. */
	int tid;
	if (itv.it_interval.tv_usec || itv.it_interval.tv_sec) {
	  setitimer(ITIMER_PROF, &itv, NULL);
	}

	tid = syscall(SYS_gettid);
	hlog(LOG_DEBUG, "Thread %s: Linux ThreadId: %d", name, tid);
#endif
}

#define PATHLEN 500
static void dbdump_all(void)
{
	FILE *fp;
	char path[PATHLEN+1];

	/*
	 *    As a general rule, dumping of databases is not a Good Idea in
	 *    operational system.  Development time debugging on other hand..
	 */

	snprintf(path, PATHLEN, "%s/historydb.dump", rundir);
	fp = fopen(path,"w");
	if (fp) {
		historydb_dump(fp);
		fclose(fp);
	}
	snprintf(path, PATHLEN, "%s/filter.wx.dump", rundir);
	fp = fopen(path,"w");
	if (fp) {
		filter_wx_dump(fp);
		fclose(fp);
	}
	snprintf(path, PATHLEN, "%s/filter.entry.dump", rundir);
	fp = fopen(path,"w");
	if (fp) {
		filter_entrycall_dump(fp);
		fclose(fp);
	}
	snprintf(path, PATHLEN, "%s/pbuf.dump", rundir);
	fp = fopen(path,"w");
	if (fp) {
		pbuf_dump(fp);
		fclose(fp);
	}
	snprintf(path, PATHLEN, "%s/pbuf.dupe.dump", rundir);
	fp = fopen(path,"w");
	if (fp) {
		pbuf_dupe_dump(fp);
		fclose(fp);
	}
}

/*
 *	switch uid
 */
struct passwd pwbuf;
char *pw_buf_s;

static void find_uid(const char *uid_s)
{
	struct passwd *pwbufp;
	int buflen;
	
	buflen = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (buflen < 10)
		buflen = 1024;
	
	pw_buf_s = hmalloc(buflen);
	
#ifdef sun
	pwbufp = getpwnam_r(uid_s, &pwbuf, pw_buf_s, buflen);
#else
	int e = getpwnam_r(uid_s, &pwbuf, pw_buf_s, buflen, &pwbufp);
	
	if (e) {
		fprintf(stderr, "aprsc: getpwnam(%s) failed, can not set UID: %s\n", uid_s, strerror(e));
		exit(1);
	}
#endif
	
	if (pwbufp == NULL) {
		fprintf(stderr, "aprsc: getpwnam(%s) failed, can not set UID: user not found\n", uid_s);
		exit(1);
	}
}

static void set_uid(const char *uid_s)
{
	if (setgid(pwbuf.pw_gid)) {
		fprintf(stderr, "aprsc: Failed to set GID %d: %s\n", pwbuf.pw_gid, strerror(errno));
		exit(1);
	}
	
	if (setuid(pwbuf.pw_uid)) {
		fprintf(stderr, "aprsc: Failed to set UID %d: %s\n", pwbuf.pw_uid, strerror(errno));
		exit(1);
	}
}

/*
 *	Check that we're not running as root. Bail out if we are
 */

static void check_uid(void)
{
	if (getuid() == 0 || geteuid() == 0) {
		fprintf(stderr,
			"aprsc: Security incident about to happen: running as root.\n"
			"Use the -u <username> switch to run as unprivileged user 'aprsc', or start\n"
			"it up as such an user.\n");
		exit(1);
	}
}

/*
 *	Generate a pseudorandom instance ID by reading the pseudorandom
 *	source and converting the binary data to lower-case alphanumeric
 */

static void generate_instance_id(void)
{
	unsigned char s[INSTANCE_ID_LEN];
	int fd, l;
	char c;
	unsigned seed;
	struct timeval tv;
	
	if ((fd = open("/dev/urandom", O_RDONLY)) == -1) {
		hlog(LOG_ERR, "open(/dev/urandom) failed: %s", strerror(errno));
		fd = -1;
	}
	
	/* seed the prng */
	if (fd >= 0) {
		l = read(fd, &seed, sizeof(seed));
		if (l != sizeof(seed)) {
			hlog(LOG_ERR, "read(/dev/urandom, %d) for seed failed: %s", sizeof(seed), strerror(errno));
			close(fd);
			fd = -1;
		}
	}
	
	if (fd < 0) {
		/* not very strong, but we're not doing cryptography here */
		gettimeofday(&tv, NULL);
		seed = tv.tv_sec + tv.tv_usec + getpid();
	}
	
	srand(seed);
	
	if (fd >= 0) {
		/* generate instance id */
		l = read(fd, s, INSTANCE_ID_LEN);
		if (l != INSTANCE_ID_LEN) {
			hlog(LOG_ERR, "read(/dev/urandom, %d) failed: %s", INSTANCE_ID_LEN, strerror(errno));
			close(fd);
			fd = -1;
		}
	}
	
	if (fd < 0) {
		/* urandom failed for us, use something inferior */
		for (l = 0; l < INSTANCE_ID_LEN; l++)
			s[l] = rand() % 256;
	}
	
	for (l = 0; l < INSTANCE_ID_LEN; l++) {
		/* 256 is not divisible by 36, the distribution is slightly skewed,
		 * but that's not serious.
		 */
		c = s[l] % (26 + 10); /* letters and numbers */
		if (c < 10)
			c += 48; /* number */
		else
			c = c - 10 + 97; /* letter */
		instance_id[l] = c;
	}
	instance_id[INSTANCE_ID_LEN] = 0;
	
	if (fd >= 0)
		close(fd);
}

/*
 *	Preload resolver libraries before performing a chroot,
 *	so that we won't need copies of the .so files within the
 *	chroot.
 *
 *	This is done by doing a DNS lookup which "accidentally"
 *	phones home with the version information.
 *
 *	Tin foil hats off. Version information and instance statistics
 *	make developers happy.
 */

void version_report(const char *state)
{
	struct addrinfo hints;
	struct addrinfo *ai;
	char v[80];
	char n[32];
	char s[300];
	int i, l;
	
	/* normalize version string to fit in a DNS label */
	strncpy(v, version_build, sizeof(v));
	l = strlen(v);
	for (i = 0; i < sizeof(v) && i < l; i++)
		if (!isalnum(v[i]))
			v[i] = '-';
	
	if (serverid) {
		/* we're configured, include serverid and normalize it */
		strncpy(n, serverid, sizeof(n));
		l = strlen(n);
		for (i = 0; i < sizeof(n) && i < l; i++)
			if (!isalnum(n[i]))
				n[i] = '-';
		snprintf(s, 300, "%s.%s.%s.%s.aprsc.he.fi", n, v, instance_id, state);
	} else {
		snprintf(s, 300, "%s.%s.%s.aprsc.he.fi", v, instance_id, state);
	}
	
	/* reduce the amount of lookups in case of a failure, don't do
	 * separate AAAA + A lookups
	 */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	
	ai = NULL;
	getaddrinfo(s, NULL, &hints, &ai);
	if (ai)
		freeaddrinfo(ai);
}

/*
 *	Time-keeping thread
 */

static double timeval_diff(struct timeval start, struct timeval end)
{
	double diff;
	
	diff = (end.tv_sec - start.tv_sec)
		+ ((end.tv_usec - start.tv_usec) / 1000000.0);
	
	return diff;
}

void time_thread(void *asdf)
{
	sigset_t sigs_to_block;
	time_t previous_tick;
	struct timespec sleep_req;
	struct timeval sleep_start, sleep_end;

	pthreads_profiling_reset("time");
	
	sigemptyset(&sigs_to_block);
	sigaddset(&sigs_to_block, SIGALRM);
	sigaddset(&sigs_to_block, SIGINT);
	sigaddset(&sigs_to_block, SIGTERM);
	sigaddset(&sigs_to_block, SIGQUIT);
	sigaddset(&sigs_to_block, SIGHUP);
	sigaddset(&sigs_to_block, SIGURG);
	sigaddset(&sigs_to_block, SIGPIPE);
	sigaddset(&sigs_to_block, SIGUSR1);
	sigaddset(&sigs_to_block, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &sigs_to_block, NULL);
	
	time(&previous_tick);
	
	sleep_req.tv_sec = 0;
	sleep_req.tv_nsec = 210 * 1000 * 1000; /* 210 ms */
	
	while (!accept_shutting_down) {
		gettimeofday(&sleep_start, NULL);
		nanosleep(&sleep_req, NULL);
		gettimeofday(&sleep_end, NULL);
		time(&tick);
		now = tick;
		
		double slept = timeval_diff(sleep_start, sleep_end);
		if (slept > 0.90)
			hlog(LOG_WARNING, "time keeping: sleep of %d ms took %.6f s!", sleep_req.tv_nsec / 1000 / 1000, slept);
		
		/* catch some oddities with time keeping */
		if (tick != previous_tick) {
			if (previous_tick > tick) {
				hlog(LOG_WARNING, "time keeping: Time jumped backward by %d seconds!", previous_tick - tick);
			} else if (previous_tick < tick-1) {
				hlog(LOG_WARNING, "time keeping: Time jumped forward by %d seconds!", tick - previous_tick);
			}
			
			previous_tick = tick;
		}
		
	}
	
}	

/*
 *	Main
 */

int main(int argc, char **argv)
{
	pthread_t accept_th;
	pthread_t http_th;
	pthread_t time_th;
	int e;
	struct rlimit rlim;
	time_t cleanup_tick;
	time_t stats_tick;
	time_t version_tick;
	
	time(&tick);
	srand(tick);
	now = tick;
	cleanup_tick = tick;
	stats_tick = tick;
	version_tick = tick + rand() % 60; /* some load distribution */
	startup_tick = tick;
	setlinebuf(stderr);
	
	/* set locale to C so that isalnum(), isupper() etc don't do anything
	 * unexpected if the environment is unexpected.
	 */
	if (!setlocale(LC_COLLATE, "C")) {
		hlog(LOG_CRIT, "Failed to set locale C for LC_COLLATE.");
		exit(1);
	}
	if (!setlocale(LC_CTYPE, "C")) {
		hlog(LOG_CRIT, "Failed to set locale C for LC_CTYPE.");
		exit(1);
	}
	if (!setlocale(LC_MESSAGES, "C")) {
		hlog(LOG_CRIT, "Failed to set locale C for LC_MESSAGES.");
		exit(1);
	}
	if (!setlocale(LC_NUMERIC, "C")) {
		hlog(LOG_CRIT, "Failed to set locale C for LC_NUMERIC.");
		exit(1);
	}

	/* Adjust process global fileno limit */
	e = getrlimit(RLIMIT_NOFILE, &rlim);
	rlim.rlim_cur = rlim.rlim_max;
	e = setrlimit(RLIMIT_NOFILE, &rlim);
	e = getrlimit(RLIMIT_NOFILE, &rlim);
	fileno_limit = rlim.rlim_cur;

	getitimer(ITIMER_PROF, &itv);
	
	/* command line */
	parse_cmdline(argc, argv);
	
	/* if setuid is needed, find the user UID */
	if (setuid_s)
		find_uid(setuid_s);
	
	/* fork a daemon */
	if (fork_a_daemon) {
		hlog(LOG_INFO, "Forking...");
		int i = fork();
		if (i < 0) {
			hlog(LOG_CRIT, "Fork to background failed: %s", strerror(errno));
			fprintf(stderr, "Fork to background failed: %s\n", strerror(errno));
			exit(1);
		} else if (i == 0) {
			/* child */
			hlog(LOG_INFO, "Child started...");
			if (setsid() == -1) {
				fprintf(stderr, "setsid() failed: %s\n", strerror(errno));
				//exit(1);
			}
		} else {
			/* parent, quitting */
			hlog(LOG_DEBUG, "Forked daemon process %d, parent quitting", i);
			exit(0);
		}
	}
	
	/* close stdin and stdout, and any other FDs that might be left open */
	/* closing stderr masks error logs between this point and opening the logs... suboptimal. */
	{
		int i;
		close(0);
		close(1);
		if (log_dest != L_STDERR)
			close(2);
		for (i = 3; i < 100; i++)
			close(i);
	}
	
	/* all of this fails in chroot, so do it before */
	if (open("/dev/null", O_RDWR) == -1) /* stdin */
		hlog(LOG_ERR, "open(/dev/null) failed for stdin: %s", strerror(errno));
	if (dup(0) == -1) /* stdout */
		hlog(LOG_ERR, "dup(0) failed for stdout: %s", strerror(errno));
	if (log_dest != L_STDERR) {
		if (dup(0) == -1) /* stderr */
			hlog(LOG_ERR, "dup(0) failed for stderr: %s", strerror(errno));
	}
	
	/* prepare for a possible chroot, force loading of
	 * resolver libraries at this point, so that we don't
	 * need a copy of the shared libs within the chroot dir
	 */
	generate_instance_id();
	version_report("startup");
	
	/* do a chroot if required */
	if (chrootdir) {
		if ((!setuid_s) || pwbuf.pw_uid == 0) {
			fprintf(stderr, "aprsc: chroot requested but no setuid to non-root user - insecure\n"
				"configuration detected. Using -u <username> required.\n");
			exit(1);
		}
		if (chdir(chrootdir) != 0) {
			fprintf(stderr, "aprsc: chdir(%s) failed before chroot: %s\n", chrootdir, strerror(errno));
			exit(1);
		}
		if (chroot(chrootdir)) {
			fprintf(stderr, "aprsc: chroot(%s) failed: %s\n", chrootdir, strerror(errno));
			exit(1);
		}
		if (chdir("/") != 0) { /* maybe redundant after the first chdir() */
			fprintf(stderr, "aprsc: chdir(/) failed after chroot: %s\n", strerror(errno));
			exit(1);
		}
	}
	
	/* if setuid is needed, do so */
	if (setuid_s)
		set_uid(setuid_s);
	
	/* check that we're not root - it would be insecure and really not required */
	check_uid();
	
	/* open syslog, write an initial log message and read configuration */
	open_log(logname, 0);
	hlog(LOG_NOTICE, "Starting up, instance id %s ...", instance_id);
	if (read_config()) {
		hlog(LOG_CRIT, "Initial configuration failed.");
		exit(1);
	}
	
	/* write pid file, now that we have our final pid... might fail, which is critical */
	if (!writepid(pidfile))
		exit(1);
	
	/* catch signals */
	signal(SIGINT, (void *)sighandler);
	signal(SIGTERM, (void *)sighandler);
	signal(SIGQUIT, (void *)sighandler);
	signal(SIGHUP, (void *)sighandler);
	/* ignore HUP for now, it's handling is buggy. */
	//signal(SIGHUP, SIG_IGN);
	signal(SIGUSR1, (void *)sighandler);
	signal(SIGUSR2, (void *)sighandler);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGURG, SIG_IGN);
	
	/* Early inits in single-thread mode */
	keyhash_init();
	filter_init();
	pbuf_init();
	dupecheck_init();
	historydb_init();
	client_heard_init();
	client_init();
	xpoll_init();
	status_init();

	time(&cleanup_tick);

	pthread_attr_init(&pthr_attrs);
	/* 128 kB stack is enough for each thread,
	   default of 10 MB is way too much...*/
	pthread_attr_setstacksize(&pthr_attrs, 128*1024);

	/* start the time thread, which will update the current time */
	if (pthread_create(&time_th, &pthr_attrs, (void *)time_thread, NULL))
		perror("pthread_create failed for time_thread");

	/* start the accept thread, which will start server threads */
	if (pthread_create(&accept_th, &pthr_attrs, (void *)accept_thread, NULL))
		perror("pthread_create failed for accept_thread");

	/* start the HTTP thread, which runs libevent's HTTP server */
	if (pthread_create(&http_th, &pthr_attrs, (void *)http_thread, NULL))
		perror("pthread_create failed for http_thread");

	/* act as statistics and housekeeping thread from now on */
	while (!shutting_down) {
		poll(NULL, 0, 300); // 0.300 sec -- or there abouts..
		
		if (want_dbdump) {
			dbdump_all();
			want_dbdump = 0;
		}
		
		if (reopen_logs) {
			reopen_logs = 0;
			close_log(1);
		}
		
		if (reconfiguring) {
			reconfiguring = 0;
			if (read_config()) {
				hlog(LOG_ERR, "New configuration was not successfully read.");
			} else {
				hlog(LOG_INFO, "New configuration read successfully. Applying....");
				accept_reconfiguring = 1;
			}
		}
		
		if (cleanup_tick < tick || cleanup_tick > tick + 80) {
			cleanup_tick = tick + 60;
			
			status_dump_file();
			historydb_cleanup();
			filter_wx_cleanup();
			filter_entrycall_cleanup();
		}
		
		if (version_tick < tick || version_tick > tick + 86500) {
			version_tick = tick + 86400;
			version_report("run");
		}
	}
	
	hlog(LOG_DEBUG, "Dumping status to file");
	status_dump_file();
	
	hlog(LOG_INFO, "Signalling threads to shut down...");
	accept_shutting_down = 1;
	
	if ((e = pthread_join(http_th, NULL)))
		hlog(LOG_ERR, "Could not pthread_join http_th: %s", strerror(e));
	else
		hlog(LOG_INFO, "HTTP thread has terminated.");
	
	if ((e = pthread_join(accept_th, NULL)))
		hlog(LOG_ERR, "Could not pthread_join accept_th: %s", strerror(e));
	else
		hlog(LOG_INFO, "Accept thread has terminated.");
		
	if ((e = pthread_join(time_th, NULL)))
		hlog(LOG_ERR, "Could not pthread_join time_th: %s", strerror(e));
	
	if (dbdump_at_exit) {
		dbdump_all();
	}

	free_config();
	dupecheck_atend();
	historydb_atend();
	filter_wx_atend();
	filter_entrycall_atend();
	status_atend();
	
	hlog(LOG_NOTICE, "Shut down.");
	close_log(0);
	
	return 0;
}

