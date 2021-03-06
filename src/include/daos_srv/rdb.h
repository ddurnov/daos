/**
 * (C) Copyright 2017 Intel Corporation.
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
 * rdb: Replicated Database
 *
 * An RDB database comprises a hierarchy of key-value stores (KVSs), much like
 * how a file system comprises a hierarchy of directories. A value in a
 * (parent) KVS may be another (child) KVS. A KVS is therefore identified by a
 * path, which is the list of keys leading from the root KVS to the key whose
 * value is the KVS in question. A newly-created database is empty; to store
 * data, callers must first create the root KVS.
 *
 * Each KVS belongs to one of the predefined KVS classes (see rdb_kvs_class).
 *
 * The key space of an example database may look like:
 *
 *   rdb_path_root_key {
 *       "containers" {
 *           5742bdea-90e2-4765-ad74-b7f19cb6d78f {
 *               "ghce"
 *               "ghpce"
 *               "lhes" {
 *                   5
 *                   12349875
 *               }
 *               "lres" {
 *                   0
 *                   10
 *               }
 *               "snapshots" {
 *               }
 *               "user.attr_a"
 *               "user.attr_b"
 *           }
 *       }
 *       "container_handles" {
 *           b0733249-0c9a-471b-86e8-027bcfccc6b1
 *           92ccc99c-c755-45f4-b4ee-78fd081e54ca
 *       }
 *   }
 *
 * The RDB API is organized mostly around three types of objects:
 *
 *   - databases
 *   - paths
 *   - transactions
 *
 * And a few distributed helper methods, rdb_dist_*, makes certain distributed
 * tasks easier.
 *
 * All access to the KVSs in a database employ transactions (TX). Ending a TX
 * without committing it discards all its updates. Ending a query-only TX
 * without committing is fine at the moment.
 *
 * A query sees all (conflicting) updates committed (successfully) before its
 * rdb_tx_begin(). It may or may not see updates committed after its
 * rdb_tx_begin(). And, it currently does not see uncommitted updates, even
 * those in the same TX.
 *
 * Updates in a TX are queued, not revealed to queries, until rdb_tx_commit().
 * They are applied sequentially. If one update fails to apply, then the TX is
 * aborted (i.e., all applied updates in the TX are rolled back), and
 * rdb_tx_commit() returns the error.
 *
 * If a TX does not include any updates, then rdb_tx_commit() will be a no-op
 * and is not required.
 *
 * Currently, rdb expects to be accessed only by one thread and takes advantage
 * of the non-preemptive scheduling to simply internal locking.
 *
 * Caller locking guidelines:
 *
 *   rdb_tx_begin()
 *   rdlock(rl)
 *   rdb_tx_<query>()
 *   rdb_tx_<update>()
 *   wrlock(wl)		// must before commit(); may not cover update()s
 *   rdb_tx_commit()
 *   unlock(wl)		// must after commit()
 *   unlock(rl)		// must after all {rd,wr}lock()s; may before commit()
 *   rdb_tx_end()
 */

#ifndef DAOS_SRV_RDB_H
#define DAOS_SRV_RDB_H

#include <daos_types.h>
#include <daos/btree_class.h>

/** Database (opaque) */
struct rdb;

/** Database callbacks */
struct rdb_cbs {
	/**
	 * Called after this replica becomes the leader of \a term. If an error
	 * is returned, rdb steps down, without calling dc_step_down. (Not
	 * implemented yet.) A replicated service over rdb may want to take the
	 * chance to start itself on this replica.
	 */
	int (*dc_step_up)(struct rdb *db, uint64_t term, void *arg);

	/**
	 * Called after this replica steps down as the leader of \a term. A
	 * replicated service over rdb may want to take the chance to stop
	 * itself on this replica.
	 */
	void (*dc_step_down)(struct rdb *db, uint64_t term, void *arg);
};

/** Database methods */
int rdb_create(const char *path, const uuid_t uuid, size_t size,
	       const daos_rank_list_t *ranks);
int rdb_destroy(const char *path);
int rdb_start(const char *path, struct rdb_cbs *cbs, void *arg,
	      struct rdb **dbp);
void rdb_stop(struct rdb *db);
bool rdb_is_leader(struct rdb *db, uint64_t *term);
int rdb_get_leader(struct rdb *db, uint64_t *term, crt_rank_t *rank);
int rdb_get_ranks(struct rdb *db, daos_rank_list_t **ranksp);

