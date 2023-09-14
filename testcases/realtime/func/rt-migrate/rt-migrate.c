// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2007-2009 Steven Rostedt <srostedt@redhat.com>
 * Copyright (c) 2023 Marius Kittler <mkittler@suse.de>
 */

/*\
 * [Description]
 *
 * This test makes sure that all the high prio tasks that are in the
 * running state are actually running on a CPU if it can.
 * Steps:
 * - Creates N+1 threads with lower real time priorities.
 *   Where N is the number of CPUs in the system.
 * - If the thread is high priority, and if a CPU is available, the
 *   thread runs on that CPU.
 * - The thread records the start time and the number of ticks in the run
 *   interval.
 * - The output indicates if lower prio task is quicker than higher
 *   priority task.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdarg.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <linux/unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>

#define TST_NO_DEFAULT_MAIN /* parse options via librttest */
#include "tst_test.h"
#include "tst_safe_pthread.h"

/* prevent definitions from config headers of test library and
 * realtime library from clashing */
#undef PACKAGE
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_URL
#undef PACKAGE_VERSION
#undef VERSION

#include <librttest.h>
#include <libstats.h>

#define gettid() syscall(__NR_gettid)

#define CLAMP(x, lower, upper) (MIN(upper, MAX(x, lower)))
#define CLAMP_PRIO(prio) CLAMP(prio, prio_min, prio_max)

int nr_tasks;
int lfd;

static int mark_fd = -1;
static __thread char buff[BUFSIZ + 1];

static void setup_ftrace_marker(void)
{
	struct stat st;
	char *files[] = {
		"/sys/kernel/debug/tracing/trace_marker",
		"/debug/tracing/trace_marker",
		"/debugfs/tracing/trace_marker",
	};
	int ret;
	unsigned int i;

	for (i = 0; i < (sizeof(files) / sizeof(char *)); i++) {
		ret = stat(files[i], &st);
		if (ret >= 0)
			goto found;
	}
	/* todo, check mounts system */
	return;
found:
	mark_fd = open(files[i], O_WRONLY);
}

static void ftrace_write(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (mark_fd < 0)
		return;

	va_start(ap, fmt);
	n = vsnprintf(buff, BUFSIZ, fmt, ap);
	va_end(ap);

	/*
	 * This doesn't return any valid vs invalid exit codes, so printing out
	 * a perror to warn the end-user of an issue is sufficient.
	 */
	if (write(mark_fd, buff, n) < 0) {
		perror("write");
	}
}

#define INTERVAL 100ULL * NS_PER_MS
#define RUN_INTERVAL 20ULL * NS_PER_MS
#define NR_RUNS 50
#define PRIO_START 2
/* 1 millisec off */
#define MAX_ERR  1000 * NS_PER_US

#define PROGRESS_CHARS 70

static unsigned long long interval = INTERVAL;
static unsigned long long run_interval = RUN_INTERVAL;
static long max_err = MAX_ERR;
static int nr_runs = NR_RUNS;
static int prio_start = PRIO_START, prio_min, prio_max;
static int check = 1;
static int stop;

static unsigned long long now;

static int done;
static int loop;

static pthread_barrier_t start_barrier;
static pthread_barrier_t end_barrier;
stats_container_t *intervals;
stats_container_t *intervals_length;
stats_container_t *intervals_loops;
static long *thread_pids;

static void print_progress_bar(int percent)
{
	int i;
	int p;

	if (percent > 100)
		percent = 100;

	/* Use stderr, so we don't capture it */
	putc('\r', stderr);
	putc('|', stderr);
	for (i = 0; i < PROGRESS_CHARS; i++)
		putc(' ', stderr);
	putc('|', stderr);
	putc('\r', stderr);
	putc('|', stderr);

	p = PROGRESS_CHARS * percent / 100;

	for (i = 0; i < p; i++)
		putc('-', stderr);

	fflush(stderr);
}

static void usage(void)
{
	rt_help();
	printf("Usage:\n"
	       "-a priority Priority of the threads"
	       "-r time     Run time (ms) to busy loop the threads (20)\n"
	       "-t time     Sleep time (ms) between intervals (100)\n"
	       "-e time     Max allowed error (microsecs)\n"
	       "-l loops    Number of iterations to run (50)\n");
}

/*
int rt_init(const char *options, int (*parse_arg)(int option, char *value),
	    int argc, char *argv[]);
 */
static int parse_args(int c, char *v)
{
	int handled = 1;
	switch (c) {
	case 'a':
		prio_start = atoi(v);
		break;
	case 'r':
		run_interval = atoi(v);
		break;
	case 't':
		interval = atoi(v);
		break;
	case 'l':
		nr_runs = atoi(v);
		break;
	case 'e':
		max_err = atoi(v) * NS_PER_US;
		break;
	case '?':
	case 'h':
		usage();
		handled = 0;
	}
	return handled;
}

