/**
 * (C) Copyright 2016 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/**
 * object shard operations.
 */
#define DD_SUBSYS	DD_FAC(object)

#include <daos/container.h>
#include <daos/pool.h>
#include <daos/pool_map.h>
#include <daos/rpc.h>
#include "obj_rpc.h"
#include "obj_internal.h"

static void
obj_shard_free(struct dc_obj_shard *shard)
{
	D_FREE_PTR(shard);
}

static struct dc_obj_shard *
obj_shard_alloc(daos_rank_t rank, daos_unit_oid_t id, uint32_t part_nr)
{
	struct dc_obj_shard *shard;

	D_ALLOC_PTR(shard);
	if (shard == NULL)
		return NULL;

	shard->do_rank	  = rank;
	shard->do_part_nr = part_nr;
	shard->do_id	  = id;
	DAOS_INIT_LIST_HEAD(&shard->do_co_list);

	return shard;
}

void
obj_shard_decref(struct dc_obj_shard *shard)
{
	D_ASSERT(shard->do_ref > 0);
	shard->do_ref--;
	if (shard->do_ref == 0)
		obj_shard_free(shard);
}

static void
obj_shard_addref(struct dc_obj_shard *shard)
{
	shard->do_ref++;
}

static void
obj_shard_hdl_link(struct dc_obj_shard *shard)
{
	obj_shard_addref(shard);
}

static void
obj_shard_hdl_unlink(struct dc_obj_shard *shard)
{
	obj_shard_decref(shard);
}

struct dc_obj_shard*
obj_shard_hdl2ptr(daos_handle_t hdl)
{
	struct dc_obj_shard *shard;

	if (hdl.cookie == 0)
		return NULL;

	shard = (struct dc_obj_shard *)hdl.cookie;
	obj_shard_addref(shard);
	return shard;
}

static daos_handle_t
obj_shard_ptr2hdl(struct dc_obj_shard *shard)
{
	daos_handle_t oh;

	oh.cookie = (uint64_t)shard;
	return oh;
}

int
dc_obj_shard_open(daos_handle_t coh, uint32_t tgt, daos_unit_oid_t id,
		  unsigned int mode, daos_handle_t *oh)
{
	struct dc_obj_shard	*dobj;
	struct pool_target	*map_tgt;
	int			rc;

	rc = dc_cont_tgt_idx2ptr(coh, tgt, &map_tgt);
	if (rc != 0)
		return rc;

	dobj = obj_shard_alloc(map_tgt->ta_comp.co_rank, id,
			       map_tgt->ta_comp.co_nr);
	if (dobj == NULL)
		return -DER_NOMEM;

	dobj->do_co_hdl = coh;
	obj_shard_hdl_link(dobj);
	*oh = obj_shard_ptr2hdl(dobj);

	return 0;
}

int
dc_obj_shard_close(daos_handle_t oh)
{
	struct dc_obj_shard *dobj;

	dobj = obj_shard_hdl2ptr(oh);
	if (dobj == NULL)
		return -DER_NO_HDL;

	obj_shard_hdl_unlink(dobj);
	obj_shard_decref(dobj);
	return 0;
}

static void
obj_shard_rw_bulk_fini(crt_rpc_t *rpc)
{
	struct obj_rw_in	*orw;
	crt_bulk_t		*bulks;
	unsigned int		nr;
	int			i;

	orw = crt_req_get(rpc);
	bulks = orw->orw_bulks.da_arrays;
	if (bulks == NULL)
		return;

	nr = orw->orw_bulks.da_count;
	for (i = 0; i < nr; i++)
		crt_bulk_free(bulks[i]);

	D_FREE(bulks, nr * sizeof(crt_bulk_t));
	orw->orw_bulks.da_arrays = NULL;
	orw->orw_bulks.da_count = 0;
}

