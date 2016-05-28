/**
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the LGPL License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 *
 * (C) Copyright 2016 Intel Corporation.
 */
/**
 * This file is part of daos
 *
 * vos/iterator.c
 */
#include <daos/btree.h>
#include <daos_srv/vos.h>
#include <vos_internal.h>
#include <vos_hhash.h>

/** Dictionary for all known vos iterators */
struct vos_iter_dict {
	vos_iter_type_t		 id_type;
	const char		*id_name;
	struct vos_iter_ops	*id_ops;
};

extern struct vos_iter_ops vos_obj_iter_ops;

static struct vos_iter_dict vos_iterators[] = {
	{
		.id_type	= VOS_ITER_DKEY,
		.id_name	= "dkey",
		.id_ops		= &vos_obj_iter_ops,
	},
	{
		.id_type	= VOS_ITER_RECX,
		.id_name	= "recx",
		.id_ops		= &vos_obj_iter_ops,
	},
	{
		.id_type	= VOS_ITER_NONE,
		.id_name	= "unknown",
		.id_ops		= NULL,
	},
};

static struct vos_iterator *
vos_hdl2iter(daos_handle_t hdl)
{
	return (struct vos_iterator *)hdl.cookie;
}

static daos_handle_t
vos_iter2hdl(struct vos_iterator *iter)
{
	daos_handle_t	hdl;

	hdl.cookie = (uint64_t)iter;
	return hdl;
}

int
vos_iter_prepare(vos_iter_type_t type, vos_iter_param_t *param,
		 daos_handle_t *ih)
{
	struct vos_iter_dict	*dict;
	struct vos_iterator	*iter;
	int			 rc;

	for (dict = &vos_iterators[0]; dict->id_ops != NULL; dict++) {
		if (dict->id_type == type)
			break;
	}

	if (dict->id_ops == NULL) {
		D_DEBUG(DF_VOS1, "Can't find iterator type %d\n", type);
		return -DER_NONEXIST;
	}

	rc = dict->id_ops->iop_prepare(type, param, &iter);
	if (rc != 0) {
		D_DEBUG(DF_VOS1, "Failed to prepare %s iterator: %d\n",
			dict->id_name, rc);
		return rc;
	}

	iter->it_type	= type;
	iter->it_ops	= dict->id_ops;
	iter->it_state	= VOS_ITS_NONE;

	*ih = vos_iter2hdl(iter);
	return 0;
}

int
vos_iter_finish(daos_handle_t ih)
{
	struct vos_iterator *iter = vos_hdl2iter(ih);

	D_ASSERT(iter->it_ops != NULL);
	return iter->it_ops->iop_finish(iter);
}

int
vos_iter_probe(daos_handle_t ih, daos_hash_out_t *anchor)
{
	struct vos_iterator *iter = vos_hdl2iter(ih);
	int		     rc;

	D_ASSERT(iter->it_ops != NULL);
	rc = iter->it_ops->iop_probe(iter, anchor);
	if (rc == 0)
		iter->it_state = VOS_ITS_OK;
	else if (rc == -DER_NONEXIST)
		iter->it_state = VOS_ITS_END;
	else
		iter->it_state = VOS_ITS_NONE;

	return rc;
}

int
vos_iter_next(daos_handle_t ih)
{
	struct vos_iterator *iter = vos_hdl2iter(ih);
	int		     rc;

	if (iter->it_state == VOS_ITS_NONE) {
		D_DEBUG(DF_VOS1,
			"Please call vos_iter_probe to initialise cursor\n");
		return -DER_NO_PERM;
	}

	if (iter->it_state == VOS_ITS_END) {
		D_DEBUG(DF_VOS1, "The end of iteration\n");
		return -DER_NONEXIST;
	}

	D_ASSERT(iter->it_ops != NULL);
	rc = iter->it_ops->iop_next(iter);
	if (rc == 0)
		iter->it_state = VOS_ITS_OK;
	else if (rc == -DER_NONEXIST)
		iter->it_state = VOS_ITS_END;
	else
		iter->it_state = VOS_ITS_NONE;

	return rc;
}

int
vos_iter_fetch(daos_handle_t ih, vos_iter_entry_t *it_entry,
	       daos_hash_out_t *anchor)
{
	struct vos_iterator *iter = vos_hdl2iter(ih);

	if (iter->it_state == VOS_ITS_NONE) {
		D_DEBUG(DF_VOS1,
			"Please call vos_iter_probe to initialise cursor\n");
		return -DER_NO_PERM;
	}

	if (iter->it_state == VOS_ITS_END) {
		D_DEBUG(DF_VOS1, "The end of iteration\n");
		return -DER_NONEXIST;
	}

	D_ASSERT(iter->it_ops != NULL);
	return iter->it_ops->iop_fetch(iter, it_entry, anchor);
}