static void record_time(int id, unsigned long long time, unsigned long l)
{
	unsigned long long ltime;
	stats_record_t rec;
	if (loop >= nr_runs)
		return;
	time -= now;
	ltime = rt_gettime() / NS_PER_US;
	ltime -= now;
	rec.x = loop;
	rec.y = time;
	stats_container_append(&intervals[id], rec);
	rec.x = loop;
	rec.y = ltime;
	stats_container_append(&intervals_length[id], rec);
	rec.x = loop;
	rec.y = l;
	stats_container_append(&intervals_loops[id], rec);
}

static void print_details(void)
{
	int i;
	int t;
	unsigned long long tasks_max[nr_tasks];
	unsigned long long tasks_min[nr_tasks];
	unsigned long long tasks_avg[nr_tasks];

	memset(tasks_max, 0, sizeof(tasks_max[0]) * nr_tasks);
	memset(tasks_min, 0xff, sizeof(tasks_min[0]) * nr_tasks);
	memset(tasks_avg, 0, sizeof(tasks_avg[0]) * nr_tasks);

	printf("Iter: ");
	for (t = 0; t < nr_tasks; t++)
		printf("%6d  ", t);
	printf("\n");

	for (t = 0; t < nr_tasks; t++) {
		tasks_max[t] = stats_max(&intervals[t]);
		tasks_min[t] = stats_min(&intervals[t]);
		tasks_avg[t] = stats_avg(&intervals[t]);
	}
	for (i = 0; i < nr_runs; i++) {
		printf("%4d:   ", i);
		for (t = 0; t < nr_tasks; t++)
			printf("%6ld  ", intervals[t].records[i].y);

		printf("\n");
		printf(" len:   ");
		for (t = 0; t < nr_tasks; t++)
			printf("%6ld  ", intervals_length[t].records[i].y);

		printf("\n");
		printf(" loops: ");
		for (t = 0; t < nr_tasks; t++)
			printf("%6ld  ", intervals_loops[t].records[i].y);

		printf("\n");
		printf("\n");
	}

	printf("Parent pid: %d\n", getpid());

	for (t = 0; t < nr_tasks; t++) {
		printf(" Task %d (prio %d) (pid %ld):\n", t,
			   CLAMP_PRIO(t + prio_start), thread_pids[t]);
		printf("   Max: %lld us\n", tasks_max[t]);
		printf("   Min: %lld us\n", tasks_min[t]);
		printf("   Tot: %lld us\n", tasks_avg[t] * nr_runs);
		printf("   Avg: %lld us\n", tasks_avg[t]);
		printf("\n");
	}
}

static unsigned long busy_loop(unsigned long long start_time)
{
	unsigned long long time;
	unsigned long l = 0;

	do {
		l++;
		time = rt_gettime();
	} while ((time - start_time) < RUN_INTERVAL);

	return l;
}

void *start_task(void *data)
{
	struct thread *thr = (struct thread *)data;
	long id = (long)thr->arg;
	thread_pids[id] = gettid();
	unsigned long long start_time;
	int ret;
	int high = 0;
	cpu_set_t cpumask;
	cpu_set_t save_cpumask;
	int cpu = 0;
	unsigned long l;
	long pid;

	ret = sched_getaffinity(0, sizeof(save_cpumask), &save_cpumask);
	if (ret < 0)
		debug(DBG_ERR, "sched_getaffinity failed: %s\n", strerror(ret));

	pid = gettid();

	/* Check if we are the highest prio task */
	if (id == nr_tasks - 1)
		high = 1;

	while (!done) {
		if (high) {
			/* rotate around the CPUS */
			if (!CPU_ISSET(cpu, &save_cpumask))
				cpu = 0;
			CPU_ZERO(&cpumask);
			CPU_SET(cpu, &cpumask);
			cpu++;
			sched_setaffinity(0, sizeof(cpumask), &cpumask);
		}
		SAFE_PTHREAD_BARRIER_WAIT(&start_barrier);
		start_time = rt_gettime();
		ftrace_write("Thread %d: started %lld diff %lld\n",
			     pid, start_time, start_time - now);
		l = busy_loop(start_time);
		record_time(id, start_time / NS_PER_US, l);
		SAFE_PTHREAD_BARRIER_WAIT(&end_barrier);
	}

	return (void *)pid;
}

static int check_times(int l)
{
	int i;
	long last;
	long last_loops;
	long last_length;

	for (i = 0; i < nr_tasks; i++) {
		if (i && last < intervals[i].records[l].y &&
		    ((intervals[i].records[l].y - last) > max_err)) {
			/*
			 * May be a false positive.
			 * Make sure that we did more loops
			 * our start is before the end
			 * and the end should be tested.
			 */
			if (intervals_loops[i].records[l].y < last_loops ||
			    intervals[i].records[l].y > last_length ||
			    (intervals_length[i].records[l].y > last_length &&
			     intervals_length[i].records[l].y - last_length >
			     max_err)) {
				check = -1;
				return 1;
			}
		}
		last = intervals[i].records[l].y;
		last_loops = intervals_loops[i].records[l].y;
		last_length = intervals_length[i].records[l].y;
	}
	return 0;
}

