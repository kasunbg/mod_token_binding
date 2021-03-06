/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/***************************************************************************
 * Copyright (C) 2017 ZmartZone IAM
 * All rights reserved.
 *
 *      ZmartZone IAM
 *      info@zmartzone.eu
 *      http://www.zmartzone.eu
 *
 * THE SOFTWARE PROVIDED HEREUNDER IS PROVIDED ON AN "AS IS" BASIS, WITHOUT
 * ANY WARRANTIES OR REPRESENTATIONS EXPRESS, IMPLIED OR STATUTORY; INCLUDING,
 * WITHOUT LIMITATION, WARRANTIES OF QUALITY, PERFORMANCE, NONINFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  NOR ARE THERE ANY
 * WARRANTIES CREATED BY A COURSE OR DEALING, COURSE OF PERFORMANCE OR TRADE
 * USAGE.  FURTHERMORE, THERE ARE NO WARRANTIES THAT THE SOFTWARE WILL MEET
 * YOUR NEEDS OR BE FREE FROM ERRORS, OR THAT THE OPERATION OF THE SOFTWARE
 * WILL BE UNINTERRUPTED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @Author: Hans Zandbelt - hans.zandbelt@zmartzone.eu
 *
 **************************************************************************/

#include <httpd.h>
#include <http_config.h>
#include <http_request.h>
#include <http_protocol.h>

#include <apr_strings.h>
#include <apr_hooks.h>
#include <apr_optional.h>
#include <apr_lib.h>

#include "openssl/rand.h"
#include <openssl/ssl.h>

#include "mod_token_binding.h"

#include "token_bind_common.h"
#include "token_bind_server.h"
#include "base64.h"

module AP_MODULE_DECLARE_DATA token_binding_module;

#define tb_log(r, level, fmt, ...) ap_log_rerror(APLOG_MARK, level, 0, r,"# %s: %s", __FUNCTION__, apr_psprintf(r->pool, fmt, ##__VA_ARGS__))
#define tb_slog(s, level, fmt, ...) ap_log_error(APLOG_MARK, level, 0, s, "# %s: %s", __FUNCTION__, apr_psprintf(s->process->pool, fmt, ##__VA_ARGS__))

#define tb_debug(r, fmt, ...) tb_log(r, APLOG_DEBUG, fmt, ##__VA_ARGS__)
#define tb_info(r, fmt, ...)  tb_log(r, APLOG_INFO, fmt, ##__VA_ARGS__)
#define tb_warn(r, fmt, ...)  tb_log(r, APLOG_WARNING, fmt, ##__VA_ARGS__)
#define tb_error(r, fmt, ...) tb_log(r, APLOG_ERR, fmt, ##__VA_ARGS__)

#define tb_sdebug(s, fmt, ...) tb_slog(s, APLOG_DEBUG, fmt, ##__VA_ARGS__)
#define tb_sinfo(r, fmt, ...)  tb_slog(r, APLOG_INFO, fmt, ##__VA_ARGS__)
#define tb_swarn(s, fmt, ...) tb_slog(s, APLOG_WARNING, fmt, ##__VA_ARGS__)
#define tb_serror(s, fmt, ...) tb_slog(s, APLOG_ERR, fmt, ##__VA_ARGS__)

#define TB_CFG_POS_INT_UNSET                  -1

#define TB_CFG_SEC_TB_HDR_NAME                "Sec-Token-Binding"
#define TB_CFG_PROVIDED_TBID_HDR_NAME         "Sec-Provided-Token-Binding-ID"
#define TB_CFG_REFERRED_TBID_HDR_NAME         "Sec-Referred-Token-Binding-ID"
#define TB_CFG_TB_CONTEXT_HDR_NAME            "Sec-Token-Binding-Context"

#define TB_CFG_ENABLED_DEFAULT                 TRUE
#define TB_CFG_PROVIDED_ENV_VAR_DEFAULT       TB_CFG_PROVIDED_TBID_HDR_NAME
#define TB_CFG_REFERRED_ENV_VAR_DEFAULT       TB_CFG_REFERRED_TBID_HDR_NAME
#define TB_CFG_CONTEXT_ENV_VAR_DEFAULT        TB_CFG_TB_CONTEXT_HDR_NAME

