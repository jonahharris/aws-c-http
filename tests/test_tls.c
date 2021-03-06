/*
 * Copyright 2010-2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <aws/http/connection.h>
#include <aws/http/request_response.h>

#include <aws/common/clock.h>
#include <aws/common/condition_variable.h>
#include <aws/common/mutex.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/logging.h>
#include <aws/io/socket.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/io/uri.h>
#include <aws/testing/aws_test_harness.h>

/* Singleton used by tests in this file */
struct test_ctx {
    struct aws_allocator *alloc;
    struct aws_logger logger;
    struct aws_event_loop_group event_loop_group;
    struct aws_host_resolver host_resolver;
    struct aws_tls_ctx *tls_ctx;
    struct aws_client_bootstrap *client_bootstrap;
    struct aws_http_connection *client_connection;
    struct aws_http_stream *stream;

    size_t body_size;
    bool stream_complete;
    bool client_connection_is_shutdown;

    struct aws_mutex wait_lock;
    struct aws_condition_variable wait_cvar;
    int wait_result;
};

static const uint32_t TEST_TIMEOUT_SEC = 4;

void s_on_connection_setup(struct aws_http_connection *connection, int error_code, void *user_data) {
    struct test_ctx *test = user_data;
    AWS_FATAL_ASSERT(aws_mutex_lock(&test->wait_lock) == AWS_OP_SUCCESS);

    test->client_connection = connection;
    test->wait_result = error_code;

    AWS_FATAL_ASSERT(aws_mutex_unlock(&test->wait_lock) == AWS_OP_SUCCESS);
    aws_condition_variable_notify_one(&test->wait_cvar);
}

void s_on_connection_shutdown(struct aws_http_connection *connection, int error_code, void *user_data) {
    (void)connection;
    struct test_ctx *test = user_data;
    AWS_FATAL_ASSERT(aws_mutex_lock(&test->wait_lock) == AWS_OP_SUCCESS);

    test->client_connection_is_shutdown = true;
    test->wait_result = error_code;

    AWS_FATAL_ASSERT(aws_mutex_unlock(&test->wait_lock) == AWS_OP_SUCCESS);
    aws_condition_variable_notify_one(&test->wait_cvar);
}

static int s_test_wait(struct test_ctx *test, bool (*pred)(void *user_data)) {
    ASSERT_SUCCESS(aws_mutex_lock(&test->wait_lock));
    int wait_result = 0;
    do {
        wait_result = aws_condition_variable_wait_for_pred(
            &test->wait_cvar,
            &test->wait_lock,
            aws_timestamp_convert(TEST_TIMEOUT_SEC, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL),
            pred,
            test);
    } while (wait_result == -1 && AWS_ERROR_COND_VARIABLE_TIMED_OUT == aws_last_error());
    ASSERT_SUCCESS(aws_mutex_unlock(&test->wait_lock));
    ASSERT_SUCCESS(wait_result);
    return AWS_OP_SUCCESS;
}

static bool s_test_connection_setup_pred(void *user_data) {
    struct test_ctx *test = user_data;
    return test->wait_result || test->client_connection;
}

static bool s_test_connection_shutdown_pred(void *user_data) {
    struct test_ctx *test = user_data;
    return test->wait_result || test->client_connection_is_shutdown;
}

/* test that if a timeout occurs during negotiation that the user code is still
 * notified. Connecting to port 80 on s3 or amazon.com and attempting TLS will get
 * you blackholed, and thus timed out */
