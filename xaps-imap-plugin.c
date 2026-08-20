/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Stefan Arentz <stefan@arentz.ca>
 * Copyright (c) 2017 Frederik Schwan <frederik dot schwan at linux dot com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <config.h>
#include <lib.h>
#include <str.h>
#include <imap-common.h>
#include <imap-quote.h>
#include <mail-storage.h>
#include <mail-namespace.h>
#include <http-client-private.h>

#include "xaps-imap-plugin.h"
#include "xaps-utils.h"

const char *xapplepushservice_plugin_version = DOVECOT_ABI_VERSION;

static struct module *xaps_imap_module;
static imap_client_created_func_t *next_hook_client_created;

/**
 * Command handler for the XAPPLEPUSHSERVICE command. The command is
 * used by iOS clients to register for push notifications.
 *
 * We receive a list of key value pairs from the client, with the
 * following keys:
 *
 *  aps-version      - always set to "2"
 *  aps-account-id   - a unique id the iOS device has associated with this account
 *  aps-device-token - the APS device token
 *  aps-subtopic     - always set to "com.apple.mobilemail"
 *  mailboxes        - list of mailboxes to send notifications for
 *
 * For example:
 *
 *  XAPPLEPUSHSERVICE aps-version 2 aps-account-id 0715A26B-CA09-4730-A419-793000CA982E
 *    aps-device-token 2918390218931890821908309283098109381029309829018310983092892829
 *    aps-subtopic com.apple.mobilemail mailboxes (INBOX Notes)
 *
 * To minimize the work that needs to be done inside the IMAP client,
 * we only parse and validate the parameters and then simply push all
 * of this to the supporting daemon that will record the mapping
 * between the account and the iOS client.
 */
static bool parse_xapplepush(struct client_command_context *cmd, struct xaps_attr *xaps_attr) {
    /*
    * Parse arguments. We expect four key value pairs. We only take
    * those that we understand for version 2 of this extension.
    */

    const struct imap_arg *args;
    const char *arg_key, *arg_val;

    i_assert(xaps_attr != NULL);
    xaps_attr->dovecot_username = get_real_mbox_user(cmd->client->user);

    if (!client_read_args(cmd, 0, 0, &args)) {
        client_send_command_error(cmd, "Invalid arguments.");
        return FALSE;
    }

    for (int i = 0; i < 5; i++) {
        if (!imap_arg_get_astring(&args[i * 2 + 0], &arg_key)) {
            client_send_command_error(cmd, "Invalid arguments.");
            return FALSE;
        }

        // i=4 is a list with which imap_arg_get_astring segfaults
        if (i < 4 && !imap_arg_get_astring(&args[i * 2 + 1], &arg_val)) {
            client_send_command_error(cmd, "Invalid arguments.");
            return FALSE;
        }

        if (strcasecmp(arg_key, "aps-version") == 0) {
            xaps_attr->aps_version = arg_val;
        } else if (strcasecmp(arg_key, "aps-account-id") == 0) {
            xaps_attr->aps_account_id = arg_val;
        } else if (strcasecmp(arg_key, "aps-device-token") == 0) {
            xaps_attr->aps_device_token = arg_val;
        } else if (strcasecmp(arg_key, "aps-subtopic") == 0) {
            xaps_attr->aps_subtopic = arg_val;
        } else if (strcasecmp(arg_key, "mailboxes") == 0) {
            if (!imap_arg_get_list(&args[i * 2 + 1], &(xaps_attr->mailboxes))) {
                client_send_command_error(cmd, "Invalid arguments.");
                return FALSE;
            }
        }
    }

    /*
     * Check if this is a version we expect
     */

    if (!xaps_attr->aps_version || strcmp(xaps_attr->aps_version, "2") != 0) {
        client_send_command_error(cmd, "Unknown aps-version.");
        return FALSE;
    }

    /*
     * Check if all of the parameters are there.
     */

    if (!xaps_attr->aps_account_id || strlen(xaps_attr->aps_account_id) == 0) {
        client_send_command_error(cmd, "Incomplete or empty aps-account-id parameter.");
        return FALSE;
    }

    if (!xaps_attr->aps_device_token || strlen(xaps_attr->aps_device_token) == 0) {
        client_send_command_error(cmd, "Incomplete or empty aps-device-token parameter.");
        return FALSE;
    }

    if (!xaps_attr->aps_subtopic || strlen(xaps_attr->aps_subtopic) == 0) {
        client_send_command_error(cmd, "Incomplete or empty aps-subtopic parameter.");
        return FALSE;
    }

    if(!xaps_attr->mailboxes) {
        client_send_command_error(cmd, "Incomplete or empty mailboxes parameter.");
        return FALSE;
    }

    return TRUE;
}

