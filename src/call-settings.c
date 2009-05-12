/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdio.h>

#include <dbus/dbus.h>
#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "driver.h"
#include "common.h"
#include "dbus-gsm.h"
#include "modem.h"
#include "ussd.h"

#define CALL_SETTINGS_INTERFACE "org.ofono.CallSettings"

#define CALL_SETTINGS_FLAG_CACHED 0x1

struct call_settings_data {
	struct ofono_call_settings_ops *ops;
	int clir;
	int colr;
	int clip;
	int colp;
	int clir_setting;
	int flags;
	DBusMessage *pending;
	int ss_req_type;
	int call_setting_type;
};

enum call_setting_type {
	CALL_SETTING_TYPE_CLIP = 0,
	CALL_SETTING_TYPE_COLP,
	CALL_SETTING_TYPE_COLR,
	CALL_SETTING_TYPE_CLIR
};

static void cs_register_ss_controls(struct ofono_modem *modem);
static void cs_unregister_ss_controls(struct ofono_modem *modem);

static const char *clip_status_to_string(int status)
{
	switch (status) {
	case CLIP_STATUS_NOT_PROVISIONED:
		return "disabled";
	case CLIP_STATUS_PROVISIONED:
		return "enabled";
	default:
		return "unknown";
	}
}

static const char *colp_status_to_string(int status)
{
	switch (status) {
	case COLP_STATUS_NOT_PROVISIONED:
		return "disabled";
	case COLP_STATUS_PROVISIONED:
		return "enabled";
	default:
		return "unknown";
	}
}

static const char *colr_status_to_string(int status)
{
	switch (status) {
	case COLR_STATUS_NOT_PROVISIONED:
		return "disabled";
	case COLR_STATUS_PROVISIONED:
		return "enabled";
	default:
		return "unknown";
	}
}

static const char *hide_callerid_to_string(int status)
{
	switch (status) {
	case OFONO_CLIR_OPTION_DEFAULT:
		return "default";
	case OFONO_CLIR_OPTION_INVOCATION:
		return "enabled";
	case OFONO_CLIR_OPTION_SUPPRESSION:
		return "disabled";
	default:
		return "default";
	}
}

static const char *clir_status_to_string(int status)
{
	switch (status) {
	case CLIR_STATUS_NOT_PROVISIONED:
		return "disabled";
	case CLIR_STATUS_PROVISIONED_PERMANENT:
		return "permanent";
	case CLIR_STATUS_TEMPORARY_RESTRICTED:
		return "on";
	case CLIR_STATUS_TEMPORARY_ALLOWED:
		return "off";
	default:
		return "unknown";
	}
}

static void set_clir_network(struct ofono_modem *modem, int clir)
{
	struct call_settings_data *cs = modem->call_settings;

	if (cs->clir != clir) {
		DBusConnection *conn = dbus_gsm_connection();
		const char *str = clir_status_to_string(clir);

		cs->clir = clir;

		dbus_gsm_signal_property_changed(conn, modem->path,
				CALL_SETTINGS_INTERFACE,
				"CallingLineRestriction",
				DBUS_TYPE_STRING, &str);
	}
}

static void set_clir_override(struct ofono_modem *modem, int override)
{
	struct call_settings_data *cs = modem->call_settings;

	if (cs->clir_setting != override) {
		DBusConnection *conn = dbus_gsm_connection();
		const char *str = hide_callerid_to_string(override);

		cs->clir_setting = override;

		dbus_gsm_signal_property_changed(conn, modem->path,
				CALL_SETTINGS_INTERFACE,
				"HideCallerId", DBUS_TYPE_STRING, &str);
	}
}

static void set_clip(struct ofono_modem *modem, int clip)
{
	struct call_settings_data *cs = modem->call_settings;

	if (cs->clip != clip) {
		DBusConnection *conn = dbus_gsm_connection();
		const char *str = clip_status_to_string(clip);

		cs->clip = clip;

		dbus_gsm_signal_property_changed(conn, modem->path,
				CALL_SETTINGS_INTERFACE,
				"CallingLinePresentation",
				DBUS_TYPE_STRING, &str);
	}
}

static void set_colp(struct ofono_modem *modem, int colp)
{
	struct call_settings_data *cs = modem->call_settings;

	if (cs->colp != colp) {
		DBusConnection *conn = dbus_gsm_connection();
		const char *str = colp_status_to_string(colp);

		cs->colp = colp;

		dbus_gsm_signal_property_changed(conn, modem->path,
				CALL_SETTINGS_INTERFACE,
				"CalledLinePresentation",
				DBUS_TYPE_STRING, &str);
	}
}