static int s_test_tls_negotiation_timeout(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    aws_tls_init_static_state(allocator);
    aws_http_library_init(allocator);
    aws_load_error_strings();
    aws_io_load_error_strings();
    aws_io_load_log_subject_strings();

    struct aws_byte_cursor url =
        aws_byte_cursor_from_c_str("https://aws-crt-test-stuff.s3.amazonaws.com/http_test_doc.txt");
    struct aws_uri uri;
    aws_uri_init_parse(&uri, allocator, &url);

    struct aws_socket_options socket_options = {
        .type = AWS_SOCKET_STREAM, .domain = AWS_SOCKET_IPV4, .connect_timeout_ms = TEST_TIMEOUT_SEC * 1000};

    struct test_ctx test;
    AWS_ZERO_STRUCT(test);
    test.alloc = allocator;

    struct aws_logger_standard_options logger_options = {.file = stdout, .level = AWS_LL_TRACE};
    ASSERT_SUCCESS(aws_logger_init_standard(&test.logger, allocator, &logger_options));
    aws_logger_set(&test.logger);

    aws_mutex_init(&test.wait_lock);
    aws_condition_variable_init(&test.wait_cvar);

    ASSERT_SUCCESS(aws_event_loop_group_default_init(&test.event_loop_group, test.alloc, 1));
    ASSERT_SUCCESS(aws_host_resolver_init_default(&test.host_resolver, test.alloc, 1, &test.event_loop_group));
    ASSERT_NOT_NULL(
        test.client_bootstrap =
            aws_client_bootstrap_new(test.alloc, &test.event_loop_group, &test.host_resolver, NULL));
    struct aws_tls_ctx_options tls_ctx_options;
    aws_tls_ctx_options_init_default_client(&tls_ctx_options, allocator);
    ASSERT_NOT_NULL(test.tls_ctx = aws_tls_client_ctx_new(allocator, &tls_ctx_options));
    struct aws_tls_connection_options tls_connection_options;
    aws_tls_connection_options_init_from_ctx(&tls_connection_options, test.tls_ctx);
    struct aws_http_client_connection_options http_options = AWS_HTTP_CLIENT_CONNECTION_OPTIONS_INIT;
    http_options.allocator = test.alloc;
    http_options.bootstrap = test.client_bootstrap;
    http_options.host_name = *aws_uri_host_name(&uri);
    http_options.port = 80; /* note that this is intentionally wrong and not 443 */
    http_options.on_setup = s_on_connection_setup;
    http_options.on_shutdown = s_on_connection_shutdown;
    http_options.socket_options = &socket_options;
    http_options.tls_options = &tls_connection_options;
    http_options.user_data = &test;

    ASSERT_SUCCESS(aws_http_client_connect(&http_options));
    ASSERT_SUCCESS(s_test_wait(&test, s_test_connection_setup_pred));

    /* the connection should have failed within TEST_TIMEOUT_SEC */
    ASSERT_NULL(test.client_connection);
    ASSERT_TRUE(0 != test.wait_result);

    aws_client_bootstrap_release(test.client_bootstrap);
    aws_host_resolver_clean_up(&test.host_resolver);
    aws_event_loop_group_clean_up(&test.event_loop_group);

    aws_tls_ctx_options_clean_up(&tls_ctx_options);
    aws_tls_connection_options_clean_up(&tls_connection_options);
    aws_tls_ctx_destroy(test.tls_ctx);

    aws_logger_set(NULL);
    aws_logger_clean_up(&test.logger);

    aws_mutex_clean_up(&test.wait_lock);
    aws_condition_variable_clean_up(&test.wait_cvar);
    aws_uri_clean_up(&uri);

    aws_http_library_clean_up();
    aws_tls_clean_up_static_state();

    return 0;
}
AWS_TEST_CASE(tls_negotiation_timeout, s_test_tls_negotiation_timeout);

static void s_on_stream_headers(
    struct aws_http_stream *stream,
    const struct aws_http_header *headers,
    size_t num_headers,
    void *user_data) {
    (void)stream;
    (void)headers;
    (void)num_headers;
    (void)user_data;
}

static void s_on_stream_body(
    struct aws_http_stream *stream,
    const struct aws_byte_cursor *data,
    size_t *out_window_update_size, /* NOLINT(readability-non-const-parameter) */
    void *user_data) {
    (void)stream;
    (void)out_window_update_size;
    struct test_ctx *test = user_data;
    test->body_size += data->len;
}

static void s_on_stream_complete(struct aws_http_stream *stream, int error_code, void *user_data) {
    (void)stream;
    struct test_ctx *test = user_data;
    test->wait_result = error_code;
    test->stream_complete = true;
}

static bool s_stream_wait_pred(void *user_data) {
    struct test_ctx *test = user_data;
    return test->wait_result || test->stream_complete;
}

