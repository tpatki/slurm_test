/*****************************************************************************\
 *  srun.c - user interface to allocate resources, submit jobs, and execute
 *	parallel jobs.
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <grondona@llnl.gov>, et. al.
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.schedmd.com/slurmdocs/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifdef WITH_PTHREADS
#  include <pthread.h>
#endif

#ifdef HAVE_AIX
#  undef HAVE_UNSETENV
#  include <sys/checkpnt.h>
#endif
#ifndef HAVE_UNSETENV
#  include "src/common/unsetenv.h"
#endif

#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <grp.h>


#include "src/common/fd.h"
#include "src/common/hostlist.h"
#include "src/common/log.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/switch.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"
#include "src/common/xstring.h"
#include "src/common/net.h"
#include "src/common/mpi.h"
#include "src/common/slurm_rlimits_info.h"
#include "src/common/plugstack.h"
#include "src/common/read_config.h"
#include "src/common/uid.h"

#include "src/srun/allocate.h"
#include "src/srun/srun_job.h"
#include "src/srun/opt.h"
#include "src/srun/debugger.h"
#include "src/srun/srun.h"
#include "src/srun/srun_pty.h"
#include "src/srun/multi_prog.h"
#include "src/srun/task_state.h"
#include "src/api/pmi_server.h"
#include "src/api/step_launch.h"

#if defined (HAVE_DECL_STRSIGNAL) && !HAVE_DECL_STRSIGNAL
#  ifndef strsignal
 extern char *strsignal(int);
#  endif
#endif /* defined HAVE_DECL_STRSIGNAL && !HAVE_DECL_STRSIGNAL */

#ifndef OPEN_MPI_PORT_ERROR
/* This exit code indicates the launched Open MPI tasks could
 *	not open the reserved port. It was already open by some
 *	other process. */
#define OPEN_MPI_PORT_ERROR 108
#endif

#define MAX_RETRIES 20
#define MAX_ENTRIES 50

#define	TYPE_NOT_TEXT	0
#define	TYPE_TEXT	1
#define	TYPE_SCRIPT	2

mpi_plugin_client_info_t mpi_job_info[1];
static struct termios termdefaults;
uint32_t global_rc = 0;
srun_job_t *job = NULL;
task_state_t task_state;

#define MAX_STEP_RETRIES 4
time_t launch_start_time;
bool   retry_step_begin = false;
int    retry_step_cnt = 0;

bool srun_max_timer = false;
bool srun_shutdown  = false;
static int sig_array[] = {
	SIGINT,  SIGQUIT, SIGCONT, SIGTERM, SIGHUP,
	SIGALRM, SIGUSR1, SIGUSR2, SIGPIPE, 0 };

/*
 * forward declaration of static funcs
 */
static int   _become_user (void);
static int   _call_spank_local_user (srun_job_t *job);
static void  _default_sigaction(int sig);
static void  _define_symbols(void);
static void  _handle_intr(void);
static void  _handle_pipe(void);
static void  _print_job_information(resource_allocation_response_msg_t *resp);
static void  _pty_restore(void);
static void  _run_srun_prolog (srun_job_t *job);
static void  _run_srun_epilog (srun_job_t *job);
static int   _run_srun_script (srun_job_t *job, char *script);
static void  _set_cpu_env_var(resource_allocation_response_msg_t *resp);
static void  _set_exit_code(void);
static void  _step_opt_exclusive(void);
static void  _set_stdio_fds(srun_job_t *job, slurm_step_io_fds_t *cio_fds);
static void  _set_submit_dir_env(void);
static void  _set_prio_process_env(void);
static int   _set_rlimit_env(void);
static int   _set_umask_env(void);
static int   _slurm_debug_env_val (void);
static void *_srun_signal_mgr(void *no_data);
static void  _task_start(launch_tasks_response_msg_t *msg);
static void  _task_finish(task_exit_msg_t *msg);
static char *_uint16_array_to_str(int count, const uint16_t *array);
static int   _validate_relative(resource_allocation_response_msg_t *resp);

/*
 * from libvirt-0.6.2 GPL2
 *
 * console.c: A dumb serial console client
 *
 * Copyright (C) 2007, 2008 Red Hat, Inc.
 *
 */
#ifndef HAVE_CFMAKERAW
void cfmakeraw(struct termios *attr)
{
	attr->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
				| INLCR | IGNCR | ICRNL | IXON);
	attr->c_oflag &= ~OPOST;
	attr->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	attr->c_cflag &= ~(CSIZE | PARENB);
	attr->c_cflag |= CS8;
}
#endif

