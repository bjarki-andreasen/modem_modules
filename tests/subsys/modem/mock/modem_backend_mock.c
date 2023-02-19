
#include "modem_backend_mock.h"

#include <string.h>

static int modem_backend_mock_open(void *data)
{
	struct modem_backend_mock *mock = (struct modem_backend_mock *)data;

	modem_pipe_notify_opened(&mock->pipe);

	return 0;
}

static int modem_backend_mock_transmit(void *data, const uint8_t *buf, size_t size)
{
	struct modem_backend_mock *mock = (struct modem_backend_mock *)data;

	size = (mock->limit < size) ? mock->limit : size;

	return ring_buf_put(&mock->tx_rb, buf, size);
}

static int modem_backend_mock_receive(void *data, uint8_t *buf, size_t size)
{
	struct modem_backend_mock *mock = (struct modem_backend_mock *)data;

	size = (mock->limit < size) ? mock->limit : size;

	return ring_buf_get(&mock->rx_rb, buf, size);
}

static int modem_backend_mock_close(void *data)
{
	struct modem_backend_mock *mock = (struct modem_backend_mock *)data;

	modem_pipe_notify_closed(&mock->pipe);

	return 0;
}

struct modem_pipe_api modem_backend_mock_api = {
	.open = modem_backend_mock_open,
	.transmit = modem_backend_mock_transmit,
	.receive = modem_backend_mock_receive,
	.close = modem_backend_mock_close,
};

static void modem_backend_mock_received_handler(struct k_work *item)
{
	struct modem_backend_mock_work *mock_work_item = (struct modem_backend_mock_work *)item;
	struct modem_backend_mock *mock = mock_work_item->mock;

	modem_pipe_notify_receive_ready(&mock->pipe);
}

struct modem_pipe *modem_backend_mock_init(struct modem_backend_mock *mock,
					   const struct modem_backend_mock_config *config)
{
	memset(mock, 0, sizeof(*mock));

	ring_buf_init(&mock->rx_rb, config->rx_buf_size, config->rx_buf);
	ring_buf_init(&mock->tx_rb, config->tx_buf_size, config->tx_buf);

	mock->received_work_item.mock = mock;
	k_work_init(&mock->received_work_item.work, modem_backend_mock_received_handler);

	mock->limit = config->limit;

	modem_pipe_init(&mock->pipe, mock, &modem_backend_mock_api);

	return &mock->pipe;
}

struct modem_pipe *modem_backend_mock_get_pipe(struct modem_backend_mock *mock)
{
	return &mock->pipe;
}

void modem_backend_mock_reset(struct modem_backend_mock *mock)
{
	ring_buf_reset(&mock->rx_rb);
	ring_buf_reset(&mock->tx_rb);
}

int modem_backend_mock_get(struct modem_backend_mock *mock, uint8_t *buf, size_t size)
{
	return ring_buf_get(&mock->tx_rb, buf, size);
}

void modem_backend_mock_put(struct modem_backend_mock *mock, const uint8_t *buf, size_t size)
{
	size_t remaining;
	size_t written;

	remaining = size;

	for (size_t i = 0; i < size; i++) {
		written = ring_buf_put(&mock->rx_rb, &buf[size - remaining], remaining);

		remaining -= written;

		k_work_submit(&mock->received_work_item.work);

		if (remaining == 0) {
			return;
		}

		k_msleep(10);
	}

	__ASSERT_NO_MSG(false);
}
