/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "memtx_engine.h"
#include "tuple.h"
#include "txn.h"
#include "index.h"
#include "memtx_hash.h"
#include "memtx_tree.h"
#include "memtx_rtree.h"
#include "memtx_bitset.h"
#include "space.h"
#include "salad/rlist.h"
#include <stdlib.h>
#include <string.h>

struct Memtx: public Engine {
	Memtx(EngineFactory *e)
		: Engine(e)
	{ }
	virtual ~Memtx()
	{
		/* do nothing */
		/* factory->close(this); */
	}
};

/**
 * This is a vtab with which a newly created space which has no
 * keys is primed.
 * At first it is set to correctly work for spaces created during
 * recovery from snapshot. In process of recovery it is updated as
 * below:
 *
 * 1) after SNAP is loaded:
 *    recover = space_build_primary_key
 * 2) when all XLOGs are loaded:
 *    recover = space_build_all_keys
*/

static inline void
memtx_recovery_prepare(struct engine_recovery *r)
{
	r->state   = READY_NO_KEYS;
	r->recover = space_begin_build_primary_key;
	r->replace = space_replace_no_keys;
}

MemtxFactory::MemtxFactory()
	:EngineFactory("memtx")
{
	flags = ENGINE_TRANSACTIONAL | ENGINE_NO_YIELD |
	        ENGINE_CAN_BE_TEMPORARY;
	memtx_recovery_prepare(&recovery);
}

void
MemtxFactory::end_recover_snapshot()
{
	recovery.recover = space_build_primary_key;
}

void
MemtxFactory::end_recovery()
{
	recovery.recover = space_build_all_keys;
}

Engine *MemtxFactory::open()
{
	return new Memtx(this);
}

Index *
MemtxFactory::createIndex(struct key_def *key_def)
{
	switch (key_def->type) {
	case HASH:
		return new MemtxHash(key_def);
	case TREE:
		return new MemtxTree(key_def);
	case RTREE:
		return new MemtxRTree(key_def);
	case BITSET:
		return new MemtxBitset(key_def);
	default:
		assert(false);
		return NULL;
	}
}

void
MemtxFactory::dropIndex(Index *index)
{
	struct iterator *it = index->position();
	index->initIterator(it, ITER_ALL, NULL, 0);
	struct tuple *tuple;
	while ((tuple = it->next(it)))
		tuple_unref(tuple);
}

void
MemtxFactory::keydefCheck(struct key_def *key_def)
{
	switch (key_def->type) {
	case HASH:
		if (! key_def->is_unique) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  (unsigned) key_def->iid,
				  (unsigned) key_def->space_id,
				  "HASH index must be unique");
		}
		break;
	case TREE:
		/* TREE index has no limitations. */
		break;
	case RTREE:
		if (key_def->part_count != 1) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  (unsigned) key_def->iid,
				  (unsigned) key_def->space_id,
				  "RTREE index key can not be multipart");
		}
		if (key_def->is_unique) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  (unsigned) key_def->iid,
				  (unsigned) key_def->space_id,
				  "RTREE index can not be unique");
		}
		break;
	case BITSET:
		if (key_def->part_count != 1) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  (unsigned) key_def->iid,
				  (unsigned) key_def->space_id,
				  "BITSET index key can not be multipart");
		}
		if (key_def->is_unique) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  (unsigned) key_def->iid,
				  (unsigned) key_def->space_id,
				  "BITSET can not be unique");
		}
		break;
	default:
		tnt_raise(ClientError, ER_INDEX_TYPE,
			  (unsigned) key_def->iid,
			  (unsigned) key_def->space_id);
		break;
	}
	for (uint32_t i = 0; i < key_def->part_count; i++) {
		switch (key_def->parts[i].type) {
		case NUM:
		case STRING:
			if (key_def->type == RTREE) {
				tnt_raise(ClientError, ER_MODIFY_INDEX,
					  (unsigned) key_def->iid,
					  (unsigned) key_def->space_id,
					  "RTREE index field type must be ARRAY");
			}
			break;
		case ARRAY:
			if (key_def->type != RTREE) {
				tnt_raise(ClientError, ER_MODIFY_INDEX,
					  (unsigned) key_def->iid,
					  (unsigned) key_def->space_id,
					  "ARRAY field type is not supported");
			}
			break;
		default:
			assert(false);
			break;
		}
	}
}

void
MemtxFactory::rollback(struct txn *txn)
{
	struct txn_stmt *stmt;
	rlist_foreach_entry_reverse(stmt, &txn->stmts, next) {
		if (stmt->old_tuple || stmt->new_tuple) {
			space_replace(stmt->space, stmt->new_tuple,
				      stmt->old_tuple, DUP_INSERT);
		}
	}
}

/** Called at start to tell memtx to recover to a given LSN. */
void
MemtxFactory::begin_recover_snapshot(int64_t /* lsn */)
{
	/*
	 * memtx snapshotting supported directly by box.
	 * do nothing here.
	 */
}

int
MemtxFactory::begin_checkpoint(int64_t /* lsn */)
{
	return 0;
}

int
MemtxFactory::wait_checkpoint(int64_t /* lsn */)
{
	return 0;
}

void
MemtxFactory::delete_checkpoint(int64_t /* lsn */)
{
}