int srun(int ac, char **av)
{
	resource_allocation_response_msg_t *resp;
	int debug_level;
	env_t *env = xmalloc(sizeof(env_t));
	log_options_t logopt = LOG_OPTS_STDERR_ONLY;
	slurm_step_launch_params_t launch_params;
	slurm_step_launch_callbacks_t callbacks;
	pthread_attr_t thread_attr;
	pthread_t signal_thread = (pthread_t) 0;
	int got_alloc = 0;

	env->stepid = -1;
	env->procid = -1;
	env->localid = -1;
	env->nodeid = -1;
	env->cli = NULL;
	env->env = NULL;
	env->ckpt_dir = NULL;

	debug_level = _slurm_debug_env_val();
	logopt.stderr_level += debug_level;
	log_init(xbasename(av[0]), logopt, 0, NULL);
	_set_exit_code();

	/* This must happen before we spawn any threads
	 * which are not designed to handle them */
	if (xsignal_block(sig_array) < 0)
		error("Unable to block signals");
#ifndef HAVE_CRAY_EMULATION
	if (is_cray_system() || is_cray_select_type()) {
		error("operation not supported on Cray systems - use aprun(1)");
		exit(error_exit);
	}
#endif
	/* Initialize plugin stack, read options from plugins, etc.
	 */
	init_spank_env();
	if (spank_init(NULL) < 0) {
		error("Plug-in initialization failed");
		exit(error_exit);
		_define_symbols();
	}

	/* Be sure to call spank_fini when srun exits.
	 */
	if (atexit((void (*) (void)) spank_fini) < 0)
		error("Failed to register atexit handler for plugins: %m");

	/* set default options, process commandline arguments, and
	 * verify some basic values
	 */
	if (initialize_and_process_args(ac, av) < 0) {
		error ("srun initialization failed");
		exit (1);
	}
	record_ppid();

	if (spank_init_post_opt() < 0) {
		error("Plugin stack post-option processing failed.");
		exit(error_exit);
	}

	/* reinit log with new verbosity (if changed by command line)
	 */
	if (_verbose || opt.quiet) {
		/* If log level is already increased, only increment the
		 *   level to the difference of _verbose an LOG_LEVEL_INFO
		 */
		if ((_verbose -= (logopt.stderr_level - LOG_LEVEL_INFO)) > 0)
			logopt.stderr_level += _verbose;
		logopt.stderr_level -= opt.quiet;
		logopt.prefix_level = 1;
		log_alter(logopt, 0, NULL);
	} else
		_verbose = debug_level;

	(void) _set_rlimit_env();
	_set_prio_process_env();
	(void) _set_umask_env();
	_set_submit_dir_env();

	/* Set up slurmctld message handler */
	slurmctld_msg_init();

	/* now global "opt" should be filled in and available,
	 * create a job from opt
	 */
	if (opt.test_only) {
		int rc = allocate_test();
		if (rc) {
			slurm_perror("allocation failure");
			exit (1);
		}
		exit (0);

	} else if (opt.no_alloc) {
		info("do not allocate resources");
		job = job_create_noalloc();
		if (create_job_step(job, false) < 0) {
			exit(error_exit);
		}
	} else if ((resp = existing_allocation())) {
		select_g_alter_node_cnt(SELECT_APPLY_NODE_MAX_OFFSET,
					&resp->node_cnt);
		if (opt.nodes_set_env && !opt.nodes_set_opt &&
		    (opt.min_nodes > resp->node_cnt)) {
			/* This signifies the job used the --no-kill option
			 * and a node went DOWN or it used a node count range
			 * specification, was checkpointed from one size and
			 * restarted at a different size */
			error("SLURM_NNODES environment varariable "
			      "conflicts with allocated node count (%u!=%u).",
			      opt.min_nodes, resp->node_cnt);
			/* Modify options to match resource allocation.
			 * NOTE: Some options are not supported */
			opt.min_nodes = resp->node_cnt;
			xfree(opt.alloc_nodelist);
			if (!opt.ntasks_set)
				opt.ntasks = opt.min_nodes;
		}
		if (opt.alloc_nodelist == NULL)
			opt.alloc_nodelist = xstrdup(resp->node_list);
		if (opt.exclusive)
			_step_opt_exclusive();
		_set_cpu_env_var(resp);
		if (_validate_relative(resp))
			exit(error_exit);
		job = job_step_create_allocation(resp);
		slurm_free_resource_allocation_response_msg(resp);

		if (opt.begin != 0) {
			error("--begin is ignored because nodes"
			      " are already allocated.");
		}
		if (!job || create_job_step(job, false) < 0)
			exit(error_exit);
	} else {
		/* Combined job allocation and job step launch */
#if defined HAVE_FRONT_END && (!defined HAVE_BGQ || !defined HAVE_BG_FILES)
		uid_t my_uid = getuid();
		if ((my_uid != 0) &&
		    (my_uid != slurm_get_slurm_user_id())) {
			error("srun task launch not supported on this system");
			exit(error_exit);
		}
#endif
		if (opt.relative_set && opt.relative) {
			fatal("--relative option invalid for job allocation "
			      "request");
		}

		if (!opt.job_name_set_env && opt.job_name_set_cmd)
			setenvfs("SLURM_JOB_NAME=%s", opt.job_name);
		else if (!opt.job_name_set_env && opt.argc)
			setenvfs("SLURM_JOB_NAME=%s", opt.argv[0]);

		if ( !(resp = allocate_nodes()) )
			exit(error_exit);
		got_alloc = 1;
		_print_job_information(resp);
		_set_cpu_env_var(resp);
		if (_validate_relative(resp)) {
			slurm_complete_job(resp->job_id, 1);
			exit(error_exit);
		}
		job = job_create_allocation(resp);

		opt.exclusive = false;	/* not applicable for this step */
		opt.time_limit = NO_VAL;/* not applicable for step, only job */
		if (!opt.job_name_set_cmd && opt.job_name_set_env) {
			/* use SLURM_JOB_NAME env var */
			opt.job_name_set_cmd = true;
		}

		/*
		 *  Become --uid user
		 */
		if (_become_user () < 0)
			info("Warning: Unable to assume uid=%u", opt.uid);

		if (!job || create_job_step(job, true) < 0) {
			slurm_complete_job(resp->job_id, 1);
			exit(error_exit);
		}

		slurm_free_resource_allocation_response_msg(resp);
	}

	/*
	 *  Become --uid user
	 */
	if (_become_user () < 0)
		info("Warning: Unable to assume uid=%u", opt.uid);

	/*
	 *  Enhance environment for job
	 */
	if (opt.cpus_set)
		env->cpus_per_task = opt.cpus_per_task;
	if (opt.ntasks_per_node != NO_VAL)
		env->ntasks_per_node = opt.ntasks_per_node;
	if (opt.ntasks_per_socket != NO_VAL)
		env->ntasks_per_socket = opt.ntasks_per_socket;
	if (opt.ntasks_per_core != NO_VAL)
		env->ntasks_per_core = opt.ntasks_per_core;
	env->distribution = opt.distribution;
	if (opt.plane_size != NO_VAL)
		env->plane_size = opt.plane_size;
	env->cpu_bind_type = opt.cpu_bind_type;
	env->cpu_bind = opt.cpu_bind;
	env->mem_bind_type = opt.mem_bind_type;
	env->mem_bind = opt.mem_bind;
	env->overcommit = opt.overcommit;
	env->slurmd_debug = opt.slurmd_debug;
	env->labelio = opt.labelio;
	env->comm_port = slurmctld_comm_addr.port;
	env->batch_flag = 0;
	if (job) {
		uint16_t *tasks = NULL;
		slurm_step_ctx_get(job->step_ctx, SLURM_STEP_CTX_TASKS,
				   &tasks);

		env->select_jobinfo = job->select_jobinfo;
		env->nodelist = job->nodelist;
		env->task_count = _uint16_array_to_str(
			job->nhosts, tasks);
		env->jobid = job->jobid;
		env->stepid = job->stepid;
	}
	if (opt.pty && (set_winsize(job) < 0)) {
		error("Not using a pseudo-terminal, disregarding --pty option");
		opt.pty = false;
	}
	if (opt.pty) {
		struct termios term;
		int fd = STDIN_FILENO;

		/* Save terminal settings for restore */
		tcgetattr(fd, &termdefaults);
		tcgetattr(fd, &term);
		/* Set raw mode on local tty */
		cfmakeraw(&term);
		tcsetattr(fd, TCSANOW, &term);
		atexit(&_pty_restore);

		block_sigwinch();
		pty_thread_create(job);
		env->pty_port = job->pty_port;
		env->ws_col   = job->ws_col;
		env->ws_row   = job->ws_row;
	}
	setup_env(env, opt.preserve_env);
	xfree(env->task_count);
	xfree(env);

 re_launch:
#if defined HAVE_BGQ
//#if defined HAVE_BGQ && defined HAVE_BG_FILES
	task_state = task_state_create(1);
#else
	task_state = task_state_create(opt.ntasks);
#endif
	slurm_step_launch_params_t_init(&launch_params);
	launch_params.gid = opt.gid;
	launch_params.argc = opt.argc;
	launch_params.argv = opt.argv;
	launch_params.multi_prog = opt.multi_prog ? true : false;
	launch_params.cwd = opt.cwd;
	launch_params.slurmd_debug = opt.slurmd_debug;
	launch_params.buffered_stdio = !opt.unbuffered;
	launch_params.labelio = opt.labelio ? true : false;
	launch_params.remote_output_filename =fname_remote_string(job->ofname);
	launch_params.remote_input_filename = fname_remote_string(job->ifname);
	launch_params.remote_error_filename = fname_remote_string(job->efname);
	launch_params.task_prolog = opt.task_prolog;
	launch_params.task_epilog = opt.task_epilog;
	launch_params.cpu_bind = opt.cpu_bind;
	launch_params.cpu_bind_type = opt.cpu_bind_type;
	launch_params.mem_bind = opt.mem_bind;
	launch_params.mem_bind_type = opt.mem_bind_type;
	launch_params.open_mode = opt.open_mode;
	if (opt.acctg_freq >= 0)
		launch_params.acctg_freq = opt.acctg_freq;
	launch_params.pty = opt.pty;
	if (opt.cpus_set)
		launch_params.cpus_per_task	= opt.cpus_per_task;
	else
		launch_params.cpus_per_task	= 1;
	launch_params.task_dist         = opt.distribution;
	launch_params.ckpt_dir		= opt.ckpt_dir;
	launch_params.restart_dir       = opt.restart_dir;
	launch_params.preserve_env      = opt.preserve_env;
	launch_params.spank_job_env     = opt.spank_job_env;
	launch_params.spank_job_env_size = opt.spank_job_env_size;
	/* job structure should now be filled in */
	if (!signal_thread) {
		slurm_attr_init(&thread_attr);
		while (pthread_create(&signal_thread, &thread_attr,
				      _srun_signal_mgr, NULL)) {
			error("pthread_create error %m");
			sleep(1);
		}
		slurm_attr_destroy(&thread_attr);
	}

	_set_stdio_fds(job, &launch_params.local_fds);

	if (MPIR_being_debugged) {
		launch_params.parallel_debug = true;
		pmi_server_max_threads(1);
	} else {
		launch_params.parallel_debug = false;
	}
	callbacks.task_start = _task_start;
	callbacks.task_finish = _task_finish;

	_run_srun_prolog(job);

	mpir_init(job->ctx_params.task_count);

	if (_call_spank_local_user (job) < 0) {
		error("Failure in local plugin stack");
		slurm_step_launch_abort(job->step_ctx);
		exit(error_exit);
	}

	update_job_state(job, SRUN_JOB_LAUNCHING);
	launch_start_time = time(NULL);
	if (slurm_step_launch(job->step_ctx, &launch_params, &callbacks) !=
	    SLURM_SUCCESS) {
		error("Application launch failed: %m");
		global_rc = 1;
		slurm_step_launch_wait_finish(job->step_ctx);
		goto cleanup;
	}

	update_job_state(job, SRUN_JOB_STARTING);
	if (slurm_step_launch_wait_start(job->step_ctx) == SLURM_SUCCESS) {
		update_job_state(job, SRUN_JOB_RUNNING);
		/* Only set up MPIR structures if the step launched
		 * correctly. */
		if (opt.multi_prog)
			mpir_set_multi_name(job->ctx_params.task_count,
					    launch_params.argv[0]);
		else
			mpir_set_executable_names(launch_params.argv[0]);
		MPIR_debug_state = MPIR_DEBUG_SPAWNED;
		if (opt.debugger_test)
			mpir_dump_proctable();
		else
			MPIR_Breakpoint(job);
	} else {
		info("Job step %u.%u aborted before step completely launched.",
		     job->jobid, job->stepid);
	}

	slurm_step_launch_wait_finish(job->step_ctx);
	if ((MPIR_being_debugged == 0) && retry_step_begin && 
	    (retry_step_cnt < MAX_STEP_RETRIES)) {
		retry_step_begin = false;
		slurm_step_ctx_destroy(job->step_ctx);
		if (got_alloc) {
			if (create_job_step(job, true) < 0)
				exit(error_exit);
		} else {
			if (create_job_step(job, false) < 0)
				exit(error_exit);
		}
		task_state_destroy(task_state);
		goto re_launch;
	}

cleanup:
	if (got_alloc) {
		cleanup_allocation();

		/* send the controller we were cancelled */
		if (job->state >= SRUN_JOB_CANCELLED)
			slurm_complete_job(job->jobid, NO_VAL);
		else
			slurm_complete_job(job->jobid, global_rc);
	}

	if (signal_thread) {
		srun_shutdown = true;
		pthread_kill(signal_thread, SIGINT);
		pthread_join(signal_thread,  NULL);
	}
	_run_srun_epilog(job);
	slurm_step_ctx_destroy(job->step_ctx);
	mpir_cleanup();
	task_state_destroy(task_state);
	log_fini();

	if (WIFEXITED(global_rc))
		global_rc = WEXITSTATUS(global_rc);
	return (int)global_rc;
}

