/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_chat);

#include <zephyr/kernel.h>
#include <string.h>

#include <zephyr/modem/modem_chat.h>

#define MODEM_CHAT_MATCHES_INDEX_RESPONSE        (0)
#define MODEM_CHAT_MATCHES_INDEX_ABORT           (1)
#define MODEM_CHAT_MATCHES_INDEX_UNSOL           (2)

#define MODEM_CHAT_SCRIPT_STATE_RUNNING_BIT      (0)

static void modem_chat_script_stop(struct modem_chat *cmd, enum modem_chat_script_result result)
{
	/* Handle result */
	if (result == MODEM_CHAT_SCRIPT_RESULT_SUCCESS) {
		LOG_DBG("%s: complete", cmd->script->name);
	} else if (result == MODEM_CHAT_SCRIPT_RESULT_ABORT){
		LOG_WRN("%s: aborted", cmd->script->name);
	} else {
		LOG_WRN("%s: timed out", cmd->script->name);
	}

	/* Clear script running state */
	atomic_clear_bit(&cmd->script_state, MODEM_CHAT_SCRIPT_STATE_RUNNING_BIT);

	/* Call back with result */
	if (cmd->script->callback != NULL) {
		cmd->script->callback(cmd, result, cmd->user_data);
	}

	/* Clear reference to script */
	cmd->script = NULL;

	/* Clear response and abort commands */
	cmd->matches[MODEM_CHAT_MATCHES_INDEX_ABORT] = NULL;
	cmd->matches_size[MODEM_CHAT_MATCHES_INDEX_ABORT] = 0;
	cmd->matches[MODEM_CHAT_MATCHES_INDEX_RESPONSE] = NULL;
	cmd->matches_size[MODEM_CHAT_MATCHES_INDEX_RESPONSE] = 0;

	/* Cancel timeout work */
	k_work_cancel_delayable(&cmd->script_timeout_work.dwork);
}

static void modem_chat_script_send(struct modem_chat *cmd)
{
	/* Initialize script send work */
	cmd->script_send_request_pos = 0;
	cmd->script_send_delimiter_pos = 0;

	/* Schedule script send work */
	k_work_schedule(&cmd->script_send_work.dwork, K_NO_WAIT);
}

static void modem_chat_script_next(struct modem_chat *cmd, bool initial)
{
	const struct modem_chat_script_cmd *script_cmd;

	/* Advance iterator if not initial */
	if (initial == true) {
		/* Reset iterator */
		cmd->script_cmd_it = 0;
	} else {
		/* Advance iterator */
		cmd->script_cmd_it++;
	}

	/* Check if end of script reached */
	if (cmd->script_cmd_it == cmd->script->script_cmds_size) {
		modem_chat_script_stop(cmd, MODEM_CHAT_SCRIPT_RESULT_SUCCESS);

		return;
	}

	LOG_DBG("%s: step: %u", cmd->script->name, cmd->script_cmd_it);

	script_cmd = &cmd->script->script_cmds[cmd->script_cmd_it];

	/* Set response command handlers */
	cmd->matches[MODEM_CHAT_MATCHES_INDEX_RESPONSE] = script_cmd->response_matches;
	cmd->matches_size[MODEM_CHAT_MATCHES_INDEX_RESPONSE] = script_cmd->response_matches_size;

	/* Check if work must be sent */
	if (strlen(script_cmd->request) > 0) {
		modem_chat_script_send(cmd);
	}
}

static void modem_chat_script_start(struct modem_chat *cmd, const struct modem_chat_script *script)
{
	/* Save script */
	cmd->script = script;

	/* Set abort matches */
	cmd->matches[MODEM_CHAT_MATCHES_INDEX_ABORT] = script->abort_matches;
	cmd->matches_size[MODEM_CHAT_MATCHES_INDEX_ABORT] = script->abort_matches_size;

	LOG_DBG("%s", cmd->script->name);

	/* Set first script command */
	modem_chat_script_next(cmd, true);

	/* Start timeout work if script started */
	if (cmd->script != NULL) {
		k_work_schedule(&cmd->script_timeout_work.dwork, K_SECONDS(cmd->script->timeout));
	}
}