static int
dc_obj_shard_sgl_copy(daos_sg_list_t *dst_sgl, uint32_t dst_nr,
		      daos_sg_list_t *src_sgl, uint32_t src_nr)
{
	int i;
	int j;

	if (src_nr > dst_nr) {
		D_ERROR("%u > %u\n", src_nr, dst_nr);
		return -DER_INVAL;
	}

	for (i = 0; i < src_nr; i++) {
		if (src_sgl[i].sg_nr.num == 0)
			continue;

		if (src_sgl[i].sg_nr.num > dst_sgl[i].sg_nr.num) {
			D_ERROR("%d : %u > %u\n", i,
				src_sgl[i].sg_nr.num, dst_sgl[i].sg_nr.num);
			return -DER_INVAL;
		}

		dst_sgl[i].sg_nr.num_out = src_sgl[i].sg_nr.num_out;
		for (j = 0; j < src_sgl[i].sg_nr.num_out; j++) {
			if (src_sgl[i].sg_iovs[j].iov_len == 0)
				continue;

			if (src_sgl[i].sg_iovs[j].iov_len >
			    dst_sgl[i].sg_iovs[j].iov_buf_len) {
				D_ERROR("%d:%d "DF_U64" > "DF_U64"\n",
					i, j, src_sgl[i].sg_iovs[j].iov_len,
					src_sgl[i].sg_iovs[j].iov_buf_len);
				return -DER_INVAL;
			}
			memcpy(dst_sgl[i].sg_iovs[j].iov_buf,
			       src_sgl[i].sg_iovs[j].iov_buf,
			       src_sgl[i].sg_iovs[j].iov_len);
			dst_sgl[i].sg_iovs[j].iov_len =
				src_sgl[i].sg_iovs[j].iov_len;
		}
	}
	return 0;
}

struct obj_rw_args {
	crt_rpc_t	*rpc;
	daos_handle_t	*hdlp;
	daos_sg_list_t	*rwaa_sgls;
	uint32_t	 rwaa_nr;
};