static slurm_step_layout_t *
_get_slurm_step_layout(srun_job_t *job)
{
	job_step_create_response_msg_t *resp;

	if (!job || !job->step_ctx)
		return (NULL);

	slurm_step_ctx_get(job->step_ctx, SLURM_STEP_CTX_RESP, &resp);
	if (!resp)
	    return (NULL);
	return (resp->step_layout);
}

static int _call_spank_local_user (srun_job_t *job)
{
	struct spank_launcher_job_info info[1];

	info->uid = opt.uid;
	info->gid = opt.gid;
	info->jobid = job->jobid;
	info->stepid = job->stepid;
	info->step_layout = _get_slurm_step_layout(job);
	info->argc = opt.argc;
	info->argv = opt.argv;

	return spank_local_user(info);
}


static int _slurm_debug_env_val (void)
{
	long int level = 0;
	const char *val;

	if ((val = getenv ("SLURM_DEBUG"))) {
		char *p;
		if ((level = strtol (val, &p, 10)) < -LOG_LEVEL_INFO)
			level = -LOG_LEVEL_INFO;
		if (p && *p != '\0')
			level = 0;
	}
	return ((int) level);
}

/*
 * Return a string representation of an array of uint32_t elements.
 * Each value in the array is printed in decimal notation and elements
 * are separated by a comma.  If sequential elements in the array
 * contain the same value, the value is written out just once followed
 * by "(xN)", where "N" is the number of times the value is repeated.
 *
 * Example:
 *   The array "1, 2, 1, 1, 1, 3, 2" becomes the string "1,2,1(x3),3,2"
 *
 * Returns an xmalloc'ed string.  Free with xfree().
 */
