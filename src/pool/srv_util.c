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
 * ds_pool: Pool Server Utilities
 */
#define DD_SUBSYS	DD_FAC(pool)

#include <daos_srv/pool.h>

#include <daos/pool_map.h>
#include "rpc.h"
#include "srv_internal.h"

enum map_ranks_class {
	MAP_RANKS_UP,
	MAP_RANKS_DOWN
};

static inline int
map_ranks_include(enum map_ranks_class class, int status)
{
	switch (class) {
	case MAP_RANKS_UP:
		return status == PO_COMP_ST_UP || status == PO_COMP_ST_UPIN;
	case MAP_RANKS_DOWN:
		return status == PO_COMP_ST_DOWN ||
		       status == PO_COMP_ST_DOWNOUT;
	default:
		D_ASSERTF(0, "%d\n", class);
	}
}

/* Build a rank list of targets with certain status. */
static int
map_ranks_init(const struct pool_map *map, enum map_ranks_class class,
	       daos_rank_list_t *ranks)
{
	struct pool_target     *targets;
	int			ntargets;
	int			n = 0;
	int			i;
	daos_rank_t	       *rs;

	ntargets = pool_map_find_target((struct pool_map *)map, PO_COMP_ID_ALL,
					&targets);
	if (ntargets == 0) {
		D_ERROR("no targets in pool map\n");
		return -DER_IO;
	}

	for (i = 0; i < ntargets; i++)
		if (map_ranks_include(class, targets[i].ta_comp.co_status))
			n++;

	if (n == 0) {
		memset(ranks, 0, sizeof(*ranks));
		return 0;
	}

	D_ALLOC(rs, sizeof(*rs) * n);
	if (rs == NULL)
		return -DER_NOMEM;

	ranks->rl_nr.num = n;
	ranks->rl_nr.num_out = 0;
	ranks->rl_ranks = rs;

	n = 0;
	for (i = 0; i < ntargets; i++) {
		if (map_ranks_include(class, targets[i].ta_comp.co_status)) {
			D_ASSERT(n < ranks->rl_nr.num);
			ranks->rl_ranks[n] = targets[i].ta_comp.co_rank;
			n++;
		}
	}
	D_ASSERTF(n == ranks->rl_nr.num, "%d != %u\n", n, ranks->rl_nr.num);

	return 0;
}

static void
map_ranks_fini(daos_rank_list_t *ranks)
{
	if (ranks->rl_ranks != NULL) {
		D_ASSERT(ranks->rl_nr.num != 0);
		D_FREE(ranks->rl_ranks,
		       sizeof(*ranks->rl_ranks) * ranks->rl_nr.num);
	} else {
		D_ASSERT(ranks->rl_nr.num == 0);
	}
}

static int
map_ranks_merge(daos_rank_list_t *src_ranks, daos_rank_list_t *ranks_merge)
{
	daos_rank_t	*rs;
	int		*indexes;
	int		num = 0;
	int		src_num;
	int		i;
	int		j;
	int		rc = 0;

	if (ranks_merge == NULL || src_ranks == NULL)
		return 0;

	src_num = src_ranks->rl_nr.num;
	D_ALLOC(indexes, sizeof(*indexes) * ranks_merge->rl_nr.num);
	if (indexes == NULL)
		return -DER_NOMEM;

	for (i = 0; i < ranks_merge->rl_nr.num; i++) {
		bool included = false;

		for (j = 0; j < src_num; j++) {
			if (src_ranks->rl_ranks[j] ==
			    ranks_merge->rl_ranks[i]) {
				included = true;
				break;
			}
		}

		if (!included) {
			indexes[num] = i;
			num++;
		}
	}

	if (num == 0)
		D_GOTO(free, rc = 0);

	D_ALLOC(rs, sizeof(*rs) * (num + src_ranks->rl_nr.num));
	if (rs == NULL)
		D_GOTO(free, rc = -DER_NOMEM);

	for (i = 0; i < src_num; i++)
		rs[i] = src_ranks->rl_ranks[i];

	for (i = src_num, j = 0; i < src_num + num; i++, j++) {
		int idx = indexes[j];

		rs[i] = ranks_merge->rl_ranks[idx];
	}

	map_ranks_fini(src_ranks);

	src_ranks->rl_nr.num = num + src_num;
	src_ranks->rl_nr.num_out = num + src_num;
	src_ranks->rl_ranks = rs;

free:
	D_FREE(indexes, sizeof(*indexes) * ranks_merge->rl_nr.num);
	return rc;
}