/**
 * Send a registration request to the daemon, which will do all the
 * hard work.
 */
int xaps_register(struct client_command_context *cmd, struct xaps_attr *xaps_attr) {
    struct http_client_request *http_req;
    struct istream *payload;
    string_t *str;

    i_assert(xaps_global != NULL);
    i_assert(xaps_global->http_client != NULL);

    http_req = http_client_request_url(
            xaps_global->http_client, "POST", xaps_global->http_url,
            xaps_register_callback, cmd->context);
    http_client_request_add_header(http_req, "Content-Type",
                                   "application/json; charset=utf-8");

    str = str_new(default_pool, 256);
    str_append(str, "{\"ApsAccountId\":\"");
    json_append_escaped(str, xaps_attr->aps_account_id);
    str_append(str, "\",\"ApsDeviceToken\":\"");
    json_append_escaped(str, xaps_attr->aps_device_token);
    str_append(str, "\",\"ApsSubtopic\":\"");
    json_append_escaped(str, xaps_attr->aps_subtopic);
    str_append(str, "\",\"Username\":\"");
    json_append_escaped(str, xaps_attr->dovecot_username);

    if (xaps_attr->mailboxes == NULL) {
        str_append(str, "\",\"Mailboxes\": [\"INBOX\"]");
    } else {
        str_append(str, "\",\"Mailboxes\": [");
        int first = 1;
        for (int i = 0; !IMAP_ARG_IS_EOL(&xaps_attr->mailboxes[i]); i++) {
            const char *mailbox;
            if (!imap_arg_get_astring(&(xaps_attr->mailboxes[i]), &mailbox)) {
                return -1;
            }
            if (!first) {
                str_append(str, ",");
            }
            str_append(str, "\"");
            json_append_escaped(str, mailbox);
            str_append(str, "\"");
            first = 0;
        }
        str_append(str, "]");
    }
    str_append(str, "}");

    i_debug("Sending registration: %s", str_c(str));

    payload = i_stream_create_from_data(str_data(str), str_len(str));
    i_stream_add_destroy_callback(payload, str_free_i, str);
    http_client_request_set_payload(http_req, payload, FALSE);

    http_client_request_submit(http_req);
    i_stream_unref(&payload);

    return 0;
}

/*
 * Check whether the given mailbox actually exists for this user. Only
 * existing mailboxes are echoed back in the registration response.
 */
static bool mailbox_is_valid(struct mail_user *user, const char *mailbox) {
    struct mail_namespace *ns;
    struct mailbox *box;
    enum mailbox_existence existence = MAILBOX_EXISTENCE_NONE;
    bool valid = FALSE;

    ns = mail_namespace_find(user->namespaces, mailbox);
    if (ns == NULL) {
        return FALSE;
    }

    box = mailbox_alloc(ns->list, mailbox, 0);
    if (mailbox_exists(box, TRUE, &existence) == 0 &&
        existence != MAILBOX_EXISTENCE_NONE) {
        valid = TRUE;
    }
    mailbox_free(&box);

    return valid;
}

/*
 * Register the client at the xapsd
 */