static int s_test_tls_download_medium_file(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    aws_tls_init_static_state(allocator);
    aws_http_library_init(allocator);

    struct aws_byte_cursor url =
        aws_byte_cursor_from_c_str("https://aws-crt-test-stuff.s3.amazonaws.com/http_test_doc.txt");
    struct aws_uri uri;
    aws_uri_init_parse(&uri, allocator, &url);

    struct aws_socket_options socket_options = {
        .type = AWS_SOCKET_STREAM,
        .domain = AWS_SOCKET_IPV4,
        .connect_timeout_ms =
            (uint32_t)aws_timestamp_convert(TEST_TIMEOUT_SEC, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_MILLIS, NULL),
    };

    struct test_ctx test;
    AWS_ZERO_STRUCT(test);
    test.alloc = allocator;

    struct aws_logger_standard_options logger_options = {.file = stdout, .level = AWS_LL_DEBUG};
    aws_logger_init_standard(&test.logger, allocator, &logger_options);
    aws_logger_set(&test.logger);

    aws_mutex_init(&test.wait_lock);
    aws_condition_variable_init(&test.wait_cvar);

    ASSERT_SUCCESS(aws_event_loop_group_default_init(&test.event_loop_group, test.alloc, 1));
    ASSERT_SUCCESS(aws_host_resolver_init_default(&test.host_resolver, test.alloc, 1, &test.event_loop_group));
    ASSERT_NOT_NULL(
        test.client_bootstrap =
            aws_client_bootstrap_new(test.alloc, &test.event_loop_group, &test.host_resolver, NULL));
    struct aws_tls_ctx_options tls_ctx_options;
    aws_tls_ctx_options_init_default_client(&tls_ctx_options, allocator);
    ASSERT_NOT_NULL(test.tls_ctx = aws_tls_client_ctx_new(allocator, &tls_ctx_options));
    struct aws_tls_connection_options tls_connection_options;
    aws_tls_connection_options_init_from_ctx(&tls_connection_options, test.tls_ctx);
    aws_tls_connection_options_set_server_name(
        &tls_connection_options, allocator, (struct aws_byte_cursor *)aws_uri_host_name(&uri));
    struct aws_http_client_connection_options http_options = AWS_HTTP_CLIENT_CONNECTION_OPTIONS_INIT;
    http_options.allocator = test.alloc;
    http_options.bootstrap = test.client_bootstrap;
    http_options.host_name = *aws_uri_host_name(&uri);
    http_options.port = 443;
    http_options.on_setup = s_on_connection_setup;
    http_options.on_shutdown = s_on_connection_shutdown;
    http_options.socket_options = &socket_options;
    http_options.tls_options = &tls_connection_options;
    http_options.user_data = &test;

    ASSERT_SUCCESS(aws_http_client_connect(&http_options));
    ASSERT_SUCCESS(s_test_wait(&test, s_test_connection_setup_pred));
    ASSERT_INT_EQUALS(0, test.wait_result);
    ASSERT_NOT_NULL(test.client_connection);

    struct aws_http_header headers[] = {
        {.name = aws_byte_cursor_from_c_str("Host"), .value = *aws_uri_host_name(&uri)}};

    struct aws_http_request_options req_options = AWS_HTTP_REQUEST_OPTIONS_INIT;
    req_options.client_connection = test.client_connection;
    req_options.method = aws_byte_cursor_from_c_str("GET");
    req_options.uri = *aws_uri_path_and_query(&uri);
    req_options.header_array = headers;
    req_options.num_headers = AWS_ARRAY_SIZE(headers);
    req_options.on_response_headers = s_on_stream_headers;
    req_options.on_response_body = s_on_stream_body;
    req_options.on_complete = s_on_stream_complete;
    req_options.user_data = &test;

    ASSERT_NOT_NULL(test.stream = aws_http_stream_new_client_request(&req_options));

    /* wait for the request to complete */
    s_test_wait(&test, s_stream_wait_pred);

    ASSERT_INT_EQUALS(14428801, test.body_size);

    aws_http_stream_release(test.stream);
    test.stream = NULL;

    aws_http_connection_release(test.client_connection);
    ASSERT_SUCCESS(s_test_wait(&test, s_test_connection_shutdown_pred));

    aws_client_bootstrap_release(test.client_bootstrap);
    aws_host_resolver_clean_up(&test.host_resolver);
    aws_event_loop_group_clean_up(&test.event_loop_group);

    aws_tls_ctx_options_clean_up(&tls_ctx_options);
    aws_tls_connection_options_clean_up(&tls_connection_options);
    aws_tls_ctx_destroy(test.tls_ctx);

    aws_logger_set(NULL);
    aws_logger_clean_up(&test.logger);

    aws_mutex_clean_up(&test.wait_lock);
    aws_condition_variable_clean_up(&test.wait_cvar);
    aws_uri_clean_up(&uri);

    aws_http_library_clean_up();
    aws_tls_clean_up_static_state();

    return 0;
}
AWS_TEST_CASE(tls_download_medium_file, s_test_tls_download_medium_file);
