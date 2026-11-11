/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * virtio/tests/test_mock.c - the simulated transport, end to end (VIO-10,
 * P9-10).
 *
 * The point of the mock is that a whole driver stack - status sequence, feature
 * negotiation, tag selection, queue setup, submission, completion, reset - runs
 * on the PC with no hardware. These tests walk exactly that path, once
 * synchronously (a device that answers on the kick) and once asynchronously (a
 * device that answers when the test steps it).
 */
#include "pgw/test.h"
#include "pgw/virtio.h"
#include "virtio_priv.h"

#include <string.h>

/* A tiny "9P-like" device: it reads a request from the readable buffers, writes
 * a fixed answer into the writable ones and counts the calls. */
typedef struct echo_dev {
    uint32_t calls;
    uint32_t last_in_len;
} echo_dev;

static uint32_t echo_fn(const pgw_vio_sg *in, size_t n_in,
                        const pgw_vio_sg *out, size_t n_out, void *user)
{
    echo_dev *e = (echo_dev *)user;
    e->calls++;
    e->last_in_len = 0;
    for (size_t i = 0; i < n_in; i++)
        e->last_in_len += in[i].len;
    if (n_out == 0 || out[0].len < 4u)
        return 0;
    static const char answer[] = "pong";
    memcpy(out[0].addr, answer, 4u);
    return 4u;
}

static void test_open_reports_identity(void)
{
    pgw_vio_dev *dev = pgw_vio_mock_open(PGW_VIRTIO_ID_9P, "myshare", 2u,
                                         PGW_VIO_F_VERSION_1, NULL, NULL);
    PGW_ASSERT(dev != NULL);
    const pgw_vio_info *info = pgw_vio_get_info(dev);
    PGW_ASSERT(info != NULL);
    PGW_ASSERT_EQ_U(info->device_id, PGW_VIRTIO_ID_9P);
    PGW_ASSERT_EQ_U(info->vendor_id, PGW_VIRTIO_PCI_VENDOR);
    PGW_ASSERT_EQ_U(info->bus_kind, 0u);
    PGW_ASSERT_EQ_U(info->num_queues, 2u);
    PGW_ASSERT_STR_EQ(info->tag, "myshare");
    pgw_vio_destroy(dev);
}

static void test_open_refuses_bad_arguments(void)
{
    PGW_ASSERT(pgw_vio_mock_open(PGW_VIRTIO_ID_9P, NULL, 1u, 0, NULL, NULL) ==
               NULL);
    PGW_ASSERT(pgw_vio_mock_open(PGW_VIRTIO_ID_9P, "", 1u, 0, NULL, NULL) ==
               NULL);
    /* Not a virtio-9p nor a virtiofs device id. */
    PGW_ASSERT(pgw_vio_mock_open(3u, "t", 1u, 0, NULL, NULL) == NULL);
    PGW_ASSERT(pgw_vio_mock_open(PGW_VIRTIO_ID_9P, "t", 0u, 0, NULL, NULL) ==
               NULL);
}

static void test_full_stack_synchronous(void)
{
    echo_dev echo = {0, 0};
    pgw_vio_dev *dev =
        pgw_vio_mock_open(PGW_VIRTIO_ID_9P, "share", 1u,
                          PGW_VIO_F_VERSION_1 | PGW_VIO_F_RING_INDIRECT_DESC,
                          echo_fn, &echo);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);

    char tag[64];
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_9p(dev, tag, sizeof(tag)), PGW_OK);
    PGW_ASSERT_STR_EQ(tag, "share");

    pgw_virtq *vq = pgw_vq_create(dev, 0, 16u, true, false);
    PGW_ASSERT(vq != NULL);

    char request[16] = "ping";
    char response[8];
    memset(response, 0, sizeof(response));
    pgw_vio_sg sg[2] = {
        {request, 0, 4u, false},
        {response, 0, sizeof(response), true},
    };
    int cookie = 1;
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 2u, &cookie), PGW_OK);
    PGW_ASSERT(pgw_vq_kick(vq));
    PGW_ASSERT_EQ_U(echo.calls, 1u);
    PGW_ASSERT_EQ_U(echo.last_in_len, 4u);

    void *got = NULL;
    uint32_t len = 0;
    PGW_ASSERT_EQ_I(pgw_vq_pop(vq, &got, &len), PGW_OK);
    PGW_ASSERT_EQ_PTR(got, &cookie);
    PGW_ASSERT_EQ_U(len, 4u);
    PGW_ASSERT_MEM_EQ(response, "pong", 4u);
    PGW_ASSERT_EQ_U(pgw_vq_free_descs(vq), 16u);

    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_full_stack_asynchronous(void)
{
    echo_dev echo = {0, 0};
    pgw_vio_dev *dev = pgw_vio_mock_open(PGW_VIRTIO_ID_9P, "share", 1u,
                                         PGW_VIO_F_VERSION_1, echo_fn, &echo);
    PGW_ASSERT(dev != NULL);
    /* Behave like a real device: nothing happens on the kick. */
    pgw_vio_mock_set_sync(dev, false);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);
    pgw_virtq *vq = pgw_vq_create(dev, 0, 8u, false, false);
    PGW_ASSERT(vq != NULL);

    char request[8] = "ping";
    char response[8];
    memset(response, 0, sizeof(response));
    pgw_vio_sg sg[2] = {
        {request, 0, 4u, false},
        {response, 0, sizeof(response), true},
    };
    PGW_ASSERT_EQ_I(pgw_vq_push(vq, sg, 2u, NULL), PGW_OK);
    PGW_ASSERT(pgw_vq_kick(vq));
    PGW_ASSERT_EQ_U(echo.calls, 0u); /* still queued */

    void *got = NULL;
    uint32_t len = 0;
    PGW_ASSERT_EQ_I(pgw_vq_pop(vq, &got, &len), PGW_EAGAIN);

    PGW_ASSERT_EQ_U(pgw_vio_mock_step(dev, 0), 1u); /* now the device answers */
    PGW_ASSERT_EQ_U(echo.calls, 1u);
    PGW_ASSERT_EQ_I(pgw_vq_pop(vq, &got, &len), PGW_OK);
    PGW_ASSERT_EQ_U(len, 4u);
    PGW_ASSERT_MEM_EQ(response, "pong", 4u);

    /* Nothing left to step. */
    PGW_ASSERT_EQ_U(pgw_vio_mock_step(dev, 0), 0u);
    pgw_vq_destroy(vq);
    pgw_vio_destroy(dev);
}

