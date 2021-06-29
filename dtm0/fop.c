/* -*- C -*- */
/*
 * Copyright (c) 2021 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */


#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_DTM0
#include "lib/trace.h"
#include "addb2/addb2.h"
#include "be/dtm0_log.h"
#include "dtm0/addb2.h"
#include "dtm0/fop.h"
#include "dtm0/fop_xc.h"
#include "dtm0/service.h"
#include "dtm0/tx_desc.h"
#include "dtm0/tx_desc_xc.h"
#include "lib/errno.h"
#include "lib/assert.h"
#include "lib/memory.h"
#include "lib/chan.h"
#include "lib/finject.h"
#include "lib/time.h"
#include "lib/misc.h"           /* M0_IN() */
#include "fop/fop.h"
#include "fop/fom.h"
#include "fop/fom_generic.h"
#include "fop/fop_item_type.h"
#include "rpc/rpc_opcodes.h"
#include "rpc/rpc.h"
#include "rpc/rpclib.h"

static void dtm0_rpc_item_reply_cb(struct m0_rpc_item *item);

/*
  RPC item operations structures.
 */
const struct m0_rpc_item_ops dtm0_req_fop_rpc_item_ops = {
        .rio_replied = dtm0_rpc_item_reply_cb,
};

struct m0_fop_type dtm0_req_fop_fopt;
struct m0_fop_type dtm0_rep_fop_fopt;

/*
  Fom specific routines for corresponding fops.
 */
static int dtm0_emsg_fom_tick(struct m0_fom *fom);
static int dtm0_pmsg_fom_tick(struct m0_fom *fom);
static int dtm0_fom_create(struct m0_fop *fop, struct m0_fom **out,
			       struct m0_reqh *reqh);
static void dtm0_fom_fini(struct m0_fom *fom);
static size_t dtm0_fom_locality(const struct m0_fom *fom);

static const struct m0_fom_ops dtm0_pmsg_fom_ops = {
	.fo_fini = dtm0_fom_fini,
	.fo_tick = dtm0_pmsg_fom_tick,
	.fo_home_locality = dtm0_fom_locality
};

static const struct m0_fom_ops dtm0_emsg_fom_ops = {
	.fo_fini = dtm0_fom_fini,
	.fo_tick = dtm0_emsg_fom_tick,
	.fo_home_locality = dtm0_fom_locality
};

extern struct m0_reqh_service_type dtm0_service_type;

static const struct m0_fom_type_ops dtm0_req_fom_type_ops = {
        .fto_create = dtm0_fom_create,
};

static void dtm0_rpc_item_reply_cb(struct m0_rpc_item *item)
{
	struct m0_fop *reply = NULL;

	M0_ENTRY("item=%p", item);

	M0_PRE(item != NULL);
	M0_PRE(M0_IN(m0_fop_opcode(m0_rpc_item_to_fop(item)),
		     (M0_DTM0_REQ_OPCODE)));

	if (m0_rpc_item_error(item) == 0) {
		reply = m0_rpc_item_to_fop(item->ri_reply);
		M0_ASSERT(M0_IN(m0_fop_opcode(reply), (M0_DTM0_REP_OPCODE)));
	}

	M0_LEAVE("reply=%p", reply);
}

M0_INTERNAL void m0_dtm0_fop_fini(void)
{
	m0_fop_type_fini(&dtm0_req_fop_fopt);
	m0_fop_type_fini(&dtm0_rep_fop_fopt);
	m0_xc_dtm0_fop_fini();
}

enum {
	M0_FOPH_DTM0_LOGGING = M0_FOPH_TYPE_SPECIFIC,
};

struct m0_sm_state_descr dtm0_phases[] = {
	[M0_FOPH_DTM0_LOGGING] = {
		.sd_name      = "logging",
		.sd_allowed   = M0_BITS(M0_FOPH_SUCCESS,
					M0_FOPH_FAILURE)
	},
};

