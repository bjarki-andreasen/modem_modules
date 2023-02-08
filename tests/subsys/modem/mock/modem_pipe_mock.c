
#include "modem_pipe_mock.h"

#include <string.h>

static int modem_pipe_mock_pipe_callback_set(struct modem_pipe *pipe,
						  modem_pipe_callback handler,
						  void *user_data)
{
	struct modem_pipe_mock *mock = (struct modem_pipe_mock *)pipe->data;

	/* Validate pipe belongs to this context */
	if ((mock->pipe != pipe)) {
		return -EPERM;
	}

	mock->pipe_callback = handler;
	mock->pipe_callback_user_data = user_data;

	return 0;
}

static int modem_pipe_mock_pipe_transmit(struct modem_pipe *pipe, const uint8_t *buf, size_t size)
{
	struct modem_pipe_mock *mock = (struct modem_pipe_mock *)pipe->data;

	return ring_buf_put(&mock->tx_rb, buf, size);
}

static int modem_pipe_mock_pipe_receive(struct modem_pipe *pipe, uint8_t *buf, size_t size)
{
	struct modem_pipe_mock *mock = (struct modem_pipe_mock *)pipe->data;

	return ring_buf_get(&mock->rx_rb, buf, size);
}

struct modem_pipe_driver_api modem_pipe_mock_api = {
	.callback_set = modem_pipe_mock_pipe_callback_set,
	.transmit = modem_pipe_mock_pipe_transmit,
	.receive = modem_pipe_mock_pipe_receive,
};

static void modem_pipe_mock_received_handler(struct k_work *item)
{
	struct modem_pipe_mock_work *mock_work_item = (struct modem_pipe_mock_work *)item;
	struct modem_pipe_mock *mock = mock_work_item->mock;

	if (mock->pipe_callback == NULL) {
		return;
	}

	mock->pipe_callback(mock->pipe, MODEM_PIPE_EVENT_RECEIVE_READY,
				 mock->pipe_callback_user_data);
}

int modem_pipe_mock_init(struct modem_pipe_mock *mock, const struct modem_pipe_mock_config *config)
{
	memset(mock, 0, sizeof(*mock));

	ring_buf_init(&mock->rx_rb, config->rx_buf_size, config->rx_buf);
	ring_buf_init(&mock->tx_rb, config->tx_buf_size, config->tx_buf);

	mock->received_work_item.mock = mock;
	k_work_init(&mock->received_work_item.work, modem_pipe_mock_received_handler);

	return 0;
}

int modem_pipe_mock_open(struct modem_pipe_mock *mock, struct modem_pipe *pipe)
{
	/* Configure pipe */
	pipe->data = mock;
	pipe->api = &modem_pipe_mock_api;

	/* Store pipe */
	mock->pipe = pipe;

	return 0;
}

int modem_pipe_mock_close(struct modem_pipe *pipe)
{
	struct modem_pipe_mock *mock = (struct modem_pipe_mock *)pipe->data;

	/* Clear pipe */
	memset(pipe, 0x00, sizeof(*pipe));

	/* Update context */
	mock->pipe = NULL;

	return 0;
}

int modem_pipe_mock_reset(struct modem_pipe_mock *mock)
{
	ring_buf_reset(&mock->rx_rb);
	ring_buf_reset(&mock->tx_rb);

	return 0;
}

int modem_pipe_mock_get(struct modem_pipe_mock *mock, uint8_t *buf, size_t size)
{
	return ring_buf_get(&mock->tx_rb, buf, size);
}

int modem_pipe_mock_put(struct modem_pipe_mock *mock, const uint8_t *buf, size_t size)
{
	int ret;

	ret = ring_buf_put(&mock->rx_rb, buf, size);

	k_work_submit(&mock->received_work_item.work);

	return ret;
}