static void modem_chat_script_run_handler(struct k_work *item)
{
	struct modem_chat_script_run_work_item *script_run_work =
		(struct modem_chat_script_run_work_item *)item;

	struct modem_chat *cmd = script_run_work->cmd;
	const struct modem_chat_script *script = script_run_work->script;

	/* Start script */
	modem_chat_script_start(cmd, script);
}

static void modem_chat_script_timeout_handler(struct k_work *item)
{
	struct modem_chat_work_item *script_timeout_work = (struct modem_chat_work_item *)item;
	struct modem_chat *cmd = script_timeout_work->cmd;

	/* Abort script */
	modem_chat_script_stop(cmd, MODEM_CHAT_SCRIPT_RESULT_TIMEOUT);
}

static void modem_chat_script_abort_handler(struct k_work *item)
{
	struct modem_chat_script_abort_work_item *script_abort_work =
		(struct modem_chat_script_abort_work_item *)item;

	struct modem_chat *cmd = script_abort_work->cmd;

	/* Validate script is currently running */
	if (cmd->script == NULL) {
		return;
	}

	/* Abort script */
	modem_chat_script_stop(cmd, MODEM_CHAT_SCRIPT_RESULT_ABORT);
}

static bool modem_chat_script_send_request(struct modem_chat *cmd)
{
	const struct modem_chat_script_cmd *script_cmd =
		&cmd->script->script_cmds[cmd->script_cmd_it];

	uint16_t script_cmd_request_size = strlen(script_cmd->request);
	uint8_t *script_cmd_request_start;
	uint16_t script_cmd_request_remaining;
	int ret;

	/* Validate data to send */
	if (script_cmd_request_size == cmd->script_send_request_pos) {
		return true;
	}

	script_cmd_request_start = (uint8_t *)&script_cmd->request[cmd->script_send_request_pos];
	script_cmd_request_remaining = script_cmd_request_size - cmd->script_send_request_pos;

	/* Send data through pipe */
	ret = modem_pipe_transmit(cmd->pipe, script_cmd_request_start,
				  script_cmd_request_remaining);

	/* Validate transmit successful */
	if (ret < 1) {
		return false;
	}

	/* Update script send position */
	cmd->script_send_request_pos += (uint16_t)ret;

	/* Check if data remains */
	if (cmd->script_send_request_pos < script_cmd_request_size) {
		return false;
	}

	return true;
}

static bool modem_chat_script_send_delimiter(struct modem_chat *cmd)
{
	uint8_t *script_cmd_delimiter_start;
	uint16_t script_cmd_delimiter_remaining;
	int ret;

	/* Validate data to send */
	if (cmd->delimiter_size == cmd->script_send_delimiter_pos) {
		return true;
	}

	script_cmd_delimiter_start = (uint8_t *)&cmd->delimiter[cmd->script_send_delimiter_pos];
	script_cmd_delimiter_remaining = cmd->delimiter_size - cmd->script_send_delimiter_pos;

	/* Send data through pipe */
	ret = modem_pipe_transmit(cmd->pipe, script_cmd_delimiter_start,
				  script_cmd_delimiter_remaining);

	/* Validate transmit successful */
	if (ret < 1) {
		return false;
	}

	/* Update script send position */
	cmd->script_send_delimiter_pos += (uint16_t)ret;

	/* Check if data remains */
	if (cmd->script_send_delimiter_pos < cmd->delimiter_size) {
		return false;
	}

	return true;
}

static bool modem_chat_script_cmd_is_no_response(struct modem_chat *cmd)
{
	const struct modem_chat_script_cmd *script_cmd =
		&cmd->script->script_cmds[cmd->script_cmd_it];

	return (script_cmd->response_matches_size == 0) ? true : false;
}