struct m0_sm_trans_descr dtm0_phases_trans[] = {
	[ARRAY_SIZE(m0_generic_phases_trans)] =
	{"dtm0_1-fail", M0_FOPH_TYPE_SPECIFIC, M0_FOPH_FAILURE},
	{"dtm0_1-success", M0_FOPH_TYPE_SPECIFIC, M0_FOPH_SUCCESS},
};

static struct m0_sm_conf dtm0_conf = {
	.scf_name      = "dtm0-fom",
	.scf_nr_states = ARRAY_SIZE(dtm0_phases),
	.scf_state     = dtm0_phases,
	.scf_trans_nr  = ARRAY_SIZE(dtm0_phases_trans),
	.scf_trans     = dtm0_phases_trans,
};

M0_INTERNAL int m0_dtm0_fop_init(void)
{
	static int init_once = 0;

	if (init_once++ > 0)
		return 0;

	m0_sm_conf_extend(m0_generic_conf.scf_state, dtm0_phases,
			  m0_generic_conf.scf_nr_states);
	m0_sm_conf_trans_extend(&m0_generic_conf, &dtm0_conf);
	m0_sm_conf_init(&dtm0_conf);


	m0_xc_dtm0_fop_init();
	M0_FOP_TYPE_INIT(&dtm0_req_fop_fopt,
			 .name      = "DTM0 request",
			 .opcode    = M0_DTM0_REQ_OPCODE,
			 .xt        = dtm0_req_fop_xc,
			 .rpc_flags = M0_RPC_MUTABO_REQ,
			 .fom_ops   = &dtm0_req_fom_type_ops,
			 .sm        = &dtm0_conf,
			 .svc_type  = &dtm0_service_type);
	M0_FOP_TYPE_INIT(&dtm0_rep_fop_fopt,
			 .name      = "DTM0 reply",
			 .opcode    = M0_DTM0_REP_OPCODE,
			 .xt        = dtm0_rep_fop_xc,
			 .rpc_flags = M0_RPC_ITEM_TYPE_REPLY,
			 .fom_ops   = &dtm0_req_fom_type_ops);
	return m0_fop_type_addb2_instrument(&dtm0_req_fop_fopt);
}


/*
  Allocates a fom.
 */
static int dtm0_fom_create(struct m0_fop *fop,
			   struct m0_fom **out,
			   struct m0_reqh *reqh)
{
	struct dtm0_fom         *fom;
	struct m0_fop           *repfop;
	struct dtm0_rep_fop     *reply;
	struct dtm0_req_fop     *req;
	struct m0_dtm0_pmsg_ast *pma;
	int                      rc;

	M0_ENTRY("reqh=%p", reqh);

	M0_ALLOC_PTR(fom);
	M0_ALLOC_PTR(pma);
	repfop = m0_fop_reply_alloc(fop, &dtm0_rep_fop_fopt);

	if (fom == NULL || repfop == NULL || pma == NULL) {
		m0_free(pma);
		m0_free(repfop);
		m0_free(fom);
		return M0_ERR(-ENOMEM);
	}

	*out = &fom->dtf_fom;

	/* TODO: calculate credits for the operation. */

	/* see ::m0_dtm0_dtx_post_persistent for the details */
	fop->f_opaque = pma;
	reply = m0_fop_data(repfop);
	req   = m0_fop_data(fop);
	reply->dr_txr = (struct m0_dtm0_tx_desc) {};
	reply->dr_rc = 0;

	if (req->dtr_msg == DMT_EXECUTE) {
		M0_ASSERT_INFO(m0_dtm0_in_ut(), "Emsg FOM is only for UTs.");
		rc = m0_dtm0_tx_desc_copy(&req->dtr_txr, &reply->dr_txr);
		M0_ASSERT(rc == 0);
		m0_fom_init(&fom->dtf_fom, &fop->f_type->ft_fom_type,
			    &dtm0_emsg_fom_ops, fop, repfop, reqh);
	} else if (req->dtr_msg == DTM_PERSISTENT) {
		m0_fom_init(&fom->dtf_fom, &fop->f_type->ft_fom_type,
			    &dtm0_pmsg_fom_ops, fop, repfop, reqh);
	} else
		M0_IMPOSSIBLE();