typedef struct {
	int enabled;
	tbCache *cache;
	const char *provided_env_var;
	const char *referred_env_var;
	const char *context_env_var;
} tb_server_config;

APR_DECLARE_OPTIONAL_FN(int, tb_add_ext, (server_rec *s, SSL_CTX *ctx));

APR_DECLARE_OPTIONAL_FN(int, ssl_is_https, (conn_rec *));
APR_DECLARE_OPTIONAL_FN(SSL *, ssl_get_ssl_from_request, (request_rec *));

static APR_OPTIONAL_FN_TYPE(ssl_is_https) *ssl_is_https_fn = NULL;
static APR_OPTIONAL_FN_TYPE(ssl_get_ssl_from_request) *get_ssl_from_request_fn =
		NULL;

static const char *tb_cfg_set_enabled(cmd_parms *cmd, void *struct_ptr,
		const char *arg) {
	tb_server_config *cfg = (tb_server_config *) ap_get_module_config(
			cmd->server->module_config, &token_binding_module);
	if (strcmp(arg, "Off") == 0) {
		cfg->enabled = 0;
		return NULL;
	}
	if (strcmp(arg, "On") == 0) {
		cfg->enabled = 1;
		return NULL;
	}
	return "Invalid value: must be \"On\" or \"Off\"";
}

static apr_byte_t tb_cfg_get_enabled(tb_server_config *cfg) {
	return (cfg->enabled != TB_CFG_POS_INT_UNSET) ?
			(cfg->enabled > 0) : TB_CFG_ENABLED_DEFAULT;
}

static const char *tb_cfg_set_provided_env_var(cmd_parms *cmd, void *struct_ptr,
		const char *arg) {
	tb_server_config *cfg = (tb_server_config *) ap_get_module_config(
			cmd->server->module_config, &token_binding_module);
	cfg->provided_env_var = arg;
	return NULL;
}

static const char * tb_cfg_get_provided_env_var(tb_server_config *cfg) {
	return cfg->provided_env_var ?
			cfg->provided_env_var : TB_CFG_PROVIDED_ENV_VAR_DEFAULT;
}

static const char *tb_cfg_set_referred_env_var(cmd_parms *cmd, void *struct_ptr,
		const char *arg) {
	tb_server_config *cfg = (tb_server_config *) ap_get_module_config(
			cmd->server->module_config, &token_binding_module);
	cfg->referred_env_var = arg;
	return NULL;
}

static const char * tb_cfg_get_referred_env_var(tb_server_config *cfg) {
	return cfg->referred_env_var ?
			cfg->referred_env_var : TB_CFG_REFERRED_ENV_VAR_DEFAULT;
}

static const char *tb_cfg_set_context_env_var(cmd_parms *cmd, void *struct_ptr,
		const char *arg) {
	tb_server_config *cfg = (tb_server_config *) ap_get_module_config(
			cmd->server->module_config, &token_binding_module);
	cfg->context_env_var = arg;
	return NULL;
}

static const char * tb_cfg_get_context_env_var(tb_server_config *cfg) {
	return cfg->context_env_var ?
			cfg->context_env_var : TB_CFG_CONTEXT_ENV_VAR_DEFAULT;
}

// called dynamically from mod_ssl
static int tb_add_ext(server_rec *s, SSL_CTX *ctx) {
	tb_sdebug(s, "enter");

	if (!tbTLSLibInit()) {
		tb_serror(s, "tbTLSLibInit() failed");
		return -1;
	}

	if (!tbEnableTLSTokenBindingNegotiation(ctx)) {
		tb_serror(s, "tbEnableTLSTokenBindingNegotiation() failed");
		return -1;
	}

	return 1;
}

static void tb_set_var(request_rec *r, const char *env_var_name,
		const char *header_name, uint8_t* tokbind_id, size_t tokbind_id_len) {

	size_t len = CalculateBase64EscapedLen(tokbind_id_len, false);
	char* val = apr_pcalloc(r->pool, len + 1);
	WebSafeBase64Escape((const char *) tokbind_id, tokbind_id_len, val, len,
			false);

	if (env_var_name) {
		tb_debug(r, "set Token Binding ID environment variable: %s=%s",
				env_var_name, val);
		apr_table_set(r->subprocess_env, env_var_name, val);
	}

	if (header_name) {
		tb_debug(r, "set Token Binding ID header: %s=%s", header_name, val);
		apr_table_set(r->headers_in, header_name, val);
	}
}