static void modem_chat_script_send_handler(struct k_work *item)
{
	struct modem_chat_work_item *send_work = (struct modem_chat_work_item *)item;
	struct modem_chat *cmd = send_work->cmd;

	/* Validate script running */
	if (cmd->script == NULL) {
		return;
	}

	/* Send request */
	if (modem_chat_script_send_request(cmd) == false) {
		k_work_schedule(&cmd->script_send_work.dwork, cmd->process_timeout);

		return;
	}

	/* Send delimiter */
	if (modem_chat_script_send_delimiter(cmd) == false) {
		k_work_schedule(&cmd->script_send_work.dwork, cmd->process_timeout);

		return;
	}

	/* Check if script command is no response */
	if (modem_chat_script_cmd_is_no_response(cmd)) {
		modem_chat_script_next(cmd, false);
	}
}

static void modem_chat_parse_reset(struct modem_chat *cmd)
{
	/* Reset parameters used for parsing */
	cmd->receive_buf_len = 0;
	cmd->delimiter_match_len = 0;
	cmd->argc = 0;
	cmd->parse_match = NULL;
}

/* Exact match is stored at end of receive buffer */
static void modem_chat_parse_save_match(struct modem_chat *cmd)
{
	uint8_t *argv;

	/* Store length of match including NULL to avoid overwriting it if buffer overruns */
	cmd->parse_match_len = cmd->receive_buf_len + 1;

	/* Copy match to end of receive buffer */
	argv = &cmd->receive_buf[cmd->receive_buf_size - cmd->parse_match_len];

	/* Copy match to end of receive buffer (excluding NULL) */
	memcpy(argv, &cmd->receive_buf[0], cmd->parse_match_len - 1);

	/* Save match */
	cmd->argv[cmd->argc] = argv;

	/* Terminate match */
	cmd->receive_buf[cmd->receive_buf_size - 1] = '\0';

	/* Increment argument count */
	cmd->argc++;
}

static bool modem_chat_match_matches_received(struct modem_chat *cmd,
					     const struct modem_chat_match *match)
{
	for (uint16_t i = 0; i < match->match_size; i++) {
		if ((match->match[i] == cmd->receive_buf[i]) ||
		    (match->wildcards == true && match->match[i] == '?')) {
			continue;
		}

		return false;
	}

	return true;
}

static bool modem_chat_parse_find_match(struct modem_chat *cmd)
{
	/* Find in all matches types */
	for (uint16_t i = 0; i < ARRAY_SIZE(cmd->matches); i++) {
		/* Find in all matches of matches type */
		for (uint16_t u = 0; u < cmd->matches_size[i]; u++) {
			/* Validate match size matches received data length */
			if (cmd->matches[i][u].match_size != cmd->receive_buf_len) {
				continue;
			}

			/* Validate match */
			if (modem_chat_match_matches_received(cmd, &cmd->matches[i][u]) == false) {
				continue;
			}

			/* Complete match found */
			cmd->parse_match = &cmd->matches[i][u];
			cmd->parse_match_type = i;

			return true;
		}
	}

	return false;
}

static bool modem_chat_parse_is_separator(struct modem_chat *cmd)
{
	for (uint16_t i = 0; i < cmd->parse_match->separators_size; i++) {
		if ((cmd->parse_match->separators[i]) ==
		    (cmd->receive_buf[cmd->receive_buf_len - 1])) {
			return true;
		}
	}

	return false;
}

static bool modem_chat_parse_end_del_start(struct modem_chat *cmd)
{
	for (uint16_t i = 0; i < cmd->delimiter_size; i++) {
		if (cmd->receive_buf[cmd->receive_buf_len - 1] == cmd->delimiter[i]) {
			return true;
		}
	}

	return false;
}

static bool modem_chat_parse_end_del_complete(struct modem_chat *cmd)
{
	/* Validate length of end delimiter */
	if (cmd->receive_buf_len < cmd->delimiter_size) {
		return false;
	}

	/* Compare end delimiter with receive buffer content */
	return (memcmp(&cmd->receive_buf[cmd->receive_buf_len - cmd->delimiter_size],
		       cmd->delimiter, cmd->delimiter_size) == 0)
		       ? true
		       : false;
}

