/**
 * Copyright (C) 2016 Daniel-Constantin Mierla (asipto.com)
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*!
 * \file
 * \brief Kamailio topos :: Module interface
 * \ingroup topos
 * Module: \ref topos
 */

/*! \defgroup topoh Kamailio :: Topology stripping
 *
 * This module removes the SIP routing headers that show topology details.
 * The script interpreter gets the SIP messages with full content, so all
 * existing functionality is preserved.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "../../sr_module.h"
#include "../../events.h"
#include "../../dprint.h"
#include "../../tcp_options.h"
#include "../../ut.h"
#include "../../forward.h"
#include "../../config.h"
#include "../../parser/msg_parser.h"
#include "../../parser/parse_uri.h"
#include "../../parser/parse_to.h"
#include "../../parser/parse_from.h"

#include "../../lib/srdb1/db.h"
#include "../../lib/srutils/sruid.h"

#include "../../modules/sanity/api.h"

#include "tps_storage.h"
#include "tps_msg.h"

MODULE_VERSION


/* Database connection handle */
db1_con_t* _tps_db_handle = NULL;
/* DB functions */
db_func_t tpsdbf;
/* sruid to get internal uid */
sruid_t _tps_sruid;

/** module parameters */
static str _tps_db_url = str_init(DEFAULT_DB_URL);
int _tps_param_mask_callid = 0;
int _tps_sanity_checks = 0;

sanity_api_t scb;

int tps_msg_received(void *data);
int tps_msg_sent(void *data);

/** module functions */
/* Module init function prototype */
static int mod_init(void);
/* Module child-init function prototype */
static int child_init(int rank);
/* Module destroy function prototype */
static void destroy(void);

static param_export_t params[]={
	{"db_url",			PARAM_STR, &_tps_db_url},
	{"mask_callid",		PARAM_INT, &_tps_param_mask_callid},
	{"sanity_checks",	PARAM_INT, &_tps_sanity_checks},
	{0,0,0}
};


/** module exports */
struct module_exports exports= {
	"topos",
	DEFAULT_DLFLAGS, /* dlopen flags */
	0,
	params,
	0,          /* exported statistics */
	0,          /* exported MI functions */
	0,          /* exported pseudo-variables */
	0,          /* extra processes */
	mod_init,   /* module initialization function */
	0,
	destroy,    /* destroy function */
	child_init  /* child initialization function */
};

/**
 * init module function
 */
static int mod_init(void)
{
	/* Find a database module */
	if (db_bind_mod(&_tps_db_url, &tpsdbf)) {
		LM_ERR("unable to bind database module\n");
		return -1;
	}
	if (!DB_CAPABILITY(tpsdbf, DB_CAP_ALL)) {
		LM_CRIT("database modules does not "
			"provide all functions needed\n");
		return -1;
	}

	if(_tps_sanity_checks!=0) {
		if(sanity_load_api(&scb)<0) {
			LM_ERR("cannot bind to sanity module\n");
			goto error;
		}
	}
	if(tps_storage_lock_set_init()<0) {
		LM_ERR("failed to initialize locks set\n");
		return -1;
	}

	if(sruid_init(&_tps_sruid, '-', "tpsh", SRUID_INC)<0)
		return -1;

	sr_event_register_cb(SREV_NET_DATA_IN,  tps_msg_received);
	sr_event_register_cb(SREV_NET_DATA_OUT, tps_msg_sent);

#ifdef USE_TCP
	tcp_set_clone_rcvbuf(1);
#endif

	return 0;
error:
	return -1;
}

/**
 *
 */
static int child_init(int rank)
{
	if(sruid_init(&_tps_sruid, '-', "tpsh", SRUID_INC)<0)
		return -1;

	if (rank==PROC_INIT || rank==PROC_MAIN || rank==PROC_TCP_MAIN)
		return 0; /* do nothing for the main process */

	_tps_db_handle = tpsdbf.init(&_tps_db_url);
	if (!_tps_db_handle) {
		LM_ERR("unable to connect database\n");
		return -1;
	}
	return 0;

}

/**
 *
 */
static void destroy(void)
{
	if (_tps_db_handle) {
		tpsdbf.close(_tps_db_handle);
		_tps_db_handle = 0;
	}
	tps_storage_lock_set_destroy();
}

/**
 *
 */
