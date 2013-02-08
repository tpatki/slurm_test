/*
 * Fork apbasil process as co-process, parse output.
 *
 * Copyright (c) 2009-2011 Centro Svizzero di Calcolo Scientifico (CSCS)
 * Portions Copyright (C) 2011 SchedMD <http://www.schedmd.com>.
 * Licensed under the GPLv2.
 */
#include "parser_internal.h"


static void _rsvn_write_reserve_xml(FILE *fp, struct basil_reservation *r)
{
	struct basil_rsvn_param *param;

	fprintf(fp, " <ReserveParamArray user_name=\"%s\"", r->user_name);
	if (*r->batch_id != '\0')
		fprintf(fp, " batch_id=\"%s\"", r->batch_id);
	if (*r->account_name != '\0')
		fprintf(fp, " account_name=\"%s\"", r->account_name);
	fprintf(fp, ">\n");

	for (param = r->params; param; param = param->next) {
		fprintf(fp, "  <ReserveParam architecture=\"%s\" "
			"width=\"%ld\" depth=\"%ld\" nppn=\"%ld\"",
			nam_arch[param->arch],
			param->width, param->depth, param->nppn);

		if (param->memory || param->labels ||
		    param->nodes  || param->accel) {
			fprintf(fp, ">\n");
		} else {
			fprintf(fp, "/>\n");
			continue;
		}

		if (param->memory) {
			struct basil_memory_param  *mem;

			fprintf(fp, "   <MemoryParamArray>\n");
			for (mem = param->memory; mem; mem = mem->next)
				fprintf(fp, "    <MemoryParam type=\"%s\""
					" size_mb=\"%u\"/>\n",
					nam_memtype[mem->type],
					mem->size_mb ? : 1);
			fprintf(fp, "   </MemoryParamArray>\n");
		}

		if (param->labels) {
			struct basil_label *label;

			fprintf(fp, "   <LabelParamArray>\n");
			for (label = param->labels; label; label = label->next)
				fprintf(fp, "    <LabelParam name=\"%s\""
					" type=\"%s\" disposition=\"%s\"/>\n",
					label->name, nam_labeltype[label->type],
					nam_ldisp[label->disp]);

			fprintf(fp, "   </LabelParamArray>\n");
		}

		if (param->nodes && *param->nodes) {
			/*
			 * The NodeParamArray is declared within ReserveParam.
			 * If the list is spread out over multiple NodeParam
			 * elements, an
			 *   "at least one command's user NID list is short"
			 * error results. Hence more than 1 NodeParam element
			 * is probably only meant to be used when suggesting
			 * alternative node lists to ALPS. This was confirmed
			 * by repeating an identical same NodeParam 20 times,
			 * which had the same effect as supplying it once.
			 * Hence the array expression is actually not needed.
			 */
			fprintf(fp, "   <NodeParamArray>\n"
				    "    <NodeParam>%s</NodeParam>\n"
				    "   </NodeParamArray>\n", param->nodes);
		}

		if (param->accel) {
			struct basil_accel_param *accel;

			fprintf(fp, "   <AccelParamArray>\n");
			for (accel = param->accel; accel; accel = accel->next) {
				fprintf(fp, "    <AccelParam type=\"%s\"",
					nam_acceltype[accel->type]);

				if (accel->memory_mb)
					fprintf(fp, " memory_mb=\"%u\"",
						accel->memory_mb);
				fprintf(fp, "/>\n");
			}
			fprintf(fp, "   </AccelParamArray>\n");
		}

		fprintf(fp, "  </ReserveParam>\n");
	}
	fprintf(fp, " </ReserveParamArray>\n"
		    "</BasilRequest>\n");
}

/*
 * basil_request - issue BASIL request and parse response
 * @bp:	method-dependent parse data to guide the parsing process
 *
 * Returns 0 if ok, a negative %basil_error otherwise.
 */
int basil_request(struct basil_parse_data *bp)
{
	int to_child, from_child;
	int ec, rc = -BE_UNKNOWN;
	FILE *apbasil;
	pid_t pid;

	if (!cray_conf->apbasil) {
		error("No alps client defined");
		return 0;
	}
	assert(bp->version < BV_MAX);
	assert(bp->method > BM_none && bp->method < BM_MAX);

	pid = popen2(cray_conf->apbasil, &to_child, &from_child, true);
	if (pid < 0)
		fatal("popen2(\"%s\", ...)", cray_conf->apbasil);

	/* write out request */
	apbasil = fdopen(to_child, "w");
	if (apbasil == NULL)
		fatal("fdopen(): %s", strerror(errno));
	setlinebuf(apbasil);

	fprintf(apbasil, "<?xml version=\"1.0\"?>\n"
		"<BasilRequest protocol=\"%s\" method=\"%s\" ",
		bv_names[bp->version], bm_names[bp->method]);

	switch (bp->method) {
	case BM_engine:
		fprintf(apbasil, "type=\"ENGINE\"/>");
		break;
	case BM_inventory:
		fprintf(apbasil, "type=\"INVENTORY\"/>");
		break;
	case BM_reserve:
		fprintf(apbasil, ">\n");
		_rsvn_write_reserve_xml(apbasil, bp->mdata.res);
		break;
	case BM_confirm:
		if (bp->version == BV_1_0 && *bp->mdata.res->batch_id != '\0')
			fprintf(apbasil, "job_name=\"%s\" ",
				bp->mdata.res->batch_id);
		fprintf(apbasil, "reservation_id=\"%u\" %s=\"%llu\"/>\n",
			bp->mdata.res->rsvn_id,
			bp->version >= BV_3_1 ? "pagg_id" : "admin_cookie",
			(unsigned long long)bp->mdata.res->pagg_id);
		break;
	case BM_release:
		fprintf(apbasil, "reservation_id=\"%u\"/>\n",
			bp->mdata.res->rsvn_id);
		break;
	case BM_switch:
	{
		char *suspend = bp->mdata.res->suspended ? "OUT" : "IN";
		fprintf(apbasil, ">\n");
		fprintf(apbasil, " <ReservationArray>\n");
		fprintf(apbasil, "  <Reservation reservation_id=\"%u\" "
			"action=\"%s\"/>\n",
			bp->mdata.res->rsvn_id, suspend);
		fprintf(apbasil, " </ReservationArray>\n");
		fprintf(apbasil, "</BasilRequest>\n");
	}
		break;
	default: /* ignore BM_none, BM_MAX, and BM_UNKNOWN covered above */
		break;
	}

	if (fclose(apbasil) < 0)	/* also closes to_child */
		error("fclose(apbasil): %s", strerror(errno));

	rc = parse_basil(bp, from_child);
	ec = wait_for_child(pid);
	if (ec)
		error("%s child process for BASIL %s method exited with %d",
		      cray_conf->apbasil, bm_names[bp->method], ec);
	return rc;
}
