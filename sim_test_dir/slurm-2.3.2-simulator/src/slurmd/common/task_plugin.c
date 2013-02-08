/*****************************************************************************\
 *  task_plugin.c - task launch plugin stub.
 *****************************************************************************
 *  Copyright (C) 2005-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2009 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
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

#include <pthread.h>

#include "src/common/plugin.h"
#include "src/common/plugrack.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/slurmd/slurmstepd/slurmstepd_job.h"

typedef struct slurmd_task_ops {
	int	(*slurmd_batch_request)	    (uint32_t job_id,
					     batch_job_launch_msg_t *req);
	int	(*slurmd_launch_request)    (uint32_t job_id,
					     launch_tasks_request_msg_t *req,
					     uint32_t node_id);
	int	(*slurmd_reserve_resources) (uint32_t job_id,
					     launch_tasks_request_msg_t *req,
					     uint32_t node_id);
	int	(*slurmd_suspend_job)	    (uint32_t job_id);
	int	(*slurmd_resume_job)	    (uint32_t job_id);
	int	(*slurmd_release_resources) (uint32_t job_id);

	int	(*pre_setuid)		    (slurmd_job_t *job);
	int	(*pre_launch)		    (slurmd_job_t *job);
	int	(*post_term)		    (slurmd_job_t *job);
	int	(*post_step)		    (slurmd_job_t *job);
} slurmd_task_ops_t;


typedef struct slurmd_task_context {
	char			*task_type;
	plugrack_t		plugin_list;
	plugin_handle_t		cur_plugin;
	slurmd_task_ops_t	ops;
} slurmd_task_context_t;

static slurmd_task_context_t	**g_task_context = NULL;
static int			g_task_context_num = -1;
static pthread_mutex_t		g_task_context_lock = PTHREAD_MUTEX_INITIALIZER;


static slurmd_task_ops_t *
_slurmd_task_get_ops(slurmd_task_context_t *c)
{
	/*
	 * Must be synchronized with slurmd_task_ops_t above.
	 */
	static const char *syms[] = {
		"task_slurmd_batch_request",
		"task_slurmd_launch_request",
		"task_slurmd_reserve_resources",
		"task_slurmd_suspend_job",
		"task_slurmd_resume_job",
		"task_slurmd_release_resources",
		"task_pre_setuid",
		"task_pre_launch",
		"task_post_term",
		"task_post_step",
	};
	int n_syms = sizeof( syms ) / sizeof( char * );

	/* Find the correct plugin. */
	c->cur_plugin = plugin_load_and_link(c->task_type, n_syms, syms,
					     (void **) &c->ops);
	if ( c->cur_plugin != PLUGIN_INVALID_HANDLE )
		return &c->ops;

	if(errno != EPLUGIN_NOTFOUND) {
		error("Couldn't load specified plugin name for %s: %s",
		      c->task_type, plugin_strerror(errno));
		return NULL;
	}

	error("Couldn't find the specified plugin name for %s "
	      "looking at all files",
	      c->task_type);

	/* Get plugin list. */
	if ( c->plugin_list == NULL ) {
		char *plugin_dir;
		c->plugin_list = plugrack_create();
		if ( c->plugin_list == NULL ) {
			error( "cannot create plugin manager" );
			return NULL;
		}
		plugrack_set_major_type( c->plugin_list, "task" );
		plugrack_set_paranoia( c->plugin_list,
				       PLUGRACK_PARANOIA_NONE,
				       0 );
		plugin_dir = slurm_get_plugin_dir();
		plugrack_read_dir( c->plugin_list, plugin_dir );
		xfree(plugin_dir);
	}

	c->cur_plugin = plugrack_use_by_type( c->plugin_list, c->task_type );
	if ( c->cur_plugin == PLUGIN_INVALID_HANDLE ) {
		error( "cannot find task plugin for %s", c->task_type );
		return NULL;
	}

	/* Dereference the API. */
	if ( plugin_get_syms( c->cur_plugin, n_syms, syms,
			(void **) &c->ops ) < n_syms ) {
		error( "incomplete task plugin detected" );
		return NULL;
	}

	return &c->ops;
}


static slurmd_task_context_t *
_slurmd_task_context_create(const char *task_plugin_type)
{
	slurmd_task_context_t *c;

	if ( task_plugin_type == NULL ) {
		debug3( "task_plugin_type is NULL" );
		return NULL;
	}

	c = xmalloc( sizeof( slurmd_task_context_t ) );
	c->task_type	= xstrdup( task_plugin_type );
	c->plugin_list	= NULL;
	c->cur_plugin	= PLUGIN_INVALID_HANDLE;

	return c;
}


