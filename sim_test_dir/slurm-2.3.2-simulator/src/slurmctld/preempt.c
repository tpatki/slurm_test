/*****************************************************************************\
 *  preempt.c - Job preemption plugin function setup.
 *****************************************************************************
 *  Copyright (C) 2009-2010 Lawrence Livermore National Security.
 *  Portions Copyright (C) 2010 SchedMD <http://www.schedmd.com>.
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

#include "src/common/log.h"
#include "src/common/plugrack.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/job_scheduler.h"


/* ************************************************************************ */
/*  TAG(                     slurm_preempt_ops_t                        )  */
/* ************************************************************************ */
typedef struct slurm_preempt_ops {
	List		(*find_jobs)	      (struct job_record *job_ptr);
	uint16_t	(*job_preempt_mode)   (struct job_record *job_ptr);
	bool		(*preemption_enabled) (void);
	bool		(*job_preempt_check)  (job_queue_rec_t *preemptor,
					       job_queue_rec_t *preemptee);
} slurm_preempt_ops_t;


/* ************************************************************************ */
/*  TAG(                     slurm_preempt_contex_t                     )  */
/* ************************************************************************ */
typedef struct slurm_preempt_context {
	char	       	*preempt_type;
	plugrack_t     	plugin_list;
	plugin_handle_t	cur_plugin;
	int		preempt_errno;
	slurm_preempt_ops_t ops;
} slurm_preempt_context_t;

static slurm_preempt_context_t *g_preempt_context = NULL;
static pthread_mutex_t	    g_preempt_context_lock = PTHREAD_MUTEX_INITIALIZER;


/* ************************************************************************ */
/*  TAG(                    _slurm_preempt_get_ops                      )  */
/* ************************************************************************ */
static slurm_preempt_ops_t *
_slurm_preempt_get_ops(slurm_preempt_context_t *c)
{
	/*
	 * Must be synchronized with slurm_preempt_ops_t above.
	 */
	static const char *syms[] = {
		"find_preemptable_jobs",
		"job_preempt_mode",
		"preemption_enabled",
		"job_preempt_check",
	};
	int n_syms = sizeof(syms) / sizeof(char *);

	/* Find the correct plugin. */
        c->cur_plugin = plugin_load_and_link(c->preempt_type, n_syms, syms,
					     (void **) &c->ops);
        if (c->cur_plugin != PLUGIN_INVALID_HANDLE)
        	return &c->ops;

	if(errno != EPLUGIN_NOTFOUND) {
		error("Couldn't load specified plugin name for %s: %s",
		      c->preempt_type, plugin_strerror(errno));
		return NULL;
	}

	error("Couldn't find the specified plugin name for %s "
	      "looking at all files",
	      c->preempt_type);

	/* Get plugin list. */
	if (c->plugin_list == NULL) {
		char *plugin_dir;
		c->plugin_list = plugrack_create();
		if (c->plugin_list == NULL) {
			error("cannot create plugin manager");
			return NULL;
		}
		plugrack_set_major_type(c->plugin_list, "preempt");
		plugrack_set_paranoia(c->plugin_list,
				      PLUGRACK_PARANOIA_NONE, 0);
		plugin_dir = slurm_get_plugin_dir();
		plugrack_read_dir(c->plugin_list, plugin_dir);
		xfree(plugin_dir);
	}

	c->cur_plugin = plugrack_use_by_type(c->plugin_list, c->preempt_type);
	if (c->cur_plugin == PLUGIN_INVALID_HANDLE) {
		error("cannot find preempt plugin for %s", c->preempt_type);
		return NULL;
	}

	/* Dereference the API. */
	if (plugin_get_syms(c->cur_plugin,
			    n_syms,
			    syms,
			    (void **) &c->ops) < n_syms) {
		error("incomplete preempt plugin detected");
		return NULL;
	}

	return &c->ops;
}


/* ************************************************************************ */
/*  TAG(               _slurm_preempt_context_create                    )  */
/* ************************************************************************ */
static slurm_preempt_context_t *
_slurm_preempt_context_create(const char *preempt_type)
{
	slurm_preempt_context_t *c;

	if (preempt_type == NULL) {
		debug3("slurm_preempt_context:  no preempt type");
		return NULL;
	}

	c = xmalloc(sizeof(slurm_preempt_context_t));
	c->preempt_type   = xstrdup(preempt_type);
	c->plugin_list    = NULL;
	c->cur_plugin     = PLUGIN_INVALID_HANDLE;
	c->preempt_errno  = SLURM_SUCCESS;

	return c;
}


/* ************************************************************************ */
/*  TAG(              _slurm_preempt_context_destroy                   )  */
/* ************************************************************************ */
static int _slurm_preempt_context_destroy(slurm_preempt_context_t *c)
{
	/*
	 * Must check return code here because plugins might still
	 * be loaded and active.
	 */
	if (c->plugin_list) {
		if (plugrack_destroy(c->plugin_list) != SLURM_SUCCESS) {
			return SLURM_ERROR;
		}
	} else {
		plugin_unload(c->cur_plugin);
	}

	xfree(c->preempt_type);
	xfree(c);

	return SLURM_SUCCESS;
}

