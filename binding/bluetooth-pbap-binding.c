/*
 * Copyright (C) 2018 Konsulko Group
 * Author: Matt Porter <mporter@konsulko.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <glib.h>
#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define AFB_BINDING_VERSION 3
#include <afb/afb-binding.h>

#include "obex_client1_interface.h"
#include "obex_session1_interface.h"
#include "obex_transfer1_interface.h"
#include "obex_phonebookaccess1_interface.h"
#include "freedesktop_dbus_properties_interface.h"

static GDBusObjectManager *obj_manager;
static OrgBluezObexClient1 *client;
static OrgBluezObexSession1 *session;
static OrgBluezObexPhonebookAccess1 *phonebook;
static GHashTable *xfer_queue;
static GMutex xfer_queue_mutex;
static GHashTable *xfer_complete;
static GMutex xfer_complete_mutex;
static GCond xfer_complete_cond;
static GMutex connected_mutex;
static gboolean connected = FALSE;
static afb_event_t status_event;

#define PBAP_UUID	"0000112f-0000-1000-8000-00805f9b34fb"

#define INTERNAL	"int"
#define SIM		"sim"
#define SIM1		SIM
#define SIM2		"sim2"

#define CONTACTS	"pb"
#define COMBINED	"cch"
#define INCOMING	"ich"
#define OUTGOING	"och"
#define MISSED		"mch"


static int update_or_insert(const char *key, const char *value)
{
	json_object *query = json_object_new_object();
	int ret;

	json_object_object_add(query, "key", json_object_new_string(key));
	json_object_object_add(query, "value", json_object_new_string(value));
	json_object_get(query);

	ret = afb_service_call_sync("persistence", "update", query, NULL, NULL, NULL);
	if (!ret) {
		AFB_DEBUG("Updating persistence value '%s'", key);
		json_object_put(query);
		return 0;
	}

	ret = afb_service_call_sync("persistence", "write", query, NULL, NULL, NULL);
	if (!ret) {
		AFB_DEBUG("Create persistence value '%s'", key);
		return 0;
	}

	return ret;
}

static int read_cached_value(const char *key, const char **data)
{
	json_object *response, *query;
	int ret;

	query = json_object_new_object();
	json_object_object_add(query, "key", json_object_new_string(key));

	ret = afb_service_call_sync("persistence", "read", query, &response, NULL, NULL);
	if (!ret) {
		json_object *val = NULL;

		json_object_object_get_ex(response, "value", &val);
		if (!val)
			return -EINVAL;

		*data = g_strdup(json_object_get_string(val));
	}

	json_object_get(response);

	return ret;
}

static void on_interface_proxy_properties_changed(
		GDBusObjectManagerClient *manager,
		GDBusObjectProxy *object_proxy,
		GDBusProxy *interface_proxy,
		GVariant *changed_properties,
		const gchar *const *invalidated_properties,
		gpointer user_data)
{
	GVariantIter iter;
	const gchar *key;
	GVariant *value;
	gchar *filename;

	const gchar *path = g_dbus_object_get_object_path(G_DBUS_OBJECT(object_proxy));

	if ((filename = g_hash_table_lookup(xfer_queue, path))) {
		g_variant_iter_init(&iter, changed_properties);
		while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
			const gchar *val = NULL;

			if (g_strcmp0(key, "Status"))
				continue;

			val = g_variant_get_string(value, NULL);

			if (g_strcmp0(val, "complete") && g_strcmp0(val, "error"))
				continue;

			g_mutex_lock(&xfer_complete_mutex);
			g_hash_table_insert(xfer_complete, g_strdup(path),
				g_strdup(!g_strcmp0(val, "complete") ? filename : ""));
			g_cond_signal(&xfer_complete_cond);
			g_mutex_unlock(&xfer_complete_mutex);
			g_mutex_lock(&xfer_queue_mutex);
			g_hash_table_remove(xfer_queue, path);
			g_mutex_unlock(&xfer_queue_mutex);
		}
	}
}

static json_object *get_vcard_xfer(gchar *filename)
{
	FILE *fp;
	json_object *vcard_str;
	gchar *vcard_data;
	size_t size, n;

	fp = fopen(filename, "ro");
	fseek(fp, 0L, SEEK_END);
	size = ftell(fp);
	vcard_data = calloc(1, size);
	fseek(fp, 0L, SEEK_SET);
	n = fread(vcard_data, 1, size, fp);
	if (n != size) {
		AFB_ERROR("Read only %ld/%ld bytes from %s", n, size, filename);
		return NULL;
	}

	vcard_str = json_object_new_string(vcard_data);
	free(vcard_data);
	fclose(fp);
	unlink(filename);

	return vcard_str;
}

static void get_filename(gchar *filename)
{
	struct tm* tm_info;;
	struct timeval tv;
	long ms;
	gchar buffer[64];

	gettimeofday(&tv, NULL);

	ms = lrint((double)tv.tv_usec/1000.0);
	if (ms >= 1000) {
		ms -=1000;
		tv.tv_sec++;
	}
	tm_info = localtime(&tv.tv_sec);

	strftime(buffer, 26, "%Y%m%d%H%M%S", tm_info);

	sprintf(filename, "/tmp/vcard-%s%03ld.dat", buffer, ms);
}

static gchar *pull_vcard(const gchar *handle)
{
	GVariantBuilder *b;
	GVariant *filter, *properties;
	gchar filename[256];
	gchar *tpath;

	b = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add(b, "{sv}", "Format", g_variant_new_string("vcard30"));
	filter = g_variant_builder_end(b);

	get_filename(filename);
	org_bluez_obex_phonebook_access1_call_pull_sync(
			phonebook, handle, filename, filter, &tpath, &properties, NULL, NULL);

	g_variant_builder_unref(b);
	g_mutex_lock(&xfer_queue_mutex);
	g_hash_table_insert(xfer_queue, g_strdup(tpath), g_strdup(filename));
	g_mutex_unlock(&xfer_queue_mutex);

	return tpath;
}

static json_object *get_vcard(const gchar *handle)
{
	json_object *vcard_str = NULL, *vcard = NULL;
	gchar *tpath, *filename;

	tpath = pull_vcard(handle);

	g_mutex_lock(&xfer_complete_mutex);
	while (!(filename = g_hash_table_lookup(xfer_complete, tpath)))
		g_cond_wait(&xfer_complete_cond, &xfer_complete_mutex);

	if (strlen(filename) > 0)
		vcard_str = get_vcard_xfer(filename);

	g_hash_table_remove(xfer_complete, tpath);
	g_free(filename);
	g_mutex_unlock(&xfer_complete_mutex);

	if (vcard_str) {
		vcard = json_object_new_object();
		json_object_object_add(vcard, "vcard", vcard_str);
	}

	return vcard;
}

static gchar *pull_vcards(int max_entries)
{
	GVariantBuilder *b;
	GVariant *filter, *properties;
	gchar filename[256];
	gchar *tpath;

	b = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add(b, "{sv}", "Format", g_variant_new_string("vcard30"));
	g_variant_builder_add(b, "{sv}", "Order", g_variant_new_string("indexed"));
	g_variant_builder_add(b, "{sv}", "Offset", g_variant_new_uint16(0));
	if (max_entries >= 0)
		g_variant_builder_add(b, "{sv}", "MaxCount", g_variant_new_uint16((guint16)max_entries));
	filter = g_variant_builder_end(b);

	get_filename(filename);
	org_bluez_obex_phonebook_access1_call_pull_all_sync(
			phonebook, filename, filter, &tpath, &properties, NULL, NULL);
	g_variant_builder_unref(b);
	g_mutex_lock(&xfer_queue_mutex);
	g_hash_table_insert(xfer_queue, g_strdup(tpath), g_strdup(filename));
	g_mutex_unlock(&xfer_queue_mutex);

	return tpath;
}

static json_object *get_vcards(int max_entries)
{
	json_object *vcards_str, *vcards;
	gchar *tpath, *filename;

	tpath = pull_vcards(max_entries);
	g_mutex_lock(&xfer_complete_mutex);
	while (!(filename = g_hash_table_lookup(xfer_complete, tpath)))
		g_cond_wait(&xfer_complete_cond, &xfer_complete_mutex);

	vcards_str = get_vcard_xfer(filename);

	g_hash_table_remove(xfer_complete, tpath);
	g_mutex_unlock(&xfer_complete_mutex);

	vcards = json_object_new_object();
	json_object_object_add(vcards, "vcards", vcards_str);

	return vcards;
}

static gboolean parse_list_parameter(afb_req_t request, gchar **list)
{
	struct json_object *list_obj, *query;
	const gchar *list_str;

	query = afb_req_json(request);

	if (json_object_object_get_ex(query, "list", &list_obj) == TRUE) {
		if (json_object_is_type(list_obj, json_type_string)) {
			list_str = json_object_get_string(list_obj);
			if (!g_strcmp0(list_str, COMBINED)) {
				*list = COMBINED;
			} else if (!g_strcmp0(list_str, INCOMING)) {
				*list = INCOMING;
			} else if (!g_strcmp0(list_str, OUTGOING)) {
				*list = OUTGOING;
			} else if (!g_strcmp0(list_str, MISSED)) {
				*list = MISSED;
			} else if (!g_strcmp0(list_str, CONTACTS)) {
				*list = CONTACTS;
			} else {
				afb_req_fail(request, "invalid list", NULL);
				return FALSE;
			}
		} else {
			afb_req_fail(request, "list not string", NULL);
			return FALSE;
		}
	} else {
		afb_req_fail(request, "no list", NULL);
		return FALSE;
	}

	return TRUE;
}

static gboolean parse_max_entries_parameter(afb_req_t request, int *max_entries)
{
	struct json_object *max_obj, *query;

	query = afb_req_json(request);

	if (json_object_object_get_ex(query, "max_entries", &max_obj) == TRUE) {
		if (json_object_is_type(max_obj, json_type_int)) {
			*max_entries = json_object_get_int(max_obj);
			if ((*max_entries < 0) || (*max_entries > (2e16-1))) {
				afb_req_fail(request, "max_entries out of range", NULL);
				return FALSE;
			}
		} else {
			afb_req_fail(request, "max_entries not integer", NULL);
			return FALSE;
		}
	}

	return TRUE;
}

void contacts(afb_req_t request)
{
	struct json_object *jresp;
	const char *cached = NULL;
	int max_entries = -1;

	if (!connected) {
		afb_req_fail(request, "not connected", NULL);
		return;
	}

	if (!parse_max_entries_parameter(request, &max_entries))
		return;

	if (max_entries == -1 && !read_cached_value("default", &cached)) {
		jresp = json_tokener_parse(cached);
	} else {
		org_bluez_obex_phonebook_access1_call_select_sync(
			phonebook, INTERNAL, CONTACTS, NULL, NULL);
		jresp = get_vcards(max_entries);
	}

	afb_req_success(request, jresp, "contacts");
}

void entry(afb_req_t request)
{
	struct json_object *handle_obj, *jresp, *query;
	const gchar *handle;
	gchar *list = NULL;

	if (!connected) {
		afb_req_fail(request, "not connected", NULL);
		return;
	}

	query = afb_req_json(request);

	if (json_object_object_get_ex(query, "handle", &handle_obj) == TRUE) {
		if (json_object_is_type(handle_obj, json_type_string)) {
			handle = json_object_get_string(handle_obj);
		} else {
			afb_req_fail(request, "handle not string", NULL);
			return;
		}
	} else {
		afb_req_fail(request, "no handle", NULL);
		return;
	}

	if (!parse_list_parameter(request, &list))
		return;

	org_bluez_obex_phonebook_access1_call_select_sync(
			phonebook, INTERNAL, list, NULL, NULL);
	jresp = get_vcard(handle);

	if (!jresp) {
		afb_req_fail(request, "invalid handle", NULL);
		return;
	}

	afb_req_success(request, jresp, "list entry");
}

void history(afb_req_t request)
{
	struct json_object *jresp;
	gchar *list = NULL;
	int max_entries = -1;

	if (!connected) {
		afb_req_fail(request, "not connected", NULL);
		return;
	}

	if (!parse_list_parameter(request, &list))
		return;

	if (!parse_max_entries_parameter(request, &max_entries))
		return;

	org_bluez_obex_phonebook_access1_call_select_sync(
			phonebook, INTERNAL, list, NULL, NULL);
	jresp = get_vcards(max_entries);

	afb_req_success(request, jresp, "call history");
}

static void search(afb_req_t request)
{
	struct json_object *query, *val, *results_array, *response;
	const char *number = NULL;
	GVariantIter iter;
	GVariantBuilder *b;
	GVariant *entry, *filter, *results;
	gchar *card, *name;
	int max_entries = -1;

	if (!connected) {
		afb_req_fail(request, "not connected", NULL);
		return;
	}

	query = afb_req_json(request);

	json_object_object_get_ex(query, "number", &val);
	if (json_object_is_type(val, json_type_string)) {
		number = json_object_get_string(val);
	}
	else {
		afb_req_fail(request, "no number", NULL);
		return;
	}

	if (!parse_max_entries_parameter(request, &max_entries))
		return;

	org_bluez_obex_phonebook_access1_call_select_sync(
			phonebook, INTERNAL, CONTACTS, NULL, NULL);

	b = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add(b, "{sv}", "Order", g_variant_new_string("indexed"));
	g_variant_builder_add(b, "{sv}", "Offset", g_variant_new_uint16(0));
	g_variant_builder_add(b, "{sv}", "Format", g_variant_new_string("vcard30"));
	if (max_entries >= 0)
		g_variant_builder_add(b, "{sv}", "MaxCount", g_variant_new_uint16((guint16)max_entries));
	filter = g_variant_builder_end(b);

	org_bluez_obex_phonebook_access1_call_search_sync(
			phonebook, "number", number, filter, &results, NULL, NULL);

	g_variant_builder_unref(b);

	results_array = json_object_new_array();
	g_variant_iter_init(&iter, results);
	while ((entry = g_variant_iter_next_value(&iter))) {
		g_variant_get(entry, "(ss)", &card, &name);
		g_variant_unref(entry);
		json_object *result_obj = json_object_new_object();
		json_object *card_str = json_object_new_string(card);
		json_object *name_str = json_object_new_string(name);
		json_object_object_add(result_obj, "handle", card_str);
		json_object_object_add(result_obj, "name", name_str);
		json_object_array_add(results_array, result_obj);
		g_free(card);
		g_free(name);
	}
	response = json_object_new_object();
	json_object_object_add(response, "results", results_array);

	afb_req_success(request, response, NULL);
}

static void status(afb_req_t request)
{
	struct json_object *response, *status;

	response = json_object_new_object();
	g_mutex_lock(&connected_mutex);
	status = json_object_new_boolean(connected);
	g_mutex_unlock(&connected_mutex);
	json_object_object_add(response, "connected", status);

	afb_req_success(request, response, NULL);
}

static void subscribe(afb_req_t request)
{
	const char *value = afb_req_value(request, "value");

	if (!value) {
		afb_req_fail(request, "failed", "No event");
		return;
	}

	if (!g_strcmp0(value, "status")) {
		afb_req_subscribe(request, status_event);
		afb_req_success(request, NULL, NULL);

		struct json_object *event, *status;
		event = json_object_new_object();
		g_mutex_lock(&connected_mutex);
		status = json_object_new_boolean(connected);
		g_mutex_unlock(&connected_mutex);
		json_object_object_add(event, "connected", status);
		afb_event_push(status_event, event);
	} else {
		afb_req_fail(request, "failed", "Invalid event");
	}
}

static void unsubscribe(afb_req_t request)
{
	const char *value = afb_req_value(request, "value");
	if (value) {
		if (!g_strcmp0(value, "status")) {
			afb_req_unsubscribe(request, status_event);
		} else {
			afb_req_fail(request, "failed", "Invalid event");
			return;
		}
	}

	afb_req_success(request, NULL, NULL);
}

static gboolean init_session(const gchar *address)
{
	GVariant *args;
	GVariantBuilder *b;
	const gchar *target;
	gchar *spath;

	xfer_queue = g_hash_table_new(g_str_hash, g_str_equal);
	xfer_complete = g_hash_table_new(g_str_hash, g_str_equal);

	obj_manager = object_manager_client_new_for_bus_sync(
			G_BUS_TYPE_SESSION,
			G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
			"org.bluez.obex", "/", NULL, NULL);

	if (obj_manager == NULL) {
		AFB_ERROR("Failed to create object manager");
		return FALSE;
	}

	g_signal_connect(obj_manager,
			"interface-proxy-properties-changed",
			G_CALLBACK (on_interface_proxy_properties_changed),
			NULL);

	client = org_bluez_obex_client1_proxy_new_for_bus_sync(
			G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE,
			"org.bluez.obex", "/org/bluez/obex", NULL, NULL);

	if (client == NULL) {
		AFB_ERROR("Failed to create client proxy");
		return FALSE;
	}

	b = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add(b, "{sv}", "Target", g_variant_new_string("pbap"));
	args = g_variant_builder_end(b);

	org_bluez_obex_client1_call_create_session_sync(
			client, address, args, &spath, NULL, NULL);

	session = org_bluez_obex_session1_proxy_new_for_bus_sync(
			G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE,
			"org.bluez.obex", spath, NULL, NULL);

	target = org_bluez_obex_session1_get_target(session);
	if (g_strcmp0(target, PBAP_UUID) != 0) {
		AFB_ERROR("Device does not support PBAP");
		return FALSE;
	}

	phonebook = org_bluez_obex_phonebook_access1_proxy_new_for_bus_sync(
			G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE,
			"org.bluez.obex", spath, NULL, NULL);

	return TRUE;
}

static gboolean is_pbap_dev_and_init(struct json_object *dev)
{
	struct json_object *jresp, *props = NULL, *val = NULL;
	int i;

	json_object_object_get_ex(dev, "properties", &props);
	if (!props)
		return FALSE;

	json_object_object_get_ex(props, "connected", &val);
	if (!val || !json_object_get_boolean(val))
		return FALSE;

	json_object_object_get_ex(props, "uuids", &val);
	for (i = 0; i < json_object_array_length(val); i++) {
		const char *uuid = json_object_get_string(json_object_array_get_idx(val, i));
		const char *address = NULL;
		struct json_object *val1 = NULL;

		if (g_strcmp0(PBAP_UUID, uuid))
			continue;

		json_object_object_get_ex(props, "address", &val1);
		address = json_object_get_string(val1);

		if (!address)
			return FALSE;

		if (init_session(address)) {
			jresp = json_object_new_object();

			g_mutex_lock(&connected_mutex);
			connected = TRUE;
			json_object_object_add(jresp, "connected",
				json_object_new_boolean(connected));
			g_mutex_unlock(&connected_mutex);

			afb_event_push(status_event, jresp);

			json_object_object_get_ex(dev, "device", &val1);
			AFB_NOTICE("PBAP device connected: %s", json_object_get_string(val1));

			/* probably should be made async */
			org_bluez_obex_phonebook_access1_call_select_sync(
				phonebook, INTERNAL, CONTACTS, NULL, NULL);
			update_or_insert("default",
				json_object_to_json_string_ext(get_vcards(-1),
				JSON_C_TO_STRING_PLAIN));

			return TRUE;
		}
		break;
	}

	return FALSE;
}