	return M0_RC_INFO(0, "fom=%p", &fom->dtf_fom);
}

static void dtm0_fom_fini(struct m0_fom *fom)
{
	M0_PRE(fom != NULL);

	m0_fom_fini(fom);
	m0_free(fom);
}

static size_t dtm0_fom_locality(const struct m0_fom *fom)
{
	static size_t locality = 0;

	M0_PRE(fom != NULL);
	return locality++;
}

static int m0_dtm0_send_msg(struct m0_fom                *fom,
			    enum m0_dtm0s_msg             msg_type,
			    const struct m0_fid          *tgt,
			    const struct m0_dtm0_tx_desc *txd)
{
	struct m0_dtm0_service *dtms;
	struct dtm0_req_fop req = {
		.dtr_msg = msg_type,
		.dtr_txr = *txd,
	};
	dtms = m0_dtm0_service_find(fom->fo_service->rs_reqh);
	return m0_dtm0_req_post(dtms, &req, tgt, fom, false);
}

M0_INTERNAL int m0_dtm0_logrec_update(struct m0_be_dtm0_log  *log,
				      struct m0_be_tx        *tx,
				      struct m0_dtm0_tx_desc *txd,
				      struct m0_buf          *payload)
{
	int rc;

	M0_ENTRY();

	m0_mutex_lock(&log->dl_lock);
	rc = m0_be_dtm0_log_update(log, tx, txd, payload);
	m0_mutex_unlock(&log->dl_lock);

	return M0_RC(rc);
}

M0_INTERNAL int m0_dtm0_on_committed(struct m0_fom            *fom,
				     const struct m0_dtm0_tid *id)
{
	struct m0_dtm0_service *dtms = m0_dtm0_service_find(
					fom->fo_service->rs_reqh);
	struct m0_be_dtm0_log  *log = dtms->dos_log;
	struct m0_dtm0_log_rec *rec;
	struct m0_dtm0_tx_desc  txd;
	struct m0_fid          *target;
	struct m0_fid          *source = &dtms->dos_generic.rs_service_fid;
	int                     rc;
	int                     i;

	M0_PRE(log != NULL);
	M0_PRE(log->dl_is_persistent);

	M0_ENTRY();

	m0_mutex_lock(&log->dl_lock);
	/* Get the latest state of the log record. */
	rec = m0_be_dtm0_log_find(log, id);
	M0_ASSERT_INFO(rec != NULL, "Log record must be inserted into the log "
		       "in cas_fom_tick().");
	rc = m0_dtm0_tx_desc_copy(&rec->dlr_txd, &txd);
	m0_mutex_unlock(&log->dl_lock);

	if (rc != 0)
		goto out;

	for (i = 0; i < txd.dtd_ps.dtp_nr; ++i) {
		target = &txd.dtd_ps.dtp_pa[i].p_fid;

		/* Send it to the originator when we encounter ourselves. */
		if (m0_fid_eq(target, source))
			target = &txd.dtd_id.dti_fid;

		rc = m0_dtm0_send_msg(fom, DTM_PERSISTENT, target, &txd);
		if (rc != 0) {
			M0_ERR_INFO(rc, "Failed to send PERSISTENT msg "
				    FID_F " -> " FID_F ".",
				    FID_P(source), FID_P(target));
			/*
			 * Pmsg should not cause failures of the CAS/IOS FOM
			 * that caused it.
			 */
			rc = 0;
		}
	}

	m0_dtm0_tx_desc_fini(&txd);

out:
	return M0_RC(rc);
}