int
ds_pool_group_create(const uuid_t pool_uuid, const struct pool_map *map,
		     crt_group_t **group)
{
	uuid_t			uuid;
	char			id[DAOS_UUID_STR_SIZE];
	daos_rank_list_t	ranks;
	int			rc;

	uuid_generate(uuid);
	uuid_unparse_lower(uuid, id);

	D_DEBUG(DF_DSMS, DF_UUID": creating pool group %s\n",
		DP_UUID(pool_uuid), id);

	rc = map_ranks_init(map, MAP_RANKS_UP, &ranks);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to create rank list: %d\n",
			DP_UUID(pool_uuid), rc);
		D_GOTO(out, rc);
	}

	if (ranks.rl_nr.num == 0) {
		D_ERROR(DF_UUID": failed to find any up targets\n",
			DP_UUID(pool_uuid));
		D_GOTO(out_ranks, rc = -DER_IO);
	}

	rc = dss_group_create(id, &ranks, group);
	if (rc != 0)
		D_GOTO(out_ranks, rc);

out_ranks:
	map_ranks_fini(&ranks);
out:
	return rc;
}

int
ds_pool_group_destroy(const uuid_t pool_uuid, crt_group_t *group)
{
	int rc;

	D_DEBUG(DF_DSMS, DF_UUID": destroying pool group %s\n",
		DP_UUID(pool_uuid), group->cg_grpid);
	rc = dss_group_destroy(group);
	if (rc != 0)
		D_ERROR(DF_UUID": failed to destroy pool group %s: %d\n",
			DP_UUID(pool_uuid), group->cg_grpid, rc);
	return rc;
}

int
ds_pool_bcast_create(crt_context_t ctx, struct ds_pool *pool,
		     enum daos_module_id module, crt_opcode_t opcode,
		     crt_rpc_t **rpc, crt_bulk_t bulk_hdl,
		     daos_rank_list_t *excluded_list)
{
	daos_rank_list_t	excluded;
	crt_opcode_t		opc;
	int			rc;

	ABT_rwlock_rdlock(pool->sp_lock);
	rc = map_ranks_init(pool->sp_map, MAP_RANKS_DOWN, &excluded);
	ABT_rwlock_unlock(pool->sp_lock);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to create rank list: %d\n",
			DP_UUID(pool->sp_uuid), rc);
		return rc;
	}

	if (excluded_list != NULL)
		map_ranks_merge(&excluded, excluded_list);

	opc = DAOS_RPC_OPCODE(opcode, module, 1);
	rc = crt_corpc_req_create(ctx, pool->sp_group,
			  excluded.rl_nr.num == 0 ? NULL : &excluded,
			  opc, bulk_hdl/* co_bulk_hdl */, NULL /* priv */,
			  0 /* flags */, crt_tree_topo(CRT_TREE_KNOMIAL, 4),
			  rpc);

	map_ranks_fini(&excluded);
	return rc;
}

static int
map_exclude_create_sanitized_tgts(const daos_rank_list_t *tgts,
				  daos_rank_list_t **tgts_sanitized)
{
	daos_rank_list_t       *ts;
	int			rc;

	rc = crt_rank_list_dup_sort_uniq(&ts, tgts, true /* input */);
	if (rc != 0)
		return rc;

	/* Save the size of this rank list in num_out. */
	ts->rl_nr.num_out = ts->rl_nr.num;

	*tgts_sanitized = ts;
	return 0;
}

static void
map_exclude_destroy_sanitized_tgts(daos_rank_list_t *tgts)
{
	/* Restore the size of this rank list from num_out. */
	tgts->rl_nr.num = tgts->rl_nr.num_out;
	crt_rank_list_free(tgts);
}