static void discovery_result_cb(void *closure, struct json_object *result,
				const char *error, const char *info,
				afb_api_t api)
{
	enum json_type type;
	struct json_object *dev, *tmp;
	int i;

	if (!json_object_object_get_ex(result, "devices", &tmp))
		return;
	type = json_object_get_type(tmp);

	if (type != json_type_array)
		return;

	for (i = 0; i < json_object_array_length(tmp); i++) {
		dev = json_object_array_get_idx(tmp, i);
		if (is_pbap_dev_and_init(dev))
			return;
	}
}

static void init_bt(afb_api_t api)
{
	struct json_object *args;

	args = json_object_new_object();
	json_object_object_add(args , "value", json_object_new_string("device_changes"));
	afb_api_call_sync(api, "Bluetooth-Manager", "subscribe", args, NULL, NULL, NULL);

	args = json_object_new_object();
	afb_api_call(api, "Bluetooth-Manager", "managed_objects", args, discovery_result_cb, NULL);
}

static const afb_verb_t binding_verbs[] = {
	{ .verb = "contacts",	.callback = contacts,		.info = "List contacts" },
	{ .verb = "entry",	.callback = entry,		.info = "List call entry" },
	{ .verb = "history",	.callback = history,		.info = "List call history" },
	{ .verb = "search",	.callback = search,		.info = "Search for entry" },
	{ .verb = "status",	.callback = status,		.info = "Get status" },
	{ .verb = "subscribe",	.callback = subscribe,		.info = "Subscribe to events" },
	{ .verb = "unsubscribe",.callback = unsubscribe,	.info = "Unsubscribe to events" },
	{ }
};

