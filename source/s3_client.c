/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include "s3_client.h"

#include "auth.h"
#include "io.h"

static const char *s_capsule_name_s3_client = "aws_s3_client";

struct s3_client_binding {
    struct aws_s3_client *native;

    bool release_called;
    bool shutdown_called;

    /* Shutdown callback, reference cleared after setting result */
    PyObject *on_shutdown;
};

static void s_s3_client_release(struct s3_client_binding *client) {
    AWS_FATAL_ASSERT(!client->release_called);
    client->release_called = true;

    bool destroy_after_release = client->shutdown_called;

    aws_s3_client_release(client->native);

    if (destroy_after_release) {
        aws_mem_release(aws_py_get_allocator(), client);
    }
}

/* Invoked when the python object get cleaned up */
static void s_s3_client_capsule_destructor(PyObject *capsule) {
    struct s3_client_binding *client = PyCapsule_GetPointer(capsule, s_capsule_name_s3_client);
    s_s3_client_release(client);
}

/* Callback from C land, invoked when the underlying shutdown process finished */
static void s_s3_client_shutdown(void *user_data) {
    struct s3_client_binding *client = user_data;

    /* Lock for python */
    PyGILState_STATE state;
    if (aws_py_gilstate_ensure(&state)) {
        return; /* Python has shut down. Nothing matters anymore, but don't crash */
    }

    client->shutdown_called = true;

    bool destroy_after_shutdown = client->release_called;

    /* Invoke on_shutdown, then clear our reference to it */
    PyObject *result = PyObject_CallFunction(client->on_shutdown, NULL);
    if (result) {
        Py_DECREF(result);
    } else {
        /* Callback might fail during application shutdown */
        PyErr_WriteUnraisable(PyErr_Occurred());
    }
    Py_CLEAR(client->on_shutdown);

    if (destroy_after_shutdown) {
        aws_mem_release(aws_py_get_allocator(), client);
    }

    PyGILState_Release(state);
}

struct aws_s3_client *aws_py_get_s3_client(PyObject *client) {
    AWS_PY_RETURN_NATIVE_FROM_BINDING(client, s_capsule_name_s3_client, "Client", s3_client_binding);
}

PyObject *aws_py_s3_client_new(PyObject *self, PyObject *args) {
    (void)self;

    struct aws_allocator *allocator = aws_py_get_allocator();

    PyObject *bootstrap_py;
    PyObject *credential_provider_py;
    PyObject *tls_options_py;
    PyObject *on_shutdown_py;
    const char *region;
    Py_ssize_t region_len;
    uint64_t part_size;
    double throughput_target_gbps;
    if (!PyArg_ParseTuple(
            args,
            "OOOOs#Kd",
            &bootstrap_py,
            &credential_provider_py,
            &tls_options_py,
            &on_shutdown_py,
            &region,
            &region_len,
            &part_size,
            &throughput_target_gbps)) {
        return NULL;
    }

    struct aws_client_bootstrap *bootstrap = aws_py_get_client_bootstrap(bootstrap_py);
    if (!bootstrap) {
        return NULL;
    }

    struct aws_credentials_provider *credential_provider = NULL;
    if (credential_provider_py != Py_None) {
        credential_provider = aws_py_get_credentials_provider(credential_provider_py);
    }

    struct aws_signing_config_aws signing_config;
    AWS_ZERO_STRUCT(signing_config);

    struct aws_byte_cursor region_cursor = aws_byte_cursor_from_array((const uint8_t *)region, region_len);

    if (credential_provider) {
        aws_s3_init_default_signing_config(&signing_config, region_cursor, credential_provider);
    }

    struct s3_client_binding *s3_clinet = aws_mem_calloc(allocator, 1, sizeof(struct s3_client_binding));
    if (!s3_clinet) {
        return PyErr_AwsLastError();
    }

    /* From hereon, we need to clean up if errors occur */
    struct aws_tls_connection_options *tls_options = NULL;
    if (tls_options_py != Py_None) {
        tls_options = aws_py_get_tls_connection_options(tls_options_py);
        if (!tls_options) {
            goto client_init_failed;
        }
    }

    struct aws_s3_client_config s3_config = {
        .region = aws_byte_cursor_from_array((const uint8_t *)region, region_len),
        .client_bootstrap = bootstrap,
        .signing_config = credential_provider ? &signing_config : NULL,
        .part_size = part_size,
        .tls_connection_options = tls_options,
        .throughput_target_gbps = throughput_target_gbps,
        .shutdown_callback = s_s3_client_shutdown,
        .shutdown_callback_user_data = s3_clinet,
    };

    s3_clinet->native = aws_s3_client_new(allocator, &s3_config);
    if (s3_clinet->native == NULL) {
        PyErr_SetAwsLastError();
        goto client_init_failed;
    }

    PyObject *capsule = PyCapsule_New(s3_clinet, s_capsule_name_s3_client, s_s3_client_capsule_destructor);
    if (!capsule) {
        goto capsule_new_failed;
    }

    s3_clinet->on_shutdown = on_shutdown_py;
    Py_INCREF(s3_clinet->on_shutdown);

    return capsule;

capsule_new_failed:
    aws_s3_client_release(s3_clinet->native);
client_init_failed:
    aws_mem_release(allocator, s3_clinet);
    return NULL;
}