/* *********************************************************************** */
/*  TAG(                    _preempt_signal                             )  */
/* *********************************************************************** */
static void _preempt_signal(struct job_record *job_ptr, uint32_t grace_time)
{
	/* allow the job termination mechanism to signal the job */

	job_ptr->preempt_time = time(NULL);
	job_ptr->end_time = job_ptr->preempt_time + (time_t)grace_time;
}
/* *********************************************************************** */
/*  TAG(                    slurm_job_check_grace                       )  */
/* *********************************************************************** */
extern int slurm_job_check_grace(struct job_record *job_ptr)
{
	/* Preempt modes: -1 (unset), 0 (none), 1 (partition), 2 (QOS) */
	static int preempt_mode = 0;
	static time_t last_update_time = 0;
	int rc = SLURM_SUCCESS;
	uint32_t grace_time = 0;

	if (job_ptr->preempt_time) {
		if (time(NULL) >= job_ptr->end_time)
			rc = SLURM_ERROR;
		return rc;
	}

	if (last_update_time != slurmctld_conf.last_update) {
		char *preempt_type = slurm_get_preempt_type();
		if ((strcmp(preempt_type, "preempt/partition_prio") == 0))
			preempt_mode = 1;
		else if ((strcmp(preempt_type, "preempt/qos") == 0))
			preempt_mode = 2;
		else
			preempt_mode = 0;
		xfree(preempt_type);
	}

	if (preempt_mode == 1)
		grace_time = job_ptr->part_ptr->grace_time;
	else if (preempt_mode == 2) {
		slurmdb_qos_rec_t *qos_ptr = (slurmdb_qos_rec_t *)
					     job_ptr->qos_ptr;
		grace_time = qos_ptr->grace_time;
	}

	if (grace_time)
		_preempt_signal(job_ptr, grace_time);
	else
		rc = SLURM_ERROR;

	return rc;
}

/* *********************************************************************** */
/*  TAG(                    slurm_preempt_init                        )  */
/* *********************************************************************** */
extern int slurm_preempt_init(void)
{
	int retval = SLURM_SUCCESS;
	char *preempt_type = NULL;

	slurm_mutex_lock(&g_preempt_context_lock);

	if (g_preempt_context)
		goto done;

	preempt_type = slurm_get_preempt_type();
	g_preempt_context = _slurm_preempt_context_create(preempt_type);
	if (g_preempt_context == NULL) {
		error("cannot create preempt context for %s",
		      preempt_type);
		retval = SLURM_ERROR;
		goto done;
	}

	if (_slurm_preempt_get_ops(g_preempt_context) == NULL) {
		error("cannot resolve preempt plugin operations");
		_slurm_preempt_context_destroy(g_preempt_context);
		g_preempt_context = NULL;
		retval = SLURM_ERROR;
	}

done:
	slurm_mutex_unlock(&g_preempt_context_lock);
	xfree(preempt_type);
	return retval;
}

/* *********************************************************************** */
/*  TAG(                    slurm_preempt_fini                        )  */
/* *********************************************************************** */
extern int slurm_preempt_fini(void)
{
	int rc;

	if (!g_preempt_context)
		return SLURM_SUCCESS;

	rc = _slurm_preempt_context_destroy(g_preempt_context);
	g_preempt_context = NULL;
	return rc;
}


/* *********************************************************************** */
/*  TAG(                  slurm_find_preemptable_jobs                 )  */
/* *********************************************************************** */
extern List slurm_find_preemptable_jobs(struct job_record *job_ptr)
{
	if (slurm_preempt_init() < 0)
		return NULL;

	return (*(g_preempt_context->ops.find_jobs))(job_ptr);
}

/*
 * Return the PreemptMode which should apply to stop this job
 */
extern uint16_t slurm_job_preempt_mode(struct job_record *job_ptr)
{
	if (slurm_preempt_init() < 0)
		return (uint16_t) PREEMPT_MODE_OFF;

	return (*(g_preempt_context->ops.job_preempt_mode))(job_ptr);
}

/*
 * Return true if any jobs can be preempted, otherwise false
 */
extern bool slurm_preemption_enabled(void)
{
	if (slurm_preempt_init() < 0)
		return false;

	return (*(g_preempt_context->ops.preemption_enabled))();
}

/*
 * Return true if the preemptor can preempt the preemptee, otherwise false
 */
extern bool slurm_job_preempt_check(job_queue_rec_t *preemptor,
				    job_queue_rec_t *preemptee)
{
	if (slurm_preempt_init() < 0)
		return false;

	return (*(g_preempt_context->ops.job_preempt_check))
		(preemptor, preemptee);
}