static int
dc_rw_cb(tse_task_t *task, void *arg)
{
	struct obj_rw_args     *rw_args = (struct obj_rw_args *)arg;
	struct obj_rw_in       *orw;
	int			opc;
	int                     ret = task->dt_result;
	int			rc = 0;

	opc = opc_get(rw_args->rpc->cr_opc);
	if (opc == DAOS_OBJ_RPC_FETCH &&
	    DAOS_FAIL_CHECK(DAOS_SHARD_OBJ_FETCH_TIMEOUT)) {
		D_ERROR("Inducing -DER_TIMEDOUT error on shard I/O fetch\n");
		D_GOTO(out, rc = -DER_TIMEDOUT);
	}
	if (opc == DAOS_OBJ_RPC_UPDATE &&
	    DAOS_FAIL_CHECK(DAOS_SHARD_OBJ_UPDATE_TIMEOUT)) {
		D_ERROR("Inducing -DER_TIMEDOUT error on shard I/O update\n");
		D_GOTO(out, rc = -DER_TIMEDOUT);
	}
	if (opc == DAOS_OBJ_RPC_UPDATE &&
	    DAOS_FAIL_CHECK(DAOS_OBJ_UPDATE_NOSPACE)) {
		D_ERROR("Inducing -DER_NOSPACE error on shard I/O update\n");
		D_GOTO(out, rc = -DER_NOSPACE);
	}

	orw = crt_req_get(rw_args->rpc);
	D_ASSERT(orw != NULL);
	if (ret != 0) {
		/*
		 * If any failure happens inside Cart, let's reset failure to
		 * TIMEDOUT, so the upper layer can retry.
		 */
		D_ERROR("RPC %d failed: %d\n",
			opc_get(rw_args->rpc->cr_opc), ret);
		D_GOTO(out, ret);
	}

	rc = obj_reply_get_status(rw_args->rpc);
	if (rc != 0) {
		D_ERROR("rpc %p RPC %d failed: %d\n",
			rw_args->rpc, opc_get(rw_args->rpc->cr_opc), rc);
		D_GOTO(out, rc);
	}

	if (opc_get(rw_args->rpc->cr_opc) == DAOS_OBJ_RPC_FETCH) {
		struct obj_rw_out *orwo;
		daos_iod_t	*iods;
		uint64_t	*sizes;
		int		 i;

		orwo = crt_reply_get(rw_args->rpc);
		iods = orw->orw_iods.da_arrays;
		sizes = orwo->orw_sizes.da_arrays;

		if (orwo->orw_sizes.da_count != orw->orw_nr) {
			D_ERROR("out:%u != in:%u\n",
				(unsigned)orwo->orw_sizes.da_count,
				orw->orw_nr);
			D_GOTO(out, rc = -DER_PROTO);
		}

		/* update the sizes in iods */
		for (i = 0; i < orw->orw_nr; i++)
			iods[i].iod_size = sizes[i];

		if (orwo->orw_sgls.da_count > 0) {
			/* inline transfer */
			rc = dc_obj_shard_sgl_copy(rw_args->rwaa_sgls,
						   rw_args->rwaa_nr,
						   orwo->orw_sgls.da_arrays,
						   orwo->orw_sgls.da_count);
		} else if (rw_args->rwaa_sgls != NULL) {
			/* for bulk transfer it needs to update sg_nr.num_out */
			daos_sg_list_t *sgls = rw_args->rwaa_sgls;
			uint32_t       *nrs;
			uint32_t	nrs_count;

			nrs = orwo->orw_nrs.da_arrays;
			nrs_count = orwo->orw_nrs.da_count;
			if (nrs_count != rw_args->rwaa_nr) {
				D_ERROR("Invalid nrs %u != %u\n", nrs_count,
					rw_args->rwaa_nr);
				D_GOTO(out, rc = -DER_PROTO);
			}

			/* update sgl_nr */
			for (i = 0; i < nrs_count; i++)
				sgls[i].sg_nr.num_out = nrs[i];
		}
	}
out:
	obj_shard_rw_bulk_fini(rw_args->rpc);
	crt_req_decref(rw_args->rpc);
	dc_pool_put((struct dc_pool *)rw_args->hdlp);

	if (ret == 0 || daos_obj_retry_error(rc))
		ret = rc;
	return ret;
}

static inline bool
obj_shard_io_check(unsigned int nr, daos_iod_t *iods)
{
	int i;

	for (i = 0; i < nr; i++) {
		if (iods[i].iod_name.iov_buf == NULL)
			/* XXX checksum & eprs should not be mandatory */
			return false;

		if (iods[i].iod_type == DAOS_IOD_ARRAY &&
		    iods[i].iod_recxs == NULL) {
			D_ERROR("ARRAY iod type should have valid iod_recxs\n");
			return false;
		}

		if (iods[i].iod_type == DAOS_IOD_SINGLE &&
		    iods[i].iod_nr != 1) {
			D_ERROR("SINGLE iod type iod_nr %d != 1\n",
				iods[i].iod_nr);
			return false;
		}
	}

	return true;
}

/**
 * XXX: Only use dkey to distribute the data among targets for
 * now, and eventually, it should use dkey + akey, but then
 * it means the I/O descriptor might needs to be split into
 * mulitple requests in obj_shard_rw()
 */
static uint32_t
obj_shard_dkey2tag(struct dc_obj_shard *dobj, daos_key_t *dkey)
{
	uint64_t hash;

	/** XXX hash is calculated twice (see cli_obj_dkey2shard) */
	hash = daos_hash_murmur64((unsigned char *)dkey->iov_buf,
				  dkey->iov_len, 5731);
	hash %= dobj->do_part_nr;

	return hash;
}

static uint64_t
iods_data_len(daos_iod_t *iods, int nr)
{
	uint64_t iod_length = 0;
	int	 i;

	for (i = 0; i < nr; i++) {
		uint64_t len = daos_iod_len(&iods[i]);

		if (len == -1) /* unknown */
			return 0;

		iod_length += len;
	}
	return iod_length;
}