static char *_uint16_array_to_str(int array_len, const uint16_t *array)
{
	int i;
	int previous = 0;
	char *sep = ",";  /* seperator */
	char *str = xstrdup("");

	if(array == NULL)
		return str;

	for (i = 0; i < array_len; i++) {
		if ((i+1 < array_len)
		    && (array[i] == array[i+1])) {
				previous++;
				continue;
		}

		if (i == array_len-1) /* last time through loop */
			sep = "";
		if (previous > 0) {
			xstrfmtcat(str, "%u(x%u)%s",
				   array[i], previous+1, sep);
		} else {
			xstrfmtcat(str, "%u%s", array[i], sep);
		}
		previous = 0;
	}

	return str;
}

static void
_print_job_information(resource_allocation_response_msg_t *resp)
{
	int i;
	char *str = NULL;
	char *sep = "";

	if (!_verbose)
		return;

	xstrfmtcat(str, "jobid %u: nodes(%u):`%s', cpu counts: ",
		   resp->job_id, resp->node_cnt, resp->node_list);

	for (i = 0; i < resp->num_cpu_groups; i++) {
		xstrfmtcat(str, "%s%u(x%u)",
			   sep, resp->cpus_per_node[i],
			   resp->cpu_count_reps[i]);
		sep = ",";
	}
	verbose("%s", str);
	xfree(str);
}

/* Set SLURM_UMASK environment variable with current state */
static int _set_umask_env(void)
{
	char mask_char[5];
	mode_t mask;

	if (getenv("SLURM_UMASK"))	/* use this value */
		return SLURM_SUCCESS;

	mask = (int)umask(0);
	umask(mask);

	sprintf(mask_char, "0%d%d%d",
		((mask>>6)&07), ((mask>>3)&07), mask&07);
	if (setenvf(NULL, "SLURM_UMASK", "%s", mask_char) < 0) {
		error ("unable to set SLURM_UMASK in environment");
		return SLURM_FAILURE;
	}
	debug ("propagating UMASK=%s", mask_char);
	return SLURM_SUCCESS;
}

/* Set SLURM_SUBMIT_DIR environment variable with current state */
static void _set_submit_dir_env(void)
{
	char buf[MAXPATHLEN + 1];

	if ((getcwd(buf, MAXPATHLEN)) == NULL) {
		error("getcwd failed: %m");
		exit(error_exit);
	}

	if (setenvf(NULL, "SLURM_SUBMIT_DIR", "%s", buf) < 0) {
		error("unable to set SLURM_SUBMIT_DIR in environment");
		return;
	}
}