static void modem_chat_on_command_received_unsol(struct modem_chat *cmd)
{
	/* Callback */
	if (cmd->parse_match->callback != NULL) {
		cmd->parse_match->callback(cmd, (char **)cmd->argv, cmd->argc, cmd->user_data);
	}
}

static void modem_chat_on_command_received_abort(struct modem_chat *cmd)
{
	/* Callback */
	if (cmd->parse_match->callback != NULL) {
		cmd->parse_match->callback(cmd, (char **)cmd->argv, cmd->argc, cmd->user_data);
	}

	/* Abort script */
	modem_chat_script_stop(cmd, MODEM_CHAT_SCRIPT_RESULT_ABORT);
}

static void modem_chat_on_command_received_resp(struct modem_chat *cmd)
{
	/* Callback */
	if (cmd->parse_match->callback != NULL) {
		cmd->parse_match->callback(cmd, (char **)cmd->argv, cmd->argc, cmd->user_data);
	}

	/* Advance script */
	modem_chat_script_next(cmd, false);
}

static bool modem_chat_parse_find_catch_all_match(struct modem_chat *cmd)
{
	/* Find in all matches types */
	for (uint16_t i = 0; i < ARRAY_SIZE(cmd->matches); i++) {
		/* Find in all matches of matches type */
		for (uint16_t u = 0; u < cmd->matches_size[i]; u++) {
			/* Validate match config is matching previous bytes */
			if (cmd->matches[i][u].match_size == 0) {
				cmd->parse_match = &cmd->matches[i][u];
				cmd->parse_match_type = i;

				return true;
			}
		}
	}

	return false;
}

static void modem_chat_on_command_received(struct modem_chat *cmd)
{
	LOG_DBG("\"%s\"", cmd->argv[0]);

	switch (cmd->parse_match_type) {
	case MODEM_CHAT_MATCHES_INDEX_UNSOL:
		modem_chat_on_command_received_unsol(cmd);
		break;

	case MODEM_CHAT_MATCHES_INDEX_ABORT:
		modem_chat_on_command_received_abort(cmd);
		break;

	case MODEM_CHAT_MATCHES_INDEX_RESPONSE:
		modem_chat_on_command_received_resp(cmd);
		break;
	}
}

static void modem_chat_on_unknown_command_received(struct modem_chat *cmd)
{
	/* Try to find catch all match */
	if (modem_chat_parse_find_catch_all_match(cmd) == false) {
		return;
	}

	/* Terminate received command */
	cmd->receive_buf[cmd->receive_buf_len - cmd->delimiter_size] = '\0';

	/* Parse command */
	cmd->argv[0] = "";
	cmd->argv[1] = cmd->receive_buf;
	cmd->argc = 2;

	/* Invoke on response received */
	modem_chat_on_command_received(cmd);
}

