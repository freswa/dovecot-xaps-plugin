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

#ifndef XAPS_PUSH_NOTIFICATION_PLUGIN_H
#define XAPS_PUSH_NOTIFICATION_PLUGIN_H

struct module;

void push_notification_driver_xaps_http_callback(const struct http_response *response, void *context);
extern const char *xaps_plugin_dependencies[];
void xaps_push_notification_plugin_init(struct module *module);
void xaps_push_notification_plugin_deinit(void);

#endif