/*
 * _set_prio_process_env
 *
 * Set the internal SLURM_PRIO_PROCESS environment variable to support
 * the propagation of the users nice value and the "PropagatePrioProcess"
 * config keyword.
 */
static void  _set_prio_process_env(void)
{
	int retval;

	errno = 0; /* needed to detect a real failure since prio can be -1 */

	if ((retval = getpriority (PRIO_PROCESS, 0)) == -1)  {
		if (errno) {
			error ("getpriority(PRIO_PROCESS): %m");
			return;
		}
	}

	if (setenvf (NULL, "SLURM_PRIO_PROCESS", "%d", retval) < 0) {
		error ("unable to set SLURM_PRIO_PROCESS in environment");
		return;
	}

	debug ("propagating SLURM_PRIO_PROCESS=%d", retval);
}

static void _set_exit_code(void)
{
	int i;
	char *val;

	if ((val = getenv("SLURM_EXIT_ERROR"))) {
		i = atoi(val);
		if (i == 0)
			error("SLURM_EXIT_ERROR has zero value");
		else
			error_exit = i;
	}

	if ((val = getenv("SLURM_EXIT_IMMEDIATE"))) {
		i = atoi(val);
		if (i == 0)
			error("SLURM_EXIT_IMMEDIATE has zero value");
		else
			immediate_exit = i;
	}
}

static void _set_cpu_env_var(resource_allocation_response_msg_t *resp)
{
	char *tmp;

	if (getenv("SLURM_JOB_CPUS_PER_NODE"))
		return;

	tmp = uint32_compressed_to_str(resp->num_cpu_groups,
				       resp->cpus_per_node,
				       resp->cpu_count_reps);
	if (setenvf(NULL, "SLURM_JOB_CPUS_PER_NODE", "%s", tmp) < 0)
		error("unable to set SLURM_JOB_CPUS_PER_NODE in environment");
	xfree(tmp);
	return;
}

static int _validate_relative(resource_allocation_response_msg_t *resp)
{

	if (opt.relative_set &&
	    ((opt.relative + opt.min_nodes) > resp->node_cnt)) {
		if (opt.nodes_set_opt) {  /* -N command line option used */
			error("--relative and --nodes option incompatible "
			      "with count of allocated nodes (%d+%d>%d)",
			      opt.relative, opt.min_nodes, resp->node_cnt);
		} else {		/* SLURM_NNODES option used */
			error("--relative and SLURM_NNODES option incompatible "
			      "with count of allocated nodes (%d+%d>%d)",
			      opt.relative, opt.min_nodes, resp->node_cnt);
		}
		return -1;
	}
	return 0;
}

/* Set SLURM_RLIMIT_* environment variables with current resource
 * limit values, reset RLIMIT_NOFILE to maximum possible value */
static int _set_rlimit_env(void)
{
	int                  rc = SLURM_SUCCESS;
	struct rlimit        rlim[1];
	unsigned long        cur;
	char                 name[64], *format;
	slurm_rlimits_info_t *rli;

	/* Modify limits with any command-line options */
	if (opt.propagate && parse_rlimits( opt.propagate, PROPAGATE_RLIMITS)){
		error( "--propagate=%s is not valid.", opt.propagate );
		exit(error_exit);
	}

	for (rli = get_slurm_rlimits_info(); rli->name != NULL; rli++ ) {

		if (rli->propagate_flag != PROPAGATE_RLIMITS)
			continue;

		if (getrlimit (rli->resource, rlim) < 0) {
			error ("getrlimit (RLIMIT_%s): %m", rli->name);
			rc = SLURM_FAILURE;
			continue;
		}

		cur = (unsigned long) rlim->rlim_cur;
		snprintf(name, sizeof(name), "SLURM_RLIMIT_%s", rli->name);
		if (opt.propagate && rli->propagate_flag == PROPAGATE_RLIMITS)
			/*
			 * Prepend 'U' to indicate user requested propagate
			 */
			format = "U%lu";
		else
			format = "%lu";

		if (setenvf (NULL, name, format, cur) < 0) {
			error ("unable to set %s in environment", name);
			rc = SLURM_FAILURE;
			continue;
		}

		debug ("propagating RLIMIT_%s=%lu", rli->name, cur);
	}

	/*
	 *  Now increase NOFILE to the max available for this srun
	 */
	if (getrlimit (RLIMIT_NOFILE, rlim) < 0)
	 	return (error ("getrlimit (RLIMIT_NOFILE): %m"));

	if (rlim->rlim_cur < rlim->rlim_max) {
		rlim->rlim_cur = rlim->rlim_max;
		if (setrlimit (RLIMIT_NOFILE, rlim) < 0)
			return (error ("Unable to increase max no. files: %m"));
	}

	return rc;
}

static int _become_user (void)
{
	char *user = uid_to_string(opt.uid);
	gid_t gid = gid_from_uid(opt.uid);

	if (strcmp(user, "nobody") == 0)
		return (error ("Invalid user id %u: %m", opt.uid));

	if (opt.uid == getuid ())
		return (0);

	if ((opt.egid != (gid_t) -1) && (setgid (opt.egid) < 0))
		return (error ("setgid: %m"));

	initgroups (user, gid); /* Ignore errors */
	xfree(user);

	if (setuid (opt.uid) < 0)
		return (error ("setuid: %m"));

	return (0);
}

static void _run_srun_prolog (srun_job_t *job)
{
	int rc;

	if (opt.prolog && strcasecmp(opt.prolog, "none") != 0) {
		rc = _run_srun_script(job, opt.prolog);
		debug("srun prolog rc = %d", rc);
	}
}