static void modem_chat_process_byte(struct modem_chat *cmd, uint8_t byte)
{
	/* Validate receive buffer not overrun */
	if (cmd->receive_buf_size == cmd->receive_buf_len) {
		LOG_WRN("Receive buffer overrun");
		modem_chat_parse_reset(cmd);

		return;
	}

	/* Validate argv buffer not overrun */
	if (cmd->argc == cmd->argv_size) {
		LOG_WRN("Argv buffer overrun");
		modem_chat_parse_reset(cmd);

		return;
	}

	/* Copy byte to receive buffer */
	cmd->receive_buf[cmd->receive_buf_len] = byte;
	cmd->receive_buf_len++;

	/* Validate end delimiter not complete */
	if (modem_chat_parse_end_del_complete(cmd) == true) {
		/* Filter out empty lines */
		if (cmd->receive_buf_len == cmd->delimiter_size) {
			/* Reset parser */
			modem_chat_parse_reset(cmd);

			return;
		}

		/* Check if match exists */
		if (cmd->parse_match == NULL) {
			/* Handle unknown command */
			modem_chat_on_unknown_command_received(cmd);

			/* Reset parser */
			modem_chat_parse_reset(cmd);

			return;
		}

		/* Check if trailing argument exists */
		if (cmd->parse_arg_len > 0) {
			cmd->argv[cmd->argc] =
				&cmd->receive_buf[cmd->receive_buf_len - cmd->delimiter_size -
						  cmd->parse_arg_len];
			cmd->receive_buf[cmd->receive_buf_len - cmd->delimiter_size] = '\0';
			cmd->argc++;
		}

		/* Handle received command */
		modem_chat_on_command_received(cmd);

		/* Reset parser */
		modem_chat_parse_reset(cmd);

		return;
	}

	/* Validate end delimiter not started */
	if (modem_chat_parse_end_del_start(cmd) == true) {
		return;
	}

	/* Find matching command if missing */
	if (cmd->parse_match == NULL) {
		/* Find matching command */
		if (modem_chat_parse_find_match(cmd) == false) {
			return;
		}

		/* Save match */
		modem_chat_parse_save_match(cmd);

		/* Prepare argument parser */
		cmd->parse_arg_len = 0;

		return;
	}

	/* Check if separator reached */
	if (modem_chat_parse_is_separator(cmd) == true) {
		/* Check if argument is empty */
		if (cmd->parse_arg_len == 0) {
			/* Save empty argument */
			cmd->argv[cmd->argc] = "";
		} else {
			/* Save pointer to start of argument */
			cmd->argv[cmd->argc] =
				&cmd->receive_buf[cmd->receive_buf_len - cmd->parse_arg_len - 1];

			/* Replace separator with string terminator */
			cmd->receive_buf[cmd->receive_buf_len - 1] = '\0';
		}

		/* Increment argument count */
		cmd->argc++;

		/* Reset parse argument length */
		cmd->parse_arg_len = 0;

		return;
	}

	/* Increment argument length */
	cmd->parse_arg_len++;
}

/* Process chunk of received bytes */
static void modem_chat_process_bytes(struct modem_chat *cmd)
{
	for (uint16_t i = 0; i < cmd->work_buf_len; i++) {
		modem_chat_process_byte(cmd, cmd->work_buf[i]);
	}
}

static void modem_chat_process_handler(struct k_work *item)
{
	struct modem_chat_work_item *process_work = (struct modem_chat_work_item *)item;
	struct modem_chat *cmd = process_work->cmd;
	int ret;

	/* Fill work buffer */
	ret = modem_pipe_receive(cmd->pipe, cmd->work_buf, sizeof(cmd->work_buf));

	/* Validate data received */
	if (ret < 1) {
		return;
	}

	/* Save received data length */
	cmd->work_buf_len = (size_t)ret;

	/* Process data */
	modem_chat_process_bytes(cmd);

	/* Schedule receive handler if data remains */
	if (cmd->work_buf_len == sizeof(cmd->work_buf)) {
		k_work_schedule(&cmd->process_work.dwork, K_NO_WAIT);
	}
}

static void modem_chat_pipe_event_handler(struct modem_pipe *pipe, enum modem_pipe_event event,
					 void *user_data)
{
	struct modem_chat *cmd = (struct modem_chat *)user_data;

	k_work_schedule(&cmd->process_work.dwork, cmd->process_timeout);
}

/*********************************************************
 * GLOBAL FUNCTIONS
 *********************************************************/