/*
 * Exclude "tgts" in "map". A new map version is generated only if actual
 * changes have been made. If "tgts_failed" is not NULL, then targets that are
 * not excluded are added to "tgts_failed", whose rank buffer must be at least
 * as large that of "tgts".
 */
int
ds_pool_map_tgts_update(struct pool_map *map, daos_rank_list_t *tgts,
			daos_rank_list_t *tgts_failed, int opc)
{
	daos_rank_list_t       *tgts_sanitized;
	uint32_t		version;
	int			i;
	int			nchanges = 0;
	int			rc;

	D_ASSERT(tgts != NULL && tgts->rl_nr.num > 0 && tgts->rl_ranks != NULL);
	D_ASSERT(tgts_failed == NULL ||
		 (tgts_failed->rl_nr.num >= tgts->rl_nr.num &&
		  tgts_failed->rl_ranks != NULL));

	if (tgts_failed != NULL)
		tgts_failed->rl_nr.num_out = 0;

	rc = map_exclude_create_sanitized_tgts(tgts, &tgts_sanitized);
	if (rc != 0)
		return rc;

	version = pool_map_get_version(map) + 1;

	for (i = 0; i < tgts_sanitized->rl_nr.num; i++) {
		struct pool_target     *target;
		daos_rank_t		rank = tgts_sanitized->rl_ranks[i];

		target = pool_map_find_target_by_rank(map, rank);
		if (target == NULL) {
			D_DEBUG(DF_DSMS, "failed to find rank %u in map %p\n",
				rank, map);
			if (tgts_failed != NULL) {
				int j = tgts_failed->rl_nr.num_out;

				tgts_failed->rl_ranks[j] = rank;
				tgts_failed->rl_nr.num_out++;
			}
			continue;
		}

		D_ASSERTF(target->ta_comp.co_status == PO_COMP_ST_UP ||
			  target->ta_comp.co_status == PO_COMP_ST_UPIN ||
			  target->ta_comp.co_status == PO_COMP_ST_DOWN ||
			  target->ta_comp.co_status == PO_COMP_ST_DOWNOUT,
			  "%u\n", target->ta_comp.co_status);
		if (opc == POOL_EXCLUDE &&
		    target->ta_comp.co_status != PO_COMP_ST_DOWN &&
		    target->ta_comp.co_status != PO_COMP_ST_DOWNOUT) {
			D_DEBUG(DF_DSMS, "changing rank %u to DOWN in map %p\n",
				target->ta_comp.co_rank, map);
			target->ta_comp.co_status = PO_COMP_ST_DOWN;
			target->ta_comp.co_fseq = version;
			nchanges++;
		} else if (opc == POOL_ADD &&
			   target->ta_comp.co_status != PO_COMP_ST_UP &&
			   target->ta_comp.co_status != PO_COMP_ST_UPIN) {
			D_DEBUG(DF_DSMS, "changing rank %u to UP in map %p\n",
				target->ta_comp.co_rank, map);
			target->ta_comp.co_status = PO_COMP_ST_UP;
			target->ta_comp.co_ver = version;
			nchanges++;
		} else if (opc == POOL_EXCLUDE_OUT &&
			   target->ta_comp.co_status == PO_COMP_ST_DOWN) {
			D_DEBUG(DF_DSMS, "changing rank %u to DOWNOUT map %p\n",
				target->ta_comp.co_rank, map);
			target->ta_comp.co_status = PO_COMP_ST_DOWNOUT;
			nchanges++;
		}
	}

	/* Set the version only if actual changes have been made. */
	if (nchanges > 0) {
		D_DEBUG(DF_DSMS, "generating map %p version %u: nchanges=%d\n",
			map, version, nchanges);
		rc = pool_map_set_version(map, version);
		D_ASSERTF(rc == 0, "%d\n", rc);
	}

	map_exclude_destroy_sanitized_tgts(tgts_sanitized);
	return 0;
}