static int dtm0_pmsg_fom_tick(struct m0_fom *fom)
{
	int                       result = M0_FSO_AGAIN;
	struct   m0_dtm0_service *svc;
	struct   m0_buf           buf = {};
	struct   dtm0_rep_fop    *rep;
	struct   dtm0_req_fop    *req = m0_fop_data(fom->fo_fop);
	int                       phase = m0_fom_phase(fom);
	struct   m0_be_tx_credit  cred = {};

	M0_PRE(req->dtr_msg == DTM_PERSISTENT);
	M0_ENTRY("fom %p phase %d", fom, phase);

	if (m0_fom_phase(fom) < M0_FOPH_NR) {
		result = m0_fom_tick_generic(fom);
		if (m0_dtm0_is_a_persistent_dtm(fom->fo_service) &&
		    m0_fom_phase(fom) == M0_FOPH_TXN_OPEN) {
			M0_ASSERT(phase == M0_FOPH_TXN_INIT);
			m0_be_dtm0_log_credit(M0_DTML_PERSISTENT,
					      &req->dtr_txr, &buf,
					      m0_fom_reqh(fom)->rh_beseg,
					      NULL, &cred);
			m0_be_tx_credit_add(&fom->fo_tx.tx_betx_cred, &cred);
		}
	} else {
		M0_ASSERT(m0_fom_phase(fom) == M0_FOPH_DTM0_LOGGING);
		rep = m0_fop_data(fom->fo_rep_fop);
		svc = m0_dtm0_fom2service(fom);
		M0_ASSERT(svc != NULL);

		M0_LOG(M0_DEBUG, "Logging a P msg in phase %" PRIu32
		       " %d, " FID_F ", %p", req->dtr_msg,
		       !!m0_dtm0_is_a_volatile_dtm(fom->fo_service),
		       FID_P(&fom->fo_service->rs_service_fid),
		       fom->fo_service->rs_reqh);

		if (m0_dtm0_is_a_volatile_dtm(fom->fo_service)) {
			/*
			 * On the client side, DTX is the owner of the log,
			 * so that it cannot be modifed right here. We have to
			 * post an AST to ensure DTX is modifed
			 * under the group lock held.
			 */
			m0_be_dtm0_log_pmsg_post(svc->dos_log, fom->fo_fop);
			rep->dr_rc = 0;
		} else {
			rep->dr_rc = m0_dtm0_logrec_update(svc->dos_log,
							   &fom->fo_tx.tx_betx,
							   &req->dtr_txr, &buf);
		}

		/* We do not handle any failures of Pmsg processing. */
		m0_fom_phase_set(fom, M0_FOPH_SUCCESS);
		result = M0_FSO_AGAIN;
	}

	return M0_RC(result);
}

static int dtm0_emsg_fom_tick(struct m0_fom *fom)
{
	int                       rc = 0;
	int                       result = M0_FSO_AGAIN;
	struct   dtm0_rep_fop    *rep = m0_fop_data(fom->fo_rep_fop);
	struct   dtm0_req_fop    *req = m0_fop_data(fom->fo_fop);
	int                       phase = m0_fom_phase(fom);

	M0_PRE(req->dtr_msg == DMT_EXECUTE);
	M0_ASSERT_INFO(m0_dtm0_in_ut(), "Emsg cannot be used outside of UT.");

	M0_ENTRY("fom %p phase %d", fom, phase);

	if (m0_fom_phase(fom) < M0_FOPH_NR) {
		result = m0_fom_tick_generic(fom);
	} else {
		M0_ASSERT(m0_fom_phase(fom) == M0_FOPH_DTM0_LOGGING);

		if (m0_dtm0_is_a_persistent_dtm(fom->fo_service))
			rc = m0_dtm0_send_msg(fom, DMT_EXECUTED,
					      &req->dtr_txr.dtd_id.dti_fid,
					      &req->dtr_txr);

		rep->dr_rc = rc;
		m0_fom_phase_set(fom, M0_FOPH_SUCCESS);
		result = M0_FSO_AGAIN;
	}

	return M0_RC(result);
}

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