static bool register_client(struct client_command_context *cmd, struct xaps_attr *xaps_attr) {
    /*
    * Forward to the helper daemon. The helper will return the
    * aps-topic, which in reality is the subject of the certificate.
    */
    if (xaps_register(cmd, xaps_attr) != 0) {
        client_send_command_error(cmd, "Registration failed.");
        return FALSE;
    }

    /*
     * Dovecot only supports asynchronous http calls. So we wait for the http call to complete and write the
     * aps-topic into the xaps_global struct.
     */
    http_client_wait(xaps_global->http_client);

    /*
     * Echo back the mailboxes that were requested and are actually valid
     * (existing) mailboxes for this user, one untagged response per
     * mailbox.
     * This is required by the iOS clients to build an index of watched
     * mailboxes and display push alerts when a mailbox hash is specified
     * in the APS payload.
     */
    unsigned int registered_mailboxes = 0;
    if (xaps_attr->mailboxes != NULL) {
        for (int i = 0; !IMAP_ARG_IS_EOL(&xaps_attr->mailboxes[i]); i++) {
            const char *mailbox;
            if (!imap_arg_get_astring(&xaps_attr->mailboxes[i], &mailbox)) {
                continue;
            }
            if (!mailbox_is_valid(cmd->client->user, mailbox)) {
                continue;
            }

            string_t *line = t_str_new(64);
            str_append(line, "* XAPPLEPUSHSERVICE mailbox ");
            imap_append_string(line, mailbox);
            client_send_line(cmd->client, str_c(line));
            registered_mailboxes++;
        }
    }

    /*
     * Return success. We assume that aps_version and aps_topic do not
     * contain anything that needs to be escaped.
     */
    client_send_line(cmd->client,
                     t_strdup_printf("* XAPPLEPUSHSERVICE aps-version \"%s\" aps-topic \"%s\"", xaps_attr->aps_version,
                                     xaps_global->aps_topic));
    client_send_tagline(cmd, "OK XAPPLEPUSHSERVICE completed.");

    i_debug("Successfully registered %u mailboxe(s) using topic %s",
            registered_mailboxes, xaps_global->aps_topic);
    return TRUE;
}

/*
 * Handle any XAPPLEPUSHSERVICE command
 */
static bool cmd_xapplepushservice(struct client_command_context *cmd) {
    struct xaps_attr xaps_attr;

    xaps_init(cmd->client->user, "/register", cmd->pool);
    if (!parse_xapplepush(cmd, &xaps_attr)) {
        return FALSE;
    }
    if (!register_client(cmd, &xaps_attr)) {
        return FALSE;
    }

    return TRUE;
}


/**
 * HTTP callback function for /register call
 */
void xaps_register_callback(const struct http_response *response, void *context) {
    size_t size;

    switch (response->status / 100) {
        case 2:
            // Success.
            i_debug("Notification sent successfully: %s", http_response_get_message(response));
            i_stream_read_data(response->payload, &xaps_global->aps_topic, &size, 128);
            i_assert(size > 31);
            break;

        default:
            // Error.
            i_error("Error when sending notification: %s", http_response_get_message(response));
            break;
    }
}

/**
 * This hook is called when a client has connected but before the
 * capability string has been sent. We simply add XAPPLEPUSHSERVICE to
 * the capabilities. This will trigger the usage of the
 * XAPPLEPUSHSERVICE command by iOS clients.
 */

static void xaps_client_created(struct client **client) {
    if (mail_user_is_plugin_loaded((*client)->user, xaps_imap_module)) {
        str_append((*client)->capability_string, " XAPPLEPUSHSERVICE");
    }

    if (next_hook_client_created != NULL) {
        next_hook_client_created(client);
    }
}


/**
 * This plugin method is called when the plugin is globally
 * initialized. We register a new command, XAPPLEPUSHSERVICE, and also
 * setup the client_created hook so that we can modify the
 * capabilities string.
 */

void xaps_imap_plugin_init(struct module *module) {
    command_register("XAPPLEPUSHSERVICE", cmd_xapplepushservice, 0);
    xaps_imap_module = module;
    next_hook_client_created = imap_client_created_hook_set(xaps_client_created);
}


/**
 * This plugin method is called when the plugin is globally
 * deinitialized. We unregister our command and remove the
 * client_created hook.
 */

void xaps_imap_plugin_deinit(void) {
    imap_client_created_hook_set(next_hook_client_created);
    command_unregister("XAPPLEPUSHSERVICE");
    push_notification_driver_xaps_cleanup();
}

/**
 * This plugin only makes sense in the context of IMAP.
 */

const char xaps_imap_plugin_binary_dependency[] = "imap";