static void set_colr(struct ofono_modem *modem, int colr)
{
	struct call_settings_data *cs = modem->call_settings;

	if (cs->colr != colr) {
		DBusConnection *conn = dbus_gsm_connection();
		const char *str = colr_status_to_string(colr);

		cs->colr = colr;

		dbus_gsm_signal_property_changed(conn, modem->path,
				CALL_SETTINGS_INTERFACE,
				"CalledLineRestriction",
				DBUS_TYPE_STRING, &str);
	}
}

static struct call_settings_data *call_settings_create()
{
	struct call_settings_data *r;

	r = g_try_new0(struct call_settings_data, 1);

	if (!r)
		return r;

	/* Set all the settings to unknown state */
	r->clip = 2;
	r->clir = 2;
	r->colp = 2;
	r->colr = 2;

	return r;
}

static void call_settings_destroy(gpointer data)
{
	struct ofono_modem *modem = data;
	struct call_settings_data *cs = modem->call_settings;

	cs_unregister_ss_controls(modem);

	g_free(cs);
}

static void generate_ss_query_reply(struct ofono_modem *modem,
					const char *context, const char *value)
{
	struct call_settings_data *cs = modem->call_settings;
	const char *sig = "(ss)";
	const char *ss_type = ss_control_type_to_string(cs->ss_req_type);
	DBusMessageIter iter;
	DBusMessageIter var;
	DBusMessageIter vstruct;
	DBusMessage *reply;

	reply = dbus_message_new_method_return(cs->pending);

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &context);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, sig, &var);

	dbus_message_iter_open_container(&var, DBUS_TYPE_STRUCT, NULL,
						&vstruct);

	dbus_message_iter_append_basic(&vstruct, DBUS_TYPE_STRING,
					&ss_type);

	dbus_message_iter_append_basic(&vstruct, DBUS_TYPE_STRING, &value);

	dbus_message_iter_close_container(&var, &vstruct);

	dbus_message_iter_close_container(&iter, &var);

	dbus_gsm_pending_reply(&cs->pending, reply);
}

static void clip_colp_colr_ss_query_cb(const struct ofono_error *error,
					int status, void *data)
{
	struct ofono_modem *modem = data;
	struct call_settings_data *cs = modem->call_settings;
	const char *context;
	const char *value;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Error occurred during ss control query");
		dbus_gsm_pending_reply(&cs->pending,
					dbus_gsm_failed(cs->pending));

		return;
	}

	switch (cs->call_setting_type) {
	case CALL_SETTING_TYPE_CLIP:
		set_clip(modem, status);
		value = clip_status_to_string(status);
		context = "CallingLinePresentation";
		break;

	case CALL_SETTING_TYPE_COLP:
		set_colp(modem, status);
		value = colp_status_to_string(status);
		context = "CalledLinePresentation";
		break;

	case CALL_SETTING_TYPE_COLR:
		set_colr(modem, status);
		value = colr_status_to_string(status);
		context = "CallingLineRestriction";
		break;

	default:
		dbus_gsm_pending_reply(&cs->pending,
				dbus_gsm_failed(cs->pending));
		ofono_error("Unknown type during COLR/COLP/CLIP ss");
		return;
	};

	generate_ss_query_reply(modem, context, value);
}

