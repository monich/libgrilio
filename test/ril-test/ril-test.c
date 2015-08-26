/*
 * Copyright (C) 2015 Jolla Ltd.
 * Contact: Slava Monich <slava.monich@jolla.com>
 *
 * You may use this file under the terms of BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the name of the Jolla Ltd nor the names of its contributors
 *      may be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "grilio_channel.h"
#include "grilio_request.h"
#include "grilio_parser.h"

#include <glibutil/gutil_log.h>

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <linux/capability.h>

#define RET_OK          (0)
#define RET_NOTFOUND    (1)
#define RET_ERR         (2)
#define RET_TIMEOUT     (3)

#define RIL_REQUEST_BASEBAND_VERSION 51
#define RIL_UNSOL_RIL_CONNECTED 1034
#define RADIO_UID 1001
#define RADIO_GID 1001

typedef struct app {
    gint timeout;
    GMainLoop* loop;
    GRilIoChannel* ril;
    const char* dev;
    const char* sub;
    int ret;
} App;

static
void
app_error_handler(
    GRilIoChannel* ril,
    const GError* error,
    void* user_data)
{
    App* app = user_data;
    app->ret = RET_ERR;
    g_main_loop_quit(app->loop);
}

static
void
app_connected(
    GRilIoChannel* channel,
    void* user_data)
{
    GINFO("RIL version %u", channel->ril_version);
}

static
void
app_baseband_version_resp(
    GRilIoChannel* ril,
    int status,
    const void* data,
    guint len,
    void* user_data)
{
    App* app = user_data;
    GINFO("Baseband request status %d", status);
    g_main_loop_quit(app->loop);
}

static
guint
app_submit_baseband_version_req(
    App* app)
{
    return grilio_channel_send_request_full(app->ril, NULL,
        RIL_REQUEST_BASEBAND_VERSION, app_baseband_version_resp, NULL, app);
}

static
void
app_radio_on()
{
    /* rild expects user radio */
    struct __user_cap_header_struct header;
    struct __user_cap_data_struct cap;

    prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
    setgid(RADIO_GID);
    setuid(RADIO_UID);

    memset(&header, 0, sizeof(header));
    memset(&cap, 0, sizeof(cap));
    header.version = _LINUX_CAPABILITY_VERSION;
    cap.effective = cap.permitted = (1 << CAP_NET_ADMIN) | (1 << CAP_NET_RAW);
    syscall(SYS_capset, &header, &cap);
}

static
int
app_run(
    App* app)
{
    app_radio_on();
    app->ril = grilio_channel_new_socket(app->dev, app->sub);
    if (app->ril) {
        guint req_id;
        gulong connected_handler_id, error_handler_id;
        app->ret = RET_ERR;
        app->loop = g_main_loop_new(NULL, FALSE);
        if (app->timeout > 0) GDEBUG("Timeout %d sec", app->timeout);
        grilio_channel_add_default_logger(app->ril, GLOG_LEVEL_VERBOSE);
        connected_handler_id = grilio_channel_add_connected_handler(app->ril,
            app_connected, app);
        error_handler_id = grilio_channel_add_error_handler(app->ril,
            app_error_handler, app);
        req_id = app_submit_baseband_version_req(app);
        if (req_id) {
            GDEBUG("Submitted request %u", req_id);
            g_main_loop_run(app->loop);
            g_main_loop_unref(app->loop);
            grilio_channel_cancel_request(app->ril, req_id, FALSE);
        }
        grilio_channel_remove_handler(app->ril, connected_handler_id);
        grilio_channel_remove_handler(app->ril, error_handler_id);
        grilio_channel_unref(app->ril);
        app->ril = NULL;
    } else {
        GERR("Failed to open RIL socket");
    }
    return app->ret;
}

static
gboolean
app_log_verbose(
    const gchar* name,
    const gchar* value,
    gpointer data,
    GError** error)
{
    gutil_log_default.level = GLOG_LEVEL_VERBOSE;
    return TRUE;
}

static
gboolean
app_log_quiet(
    const gchar* name,
    const gchar* value,
    gpointer data,
    GError** error)
{
    gutil_log_default.level = GLOG_LEVEL_ERR;
    return TRUE;
}

static
gboolean
app_init(
    App* app,
    int argc,
    char* argv[])
{
    gboolean ok = FALSE;
    GOptionEntry entries[] = {
        { "verbose", 'v', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
          app_log_verbose, "Enable verbose output", NULL },
        { "quiet", 'q', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
          app_log_quiet, "Be quiet", NULL },
        { "timeout", 't', 0, G_OPTION_ARG_INT,
          &app->timeout, "Timeout in seconds", "SECONDS" },
        { NULL }
    };
    GError* error = NULL;
    GOptionContext* options = g_option_context_new("[DEV [SUB]]");
    g_option_context_add_main_entries(options, entries, NULL);
    if (g_option_context_parse(options, &argc, &argv, &error)) {
        if (argc < 4) {
            app->dev = (argc > 1) ? argv[1] : "/dev/socket/rild";
            app->sub = (argc > 2) ? argv[2] : NULL;
            ok = TRUE;
        } else {
            char* help = g_option_context_get_help(options, TRUE, NULL);
            fprintf(stderr, "%s", help);
            g_free(help);
        }
    } else {
        GERR("%s", error->message);
        g_error_free(error);
    }
    g_option_context_free(options);
    return ok;
}

int main(int argc, char* argv[])
{
    int ret = RET_ERR;
    App app;
    memset(&app, 0, sizeof(app));
    app.timeout = -1;
    gutil_log_timestamp = FALSE;
    gutil_log_set_type(GLOG_TYPE_STDERR, "ril-test");
    gutil_log_default.level = GLOG_LEVEL_DEFAULT;
    if (app_init(&app, argc, argv)) {
        ret = app_run(&app);
    }
    return ret;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