static void *main_loop_thread(void *unused)
{
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);

	return NULL;
}

static int init(afb_api_t api)
{
	AFB_NOTICE("PBAP binding init");

	pthread_t tid;
	int ret = 0;

	status_event = afb_daemon_make_event("status");

	ret = afb_daemon_require_api("Bluetooth-Manager", 1);
	if (ret) {
		AFB_ERROR("unable to initialize bluetooth binding");
		return -1;
	}

	/* Start the main loop thread */
	pthread_create(&tid, NULL, main_loop_thread, NULL);

	init_bt(api);

	return ret;
}


static void process_connection_event(afb_api_t api, struct json_object *object)
{
	struct json_object *jresp, *val = NULL, *props = NULL;
	const char *action, *device;

	json_object_object_get_ex(object, "action", &val);
	if (!val)
		return;
	action = json_object_get_string(val);
	if (g_strcmp0("changed", action))
		return;

	json_object_object_get_ex(object, "properties", &props);
	if (!props)
		return;

	json_object_object_get_ex(props, "connected", &val);
	if (!val)
		return;

	if (json_object_get_boolean(val)) {
		struct json_object *args = json_object_new_object();
		afb_api_call(api, "Bluetooth-Manager", "managed_objects",
			     args, discovery_result_cb, NULL);
		return;
	}

	json_object_object_get_ex(object, "device", &val);
	if (!val)
		return;

	device = json_object_get_string(val);
	jresp = json_object_new_object();

	g_mutex_lock(&connected_mutex);
	connected = FALSE;
	json_object_object_add(jresp, "connected", json_object_new_boolean(connected));
	g_mutex_unlock(&connected_mutex);

	afb_event_push(status_event, jresp);

	AFB_NOTICE("PBAP device disconnected: %s", device);
}

static void onevent(afb_api_t api, const char *event, struct json_object *object)
{
	if (!g_ascii_strcasecmp(event, "Bluetooth-Manager/device_changes"))
		process_connection_event(api, object);
	else
		AFB_ERROR("Unsupported event: %s\n", event);
}

const afb_binding_t afbBindingV3 = {
	.api = "bluetooth-pbap",
	.verbs = binding_verbs,
	.init = init,
	.onevent = onevent,
};