static void _run_srun_epilog (srun_job_t *job)
{
	int rc;

	if (opt.epilog && strcasecmp(opt.epilog, "none") != 0) {
		rc = _run_srun_script(job, opt.epilog);
		debug("srun epilog rc = %d", rc);
	}
}

static int _run_srun_script (srun_job_t *job, char *script)
{
	int status;
	pid_t cpid;
	int i;
	char **args = NULL;

	if (script == NULL || script[0] == '\0')
		return 0;

	if (access(script, R_OK | X_OK) < 0) {
		info("Access denied for %s: %m", script);
		return 0;
	}

	if ((cpid = fork()) < 0) {
		error ("run_srun_script: fork: %m");
		return -1;
	}
	if (cpid == 0) {

		/* set the scripts command line arguments to the arguments
		 * for the application, but shifted one higher
		 */
		args = xmalloc(sizeof(char *) * 1024);
		args[0] = script;
		for (i = 0; i < opt.argc; i++) {
			args[i+1] = opt.argv[i];
		}
		args[i+1] = NULL;
		execv(script, args);
		error("help! %m");
		exit(127);
	}

	do {
		if (waitpid(cpid, &status, 0) < 0) {
			if (errno == EINTR)
				continue;
			error("waitpid: %m");
			return 0;
		} else
			return status;
	} while(1);

	/* NOTREACHED */
}

static int
_is_local_file (fname_t *fname)
{
	if (fname->name == NULL)
		return 1;

	if (fname->taskid != -1)
		return 1;

	return ((fname->type != IO_PER_TASK) && (fname->type != IO_ONE));
}

static void
_set_stdio_fds(srun_job_t *job, slurm_step_io_fds_t *cio_fds)
{
	bool err_shares_out = false;
	int file_flags;

	if (opt.open_mode == OPEN_MODE_APPEND)
		file_flags = O_CREAT|O_WRONLY|O_APPEND;
	else if (opt.open_mode == OPEN_MODE_TRUNCATE)
		file_flags = O_CREAT|O_WRONLY|O_APPEND|O_TRUNC;
	else {
		slurm_ctl_conf_t *conf;
		conf = slurm_conf_lock();
		if (conf->job_file_append)
			file_flags = O_CREAT|O_WRONLY|O_APPEND;
		else
			file_flags = O_CREAT|O_WRONLY|O_APPEND|O_TRUNC;
		slurm_conf_unlock();
	}

	/*
	 * create stdin file descriptor
	 */
	if (_is_local_file(job->ifname)) {
		if ((job->ifname->name == NULL) ||
		    (job->ifname->taskid != -1)) {
			cio_fds->in.fd = STDIN_FILENO;
		} else {
			cio_fds->in.fd = open(job->ifname->name, O_RDONLY);
			if (cio_fds->in.fd == -1) {
				error("Could not open stdin file: %m");
				exit(error_exit);
			}
		}
		if (job->ifname->type == IO_ONE) {
			cio_fds->in.taskid = job->ifname->taskid;
			cio_fds->in.nodeid = slurm_step_layout_host_id(
				_get_slurm_step_layout(job),
				job->ifname->taskid);
		}
	}

	/*
	 * create stdout file descriptor
	 */
	if (_is_local_file(job->ofname)) {
		if ((job->ofname->name == NULL) ||
		    (job->ofname->taskid != -1)) {
			cio_fds->out.fd = STDOUT_FILENO;
		} else {
			cio_fds->out.fd = open(job->ofname->name,
					       file_flags, 0644);
			if (cio_fds->out.fd == -1) {
				error("Could not open stdout file: %m");
				exit(error_exit);
			}
		}
		if (job->ofname->name != NULL
		    && job->efname->name != NULL
		    && !strcmp(job->ofname->name, job->efname->name)) {
			err_shares_out = true;
		}
	}

	/*
	 * create seperate stderr file descriptor only if stderr is not sharing
	 * the stdout file descriptor
	 */
	if (err_shares_out) {
		debug3("stdout and stderr sharing a file");
		cio_fds->err.fd = cio_fds->out.fd;
		cio_fds->err.taskid = cio_fds->out.taskid;
	} else if (_is_local_file(job->efname)) {
		if ((job->efname->name == NULL) ||
		    (job->efname->taskid != -1)) {
			cio_fds->err.fd = STDERR_FILENO;
		} else {
			cio_fds->err.fd = open(job->efname->name,
					       file_flags, 0644);
			if (cio_fds->err.fd == -1) {
				error("Could not open stderr file: %m");
				exit(error_exit);
			}
		}
	}
}

/* Plugins must be able to resolve symbols.
 * Since srun statically links with src/api/libslurmhelper rather than
 * dynamicaly linking with libslurm, we need to reference all needed
 * symbols within srun. None of the functions below are actually
 * used, but we need to load the symbols. */
static void _define_symbols(void)
{
	/* needed by mvapich and mpichgm */
	slurm_signal_job_step(NO_VAL, NO_VAL, 0);
}

static void _pty_restore(void)
{
	/* STDIN is probably closed by now */
	if (tcsetattr(STDOUT_FILENO, TCSANOW, &termdefaults) < 0)
		fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
}

/* opt.exclusive is set, disable user task layout controls */
static void _step_opt_exclusive(void)
{
	if (!opt.ntasks_set) {
		error("--ntasks must be set with --exclusive");
		exit(error_exit);
	}
	if (opt.relative_set) {
		error("--relative disabled, incompatible with --exclusive");
		exit(error_exit);
	}
	if (opt.exc_nodes) {
		error("--exclude is incompatible with --exclusive");
		exit(error_exit);
	}
}

