/*
 * Copyright 2014-2015 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <jansson.h>
#include <stdio.h>
#include <unistd.h>

#include "api.h"

#include "libckpool.h"
#include "generator.h"
#include "stratifier.h"
#include "connector.h"

#define PROC_MAIN	0
#define PROC_GENERATOR	1
#define PROC_STRATIFER	2
#define PROC_CONNECTOR	3

struct api_command {
	const char *cmd;	/* API command we receive */
	int process;		/* Process to send request to */
	const char *proccmd;	/* Command to send to process */
	bool params;		/* Does this command take parameters */
} api_cmds[] = {
	{ "connector.stats",	PROC_CONNECTOR,	"stats",	0},
	{ "stratifier.stats",	PROC_STRATIFER,	"stats",	0},
	{ "generator.stats",	PROC_GENERATOR, "stats",	0},
	{ "", -1, "" , 0}
};

/* Receive a command, find which process to send the command to, get its
 * response and return it on the original socket. */
void ckpool_api(ckpool_t __maybe_unused *ckp, apimsg_t *apimsg)
{
	char *cmd = NULL, *response = NULL, *procresponse = NULL;
	json_t *val = NULL, *response_val = NULL, *params = NULL;
	struct api_command *ac = NULL;
	json_error_t err_val;
	int i = 0;

	if (unlikely(!apimsg->buf || !strlen(apimsg->buf))) {
		LOGWARNING("Received NULL buffer in ckpool_api");
		goto out;
	}
	LOGDEBUG("API received request %s", apimsg->buf);
	val = json_loads(apimsg->buf, 0, &err_val);
	if (unlikely(!val)) {
		LOGWARNING("Failed to JSON decode API message \"%s\" (%d):%s", apimsg->buf,
			   err_val.line, err_val.text);
		JSON_CPACK(response_val, "{s:b,s:[is],s:o}",
			   "result", false, "error", -1, "Invalid json", "response", json_null());
		goto out_send;
	}
	if (unlikely(!json_get_string(&cmd, val, "command"))) {
		LOGWARNING("Failed to find API command in message \"%s\"", apimsg->buf);
		JSON_CPACK(response_val, "{s:b,s:[is],s:o}",
			   "result", false, "error", -2, "No command", "response", json_null());
		goto out_send;
	}
	/* It's okay for there to be no parameters for many commands */
	params = json_object_get(val, "params");
	do {
		if (!safecmp(api_cmds[i].cmd, cmd))
			ac = &api_cmds[i];
		else
			i++;
	} while (!ac && api_cmds[i].process != -1);
	if (unlikely(!ac)) {
		LOGWARNING("Failed to find matching API command %s", cmd);
		JSON_CPACK(response_val, "{s:b,s:[is],s:o}",
			   "result", false, "error", -3, "Unknown command", "response", json_null());
		goto out_send;
	}
	if (unlikely(ac->params && !params)) {
		LOGWARNING("Failed to find mandatory params in API command %s", apimsg->buf);
		JSON_CPACK(response_val, "{s:b,s:[is],s:o}",
			   "result", false, "error", -4, "Missing params", "response", json_null());
		goto out_send;
	}
	/* FIXME: Need to figure out how to encapsulate mandatory parameters */
	switch(ac->process) {
		case PROC_MAIN:
			procresponse = send_recv_proc(&ckp->main, ac->proccmd);
			break;
		case PROC_GENERATOR:
			procresponse = send_recv_proc(ckp->generator, ac->proccmd);
			break;
		case PROC_STRATIFER:
			procresponse = send_recv_proc(ckp->stratifier, ac->proccmd);
			break;
		case PROC_CONNECTOR:
			procresponse = send_recv_proc(ckp->connector, ac->proccmd);
			break;
	}
	if (unlikely(!procresponse)) {
		LOGWARNING("Failed to get API response from process %d to command %s msg %s",
			   ac->process, ac->proccmd, apimsg->buf);
		JSON_CPACK(response_val, "{s:b,s:[is],s:o}",
			   "result", false, "error", -4, "No process response", "response", json_null());
		goto out_send;
	}
	JSON_CPACK(response_val, "{s:b,s:o,s:[s]}",
		   "result", false, "error", json_null(), "response", procresponse);
out_send:
	response = json_dumps(response_val, JSON_NO_UTF8 | JSON_PRESERVE_ORDER);
	if (unlikely(!send_unix_msg(apimsg->sockd, response))) {
		LOGWARNING("Failed to send API response: %s to sockd %d", response,
			   apimsg->sockd);
		goto out;
	}
	if (!wait_close(apimsg->sockd, 5))
		LOGWARNING("ckpool_api did not detect close from sockd %d", apimsg->sockd);
out:
	if (val)
		json_decref(val);
	free(procresponse);
	free(response);
	close(apimsg->sockd);
	free(apimsg->buf);
	free(apimsg);
}
