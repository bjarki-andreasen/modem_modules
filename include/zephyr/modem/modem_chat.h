/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/sys/ring_buffer.h>

#include <zephyr/modem/modem_pipe.h>

#ifndef ZEPHYR_MODEM_MODEM_CHAT_
#define ZEPHYR_MODEM_MODEM_CHAT_

#ifdef __cplusplus
extern "C" {
#endif

struct modem_chat;

/**
 * @brief Callback called when matching command is received
 *
 * @param chat Pointer to command handler instance
 * @param argv Pointer to array of parsed arguments
 * @param argc Number of parsed arguments, arg 0 holds the exact match
 * @param user_data Free to use user data set during modem_chat_init()
 */
typedef void (*modem_chat_match_callback)(struct modem_chat *chat, char **argv, uint16_t argc,
					  void *user_data);

/**
 * @brief Modem command match
 */
struct modem_chat_match {
	/* Match array */
	const uint8_t *match;
	const uint8_t match_size;

	/* Separators array */
	const uint8_t *separators;
	const uint8_t separators_size;

	/* Set if modem command handler shall use wildcards when matching */
	const bool wildcards;

	/* Type of modem command handler */
	const modem_chat_match_callback callback;
};

#define MODEM_CHAT_MATCH(_match, _separators, _callback)                                           \
	{                                                                                          \
		.match = (uint8_t *)(_match), .match_size = (uint8_t)(sizeof(_match) - 1),         \
		.separators = (uint8_t *)(_separators),                                            \
		.separators_size = (uint8_t)(sizeof(_separators) - 1), .wildcards = false,         \
		.callback = _callback,                                                             \
	}

#define MODEM_CHAT_MATCH_WILDCARD(_match, _separators, _callback)                                  \
	{                                                                                          \
		.match = (uint8_t *)(_match), .match_size = (uint8_t)(sizeof(_match) - 1),         \
		.separators = (uint8_t *)(_separators),                                            \
		.separators_size = (uint8_t)(sizeof(_separators) - 1), .wildcards = true,          \
		.callback = _callback,                                                             \
	}

#define MODEM_CHAT_MATCH_DEFINE(_sym, _match, _separators, _callback)                              \
	const static struct modem_chat_match _sym = MODEM_CHAT_MATCH(_match, _separators, _callback)

#define MODEM_CHAT_MATCHES_DEFINE(_sym, ...)                                                       \
	const static struct modem_chat_match _sym[] = {__VA_ARGS__}

/**
 * @brief Modem command script command
 *
 * @param request Request to send to modem formatted as char string
 * @param response_matches Expected responses to request
 * @param response_matches_size Elements in expected responses
 * @param timeout Timeout before chat script may continue to next step
 */
struct modem_chat_script_chat {
	const char *request;
	const struct modem_chat_match *const response_matches;
	const uint16_t response_matches_size;
	uint16_t timeout;
};

#define MODEM_CHAT_SCRIPT_CMD_RESP(_request, _response_match)                                      \
	{                                                                                          \
		.request = _request, .response_matches = &_response_match,                         \
		.response_matches_size = 1, .timeout = 0,                                          \
	}

#define MODEM_CHAT_SCRIPT_CMD_RESP_MULT(_request, _response_matches)                               \
	{                                                                                          \
		.request = _request, .response_matches = _response_matches,                        \
		.response_matches_size = ARRAY_SIZE(_response_matches), .timeout = 0,              \
	}

#define MODEM_CHAT_SCRIPT_CMD_RESP_NONE(_request, _timeout)                                        \
	{                                                                                          \
		.request = _request, .response_matches = NULL, .response_matches_size = 0,         \
		.timeout = _timeout,                                                               \
	}

#define MODEM_CHAT_SCRIPT_CMDS_DEFINE(_sym, ...)                                                   \
	const static struct modem_chat_script_chat _sym[] = {__VA_ARGS__}

enum modem_chat_script_result {
	MODEM_CHAT_SCRIPT_RESULT_SUCCESS,
	MODEM_CHAT_SCRIPT_RESULT_ABORT,
	MODEM_CHAT_SCRIPT_RESULT_TIMEOUT
};

/**
 * @brief Callback called when script command is received
 *
 * @param chat Pointer to command handler instance
 * @param result Result of script execution
 * @param user_data Free to use user data set during modem_chat_init()
 */
typedef void (*modem_chat_script_callback)(struct modem_chat *chat,
					   enum modem_chat_script_result result, void *user_data);

/**
 * @brief Modem command script
 *
 * @param name Name of script
 * @param name_size Size of name of script
 * @param script_chats Array of script commands
 * @param script_chats_size Elements in array of script commands
 * @param abort_matches Array of abort matches
 * @param abort_matches_size Elements in array of abort matches
 * @param callback Callback called when script execution terminates
 * @param timeout Timeout in seconds within which the script execution must terminate
 */
struct modem_chat_script {
	const char *name;
	const struct modem_chat_script_chat *script_chats;
	const uint16_t script_chats_size;
	const struct modem_chat_match *const abort_matches;
	const uint16_t abort_matches_size;
	const modem_chat_script_callback callback;
	const uint32_t timeout;
};

#define MODEM_CHAT_SCRIPT_DEFINE(_sym, _script_chats, _abort_matches, _callback, _timeout)         \
	static struct modem_chat_script _sym = {                                                   \
		.name = #_sym,                                                                     \
		.script_chats = _script_chats,                                                     \
		.script_chats_size = ARRAY_SIZE(_script_chats),                                    \
		.abort_matches = _abort_matches,                                                   \
		.abort_matches_size = ARRAY_SIZE(_abort_matches),                                  \
		.callback = _callback,                                                             \
		.timeout = _timeout,                                                               \
	}

/**
 * @brief Process work item
 *
 * @note k_work struct must be placed first
 */
struct modem_chat_work_item {
	struct k_work_delayable dwork;
	struct modem_chat *chat;
};

/**
 * @brief Script run work item
 *
 * @param work Work item
 * @param chat Modem command instance
 * @param script Pointer to new script to run
 *
 * @note k_work struct must be placed first
 */
struct modem_chat_script_run_work_item {
	struct k_work work;
	struct modem_chat *chat;
	const struct modem_chat_script *script;
};

/**
 * @brief Script abort work item
 *
 * @param work Work item
 * @param chat Modem command instance
 *
 * @note k_work struct must be placed first
 */
struct modem_chat_script_abort_work_item {
	struct k_work work;
	struct modem_chat *chat;
};

enum modem_chat_script_send_state {
	/* No data to send */
	MODEM_CHAT_SCRIPT_SEND_STATE_IDLE,
	/* Sending request */
	MODEM_CHAT_SCRIPT_SEND_STATE_REQUEST,
	/* Sending delimiter */
	MODEM_CHAT_SCRIPT_SEND_STATE_DELIMITER,
};

/**
 * @brief Modem command internal context
 * @warning Do not mopdify any members of this struct directly
 */
struct modem_chat {
	/* Pipe used to send and receive data */
	struct modem_pipe *pipe;

	/* User data passed with match callbacks */
	void *user_data;

	/* Receive buffer */
	uint8_t *receive_buf;
	uint16_t receive_buf_size;
	uint16_t receive_buf_len;

	/* Work buffer */
	uint8_t work_buf[32];
	uint16_t work_buf_len;

	/* Command delimiter */
	uint8_t *delimiter;
	uint16_t delimiter_size;
	uint16_t delimiter_match_len;

	/* Array of bytes which are discarded out by parser */
	uint8_t *filter;
	uint16_t filter_size;

	/* Parsed arguments */
	uint8_t **argv;
	uint16_t argv_size;
	uint16_t argc;

	/* Matches
	 * Index 0 -> Response matches
	 * Index 1 -> Abort matches
	 * Index 2 -> Unsolicited matches
	 */
	const struct modem_chat_match *matches[3];
	uint16_t matches_size[3];

	/* Script execution */
	const struct modem_chat_script *script;
	struct modem_chat_script_run_work_item script_run_work;
	struct modem_chat_work_item script_timeout_work;
	struct modem_chat_script_abort_work_item script_abort_work;
	uint16_t script_chat_it;
	atomic_t script_state;

	/* Script sending */
	uint16_t script_send_request_pos;
	uint16_t script_send_delimiter_pos;
	struct modem_chat_work_item script_send_work;
	struct modem_chat_work_item script_send_timeout_work;

	/* Match parsing */
	const struct modem_chat_match *parse_match;
	uint16_t parse_match_len;
	uint16_t parse_arg_len;
	uint16_t parse_match_type;

	/* Process received data */
	struct modem_chat_work_item process_work;
	k_timeout_t process_timeout;
};

/**
 * @brief Modem command configuration
 *
 * @param user_data Free to use data passed with modem match callbacks
 * @param receive_buf Receive buffer used to store parsed arguments
 * @param receive_buf_size Size of receive buffer should be longest line + longest match
 * @param delimiter Delimiter
 * @param delimiter_size Size of delimiter
 * @param filter Bytes which are discarded by parser
 * @param filter_size Size of filter
 * @param argv Array of pointers used to point to parsed arguments
 * @param argv_size Elements in array of pointers
 * @param unsol_matches Array of unsolicited matches
 * @param unsol_matches_size Elements in array of unsolicited matches
 * @param process_timeout Delay from receive ready event to pipe receive occurs
 */
struct modem_chat_config {
	void *user_data;
	uint8_t *receive_buf;
	uint16_t receive_buf_size;
	uint8_t *delimiter;
	uint8_t delimiter_size;
	uint8_t *filter;
	uint8_t filter_size;
	uint8_t **argv;
	uint16_t argv_size;
	const struct modem_chat_match *unsol_matches;
	uint16_t unsol_matches_size;
	k_timeout_t process_timeout;
};

/**
 * @brief Initialize modem pipe command handler
 * @note Command handler must be attached to pipe
 */
int modem_chat_init(struct modem_chat *chat, const struct modem_chat_config *config);

/**
 * @brief Attach modem command handler to pipe
 *
 * @returns 0 if successful
 * @returns negative errno code if failure
 *
 * @note Command handler is enabled if successful
 */
int modem_chat_attach(struct modem_chat *chat, struct modem_pipe *pipe);

/**
 * @brief Run script
 *
 * @param chat Modem command instance
 * @param script Script to run
 *
 * @returns 0 if successful
 * @returns -EBUSY if a script is currently running
 * @returns -EPERM if modem pipe is not attached
 * @returns -EINVAL if arguments or script is invalid
 *
 * @note Script runs asynchronously until complete or aborted.
 * @note Use modem_chat_script_wait() to synchronize with script termination
 */
int modem_chat_script_run(struct modem_chat *chat, const struct modem_chat_script *script);

/**
 * @brief Abort script
 *
 * @param chat Modem command instance
 *
 * @note Use modem_chat_script_wait() to synchronize with script termination
 */
void modem_chat_script_abort(struct modem_chat *chat);

/**
 * @brief Release pipe from command handler
 * @note Command handler is now disabled
 */
void modem_chat_release(struct modem_chat *chat);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_MODEM_MODEM_CHAT_ */