static daos_size_t
sgls_buf_len(daos_sg_list_t *sgls, int nr)
{
	daos_size_t sgls_len = 0;
	int	    i;

	if (sgls == NULL)
		return 0;

	/* create bulk transfer for daos_sg_list */
	for (i = 0; i < nr; i++)
		sgls_len += daos_sgl_buf_len(&sgls[i]);

	return sgls_len;
}

static int
obj_shard_rw_bulk_prep(crt_rpc_t *rpc, unsigned int nr, daos_sg_list_t *sgls,
		       tse_task_t *task)
{
	struct obj_rw_in	*orw;
	crt_bulk_t		*bulks;
	crt_bulk_perm_t		 bulk_perm;
	int			 i;
	int			 rc = 0;

	bulk_perm = (opc_get(rpc->cr_opc) == DAOS_OBJ_RPC_UPDATE) ?
		    CRT_BULK_RO : CRT_BULK_RW;
	D_ALLOC(bulks, nr * sizeof(*bulks));
	if (bulks == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	/* create bulk transfer for daos_sg_list */
	for (i = 0; i < nr; i++) {
		if (sgls != NULL && sgls[i].sg_iovs != NULL &&
		    sgls[i].sg_iovs[0].iov_buf != NULL) {
			rc = crt_bulk_create(daos_task2ctx(task),
					     daos2crt_sg(&sgls[i]),
					     bulk_perm, &bulks[i]);
			if (rc < 0) {
				int j;

				for (j = 0; j < i; j++)
					crt_bulk_free(bulks[j]);

				D_GOTO(out, rc);
			}
		}
	}

	orw = crt_req_get(rpc);
	D_ASSERT(orw != NULL);
	orw->orw_bulks.da_count = nr;
	orw->orw_bulks.da_arrays = bulks;
out:
	if (rc != 0 && bulks != NULL)
		D_FREE(bulks, nr * sizeof(*bulks));

	return rc;
}

static struct dc_pool *
obj_shard_ptr2pool(struct dc_obj_shard *shard)
{
	daos_handle_t poh;

	poh = dc_cont_hdl2pool_hdl(shard->do_co_hdl);
	if (daos_handle_is_inval(poh))
		return NULL;

	return dc_hdl2pool(poh);
}

static int
obj_shard_rw(daos_handle_t oh, enum obj_rpc_opc opc, daos_epoch_t epoch,
	     daos_key_t *dkey, unsigned int nr, daos_iod_t *iods,
	     daos_sg_list_t *sgls, unsigned int map_ver, tse_task_t *task)
{
	struct dc_obj_shard    *dobj;
	struct dc_pool	       *pool;
	crt_rpc_t	       *req;
	struct obj_rw_in       *orw;
	struct obj_rw_args	rw_args;
	crt_endpoint_t		tgt_ep;
	uuid_t			cont_hdl_uuid;
	uuid_t			cont_uuid;
	daos_size_t		total_len;
	int			rc;

	/** sanity check input parameters */
	if (dkey == NULL || dkey->iov_buf == NULL || nr == 0 ||
	    !obj_shard_io_check(nr, iods))
		D_GOTO(out_task, rc = -DER_INVAL);

	dobj = obj_shard_hdl2ptr(oh);
	if (dobj == NULL)
		D_GOTO(out_task, rc = -DER_NO_HDL);

	rc = dc_cont_hdl2uuid(dobj->do_co_hdl, &cont_hdl_uuid, &cont_uuid);
	if (rc != 0) {
		obj_shard_decref(dobj);
		D_GOTO(out_task, rc);
	}

	pool = obj_shard_ptr2pool(dobj);
	if (pool == NULL) {
		obj_shard_decref(dobj);
		D_GOTO(out_task, rc);
	}

	tgt_ep.ep_grp = pool->dp_group;
	tgt_ep.ep_rank = dobj->do_rank;
	tgt_ep.ep_tag = obj_shard_dkey2tag(dobj, dkey);

	D_DEBUG(DB_TRACE, "opc %d %.*s rank %d tag %d\n",
		opc, (int)dkey->iov_len, (char *)dkey->iov_buf,
		tgt_ep.ep_rank, tgt_ep.ep_tag);
	rc = obj_req_create(daos_task2ctx(task), &tgt_ep, opc, &req);
	if (rc != 0) {
		obj_shard_decref(dobj);
		D_GOTO(out_pool, rc);
	}

	orw = crt_req_get(req);
	D_ASSERT(orw != NULL);

	orw->orw_map_ver = map_ver;
	orw->orw_oid = dobj->do_id;
	uuid_copy(orw->orw_co_hdl, cont_hdl_uuid);
	uuid_copy(orw->orw_co_uuid, cont_uuid);

	obj_shard_decref(dobj);
	dobj = NULL;

	orw->orw_epoch = epoch;
	orw->orw_nr = nr;
	/** FIXME: large dkey should be transferred via bulk */
	orw->orw_dkey = *dkey;

	/* FIXME: if iods is too long, then we needs to do bulk transfer
	 * as well, but then we also needs to serialize the iods
	 **/
	orw->orw_iods.da_count = nr;
	orw->orw_iods.da_arrays = iods;

	total_len = iods_data_len(iods, nr);
	/* If it is read, let's try to get the size from sg list */
	if (total_len == 0 && opc == DAOS_OBJ_RPC_FETCH)
		total_len = sgls_buf_len(sgls, nr);

	if (DAOS_FAIL_CHECK(DAOS_SHARD_OBJ_FAIL))
		D_GOTO(out_req, rc = -DER_INVAL);

	if (total_len >= OBJ_BULK_LIMIT) {
		/* Transfer data by bulk */
		rc = obj_shard_rw_bulk_prep(req, nr, sgls, task);
		if (rc != 0)
			D_GOTO(out_req, rc);
		orw->orw_sgls.da_count = 0;
		orw->orw_sgls.da_arrays = NULL;
	} else {
		/* Transfer data inline */
		if (sgls != NULL)
			orw->orw_sgls.da_count = nr;
		else
			orw->orw_sgls.da_count = 0;
		orw->orw_sgls.da_arrays = sgls;
		orw->orw_bulks.da_count = 0;
		orw->orw_bulks.da_arrays = NULL;
	}

	crt_req_addref(req);
	rw_args.rpc = req;
	rw_args.hdlp = (daos_handle_t *)pool;

	if (opc == DAOS_OBJ_RPC_FETCH) {
		/* remember the sgl to copyout the data inline for fetch */
		rw_args.rwaa_nr = nr;
		rw_args.rwaa_sgls = sgls;
	} else {
		rw_args.rwaa_nr = 0;
		rw_args.rwaa_sgls = NULL;
	}

	if (DAOS_FAIL_CHECK(DAOS_SHARD_OBJ_RW_CRT_ERROR))
		D_GOTO(out_args, rc = -DER_CRT_HG);

	rc = tse_task_register_comp_cb(task, dc_rw_cb, &rw_args,
				       sizeof(rw_args));
	if (rc != 0)
		D_GOTO(out_args, rc);

	rc = daos_rpc_send(req, task);
	if (rc != 0) {
		D_ERROR("update/fetch rpc failed rc %d\n", rc);
		D_GOTO(out_args, rc);
	}

	return rc;

out_args:
	crt_req_decref(req);
	if (total_len >= OBJ_BULK_LIMIT)
		obj_shard_rw_bulk_fini(req);
out_req:
	crt_req_decref(req);
out_pool:
	dc_pool_put(pool);
out_task:
	tse_task_complete(task, rc);
	return rc;
}

int
dc_obj_shard_update(daos_handle_t oh, daos_epoch_t epoch,
		    daos_key_t *dkey, unsigned int nr,
		    daos_iod_t *iods, daos_sg_list_t *sgls,
		    unsigned int map_ver, tse_task_t *task)
{
	return obj_shard_rw(oh, DAOS_OBJ_RPC_UPDATE, epoch, dkey, nr, iods,
			    sgls, map_ver, task);
}

int
dc_obj_shard_fetch(daos_handle_t oh, daos_epoch_t epoch,
		   daos_key_t *dkey, unsigned int nr,
		   daos_iod_t *iods, daos_sg_list_t *sgls,
		   daos_iom_t *maps, unsigned int map_ver,
		   tse_task_t *task)
{
	return obj_shard_rw(oh, DAOS_OBJ_RPC_FETCH, epoch, dkey, nr, iods,
			    sgls, map_ver, task);
}

struct obj_enum_args {
	crt_rpc_t		*rpc;
	daos_handle_t		*hdlp;
	uint32_t		*eaa_nr;
	daos_key_desc_t		*eaa_kds;
	daos_hash_out_t		*eaa_anchor;
	struct dc_obj_shard	*eaa_obj;
	daos_sg_list_t		*eaa_sgl;
	daos_epoch_range_t	*eaa_eprs;
	daos_recx_t		*eaa_recxs;
	daos_size_t		*eaa_size;
	uuid_t			*eaa_cookies;
	uint32_t		*eaa_versions;
};

static int
dc_enumerate_cb(tse_task_t *task, void *arg)
{
	struct obj_enum_args	*enum_args = (struct obj_enum_args *)arg;
	struct obj_key_enum_in	*oei;
	struct obj_key_enum_out	*oeo;
	int			 tgt_tag;
	int			 ret = task->dt_result;
	int			 rc = 0;

	oei = crt_req_get(enum_args->rpc);
	D_ASSERT(oei != NULL);

	if (ret != 0) {
		/* If any failure happens inside Cart, let's reset
		 * failure to TIMEDOUT, so the upper layer can retry
		 **/
		D_ERROR("RPC %d failed: %d\n",
			opc_get(enum_args->rpc->cr_opc), ret);
		D_GOTO(out, ret);
	}

	oeo = crt_reply_get(enum_args->rpc);
	if (oeo->oeo_ret < 0)
		D_GOTO(out, rc = oeo->oeo_ret);

	if (*enum_args->eaa_nr < oeo->oeo_kds.da_count) {
		D_ERROR("DAOS_OBJ_RPC_ENUMERATE return more kds, rc: %d\n",
			-DER_PROTO);
		D_GOTO(out, rc = -DER_PROTO);
	}

	if (opc_get(enum_args->rpc->cr_opc) == DAOS_OBJ_RECX_RPC_ENUMERATE) {
		*(enum_args->eaa_nr) = oeo->oeo_eprs.da_count;
		if (enum_args->eaa_eprs && oeo->oeo_eprs.da_count > 0)
			memcpy(enum_args->eaa_eprs, oeo->oeo_eprs.da_arrays,
			       sizeof(*enum_args->eaa_eprs) *
			       oeo->oeo_eprs.da_count);
		if (enum_args->eaa_recxs && oeo->oeo_recxs.da_count > 0)
			memcpy(enum_args->eaa_recxs, oeo->oeo_recxs.da_arrays,
			       sizeof(*enum_args->eaa_recxs) *
			       oeo->oeo_recxs.da_count);
		*enum_args->eaa_size = oeo->oeo_size;
		if (enum_args->eaa_cookies && oeo->oeo_cookies.da_count > 0) {
			uuid_t *cookies = oeo->oeo_cookies.da_arrays;
			int i;

			for (i = 0; i < oeo->oeo_cookies.da_count; i++)
				uuid_copy(enum_args->eaa_cookies[i],
					  cookies[i]);
		}
		if (enum_args->eaa_versions && oeo->oeo_vers.da_count > 0)
			memcpy(enum_args->eaa_versions, oeo->oeo_vers.da_arrays,
			       sizeof(*enum_args->eaa_versions) *
			       oeo->oeo_vers.da_count);
	} else {
		*(enum_args->eaa_nr) = oeo->oeo_kds.da_count;
		if (enum_args->eaa_kds && oeo->oeo_kds.da_count > 0)
			memcpy(enum_args->eaa_kds, oeo->oeo_kds.da_arrays,
			       sizeof(*enum_args->eaa_kds) *
			       oeo->oeo_kds.da_count);
	}

	/* Update hkey hash and tag */
	enum_anchor_copy_hkey(enum_args->eaa_anchor, &oeo->oeo_anchor);
	tgt_tag = enum_anchor_get_tag(&oeo->oeo_anchor);
	enum_anchor_set_tag(enum_args->eaa_anchor, tgt_tag);

	if (oeo->oeo_sgl.sg_nr.num > 0 && oeo->oeo_sgl.sg_iovs != NULL)
		rc = dc_obj_shard_sgl_copy(enum_args->eaa_sgl, 1, &oeo->oeo_sgl,
					   1);

out:
	if (enum_args->eaa_obj != NULL)
		obj_shard_decref(enum_args->eaa_obj);

	if (oei->oei_bulk != NULL)
		crt_bulk_free(oei->oei_bulk);

	crt_req_decref(enum_args->rpc);
	dc_pool_put((struct dc_pool *)enum_args->hdlp);

	if (ret == 0 || daos_obj_retry_error(rc))
		ret = rc;
	return ret;
}

int
dc_obj_shard_list_internal(daos_handle_t oh, enum obj_rpc_opc opc,
			   daos_epoch_t epoch, daos_key_t *dkey,
			   daos_key_t *akey, daos_iod_type_t type,
			   daos_size_t *size, uint32_t *nr,
			   daos_key_desc_t *kds, daos_sg_list_t *sgl,
			   daos_recx_t *recxs, daos_epoch_range_t *eprs,
			   uuid_t *cookies, uint32_t *versions,
			   daos_hash_out_t *anchor, unsigned int map_ver,
			   tse_task_t *task)
{
	crt_endpoint_t		tgt_ep;
	struct dc_pool	       *pool;
	crt_rpc_t	       *req;
	struct dc_obj_shard    *dobj;
	uuid_t			cont_hdl_uuid;
	uuid_t			cont_uuid;
	struct obj_key_enum_in	*oei;
	struct obj_enum_args	enum_args;
	daos_size_t		sgl_len = 0;
	int			rc;

	dobj = obj_shard_hdl2ptr(oh);
	if (dobj == NULL)
		D_GOTO(out_task, rc = -DER_NO_HDL);

	rc = dc_cont_hdl2uuid(dobj->do_co_hdl, &cont_hdl_uuid, &cont_uuid);
	if (rc != 0)
		D_GOTO(out_put, rc);

	pool = obj_shard_ptr2pool(dobj);
	if (pool == NULL)
		D_GOTO(out_put, rc);

	tgt_ep.ep_grp = pool->dp_group;
	tgt_ep.ep_rank = dobj->do_rank;
	if (opc == DAOS_OBJ_DKEY_RPC_ENUMERATE) {
		tgt_ep.ep_tag = enum_anchor_get_tag(anchor);
	} else {
		D_ASSERT(dkey != NULL);
		tgt_ep.ep_tag = obj_shard_dkey2tag(dobj, dkey);
	}

	D_DEBUG(DB_TRACE, "opc %d "DF_UOID" rank %d tag %d\n",
		opc, DP_UOID(dobj->do_id), tgt_ep.ep_rank, tgt_ep.ep_tag);

	rc = obj_req_create(daos_task2ctx(task), &tgt_ep, opc, &req);
	if (rc != 0)
		D_GOTO(out_pool, rc);

	oei = crt_req_get(req);
	if (dkey != NULL)
		oei->oei_dkey = *dkey;

	if (akey != NULL)
		oei->oei_akey = *akey;

	D_ASSERT(oei != NULL);
	oei->oei_oid = dobj->do_id;
	uuid_copy(oei->oei_co_hdl, cont_hdl_uuid);
	uuid_copy(oei->oei_co_uuid, cont_uuid);

	oei->oei_map_ver = map_ver;
	oei->oei_epoch = epoch;
	oei->oei_nr = *nr;
	oei->oei_rec_type = type;
	enum_anchor_copy_hkey(&oei->oei_anchor, anchor);
	if (sgl != NULL) {
		oei->oei_sgl = *sgl;
		sgl_len = sgls_buf_len(sgl, 1);
		if (sgl_len >= OBJ_BULK_LIMIT) {
			/* Create bulk */
			rc = crt_bulk_create(daos_task2ctx(task),
					     daos2crt_sg(sgl), CRT_BULK_RW,
					     &oei->oei_bulk);
			if (rc < 0)
				D_GOTO(out_req, rc);
		}
	}

	crt_req_addref(req);
	enum_args.rpc = req;
	enum_args.hdlp = (daos_handle_t *)pool;
	enum_args.eaa_nr = nr;
	enum_args.eaa_kds = kds;
	enum_args.eaa_anchor = anchor;
	enum_args.eaa_obj = dobj;
	enum_args.eaa_size = size;
	enum_args.eaa_sgl = sgl;
	enum_args.eaa_eprs = eprs;
	enum_args.eaa_cookies = cookies;
	enum_args.eaa_versions = versions;
	enum_args.eaa_recxs = recxs;

	rc = tse_task_register_comp_cb(task, dc_enumerate_cb, &enum_args,
				       sizeof(enum_args));
	if (rc != 0)
		D_GOTO(out_eaa, rc);

	rc = daos_rpc_send(req, task);
	if (rc != 0) {
		D_ERROR("enumerate rpc failed rc %d\n", rc);
		D_GOTO(out_eaa, rc);
	}

	return rc;

out_eaa:
	crt_req_decref(req);
	if (sgl != NULL && sgl_len >= OBJ_BULK_LIMIT)
		crt_bulk_free(oei->oei_bulk);
out_req:
	crt_req_decref(req);
out_pool:
	dc_pool_put(pool);
out_put:
	obj_shard_decref(dobj);
out_task:
	tse_task_complete(task, rc);
	return rc;
}

int
dc_obj_shard_list_rec(daos_handle_t oh, enum obj_rpc_opc opc,
		      daos_epoch_t epoch, daos_key_t *dkey,
		      daos_key_t *akey, daos_iod_type_t type,
		      daos_size_t *size, uint32_t *nr,
		      daos_recx_t *recxs, daos_epoch_range_t *eprs,
		      uuid_t *cookies, uint32_t *versions,
		      daos_hash_out_t *anchor, unsigned int map_ver,
		      bool incr_order, tse_task_t *task)
{
	/* did not handle incr_order yet */
	return dc_obj_shard_list_internal(oh, opc, epoch, dkey, akey,
					  type, size, nr, NULL, NULL,
					  recxs, eprs, cookies, versions,
					  anchor, map_ver, task);
}

int
dc_obj_shard_list_key(daos_handle_t oh, enum obj_rpc_opc opc,
		      daos_epoch_t epoch, daos_key_t *key, uint32_t *nr,
		      daos_key_desc_t *kds, daos_sg_list_t *sgl,
		      daos_hash_out_t *anchor, unsigned int map_ver,
		      tse_task_t *task)
{
	return dc_obj_shard_list_internal(oh, opc, epoch, key, NULL,
					  DAOS_IOD_NONE, NULL, nr, kds, sgl,
					  NULL, NULL, NULL, NULL, anchor,
					  map_ver, task);
}