static void
_task_start(launch_tasks_response_msg_t *msg)
{
	MPIR_PROCDESC *table;
	int taskid;
	int i;

	if(msg->count_of_pids)
		verbose("Node %s, %d tasks started",
			msg->node_name, msg->count_of_pids);
	else
		/* This message should be displayed through the api,
		   hense it is a debug2 instead of an error.
		*/
		debug2("No tasks started on node %s: %s",
		      msg->node_name, slurm_strerror(msg->return_code));


	for (i = 0; i < msg->count_of_pids; i++) {
		taskid = msg->task_ids[i];
		table = &MPIR_proctable[taskid];
		table->host_name = xstrdup(msg->node_name);
		/* table->executable_name is set elsewhere */
		table->pid = msg->local_pids[i];

		if (msg->return_code == 0) {
			task_state_update(task_state, taskid, TS_START_SUCCESS);
		} else {
			task_state_update(task_state, taskid, TS_START_FAILURE);
		}
	}

}

static void
_terminate_job_step(slurm_step_ctx_t *step_ctx)
{
	uint32_t job_id, step_id;

	slurm_step_ctx_get(step_ctx, SLURM_STEP_CTX_JOBID, &job_id);
	slurm_step_ctx_get(step_ctx, SLURM_STEP_CTX_STEPID, &step_id);
	info("Terminating job step %u.%u", job_id, step_id);
	update_job_state(job, SRUN_JOB_CANCELLED);
	slurm_kill_job_step(job_id, step_id, SIGKILL);
}

static char *
_hostset_to_string(hostset_t hs)
{
	size_t n = 1024;
	size_t maxsize = 1024*64;
	char *str = NULL;

	do {
		str = xrealloc(str, n);
	} while (hostset_ranged_string(hs, n*=2, str) < 0 && (n < maxsize));

	/*
	 *  If string was truncated, indicate this with a '+' suffix.
	 */
	if (n >= maxsize)
		strcpy(str + (maxsize - 2), "+");

	return str;
}

/* Convert an array of task IDs into a list of host names
 * RET: the string, caller must xfree() this value */
static char *
_task_ids_to_host_list(int ntasks, uint32_t taskids[])
{
	int i;
	hostset_t hs;
	char *hosts;
	slurm_step_layout_t *sl;

	if ((sl = _get_slurm_step_layout(job)) == NULL)
		return (xstrdup("Unknown"));

	hs = hostset_create(NULL);
	for (i = 0; i < ntasks; i++) {
		char *host = slurm_step_layout_host_name(sl, taskids[i]);
		if (host) {
			hostset_insert(hs, host);
			free(host);
		} else {
			error("Could not identify host name for task %u",
			      taskids[i]);
		}
	}

	hosts = _hostset_to_string(hs);
	hostset_destroy(hs);

	return (hosts);
}

/* Convert an array of task IDs into a string.
 * RET: the string, caller must xfree() this value
 * NOTE: the taskids array is not necessarily in numeric order,
 *       so we use existing bitmap functions to format */
static char *
_task_array_to_string(int ntasks, uint32_t taskids[])
{
	bitstr_t *tasks_bitmap = NULL;
	char *str;
	int i;

	tasks_bitmap = bit_alloc(job->ntasks);
	if (!tasks_bitmap) {
		error("bit_alloc: memory allocation failure");
		exit(error_exit);
	}
	for (i=0; i<ntasks; i++)
		bit_set(tasks_bitmap, taskids[i]);
	str = xmalloc(2048);
	bit_fmt(str, 2048, tasks_bitmap);
	FREE_NULL_BITMAP(tasks_bitmap);

	return str;
}

static void
_update_task_exit_state(uint32_t ntasks, uint32_t taskids[], int abnormal)
{
	int i;
	task_state_type_t t = abnormal ? TS_ABNORMAL_EXIT : TS_NORMAL_EXIT;

	for (i = 0; i < ntasks; i++)
		task_state_update(task_state, taskids[i], t);
}

static int _kill_on_bad_exit(void)
{
	if (opt.kill_bad_exit == NO_VAL)
		return slurm_get_kill_on_bad_exit();
	return opt.kill_bad_exit;
}

static void _setup_max_wait_timer(void)
{
	/*  If these are the first tasks to finish we need to
	 *   start a timer to kill off the job step if the other
	 *   tasks don't finish within opt.max_wait seconds.
	 */
	verbose("First task exited. Terminating job in %ds.", opt.max_wait);
	srun_max_timer = true;
	alarm(opt.max_wait);
}

static const char *
_taskstr(int n)
{
	if (n == 1)
		return "task";
	else
		return "tasks";
}

static int
_is_openmpi_port_error(int errcode)
{
	if (errcode != OPEN_MPI_PORT_ERROR)
		return 0;
	if (opt.resv_port_cnt == NO_VAL)
		return 0;
	if (difftime(time(NULL), launch_start_time) > slurm_get_msg_timeout())
		return 0;
	return 1;
}

static void
_handle_openmpi_port_error(const char *tasks, const char *hosts,
			   slurm_step_ctx_t *step_ctx)
{
	uint32_t job_id, step_id;
	char *msg = "retrying";

	if (!retry_step_begin) {
		retry_step_begin = true;
		retry_step_cnt++;
	}

	if (retry_step_cnt >= MAX_STEP_RETRIES)
		msg = "aborting";
	error("%s: tasks %s unable to claim reserved port, %s.",
	      hosts, tasks, msg);

	slurm_step_ctx_get(step_ctx, SLURM_STEP_CTX_JOBID, &job_id);
	slurm_step_ctx_get(step_ctx, SLURM_STEP_CTX_STEPID, &step_id);
	info("Terminating job step %u.%u", job_id, step_id);
	slurm_kill_job_step(job_id, step_id, SIGKILL);
}