int tps_prepare_msg(sip_msg_t *msg)
{
	if (parse_msg(msg->buf, msg->len, msg)!=0) {
		LM_DBG("outbuf buffer parsing failed!");
		return 1;
	}

	if(msg->first_line.type==SIP_REQUEST) {
		if(!IS_SIP(msg)) {
			LM_DBG("non sip request message\n");
			return 1;
		}
	} else if(msg->first_line.type!=SIP_REPLY) {
		LM_DBG("non sip message\n");
		return 1;
	}

	if (parse_headers(msg, HDR_EOH_F, 0)==-1) {
		LM_DBG("parsing headers failed [[%.*s]]\n",
				msg->len, msg->buf);
		return 2;
	}

	/* force 2nd via parsing here - it helps checking it later */
	if (parse_headers(msg, HDR_VIA2_F, 0)==-1
			|| (msg->via2==0) || (msg->via2->error!=PARSE_OK)) {
		LM_DBG("no second via in this message \n");
	}

	if(parse_from_header(msg)<0) {
		LM_ERR("cannot parse FROM header\n");
		return 3;
	}

	if(parse_to_header(msg)<0 || msg->to==NULL) {
		LM_ERR("cannot parse TO header\n");
		return 3;
	}

	if(get_to(msg)==NULL) {
		LM_ERR("cannot get TO header\n");
		return 3;
	}

	return 0;
}

/**
 *
 */
int tps_msg_received(void *data)
{
	sip_msg_t msg;
	str *obuf;
	char *nbuf = NULL;
	int direction;
	int dialog;

	obuf = (str*)data;
	memset(&msg, 0, sizeof(sip_msg_t));
	msg.buf = obuf->s;
	msg.len = obuf->len;

	if(tps_prepare_msg(&msg)!=0) {
		goto done;
	}

	if(tps_skip_msg(&msg)) {
		goto done;
	}

	direction = 0;
	if(msg.first_line.type==SIP_REQUEST) {
		if(_tps_sanity_checks!=0) {
			if(scb.check_defaults(&msg)<1) {
				LM_ERR("sanity checks failed\n");
				goto done;
			}
		}
		dialog = (get_to(&msg)->tag_value.len>0)?1:0;
		if(dialog) {
			direction = tps_route_direction(&msg);
			if(direction<0) {
				LM_ERR("not able to detect direction\n");
				goto done;
			}
		}
		if(dialog) {
			/* dialog request */
			tps_request_received(&msg, dialog, direction);
		}
	} else {
		/* reply */
		if(msg.via2==0) {
			/* one Via in received reply -- it is for local generated request
			 * - nothing to unhide unless is CANCEL/ACK */
			if(!((get_cseq(&msg)->method_id)&(METHOD_CANCEL)))
				goto done;
		}
		tps_response_received(&msg);
	}

	nbuf = tps_msg_update(&msg, (unsigned int*)&obuf->len);

	if(obuf->len>=BUF_SIZE) {
		LM_ERR("new buffer overflow (%d)\n", obuf->len);
		pkg_free(nbuf);
		return -1;
	}
	memcpy(obuf->s, nbuf, obuf->len);
	obuf->s[obuf->len] = '\0';

done:
	if(nbuf!=NULL)
		pkg_free(nbuf);
	free_sip_msg(&msg);
	return 0;
}

/**
 *
 */
int tps_msg_sent(void *data)
{
	sip_msg_t msg;
	str *obuf;
	int direction;
	int dialog;
	int local;

	obuf = (str*)data;
	memset(&msg, 0, sizeof(sip_msg_t));
	msg.buf = obuf->s;
	msg.len = obuf->len;

	if(tps_prepare_msg(&msg)!=0) {
		goto done;
	}

	if(tps_skip_msg(&msg)) {
		goto done;
	}

	if(msg.first_line.type==SIP_REQUEST) {
		direction = tps_route_direction(&msg);
		dialog = (get_to(&msg)->tag_value.len>0)?1:0;

		local = 0;
		if(msg.via2==0) {
			local = 1;
			if(direction==0) {
				/* downstream local request (e.g., dlg bye) */
				local = 2;
			}
		}

		tps_request_sent(&msg, dialog, direction, local);
	} else {
		tps_response_sent(&msg);
	}

ready:
	obuf->s = tps_msg_update(&msg, (unsigned int*)&obuf->len);

done:
	free_sip_msg(&msg);
	return 0;
}