static int tb_is_enabled(request_rec *r, tb_server_config *c,
		tbKeyType *tls_key_type) {

	if (tb_cfg_get_enabled(c) == FALSE) {
		tb_debug(r, "token binding is not enabled in the configuration");
		return 0;
	}

	if (ssl_is_https_fn == NULL) {
		tb_warn(r,
				"no ssl_is_https_fn function found: perhaps mod_ssl is not loaded?");
		return 0;
	}

	if (ssl_is_https_fn(r->connection) != 1) {
		tb_debug(r,
				"no ssl_is_https_fn returned != 1: looks like this is not an SSL connection");
		return 0;
	}

	if (get_ssl_from_request_fn == NULL) {
		tb_warn(r,
				"no ssl_get_ssl_from_request function found: perhaps a version of mod_ssl is loaded that is not patched for token binding?");
		return 0;
	}

	if (!tbTokenBindingEnabled(get_ssl_from_request_fn(r), tls_key_type)) {
		tb_debug(r, "Token Binding is not enabled by the peer");
		return 0;
	}

	tb_debug(r, "Token Binding is enabled on this connection: key_type=%d!",
			*tls_key_type);

	return 1;
}

static int tb_char_to_env(int c) {
	return apr_isalnum(c) ? apr_toupper(c) : '_';
}

static int tb_strnenvcmp(const char *a, const char *b) {
	int d, i = 0;
	while (1) {
		if (!*a && !*b)
			return 0;
		if (*a && !*b)
			return 1;
		if (!*a && *b)
			return -1;
		d = tb_char_to_env(*a) - tb_char_to_env(*b);
		if (d)
			return d;
		a++;
		b++;
		i++;
	}
	return 0;
}

static void tb_clean_header(request_rec *r, const char *name) {
	const apr_array_header_t * const h = apr_table_elts(r->headers_in);
	apr_table_t *clean_headers = apr_table_make(r->pool, h->nelts);
	const apr_table_entry_t * const e = (const apr_table_entry_t *) h->elts;
	int i = 0;
	while (i < h->nelts) {
		if (e[i].key != NULL) {
			if (tb_strnenvcmp(e[i].key, name) != 0)
				apr_table_addn(clean_headers, e[i].key, e[i].val);
			else
				tb_warn(r, "removing incoming request header (%s: %s)",
						e[i].key, e[i].val);
		}
		i++;
	}
	r->headers_in = clean_headers;
}

static int tb_get_decoded_header(request_rec *r, tb_server_config *cfg,
		char **message, size_t *message_len) {

	const char *header = apr_table_get(r->headers_in, TB_CFG_SEC_TB_HDR_NAME);
	if (header == NULL) {
		tb_warn(r, "no \"%s\" header found in request", TB_CFG_SEC_TB_HDR_NAME);
		return 0;
	}

	tb_debug(r, "Token Binding header found: %s=%s", TB_CFG_SEC_TB_HDR_NAME, header);

	size_t maxlen = strlen(header);
	*message = apr_pcalloc(r->pool, maxlen);
	*message_len = WebSafeBase64Unescape(header, *message, maxlen);

	tb_clean_header(r, TB_CFG_SEC_TB_HDR_NAME);

	if (*message_len == 0) {
		tb_error(r, "could not base64url decode Token Binding header");
		return 0;
	}

	return 1;
}

static void tb_draft_campbell_tokbind_tls_term(request_rec *r,
		tb_server_config *cfg, SSL* ssl, tbKeyType key_type, uint8_t *ekm,
		size_t ekm_len) {
	static const size_t kHeaderSize = 2;
	uint8_t* buf;
	size_t buf_len;

	if (key_type >= TB_INVALID_KEY_TYPE) {
		tb_error(r, "key_type is invalid");
		return;
	}

	buf_len = kHeaderSize + ekm_len + 1;
	buf = apr_pcalloc(r->pool, buf_len * sizeof(uint8_t));
	if (buf == NULL) {
		tb_error(r, "could not allocate memory for buf");
		return;
	}

	getNegotiatedVersion(ssl, buf);
	buf[kHeaderSize] = key_type;
	memcpy(buf + kHeaderSize + 1, ekm, ekm_len);

	tb_set_var(r, tb_cfg_get_context_env_var(cfg), TB_CFG_TB_CONTEXT_HDR_NAME,
			buf, buf_len);
}

