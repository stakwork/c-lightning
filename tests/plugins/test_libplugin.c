#include "config.h"
#include <ccan/array_size/array_size.h>
#include <ccan/tal/str/str.h>
#include <common/json_param.h>
#include <common/json_stream.h>
#include <common/memleak.h>
#include <plugins/libplugin.h>

static const char *somearg;
static bool self_disable = false;
static bool dont_shutdown = false;

static struct command_result *get_ds_done(struct command *cmd,
					  const char *val,
					  char *arg)
{
	if (!val)
		val = "NOT FOUND";
	return command_success(cmd, json_out_obj(cmd, arg, val));
}

static struct command_result *get_ds_bin_done(struct command *cmd,
					      const u8 *val,
					      char *arg)
{
	plugin_log(cmd->plugin, LOG_INFORM, "get_ds_bin_done: %s",
		   val ? tal_hex(tmpctx, val) : "NOT FOUND");

	return jsonrpc_get_datastore_string(cmd->plugin, cmd,
					    "test_libplugin/name",
					    get_ds_done, arg);
}

static struct command_result *json_helloworld(struct command *cmd,
					      const char *buf,
					      const jsmntok_t *params)
{
	const char *name;

	if (!param(cmd, buf, params,
		   p_opt("name", param_string, &name),
		   NULL))
		return command_param_failed();

	plugin_notify_message(cmd, LOG_INFORM, "Notification from %s", "json_helloworld");

	if (!name)
		return jsonrpc_get_datastore_binary(cmd->plugin, cmd,
						    "test_libplugin/name",
						    get_ds_bin_done,
						    "hello");

	return command_success(cmd, json_out_obj(cmd, "hello", name));
}

static struct command_result *
json_peer_connected(struct command *cmd,
		    const char *buf,
		    const jsmntok_t *params)
{
	const jsmntok_t *peertok, *idtok;
	struct json_stream *response;

	peertok = json_get_member(buf, params, "peer");
	assert(peertok);
	idtok = json_get_member(buf, peertok, "id");
	assert(idtok);
	plugin_log(cmd->plugin, LOG_INFORM, "%s peer_connected",
		   json_strdup(tmpctx, buf, idtok));

	response = jsonrpc_stream_success(cmd);
	json_add_string(response, "result", "continue");

	return command_finished(cmd, response);
}

static struct command_result *json_connected(struct command *cmd,
					     const char *buf,
					     const jsmntok_t *params)
{
	const jsmntok_t *idtok = json_get_member(buf, params, "id");
	assert(idtok);
	plugin_log(cmd->plugin, LOG_INFORM, "%s connected",
		   json_strdup(tmpctx, buf, idtok));
	return notification_handled(cmd);
}

static struct command_result *json_shutdown(struct command *cmd,
					    const char *buf,
					    const jsmntok_t *params)
{
	plugin_log(cmd->plugin, LOG_DBG, "shutdown called");

	if (dont_shutdown)
		return notification_handled(cmd);

	plugin_exit(cmd->plugin, 0);
}

static struct command_result *testrpc_cb(struct command *cmd,
					 const char *buf,
					 const jsmntok_t *params,
					 void *cb_arg UNUSED)
{
	int i = 0;
	const jsmntok_t *t;
	struct json_stream *response;

	response = jsonrpc_stream_success(cmd);
	json_for_each_obj(i, t, params)
		json_add_tok(response, json_strdup(tmpctx, buf, t), t+1, buf);

	return command_finished(cmd, response);
}

static struct command_result *json_testrpc(struct command *cmd,
					   const char *buf,
					   const jsmntok_t *params)
{
	struct out_req *req;

	if (!param(cmd, buf, params, NULL))
		return command_param_failed();

	req = jsonrpc_request_start(cmd->plugin, cmd, "getinfo", testrpc_cb,
				    testrpc_cb, NULL);
	return send_outreq(cmd->plugin, req);
}

static const char *init(struct plugin *p,
			const char *buf UNUSED,
			const jsmntok_t *config UNUSED)
{
	const char *name, *err_str, *err_hex;
	const u8 *binname;

	plugin_log(p, LOG_DBG, "test_libplugin initialised!");
	if (somearg)
		plugin_log(p, LOG_DBG, "somearg = %s", somearg);
	somearg = tal_free(somearg);

	if (self_disable)
		return "Disabled via selfdisable option";

	/* Test rpc_scan_datastore funcs */
	err_str = rpc_scan_datastore_str(tmpctx, p, "test_libplugin/name",
					 JSON_SCAN_TAL(tmpctx, json_strdup,
						       &name));
	if (err_str)
		name = NULL;
	err_hex = rpc_scan_datastore_hex(tmpctx, p, "test_libplugin/name",
					 JSON_SCAN_TAL(tmpctx, json_tok_bin_from_hex,
						       &binname));
	if (err_hex)
		binname = NULL;

	plugin_log(p, LOG_INFORM, "String name from datastore: %s",
		   name ? name : err_str);
	plugin_log(p, LOG_INFORM, "Hex name from datastore: %s",
		   binname ? tal_hex(tmpctx, binname) : err_hex);

 	return NULL;
}

static const struct plugin_command commands[] = { {
		"helloworld",
		"utils",
		"Say hello to the world.",
		"Returns 'hello world' by default, 'hello {name}' if the name"
		" option was set, and 'hello {name}' if the name parameter "
		"was passed (takes over the option)",
		json_helloworld,
	},
	{
		"testrpc",
		"utils",
		"Makes a simple getinfo call, to test rpc socket.",
		"",
		json_testrpc,
	},
	{
		"testrpc-deprecated",
		"utils",
		"Makes a simple getinfo call, to test rpc socket.",
		"",
		json_testrpc,
		true,
	}
};

static const char *before[] = { "dummy", NULL };
static const char *after[] = { "dummy", NULL };

static const struct plugin_hook hooks[] = { {
		"peer_connected",
		json_peer_connected,
		before,
		after
	}
};

static const struct plugin_notification notifs[] = { {
		"connect",
		json_connected,
	}, {
		"shutdown",
		json_shutdown
	}
};

int main(int argc, char *argv[])
{
	setup_locale();
	plugin_main(argv, init, PLUGIN_RESTARTABLE, true, NULL,
		    commands, ARRAY_SIZE(commands),
	            notifs, ARRAY_SIZE(notifs), hooks, ARRAY_SIZE(hooks),
		    NULL, 0,  /* Notification topics we publish */
		    plugin_option("somearg",
				  "string",
				  "Argument to print at init.",
				  charp_option, &somearg),
		    plugin_option_deprecated("somearg-deprecated",
					     "string",
					     "Deprecated arg for init.",
					     charp_option, &somearg),
		    plugin_option("selfdisable",
				  "flag",
				  "Whether to disable.",
				  flag_option, &self_disable),
		    plugin_option("dont_shutdown",
				  "flag",
				  "Whether to timeout when asked to shutdown.",
				  flag_option, &dont_shutdown),
		    NULL);
}