static void
_task_finish(task_exit_msg_t *msg)
{
	char *tasks;
	char *hosts;
	uint32_t rc = 0;
	int normal_exit = 0;

	const char *task_str = _taskstr(msg->num_tasks);

	verbose("Received task exit notification for %d %s (status=0x%04x).",
	      msg->num_tasks, task_str, msg->return_code);

	tasks = _task_array_to_string(msg->num_tasks, msg->task_id_list);
	hosts = _task_ids_to_host_list(msg->num_tasks, msg->task_id_list);

	if (WIFEXITED(msg->return_code)) {
		if ((rc = WEXITSTATUS(msg->return_code)) == 0) {
			verbose("%s: %s %s: Completed", hosts, task_str, tasks);
			normal_exit = 1;
		}
		else if (_is_openmpi_port_error(rc)) {
			_handle_openmpi_port_error(tasks, hosts,
						   job->step_ctx);
		} else {
			error("%s: %s %s: Exited with exit code %d",
			      hosts, task_str, tasks, rc);
		}
		if (!WIFEXITED(global_rc) || (rc > WEXITSTATUS(global_rc)))
			global_rc = msg->return_code;
	}
	else if (WIFSIGNALED(msg->return_code)) {
		const char *signal_str = strsignal(WTERMSIG(msg->return_code));
		char * core_str = "";
#ifdef WCOREDUMP
		if (WCOREDUMP(msg->return_code))
			core_str = " (core dumped)";
#endif
		if (job->state >= SRUN_JOB_CANCELLED) {
			verbose("%s: %s %s: %s%s",
				hosts, task_str, tasks, signal_str, core_str);
		} else {
			rc = msg->return_code;
			error("%s: %s %s: %s%s",
			      hosts, task_str, tasks, signal_str, core_str);
		}
		if (global_rc == 0)
			global_rc = msg->return_code;
	}

	xfree(tasks);
	xfree(hosts);

	_update_task_exit_state(msg->num_tasks, msg->task_id_list,
				!normal_exit);

	if (task_state_first_abnormal_exit(task_state) && _kill_on_bad_exit())
  		_terminate_job_step(job->step_ctx);

	if (task_state_first_exit(task_state) && (opt.max_wait > 0))
		_setup_max_wait_timer();
}

static void _handle_intr(void)
{
	static time_t last_intr      = 0;
	static time_t last_intr_sent = 0;
	time_t now = time(NULL);

	if (!opt.quit_on_intr && ((now - last_intr) > 1)) {
		if  (opt.disable_status) {
			info("sending Ctrl-C to job %u.%u",
			     job->jobid, job->stepid);
			slurm_step_launch_fwd_signal(job->step_ctx, SIGINT);
		} else if (job->state < SRUN_JOB_FORCETERM) {
			info("interrupt (one more within 1 sec to abort)");
			task_state_print(task_state, (log_f) info);
		} else {
			info("interrupt (abort already in progress)");
			task_state_print(task_state, (log_f) info);
		}
		last_intr = time(NULL);
	} else  { /* second Ctrl-C in half as many seconds */
		update_job_state(job, SRUN_JOB_CANCELLED);
		/* terminate job */
		if (job->state < SRUN_JOB_FORCETERM) {
			if ((now - last_intr_sent) < 1) {
				job_force_termination(job);
				slurm_step_launch_abort(job->step_ctx);
				return;
			}

			info("sending Ctrl-C to job %u.%u",
			     job->jobid, job->stepid);
			last_intr_sent = now;
			slurm_step_launch_fwd_signal(job->step_ctx, SIGINT);
			slurm_step_launch_abort(job->step_ctx);
		} else {
			job_force_termination(job);
			slurm_step_launch_abort(job->step_ctx);
		}
	}
}

static void _default_sigaction(int sig)
{
	struct sigaction act;
	if (sigaction(sig, NULL, &act)) {
		error("sigaction(%d): %m", sig);
		return;
	}
	if (act.sa_handler != SIG_IGN)
		return;

	act.sa_handler = SIG_DFL;
	if (sigaction(sig, &act, NULL))
		error("sigaction(%d): %m", sig);
}

static void _handle_pipe(void)
{
	static int ending = 0;

	if (ending)
		return;
	ending = 1;
	slurm_step_launch_abort(job->step_ctx);
}

/* _srun_signal_mgr - Process daemon-wide signals */
static void *_srun_signal_mgr(void *no_data)
{
	int sig;
	int i, rc;
	sigset_t set;

	/* Make sure no required signals are ignored (possibly inherited) */
	for (i = 0; sig_array[i]; i++)
		_default_sigaction(sig_array[i]);
	while (!srun_shutdown) {
		xsignal_sigset_create(sig_array, &set);
		rc = sigwait(&set, &sig);
		if (rc == EINTR)
			continue;
		switch (sig) {
		case SIGINT:
			if (!srun_shutdown)
				_handle_intr();
			break;
		case SIGQUIT:
			info("Quit");
			/* continue with slurm_step_launch_abort */
		case SIGTERM:
		case SIGHUP:
			/* No need to call job_force_termination here since we
			 * are ending the job now and we don't need to update
			 * the state. */
			info("forcing job termination");
			slurm_step_launch_abort(job->step_ctx);
			break;
		case SIGCONT:
			info("got SIGCONT");
			break;
		case SIGPIPE:
			_handle_pipe();
			break;
		case SIGALRM:
			if (srun_max_timer) {
				info("First task exited %ds ago", opt.max_wait);
				task_state_print(task_state, (log_f) info);
				_terminate_job_step(job->step_ctx);
			}
			break;
		default:
			slurm_step_launch_fwd_signal(job->step_ctx, sig);
			break;
		}
	}
	return NULL;
}