static void tb_draft_campbell_tokbind_ttrp(request_rec *r,
		tb_server_config *cfg, uint8_t* out_tokbind_id,
		size_t out_tokbind_id_len, uint8_t* referred_tokbind_id,
		size_t referred_tokbind_id_len) {

	if ((out_tokbind_id != NULL) && (out_tokbind_id_len > 0))
		tb_set_var(r, tb_cfg_get_provided_env_var(cfg),
				TB_CFG_PROVIDED_TBID_HDR_NAME, out_tokbind_id, out_tokbind_id_len);
	else
		tb_debug(r, "no provided token binding ID found");

	if ((referred_tokbind_id != NULL) && (referred_tokbind_id_len > 0))
		tb_set_var(r, tb_cfg_get_referred_env_var(cfg),
				TB_CFG_REFERRED_TBID_HDR_NAME, referred_tokbind_id,
				referred_tokbind_id_len);
	else
		tb_debug(r, "no referred token binding ID found");
}

static int tb_post_read_request(request_rec *r) {

	tb_server_config *cfg = (tb_server_config*) ap_get_module_config(
			r->server->module_config, &token_binding_module);
	tbKeyType tls_key_type;
	char *message = NULL;
	size_t message_len;

	tb_debug(r, "enter");

	tb_clean_header(r, TB_CFG_TB_CONTEXT_HDR_NAME);
	tb_clean_header(r, TB_CFG_PROVIDED_TBID_HDR_NAME);
	tb_clean_header(r, TB_CFG_REFERRED_TBID_HDR_NAME);

	if (tb_is_enabled(r, cfg, &tls_key_type) == 0) {
		tb_clean_header(r, TB_CFG_SEC_TB_HDR_NAME);
		return DECLINED;
	}

	if (tb_get_decoded_header(r, cfg, &message, &message_len) == 0)
		return HTTP_UNAUTHORIZED;

	uint8_t* out_tokbind_id = NULL;
	size_t out_tokbind_id_len = -1;
	uint8_t* referred_tokbind_id = NULL;
	size_t referred_tokbind_id_len = -1;

	if (tbCacheMessageAlreadyVerified(cfg->cache, (uint8_t*) message,
			message_len, &out_tokbind_id, &out_tokbind_id_len,
			&referred_tokbind_id, &referred_tokbind_id_len)) {
		tb_debug(r, "tbCacheMessageAlreadyVerified returned true");
		tb_draft_campbell_tokbind_ttrp(r, cfg, out_tokbind_id,
				out_tokbind_id_len, referred_tokbind_id,
				referred_tokbind_id_len);
		return DECLINED;
	}

	uint8_t ekm[TB_HASH_LEN];
	if (!tbGetEKM(get_ssl_from_request_fn(r), ekm)) {
		tb_warn(r, "unable to get EKM from TLS connection");
		return DECLINED;
	}

	if (!tbCacheVerifyTokenBindingMessage(cfg->cache, (uint8_t*) message,
			message_len, tls_key_type, ekm, &out_tokbind_id,
			&out_tokbind_id_len, &referred_tokbind_id,
			&referred_tokbind_id_len)) {
		tb_error(r,
				"tbCacheVerifyTokenBindingMessage returned false: bad Token Binding header");
		return DECLINED;
	}

	tb_debug(r, "verified Token Binding header!");

	tb_draft_campbell_tokbind_ttrp(r, cfg, out_tokbind_id, out_tokbind_id_len,
			referred_tokbind_id, referred_tokbind_id_len);
	tb_draft_campbell_tokbind_tls_term(r, cfg, get_ssl_from_request_fn(r),
			tls_key_type, ekm, TB_HASH_LEN);

	return DECLINED;
}