static void stop_log(LTP_ATTRIBUTE_UNUSED int sig)
{
	stop = 0;
}

static void run_test(void)
{
	/*
	 * Determine the valid priority range; subtracting one from the
	 * maximum to reserve the highest prio for main thread.
	 */
	prio_min = sched_get_priority_min(SCHED_FIFO);
	prio_max = sched_get_priority_max(SCHED_FIFO) - 1;

	int *threads;
	long i;
	struct timespec intv;
	struct sched_param param;

	signal(SIGINT, stop_log);

	intervals = SAFE_MALLOC(sizeof(stats_container_t) * nr_tasks);
	memset(intervals, 0, sizeof(stats_container_t) * nr_tasks);
	intervals_length = SAFE_MALLOC(sizeof(stats_container_t) * nr_tasks);
	memset(intervals_length, 0, sizeof(stats_container_t) * nr_tasks);
	intervals_loops = SAFE_MALLOC(sizeof(stats_container_t) * nr_tasks);
	memset(intervals_loops, 0, sizeof(stats_container_t) * nr_tasks);
	threads = SAFE_MALLOC(sizeof(*threads) * nr_tasks);
	memset(threads, 0, sizeof(*threads) * nr_tasks);

	SAFE_PTHREAD_BARRIER_INIT(&start_barrier, NULL, nr_tasks + 1);
	SAFE_PTHREAD_BARRIER_INIT(&end_barrier, NULL, nr_tasks + 1);
	for (i = 0; i < nr_tasks; i++) {
		stats_container_init(&intervals[i], nr_runs);
		stats_container_init(&intervals_length[i], nr_runs);
		stats_container_init(&intervals_loops[i], nr_runs);
	}

	thread_pids = malloc(sizeof(long) * nr_tasks);
	if (!thread_pids)
		debug(DBG_ERR, "malloc thread_pids failed\n");

	for (i = 0; i < nr_tasks; i++) {
		threads[i] = create_fifo_thread(start_task, (void *)i,
						CLAMP_PRIO(prio_start + i));
	}

	/*
	 * Progress bar uses stderr to let users see it when
	 * redirecting output. So we convert stderr to use line
	 * buffering so the progress bar doesn't flicker.
	 */
	setlinebuf(stderr);

	/* up our prio above all tasks */
	memset(&param, 0, sizeof(param));
	param.sched_priority = CLAMP(nr_tasks + prio_start, prio_min,
								 prio_max + 1);
	if (sched_setscheduler(0, SCHED_FIFO, &param))
		debug(DBG_WARN, "Warning, can't set priority of main thread!\n");
	intv.tv_sec = INTERVAL / NS_PER_SEC;
	intv.tv_nsec = INTERVAL % (1 * NS_PER_SEC);

	print_progress_bar(0);

	setup_ftrace_marker();

	for (loop = 0; loop < nr_runs; loop++) {
		unsigned long long end;

		now = rt_gettime() / NS_PER_US;

		ftrace_write("Loop %d now=%lld\n", loop, now);

		SAFE_PTHREAD_BARRIER_WAIT(&start_barrier);

		ftrace_write("All running!!!\n");

		rt_nanosleep(intv.tv_nsec);
		print_progress_bar((loop * 100) / nr_runs);

		end = rt_gettime() / NS_PER_US;
		ftrace_write("Loop %d end now=%lld diff=%lld\n",
			     loop, end, end - now);
		SAFE_PTHREAD_BARRIER_WAIT(&end_barrier);

		if (stop || (check && check_times(loop))) {
			loop++;
			nr_runs = loop;
			break;
		}
	}
	putc('\n', stderr);

	SAFE_PTHREAD_BARRIER_WAIT(&start_barrier);
	done = 1;
	SAFE_PTHREAD_BARRIER_WAIT(&end_barrier);

	join_threads();
	print_details();
	tst_res(check < 0 ? TFAIL : TPASS,
			"high prio tasks get more cpu time than low prio tasks");
}

static struct tst_test test = {
	.setup = setup,
	.test_all = run_test
};

int main(int argc, char *argv[])
{
	rt_init("a:r:t:e:l:h:", parse_args, argc, argv);

	if (argc >= (optind + 1)) {
		if (tst_parse_int(argv[optind], &nr_tasks, 0, INT_MAX))
			tst_brk(TBROK, "Invalid number of tasks '%s'", argv[optind]);
	} else {
		nr_tasks = tst_ncpus();
	}

	tst_run_tcases(0, argv, &test);
}