static int
_slurmd_task_context_destroy(slurmd_task_context_t *c)
{
	int rc = SLURM_SUCCESS;
	/*
	 * Must check return code here because plugins might still
	 * be loaded and active.
	 */
	if ( c->plugin_list ) {
		if ( plugrack_destroy( c->plugin_list ) != SLURM_SUCCESS ) {
			rc = SLURM_ERROR;
		}
	} else {
		plugin_unload(c->cur_plugin);
	}

	xfree( c->task_type );
	xfree( c );

	return rc;
}


/*
 * Initialize the task plugin.
 *
 * RET - slurm error code
 */
extern int slurmd_task_init(void)
{
	int retval = SLURM_SUCCESS, i;
	char *task_plugin_type = NULL;
	char *last = NULL, *task_plugin_list, *task_plugin = NULL;

	slurm_mutex_lock( &g_task_context_lock );

	if ( g_task_context_num >= 0 )
		goto done;

	task_plugin_type = slurm_get_task_plugin();
	g_task_context_num = 0; /* mark it before anything else */
	if (task_plugin_type == NULL || task_plugin_type[0] == '\0')
		goto done;

	task_plugin_list = task_plugin_type;
	while ((task_plugin = strtok_r(task_plugin_list, ",", &last))) {
		i = g_task_context_num++;
		xrealloc(g_task_context,
			 (sizeof(slurmd_task_context_t *) * g_task_context_num));
		if (strncmp(task_plugin, "task/", 5) == 0)
			task_plugin += 5; /* backward compatibility */
		task_plugin = xstrdup_printf("task/%s", task_plugin);
		g_task_context[i] = _slurmd_task_context_create( task_plugin );
		if ( g_task_context[i] == NULL ) {
			error( "cannot create task context for %s",
				 task_plugin );
			goto error;
		}

		if ( _slurmd_task_get_ops( g_task_context[i] ) == NULL ) {
			error( "cannot resolve task plugin operations for %s",
			       task_plugin );
			goto error;
		}
		xfree(task_plugin);
		task_plugin_list = NULL; /* for next iteration */
	}

 done:
	slurm_mutex_unlock( &g_task_context_lock );
	xfree(task_plugin_type);
	return retval;

error:
	xfree(task_plugin);
	retval = SLURM_ERROR;
	for (i = 0; i < g_task_context_num; i++)
		if (g_task_context[i])
			_slurmd_task_context_destroy(g_task_context[i]);
	xfree(g_task_context);
	g_task_context_num = -1;
	goto done;
}

/*
 * Terminate the task plugin, free memory.
 *
 * RET - slurm error code
 */
extern int slurmd_task_fini(void)
{
	int i, rc = SLURM_SUCCESS;

	slurm_mutex_lock( &g_task_context_lock );
	if (!g_task_context)
		goto done;

	for (i = 0; i < g_task_context_num; i++) {
		if (_slurmd_task_context_destroy(g_task_context[i]) !=
		    SLURM_SUCCESS) {
			rc = SLURM_ERROR;
		}
	}

	xfree(g_task_context);
	g_task_context_num = -1;

done:
	slurm_mutex_unlock( &g_task_context_lock );
	return rc;
}

/*
 * Slurmd has received a batch job launch request.
 *
 * RET - slurm error code
 */
extern int slurmd_batch_request(uint32_t job_id, batch_job_launch_msg_t *req)
{
	int i, rc = SLURM_SUCCESS;

	if (slurmd_task_init())
		return SLURM_ERROR;

	slurm_mutex_lock( &g_task_context_lock );
	for (i = 0; ((i < g_task_context_num) && (rc == SLURM_SUCCESS)); i++) {
		rc = (*(g_task_context[i]->ops.slurmd_batch_request))(job_id,
								      req);
	}
	slurm_mutex_unlock( &g_task_context_lock );

	return (rc);
}

/*
 * Slurmd has received a launch request.
 *
 * RET - slurm error code
 */
extern int slurmd_launch_request(uint32_t job_id,
				 launch_tasks_request_msg_t *req,
				 uint32_t node_id)
{
	int i, rc = SLURM_SUCCESS;

	if (slurmd_task_init())
		return SLURM_ERROR;

	slurm_mutex_lock( &g_task_context_lock );
	for (i = 0; ((i < g_task_context_num) && (rc == SLURM_SUCCESS)); i++) {
		rc = (*(g_task_context[i]->ops.slurmd_launch_request))
					(job_id, req, node_id);
	}
	slurm_mutex_unlock( &g_task_context_lock );

	return (rc);
}

/*
 * Slurmd is reserving resources for the task.
 *
 * RET - slurm error code
 */
