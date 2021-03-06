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
/*
 * dctc: Client container operations
 *
 * dctc is the DCT client module/library. It exports the DCT API defined in
 * daos_tier.h.
 */
#define DD_SUBSYS	DD_FAC(tier)

#include <daos/tier.h>
#include <daos_tier.h>
#include <pthread.h>
#include <daos/rpc.h>
#include "rpc.h"

int
dc_tier_cont_create(daos_handle_t poh, const uuid_t uuid, daos_event_t *ev)
{
	return 0;
}


int
dc_tier_cont_open(daos_handle_t poh, const uuid_t uuid, unsigned int flags,
	       daos_rank_list_t *failed, daos_handle_t *coh,
	       daos_cont_info_t *info, daos_event_t *ev)
{
	return 0;
}

int
dc_tier__cont_close(daos_handle_t coh, daos_event_t *ev)
{
	return 0;
}


int
dc_tier_cont_destroy(daos_handle_t poh, const uuid_t uuid, int force,
		  daos_event_t *ev)
{
	return 0;
}