static gboolean clip_colp_colr_ss(struct ofono_modem *modem, int type,
				const char *sc, const char *sia,
				const char *sib, const char *sic,
				const char *dn, DBusMessage *msg)
{
	struct call_settings_data *cs = modem->call_settings;
	DBusConnection *conn = dbus_gsm_connection();
	void (*query_op)(struct ofono_modem *modem, ofono_call_setting_status_cb_t cb,
				void *data);

	if (!cs)
		return FALSE;

	if (cs->pending) {
		DBusMessage *reply = dbus_gsm_busy(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	if (!strcmp(sc, "30")) {
		cs->call_setting_type = CALL_SETTING_TYPE_CLIP;
		query_op = cs->ops->clip_query;
	} else if (!strcmp(sc, "76")) {
		cs->call_setting_type = CALL_SETTING_TYPE_COLP;
		query_op = cs->ops->colp_query;
	} else if (!strcmp(sc, "77")) {
		cs->call_setting_type = CALL_SETTING_TYPE_COLR;
		query_op = cs->ops->colr_query;
	} else
		return FALSE;

	if (type != SS_CONTROL_TYPE_QUERY || strlen(sia) || strlen(sib) ||
		strlen(sic) || strlen(dn)) {
		DBusMessage *reply = dbus_gsm_invalid_format(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	if (!query_op) {
		DBusMessage *reply = dbus_gsm_not_implemented(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	ofono_debug("Received CLIP/COLR/COLP query ss control");

	cs->pending = dbus_message_ref(msg);

	query_op(modem, clip_colp_colr_ss_query_cb, modem);

	return TRUE;
}

static void clir_ss_query_callback(const struct ofono_error *error,
					int override, int network, void *data)
{
	struct ofono_modem *modem = data;
	struct call_settings_data *cs = modem->call_settings;
	const char *value;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("setting clir via SS failed");
		dbus_gsm_pending_reply(&cs->pending,
					dbus_gsm_failed(cs->pending));

		return;
	}

	switch (network) {
	case CLIR_STATUS_UNKNOWN:
		value = "uknown";
		break;

	case CLIR_STATUS_PROVISIONED_PERMANENT:
		value = "enabled";
		break;

	case CLIR_STATUS_NOT_PROVISIONED:
		value = "disabled";
		break;

	case CLIR_STATUS_TEMPORARY_RESTRICTED:
		if (override == OFONO_CLIR_OPTION_SUPPRESSION)
			value = "enabled";
		else
			value = "disabled";
		break;

	case CLIR_STATUS_TEMPORARY_ALLOWED:
		if (override == OFONO_CLIR_OPTION_INVOCATION)
			value = "enabled";
		else
			value = "disabled";
		break;
	};

	generate_ss_query_reply(modem, "CallingLineRestriction", value);

	set_clir_network(modem, network);
	set_clir_override(modem, override);
}

static void clir_ss_set_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct call_settings_data *cs = modem->call_settings;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("setting clir via SS failed");
		dbus_gsm_pending_reply(&cs->pending,
					dbus_gsm_failed(cs->pending));

		return;
	}

	cs->ops->clir_query(modem, clir_ss_query_callback, modem);
}

static gboolean clir_ss_control(struct ofono_modem *modem, int type,
				const char *sc, const char *sia,
				const char *sib, const char *sic,
				const char *dn, DBusMessage *msg)
{
	struct call_settings_data *cs = modem->call_settings;
	DBusConnection *conn = dbus_gsm_connection();

	if (!cs)
		return FALSE;

	if (strcmp(sc, "31"))
		return FALSE;

	if (cs->pending) {
		DBusMessage *reply = dbus_gsm_busy(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	/* This is the temporary form of CLIR, handled in voicecalls */
	if (!strlen(sia) && !strlen(sib) & !strlen(sic) &&
		strlen(dn) && type != SS_CONTROL_TYPE_QUERY)
		return FALSE;

	if (strlen(sia) || strlen(sib) || strlen(sic) || strlen(dn)) {
		DBusMessage *reply = dbus_gsm_invalid_format(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	if ((type == SS_CONTROL_TYPE_QUERY && !cs->ops->clir_query) ||
		(type != SS_CONTROL_TYPE_QUERY && !cs->ops->clir_set)) {
		DBusMessage *reply = dbus_gsm_not_implemented(msg);
		g_dbus_send_message(conn, reply);

		return TRUE;
	}

	cs->call_setting_type = CALL_SETTING_TYPE_CLIR;
	cs->pending = dbus_message_ref(msg);

	switch (type) {
	case SS_CONTROL_TYPE_REGISTRATION:
	case SS_CONTROL_TYPE_ACTIVATION:
		cs->ss_req_type = SS_CONTROL_TYPE_ACTIVATION;
		cs->ops->clir_set(modem, OFONO_CLIR_OPTION_INVOCATION,
					clir_ss_set_callback, modem);
		break;

	case SS_CONTROL_TYPE_QUERY:
		cs->ss_req_type = SS_CONTROL_TYPE_QUERY;
		cs->ops->clir_query(modem, clir_ss_query_callback,
					modem);
		break;

	case SS_CONTROL_TYPE_DEACTIVATION:
	case SS_CONTROL_TYPE_ERASURE:
		cs->ss_req_type = SS_CONTROL_TYPE_DEACTIVATION;
		cs->ops->clir_set(modem, OFONO_CLIR_OPTION_SUPPRESSION,
					clir_ss_set_callback, modem);
		break;
	};

	return TRUE;
}

static void cs_register_ss_controls(struct ofono_modem *modem)
{
	struct call_settings_data *cs = modem->call_settings;

	ss_control_register(modem, "30", clip_colp_colr_ss);
	ss_control_register(modem, "31", clir_ss_control);
	ss_control_register(modem, "76", clip_colp_colr_ss);

	if (cs->ops->colr_query)
		ss_control_register(modem, "77", clip_colp_colr_ss);
}

static void cs_unregister_ss_controls(struct ofono_modem *modem)
{
	struct call_settings_data *cs = modem->call_settings;

	ss_control_unregister(modem, "30", clip_colp_colr_ss);
	ss_control_unregister(modem, "31", clir_ss_control);
	ss_control_unregister(modem, "76", clip_colp_colr_ss);

	if (cs->ops->colr_query)
		ss_control_unregister(modem, "77", clip_colp_colr_ss);
}

static DBusMessage *generate_get_properties_reply(struct ofono_modem *modem,
							DBusMessage *msg)
{
	struct call_settings_data *cs = modem->call_settings;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *str;

	reply = dbus_message_new_method_return(msg);

	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
						PROPERTIES_ARRAY_SIGNATURE,
						&dict);

	str = clip_status_to_string(cs->clip);
	dbus_gsm_dict_append(&dict, "CallingLinePresentation",
				DBUS_TYPE_STRING, &str);

	str = colp_status_to_string(cs->colp);
	dbus_gsm_dict_append(&dict, "CalledLinePresentation",
				DBUS_TYPE_STRING, &str);

	str = colr_status_to_string(cs->colr);
	dbus_gsm_dict_append(&dict, "CalledLineRestriction",
				DBUS_TYPE_STRING, &str);

	str = clir_status_to_string(cs->clir);
	dbus_gsm_dict_append(&dict, "CallingLineRestriction",
				DBUS_TYPE_STRING, &str);

	str = hide_callerid_to_string(cs->clir_setting);
	dbus_gsm_dict_append(&dict, "HideCallerId", DBUS_TYPE_STRING, &str);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void cs_clir_callback(const struct ofono_error *error,
				int override_setting, int network_setting,
				void *data)
{
	struct ofono_modem *modem = data;
	struct call_settings_data *cs = modem->call_settings;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto out;

	set_clir_network(modem, network_setting);
	set_clir_override(modem, override_setting);

	cs->flags |= CALL_SETTINGS_FLAG_CACHED;

out:
	if (cs->pending) {
		DBusMessage *reply = generate_get_properties_reply(modem,
								cs->pending);
		dbus_gsm_pending_reply(&cs->pending, reply);
	}
}

static gboolean query_clir(gpointer user)
{
	struct ofono_modem *modem = user;
	struct call_settings_data *cs = modem->call_settings;

	if (!cs->ops->clir_query) {
		if (cs->pending) {
			DBusMessage *reply =
				generate_get_properties_reply(modem,
								cs->pending);
			dbus_gsm_pending_reply(&cs->pending, reply);
		}

		return FALSE;
	}

	cs->ops->clir_query(modem, cs_clir_callback, modem);

	return FALSE;
}

static void cs_clip_callback(const struct ofono_error *error,
				int state, void *data)
{
	struct ofono_modem *modem = data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		set_clip(modem, state);

	g_timeout_add(0, query_clir, modem);
}

static gboolean query_clip(gpointer user)
{
	struct ofono_modem *modem = user;
	struct call_settings_data *cs = modem->call_settings;

	if (!cs->ops->clip_query) {
		query_clir(modem);
		return FALSE;
	}

	cs->ops->clip_query(modem, cs_clip_callback, modem);

	return FALSE;
}

static void cs_colp_callback(const struct ofono_error *error,
				int state, void *data)
{
	struct ofono_modem *modem = data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		set_colp(modem, state);

	g_timeout_add(0, query_clip, modem);
}

static gboolean query_colp(gpointer user)
{
	struct ofono_modem *modem = user;
	struct call_settings_data *cs = modem->call_settings;

	if (!cs->ops->colp_query) {
		query_clip(modem);
		return FALSE;
	}

	cs->ops->colp_query(modem, cs_colp_callback, modem);

	return FALSE;
}

static void cs_colr_callback(const struct ofono_error *error,
				int state, void *data)
{
	struct ofono_modem *modem = data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		set_colr(modem, state);

	g_timeout_add(0, query_colp, modem);
}

static gboolean query_colr(gpointer user)
{
	struct ofono_modem *modem = user;
	struct call_settings_data *cs = modem->call_settings;

	if (!cs->ops->colr_query) {
		query_colp(modem);
		return FALSE;
	}

	cs->ops->colr_query(modem, cs_colr_callback, modem);

	return FALSE;
}

static DBusMessage *cs_get_properties(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_settings_data *cs = modem->call_settings;

	if (cs->pending)
		return dbus_gsm_busy(msg);

	if (cs->flags & CALL_SETTINGS_FLAG_CACHED)
		return generate_get_properties_reply(modem, msg);

	/* Query the settings and report back */
	cs->pending = dbus_message_ref(msg);

	query_colr(modem);

	return NULL;
}

static void clir_set_query_callback(const struct ofono_error *error,
					int override_setting,
					int network_setting, void *data)
{
	struct ofono_modem *modem = data;
	struct call_settings_data *cs = modem->call_settings;
	DBusMessage *reply;

	if (!cs->pending)
		return;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("setting clir was successful, but the query was not");

		cs->flags &= ~CALL_SETTINGS_FLAG_CACHED;

		reply = dbus_gsm_failed(cs->pending);
		dbus_gsm_pending_reply(&cs->pending, reply);
		return;
	}

	reply = dbus_message_new_method_return(cs->pending);
	dbus_gsm_pending_reply(&cs->pending, reply);

	set_clir_override(modem, override_setting);
	set_clir_network(modem, network_setting);
}

static void clir_set_callback(const struct ofono_error *error, void *data)
{
	struct ofono_modem *modem = data;
	struct call_settings_data *cs = modem->call_settings;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("setting clir failed");
		dbus_gsm_pending_reply(&cs->pending,
					dbus_gsm_failed(cs->pending));

		return;
	}

	/* Assume that if we have clir_set, we have clir_query */
	cs->ops->clir_query(modem, clir_set_query_callback, modem);
}

static DBusMessage *set_clir(DBusMessage *msg, struct ofono_modem *modem,
				const char *setting)
{
	struct call_settings_data *cs = modem->call_settings;
	int clir = -1;

	if (cs->ops->clir_set == NULL)
		return dbus_gsm_not_implemented(msg);

	if (!strcmp(setting, "default"))
		clir = 0;
	else if (!strcmp(setting, "enabled"))
		clir = 1;
	else if (!strcmp(setting, "disabled"))
		clir = 2;

	if (clir == -1)
		return dbus_gsm_invalid_format(msg);

	cs->pending = dbus_message_ref(msg);

	cs->ops->clir_set(modem, clir, clir_set_callback, modem);

	return NULL;
}

static DBusMessage *cs_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct call_settings_data *cs = modem->call_settings;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;

	if (cs->pending)
		return dbus_gsm_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return dbus_gsm_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return dbus_gsm_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return dbus_gsm_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (!strcmp(property, "HideCallerId")) {
		const char *setting;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return dbus_gsm_invalid_format(msg);

		dbus_message_iter_get_basic(&var, &setting);

		return set_clir(msg, modem, setting);
	}

	return dbus_gsm_invalid_args(msg);
}

static GDBusMethodTable cs_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	cs_get_properties,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "SetProperty",	"sv",	"",		cs_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable cs_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

int ofono_call_settings_register(struct ofono_modem *modem,
				struct ofono_call_settings_ops *ops)
{
	DBusConnection *conn = dbus_gsm_connection();

	if (modem == NULL)
		return -1;

	if (ops == NULL)
		return -1;

	modem->call_settings = call_settings_create();

	if (!modem->call_settings)
		return -1;

	modem->call_settings->ops = ops;

	if (!g_dbus_register_interface(conn, modem->path,
					CALL_SETTINGS_INTERFACE,
					cs_methods, cs_signals, NULL,
					modem, call_settings_destroy)) {
		ofono_error("Could not register CallSettings %s", modem->path);
		call_settings_destroy(modem);

		return -1;
	}

	ofono_debug("Registered call settings interface");

	cs_register_ss_controls(modem);

	modem_add_interface(modem, CALL_SETTINGS_INTERFACE);
	return 0;
}

void ofono_call_settings_unregister(struct ofono_modem *modem)
{
	struct call_settings_data *cs = modem->call_settings;
	DBusConnection *conn = dbus_gsm_connection();

	if (!cs)
		return;

	modem_remove_interface(modem, CALL_SETTINGS_INTERFACE);
	g_dbus_unregister_interface(conn, modem->path,
					CALL_SETTINGS_INTERFACE);

	modem->call_settings = NULL;
}