extern int slurmd_reserve_resources(uint32_t job_id,
				    launch_tasks_request_msg_t *req,
				    uint32_t node_id )
{
	int i, rc = SLURM_SUCCESS;

	if (slurmd_task_init())
		return SLURM_ERROR;

	slurm_mutex_lock( &g_task_context_lock );
	for (i = 0; ((i < g_task_context_num) && (rc == SLURM_SUCCESS)); i++) {
		rc = (*(g_task_context[i]->ops.slurmd_reserve_resources))
					(job_id, req, node_id);
	}
	slurm_mutex_unlock( &g_task_context_lock );

	return (rc);
}

/*
 * Slurmd is suspending a job.
 *
 * RET - slurm error code
 */
extern int slurmd_suspend_job(uint32_t job_id)
{
	int i, rc = SLURM_SUCCESS;

	if (slurmd_task_init())
		return SLURM_ERROR;

	slurm_mutex_lock( &g_task_context_lock );
	for (i = 0; ((i < g_task_context_num) && (rc == SLURM_SUCCESS)); i++)
		rc = (*(g_task_context[i]->ops.slurmd_suspend_job))(job_id);
	slurm_mutex_unlock( &g_task_context_lock );

	return (rc);
}

/*
 * Slurmd is resuming a previously suspended job.
 *
 * RET - slurm error code
 */
extern int slurmd_resume_job(uint32_t job_id)
{
	int i, rc = SLURM_SUCCESS;

	if (slurmd_task_init())
		return SLURM_ERROR;

	slurm_mutex_lock( &g_task_context_lock );
	for (i = 0; ((i < g_task_context_num) && (rc == SLURM_SUCCESS)); i++)
		rc = (*(g_task_context[i]->ops.slurmd_resume_job))(job_id);
	slurm_mutex_unlock( &g_task_context_lock );

	return (rc);
}

/*
 * Slurmd is releasing resources for the task.
 *
 * RET - slurm error code
 */
extern int slurmd_release_resources(uint32_t job_id)
{
	int i, rc = SLURM_SUCCESS;

	if (slurmd_task_init())
		return SLURM_ERROR;

	slurm_mutex_lock( &g_task_context_lock );
	for (i = 0; ((i < g_task_context_num) && (rc == SLURM_SUCCESS)); i++) {
		rc = (*(g_task_context[i]->ops.slurmd_release_resources))
				(job_id);
	}
	slurm_mutex_unlock( &g_task_context_lock );

	return (rc);
}

/*
 * Note that a task launch is about to occur.
 * Run before setting UID to the user.
 *
 * RET - slurm error code
 */
extern int pre_setuid(slurmd_job_t *job)
{
	int i, rc = SLURM_SUCCESS;

	if (slurmd_task_init())
		return SLURM_ERROR;

	slurm_mutex_lock( &g_task_context_lock );
	for (i = 0; ((i < g_task_context_num) && (rc == SLURM_SUCCESS)); i++)
		rc = (*(g_task_context[i]->ops.pre_setuid))(job);
	slurm_mutex_unlock( &g_task_context_lock );

	return (rc);
}

/*
 * Note that a task launch is about to occur.
 *
 * RET - slurm error code
 */
extern int pre_launch(slurmd_job_t *job)
{
	int i, rc = SLURM_SUCCESS;

	if (slurmd_task_init())
		return SLURM_ERROR;

	slurm_mutex_lock( &g_task_context_lock );
	for (i = 0; ((i < g_task_context_num) && (rc == SLURM_SUCCESS)); i++)
		rc = (*(g_task_context[i]->ops.pre_launch))(job);
	slurm_mutex_unlock( &g_task_context_lock );

	return (rc);
}

/*
 * Note that a task has terminated.
 *
 * RET - slurm error code
 */
extern int post_term(slurmd_job_t *job)
{
	int i, rc = SLURM_SUCCESS;

	if (slurmd_task_init())
		return SLURM_ERROR;

	slurm_mutex_lock( &g_task_context_lock );
	for (i = 0; ((i < g_task_context_num) && (rc == SLURM_SUCCESS)); i++)
		rc = (*(g_task_context[i]->ops.post_term))(job);
	slurm_mutex_unlock( &g_task_context_lock );

	return (rc);
}

/*
 * Note that a step has terminated.
 *
 * RET - slurm error code
 */
extern int post_step(slurmd_job_t *job)
{
	int i, rc = SLURM_SUCCESS;

	if (slurmd_task_init())
		return SLURM_ERROR;

	slurm_mutex_lock( &g_task_context_lock );
	for (i = 0; ((i < g_task_context_num) && (rc == SLURM_SUCCESS)); i++)
		rc = (*(g_task_context[i]->ops.post_step))(job);
	slurm_mutex_unlock( &g_task_context_lock );

	return (rc);
}