static void test_step_without_handler_is_a_no_op(void)
{
    pgw_vio_dev *dev = pgw_vio_mock_open(PGW_VIRTIO_ID_9P, "share", 1u,
                                         PGW_VIO_F_VERSION_1, NULL, NULL);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_vio_mock_step(dev, 0), 0u);
    /* A queue index the device does not have is refused, not walked. */
    PGW_ASSERT_EQ_U(pgw_vio_mock_step(dev, 7u), 0u);
    pgw_vio_destroy(dev);
}

static void test_reset_sequence_is_recorded(void)
{
    pgw_vio_dev *dev = pgw_vio_mock_open(PGW_VIRTIO_ID_9P, "share", 1u,
                                         PGW_VIO_F_VERSION_1, NULL, NULL);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(dev, PGW_VIO_F_VERSION_1), PGW_OK);
    uint32_t resets = pgw_vio_mock_reset_count(dev);
    uint32_t teardowns = pgw_vio_mock_teardown_count(dev);

    pgw_vio_reset(dev);
    PGW_ASSERT_EQ_U(pgw_vio_mock_reset_count(dev), resets + 1u);
    PGW_ASSERT_EQ_U(pgw_vio_mock_teardown_count(dev), teardowns + 1u);
    PGW_ASSERT_EQ_U(pgw_vio_mock_notify_count(dev), 0u);
    pgw_vio_destroy(dev);
}

static void test_virtiofs_tag_is_readable(void)
{
    pgw_vio_dev *dev = pgw_vio_mock_open(PGW_VIRTIO_ID_FS, "vfsroot", 3u,
                                         PGW_VIO_F_VERSION_1, NULL, NULL);
    PGW_ASSERT(dev != NULL);
    PGW_ASSERT_EQ_U(pgw_vio_get_info(dev)->device_id, PGW_VIRTIO_ID_FS);

    char tag[64];
    uint32_t req = 0;
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_fs(dev, tag, sizeof(tag), &req), PGW_OK);
    PGW_ASSERT_STR_EQ(tag, "vfsroot");
    PGW_ASSERT_EQ_U(req, 2u); /* num_queues - 1, the high priority queue */
    pgw_vio_destroy(dev);
}

static void test_two_devices_are_independent(void)
{
    /* VIO-06: several shares mounted at once means several devices, each with
     * its own state. Nothing here may be shared behind the caller's back. */
    echo_dev echo_a = {0, 0};
    echo_dev echo_b = {0, 0};
    pgw_vio_dev *a = pgw_vio_mock_open(PGW_VIRTIO_ID_9P, "share-a", 1u,
                                       PGW_VIO_F_VERSION_1, echo_fn, &echo_a);
    pgw_vio_dev *b = pgw_vio_mock_open(PGW_VIRTIO_ID_9P, "share-b", 1u,
                                       PGW_VIO_F_VERSION_1, echo_fn, &echo_b);
    PGW_ASSERT(a != NULL && b != NULL);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(a, PGW_VIO_F_VERSION_1), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_vio_init_device(b, PGW_VIO_F_VERSION_1), PGW_OK);

    char tag[64];
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_9p(a, tag, sizeof(tag)), PGW_OK);
    PGW_ASSERT_STR_EQ(tag, "share-a");
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_9p(b, tag, sizeof(tag)), PGW_OK);
    PGW_ASSERT_STR_EQ(tag, "share-b");

    pgw_vio_destroy(a);
    /* b is untouched by the destruction of a. */
    PGW_ASSERT_EQ_I(pgw_vio_read_tag_9p(b, tag, sizeof(tag)), PGW_OK);
    PGW_ASSERT_STR_EQ(tag, "share-b");
    pgw_vio_destroy(b);
}

PGW_TEST(test_open_reports_identity);
PGW_TEST(test_open_refuses_bad_arguments);
PGW_TEST(test_full_stack_synchronous);
PGW_TEST(test_full_stack_asynchronous);
PGW_TEST(test_step_without_handler_is_a_no_op);
PGW_TEST(test_reset_sequence_is_recorded);
PGW_TEST(test_virtiofs_tag_is_readable);
PGW_TEST(test_two_devices_are_independent);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