int modem_chat_init(struct modem_chat *cmd, const struct modem_chat_config *config)
{
	/* Validate arguments */
	if ((cmd == NULL) || (config == NULL)) {
		return -EINVAL;
	}

	/* Validate config */
	if ((config->receive_buf == NULL) || (config->receive_buf_size == 0) ||
	    (config->argv == NULL) || (config->argv_size == 0) || (config->delimiter == NULL) ||
	    (config->delimiter_size == 0) || (config->unsol_matches == NULL) ||
	    (config->unsol_matches_size == 0)) {
		return -EINVAL;
	}

	/* Clear context */
	memset(cmd, 0x00, sizeof(*cmd));

	/* Configure command handler */
	cmd->pipe = NULL;
	cmd->user_data = config->user_data;
	cmd->receive_buf = config->receive_buf;
	cmd->receive_buf_size = config->receive_buf_size;
	cmd->argv = config->argv;
	cmd->argv_size = config->argv_size;
	cmd->delimiter = config->delimiter;
	cmd->delimiter_size = config->delimiter_size;
	cmd->matches[MODEM_CHAT_MATCHES_INDEX_UNSOL] = config->unsol_matches;
	cmd->matches_size[MODEM_CHAT_MATCHES_INDEX_UNSOL] = config->unsol_matches_size;
	cmd->process_timeout = config->process_timeout;

	/* Initial script state */
	atomic_set(&cmd->script_state, 0);

	/* Initialize work items */
	cmd->process_work.cmd = cmd;
	k_work_init_delayable(&cmd->process_work.dwork, modem_chat_process_handler);

	cmd->script_run_work.cmd = cmd;
	k_work_init(&cmd->script_run_work.work, modem_chat_script_run_handler);

	cmd->script_timeout_work.cmd = cmd;
	k_work_init_delayable(&cmd->script_timeout_work.dwork, modem_chat_script_timeout_handler);

	cmd->script_abort_work.cmd = cmd;
	k_work_init(&cmd->script_abort_work.work, modem_chat_script_abort_handler);

	cmd->script_send_work.cmd = cmd;
	k_work_init_delayable(&cmd->script_send_work.dwork, modem_chat_script_send_handler);

	return 0;
}

int modem_chat_attach(struct modem_chat *cmd, struct modem_pipe *pipe)
{
	/* Validate arguments */
	if ((cmd == NULL) || (pipe == NULL)) {
		return -EINVAL;
	}

	/* Associate command handler with pipe */
	cmd->pipe = pipe;

	/* Reset parser */
	modem_chat_parse_reset(cmd);

	/* Set pipe event handler */
	return modem_pipe_event_handler_set(pipe, modem_chat_pipe_event_handler, cmd);
}

int modem_chat_script_run(struct modem_chat *cmd, const struct modem_chat_script *script)
{
	bool script_is_running;

	/* Validate arguments */
	if ((cmd == NULL) || (script == NULL)) {
		return -EINVAL;
	}

	/* Validate attached */
	if (cmd->pipe == NULL) {
		return -EPERM;
	}

	/* Validate script */
	if ((script->script_cmds == NULL) || (script->script_cmds_size == 0) ||
	    ((script->abort_matches != NULL) && (script->abort_matches_size == 0))) {
		return -EINVAL;
	}

	/* Validate script commands */
	for (uint16_t i = 0; i < script->script_cmds_size; i++) {
		if ((strlen(script->script_cmds[i].request) == 0) &&
		    (script->script_cmds[i].response_matches_size == 0)) {
			return -EINVAL;
		}
	}

	/* Test if script is running */
	script_is_running = atomic_test_and_set_bit(&cmd->script_state,
						    MODEM_CHAT_SCRIPT_STATE_RUNNING_BIT);

	/* Validate script not running */
	if (script_is_running == true) {
		return -EBUSY;
	}

	/* Initialize work item data */
	cmd->script_run_work.script = script;

	/* Submit script run work */
	k_work_submit(&cmd->script_run_work.work);

	return 0;
}

void modem_chat_script_abort(struct modem_chat *cmd)
{
	/* Submit script abort work */
	k_work_submit(&cmd->script_abort_work.work);
}

int modem_chat_release(struct modem_chat *cmd)
{
	/* Verify attached */
	if (cmd->pipe == NULL) {
		return 0;
	}

	/* Release pipe */
	modem_pipe_event_handler_set(cmd->pipe, NULL, NULL);

	/* Cancel all work */
	struct k_work_sync sync;
	k_work_cancel_sync(&cmd->script_run_work.work, &sync);
	k_work_cancel_sync(&cmd->script_abort_work.work, &sync);
	k_work_cancel_delayable_sync(&cmd->process_work.dwork, &sync);
	k_work_cancel_delayable_sync(&cmd->script_send_work.dwork, &sync);

	/* Unreference pipe */
	cmd->pipe = NULL;

	return 0;
}