/**
 * Path (opaque)
 *
 * A path is a list of keys. An absolute path begins with a special key
 * (rdb_path_root_key) representing the root KVS.
 */
typedef daos_iov_t rdb_path_t;

/**
 * Root key (opaque)
 *
 * A special key representing the root KVS in a path.
 */
extern daos_iov_t rdb_path_root_key;

/** Path methods */
int rdb_path_init(rdb_path_t *path);
void rdb_path_fini(rdb_path_t *path);
int rdb_path_clone(const rdb_path_t *path, rdb_path_t *new_path);
int rdb_path_push(rdb_path_t *path, const daos_iov_t *key);

/**
 * Define a daos_iov_t object, named \a prefix + \a name, that represents a
 * constant string key. See rdb_layout.[ch] for an example of the usage of this
 * helper macro.
 */
#define RDB_STRING_KEY(prefix, name)					\
static char	prefix ## name ## _buf[] = #name;			\
daos_iov_t	prefix ## name = {					\
	.iov_buf	= prefix ## name ## _buf,			\
	.iov_buf_len	= sizeof(prefix ## name ## _buf),		\
	.iov_len	= sizeof(prefix ## name ## _buf)		\
}

/** KVS classes */
enum rdb_kvs_class {
	RDB_KVS_GENERIC,	/**< hash-ordered byte-stream keys */
	RDB_KVS_INTEGER		/**< ordered fixed-size integer keys */
};

/** KVS attributes */
struct rdb_kvs_attr {
	enum rdb_kvs_class	dsa_class;
	unsigned int		dsa_order;	/**< dbtree order */
};

/**
 * Transaction (TX) (opaque)
 *
 * All fields are private. These are revealed to callers so that they may
 * allocate rdb_tx objects, possibly on their stacks.
 */
struct rdb_tx {
	struct rdb     *dt_db;
	uint64_t	dt_term;	/* raft term this tx begins in */
	void	       *dt_entry;	/* raft entry buffer */
	size_t		dt_entry_cap;	/* buffer capacity */
	size_t		dt_entry_len;	/* data length */
};

/** TX methods */
int rdb_tx_begin(struct rdb *db, struct rdb_tx *tx);
int rdb_tx_commit(struct rdb_tx *tx);
void rdb_tx_end(struct rdb_tx *tx);

/** TX update methods */
int rdb_tx_create_root(struct rdb_tx *tx, const struct rdb_kvs_attr *attr);
int rdb_tx_destroy_root(struct rdb_tx *tx);
int rdb_tx_create_kvs(struct rdb_tx *tx, const rdb_path_t *parent,
		      const daos_iov_t *key, const struct rdb_kvs_attr *attr);
int rdb_tx_destroy_kvs(struct rdb_tx *tx, const rdb_path_t *parent,
		       const daos_iov_t *key);
int rdb_tx_update(struct rdb_tx *tx, const rdb_path_t *kvs,
		  const daos_iov_t *key, const daos_iov_t *value);
int rdb_tx_delete(struct rdb_tx *tx, const rdb_path_t *kvs,
		  const daos_iov_t *key);

/** Probe operation codes */
enum rdb_probe_opc {
	RDB_PROBE_FIRST	= BTR_PROBE_FIRST,
	RDB_PROBE_LAST	= BTR_PROBE_LAST,
	RDB_PROBE_EQ	= BTR_PROBE_EQ,
	RDB_PROBE_GE	= BTR_PROBE_GE,
	RDB_PROBE_LE	= BTR_PROBE_LE
};

/** Iteration callback */
typedef dbtree_iterate_cb_t rdb_iterate_cb_t;

/** TX query methods */
int rdb_tx_lookup(struct rdb_tx *tx, const rdb_path_t *kvs,
		  const daos_iov_t *key, daos_iov_t *value);
int rdb_tx_fetch(struct rdb_tx *tx, const rdb_path_t *kvs,
		 enum rdb_probe_opc opc, const daos_iov_t *key_in,
		 daos_iov_t *key_out, daos_iov_t *value);
int rdb_tx_iterate(struct rdb_tx *tx, const rdb_path_t *kvs, bool backward,
		   rdb_iterate_cb_t cb, void *arg);

/** Distributed helper methods */
int rdb_dist_start(const uuid_t uuid, const uuid_t pool_uuid,
		   const daos_rank_list_t *ranks, bool create, size_t size);
int rdb_dist_stop(const uuid_t uuid, const uuid_t pool_uuid,
		  const daos_rank_list_t *ranks, bool destroy);

#endif /* DAOS_SRV_RDB_H */