void *tb_create_server_config(apr_pool_t *pool, server_rec *svr) {
	tb_server_config *c = apr_pcalloc(pool, sizeof(tb_server_config));
	c->enabled = TB_CFG_POS_INT_UNSET;

	uint64_t rand_seed = 0;
	RAND_seed(&rand_seed, sizeof(uint64_t));
	tbCacheLibInit(rand_seed);

	c->cache = tbCacheCreate();
	c->provided_env_var = NULL;
	c->referred_env_var = NULL;
	c->context_env_var = NULL;
	return c;
}

void *tb_merge_server_config(apr_pool_t *pool, void *BASE, void *ADD) {
	tb_server_config *c = apr_pcalloc(pool, sizeof(tb_server_config));
	tb_server_config *base = BASE;
	tb_server_config *add = ADD;
	c->enabled =
			add->enabled != TB_CFG_POS_INT_UNSET ? add->enabled : base->enabled;
	c->cache = add->cache;
	c->provided_env_var =
			add->provided_env_var != NULL ?
					add->provided_env_var : base->provided_env_var;
	c->referred_env_var =
			add->referred_env_var != NULL ?
					add->referred_env_var : base->referred_env_var;
	c->context_env_var =
			add->context_env_var != NULL ?
					add->context_env_var : base->context_env_var;
	return c;
}

static apr_status_t tb_cleanup_handler(void *data) {
	server_rec *s = (server_rec *) data;
	tb_sinfo(s, "%s - shutdown", NAMEVERSION);
	return APR_SUCCESS;
}

static int tb_post_config_handler(apr_pool_t *pool, apr_pool_t *p1,
		apr_pool_t *p2, server_rec *s) {
	tb_sinfo(s, "%s - init", NAMEVERSION);
	apr_pool_cleanup_register(pool, s, tb_cleanup_handler,
			apr_pool_cleanup_null);
	return OK;
}

static void tb_retrieve_optional_fn() {
	ssl_is_https_fn = APR_RETRIEVE_OPTIONAL_FN(ssl_is_https);
	get_ssl_from_request_fn = APR_RETRIEVE_OPTIONAL_FN(
			ssl_get_ssl_from_request);
}

static void tb_register_hooks(apr_pool_t *p) {
	ap_hook_post_config(tb_post_config_handler, NULL, NULL, APR_HOOK_LAST);
	ap_hook_post_read_request(tb_post_read_request, NULL, NULL, APR_HOOK_LAST);
	ap_hook_optional_fn_retrieve(tb_retrieve_optional_fn, NULL, NULL,
			APR_HOOK_MIDDLE);
	APR_REGISTER_OPTIONAL_FN(tb_add_ext);
}

static const command_rec tb_cmds[] = {
		AP_INIT_TAKE1(
			"TokenBindingEnabled",
			tb_cfg_set_enabled,
			NULL,
			RSRC_CONF,
			"enable or disable mod_token_binding. (default: On)"),
		AP_INIT_TAKE1(
			"TokenBindingProvidedEnvVar",
			tb_cfg_set_provided_env_var,
			NULL,
			RSRC_CONF,
			"set the environment variable name in which the Provided Token Binding ID will be passed. (default: " TB_CFG_PROVIDED_ENV_VAR_DEFAULT ")"),
		AP_INIT_TAKE1(
			"TokenBindingReferredEnvVar",
			tb_cfg_set_referred_env_var,
			NULL,
			RSRC_CONF,
			"set the environment variable name in which the Referred Token Binding ID will be passed. (default: " TB_CFG_REFERRED_ENV_VAR_DEFAULT ")"),
		AP_INIT_TAKE1(
			"TokenBindingContextEnvVar",
			tb_cfg_set_context_env_var,
			NULL,
			RSRC_CONF,
			"set the environment variable name in which the Token Binding Context re. draft_campbell_tokbind_tls_term will be passed. (default: " TB_CFG_CONTEXT_ENV_VAR_DEFAULT ")"),
		{ NULL }
};

module AP_MODULE_DECLARE_DATA token_binding_module = {
		STANDARD20_MODULE_STUFF,
		NULL,
		NULL,
		tb_create_server_config,
		tb_merge_server_config,
		tb_cmds,
		tb_register_hooks
};
