/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2009, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Anthony Minessale II <anthm@freeswitch.org>
 * Ken Rice, Asteria Solutions Group, Inc <ken@asteriasgi.com>
 * Paul D. Tinsley <pdt at jackhammer.org>
 * Bret McDanel <trixter AT 0xdecafbad.com>
 * Marcel Barbulescu <marcelbarbulescu@gmail.com>
 * Norman Brandinger
 *
 *
 * sofia.c -- SOFIA SIP Endpoint (sofia code)
 *
 */
#include "mod_sofia.h"


extern su_log_t tport_log[];
extern su_log_t iptsec_log[];
extern su_log_t nea_log[];
extern su_log_t nta_log[];
extern su_log_t nth_client_log[];
extern su_log_t nth_server_log[];
extern su_log_t nua_log[];
extern su_log_t soa_log[];
extern su_log_t sresolv_log[];
extern su_log_t stun_log[];
extern su_log_t su_log_default[];

static void set_variable_sip_param(switch_channel_t *channel, char *header_type, sip_param_t const *params);

static void sofia_info_send_sipfrag(switch_core_session_t *aleg, switch_core_session_t *bleg);

static void sofia_handle_sip_i_state(switch_core_session_t *session, int status,
									 char const *phrase,
									 nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip,
									 tagi_t tags[]);

static void sofia_handle_sip_r_invite(switch_core_session_t *session, int status,
									  char const *phrase,
									  nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip,
									  tagi_t tags[]);
static void sofia_handle_sip_r_options(switch_core_session_t *session, int status, char const *phrase, nua_t *nua, sofia_profile_t *profile,
									   nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip, tagi_t tags[]);

void sofia_handle_sip_r_notify(switch_core_session_t *session, int status,
							   char const *phrase,
							   nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip, tagi_t tags[])
{
#if 0
	if (status >= 300 && sip && sip->sip_call_id) {
		char *sql;
		sql = switch_mprintf("delete from sip_subscriptions where call_id='%q'", sip->sip_call_id->i_id);
		switch_assert(sql != NULL);
		sofia_glue_execute_sql(profile, &sql, SWITCH_TRUE);
	}
#endif
}

void sofia_handle_sip_i_notify(switch_core_session_t *session, int status,
							   char const *phrase,
							   nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip, tagi_t tags[])
{
	switch_channel_t *channel = NULL;
	private_object_t *tech_pvt = NULL;
	switch_event_t *s_event = NULL;
	sofia_gateway_subscription_t *gw_sub_ptr;
	int sub_state;

	tl_gets(tags, NUTAG_SUBSTATE_REF(sub_state), TAG_END());

	/* make sure we have a proper event */
	if (!sip || !sip->sip_event) {
		goto error;
	}

	/* the following could be refactored back to the calling event handler here in sofia.c XXX MTK */
	/* potentially interesting note: for Linksys shared appearance, we'll probably have to set up to get bare notifies
	 * and pass them inward to the sla handler. we'll have to set NUTAG_APPL_METHOD("NOTIFY") when creating
	 * nua, and also pick them off special elsewhere here in sofia.c - MTK
	 * *and* for Linksys, I believe they use "sa" as their magic appearance agent name for those blind notifies, so
	 * we'll probably have to change to match
	*/
	if (sofia_test_pflag(profile, PFLAG_MANAGE_SHARED_APPEARANCE)) {
		
		if (sip->sip_request->rq_url->url_user && !strncmp(sip->sip_request->rq_url->url_user, "sla-agent", sizeof("sla-agent"))) {
			sofia_sla_handle_sip_i_notify(nua, profile, nh, sip, tags);
			goto end;
		}
	}

	/* Automatically return a 200 OK for Event: keep-alive */
	if (!strcasecmp(sip->sip_event->o_type, "keep-alive")) {
		/* XXX MTK - is this right? in this case isn't sofia is already sending a 200 itself also? */
		nua_respond(nh, SIP_200_OK, NUTAG_WITH_THIS(nua), TAG_END());
		goto end;
	}

	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_assert(channel != NULL);
		tech_pvt = switch_core_session_get_private(session);
		switch_assert(tech_pvt != NULL);
	}

	/* For additional NOTIFY event packages see http://www.iana.org/assignments/sip-events. */
	if (sip->sip_content_type &&
		sip->sip_content_type->c_type &&
		sip->sip_payload &&
		sip->sip_payload->pl_data &&
		!strcasecmp(sip->sip_event->o_type, "refer")) {
		if (switch_event_create_subclass(&s_event, SWITCH_EVENT_CUSTOM, MY_EVENT_NOTIFY_REFER) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "content-type", sip->sip_content_type->c_type);
			switch_event_add_body(s_event, "%s", sip->sip_payload->pl_data);
		}
	}

	/* add common headers for the NOTIFY to the switch_event and fire if it exists */
	if (s_event != NULL) {
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "event-package", sip->sip_event->o_type);
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "event-id", sip->sip_event->o_id);

		switch_event_add_header(s_event, SWITCH_STACK_BOTTOM, "contact", "%s@%s", 
								sip->sip_contact->m_url->url_user, sip->sip_contact->m_url->url_host);
		switch_event_add_header(s_event, SWITCH_STACK_BOTTOM, "from", "%s@%s", 
								sip->sip_from->a_url->url_user, sip->sip_from->a_url->url_host);
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "from-tag", sip->sip_from->a_tag);
		switch_event_add_header(s_event, SWITCH_STACK_BOTTOM, "to", "%s@%s", 
								sip->sip_to->a_url->url_user, sip->sip_to->a_url->url_host);
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "to-tag", sip->sip_to->a_tag);

		if (sip->sip_call_id && sip->sip_call_id->i_id) {
			switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "call-id", sip->sip_call_id->i_id);
		}
		if (sip->sip_subscription_state && sip->sip_subscription_state->ss_substate) {
			switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "subscription-substate", sip->sip_subscription_state->ss_substate);
		}
		if (sip->sip_subscription_state && sip->sip_subscription_state->ss_reason) {
			switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "subscription-reason", sip->sip_subscription_state->ss_reason);
		}
		if (sip->sip_subscription_state && sip->sip_subscription_state->ss_retry_after) {
			switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "subscription-retry-after", sip->sip_subscription_state->ss_retry_after);
		}
		if (sip->sip_subscription_state && sip->sip_subscription_state->ss_expires) {
			switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "subscription-expires", sip->sip_subscription_state->ss_expires);
		}
		if (session) {
			switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "UniqueID", switch_core_session_get_uuid(session));
		}
		switch_event_fire(&s_event);
	}

	if (!strcasecmp(sip->sip_event->o_type, "refer")) {
		if (session && channel && tech_pvt) {
			if (sip->sip_payload && sip->sip_payload->pl_data) {
				char *p;
				int status_val = 0;
				if ((p = strchr(sip->sip_payload->pl_data, ' '))) {
					p++;
					if (p) {
						status_val = atoi(p);
					}
				}
				if (!status_val || status_val >= 200) {
					switch_channel_set_variable(channel, "sip_refer_reply", sip->sip_payload->pl_data);
					if (status_val == 200) {
						switch_channel_hangup(channel, SWITCH_CAUSE_BLIND_TRANSFER);
					}
					if (tech_pvt->want_event == 9999) {
						tech_pvt->want_event = 0;
					}
				}
			}
		}
		nua_respond(nh, SIP_200_OK, NUTAG_WITH_THIS(nua), TAG_END());
	}

	/* if no session, assume it could be an incoming notify from a gateway subscription */
	if (session) {
		/* make sure we have a proper "talk" event */
		if (strcasecmp(sip->sip_event->o_type, "talk")) {
			goto error;
		}

		if (!switch_channel_test_flag(channel, CF_OUTBOUND)) {
			switch_channel_answer(channel);
			switch_channel_set_variable(channel, "auto_answer_destination", switch_channel_get_variable(channel, "destination_number"));
			switch_ivr_session_transfer(session, "auto_answer", NULL, NULL);
			nua_respond(nh, SIP_200_OK, NUTAG_WITH_THIS(nua), TAG_END());
			goto end;
		}
	}
	
	if (!sofia_private || !sofia_private->gateway) {
		if (profile->debug) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Gateway information missing Subscription Event: %s\n", sip->sip_event->o_type);
		}
		goto error;	
	}
				
	/* find the corresponding gateway subscription (if any) */
	if (!(gw_sub_ptr = sofia_find_gateway_subscription(sofia_private->gateway, sip->sip_event->o_type))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "Could not find gateway subscription.  Gateway: %s.  Subscription Event: %s\n",
						  sofia_private->gateway->name, sip->sip_event->o_type);
		goto error;	
	}

	if (!(gw_sub_ptr->state == SUB_STATE_SUBED || gw_sub_ptr->state == SUB_STATE_SUBSCRIBE)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "Ignoring notify due to subscription state: %d\n",
						  gw_sub_ptr->state);
		goto error;	
	}

	/* dispatch freeswitch event */
	if (switch_event_create(&s_event, SWITCH_EVENT_NOTIFY_IN) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "event", sip->sip_event->o_type);
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "pl_data", sip->sip_payload->pl_data);
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "sip_content_type", sip->sip_content_type->c_type);
		switch_event_add_header(s_event, SWITCH_STACK_BOTTOM, "port", "%d", sofia_private->gateway->profile->sip_port);
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "module_name", "mod_sofia");
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "profile_name", sofia_private->gateway->profile->name);
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "profile_uri", sofia_private->gateway->profile->url);
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "gateway_name", sofia_private->gateway->name);
		switch_event_fire(&s_event);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "dispatched freeswitch event for message-summary NOTIFY\n");
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to create event\n");
		goto error;	
	}

	goto end;

  error:


	if (sip && sip->sip_event && sip->sip_event->o_type && !strcasecmp(sip->sip_event->o_type, "message-summary")) {
		/* unsolicited mwi, just say ok */
		nua_respond(nh, SIP_200_OK, NUTAG_WITH_THIS(nua), TAG_END());
	} else {
		nua_respond(nh, 481, "Subscription Does Not Exist", NUTAG_WITH_THIS(nua), TAG_END());
	}
	
 end:
	
	if (sub_state == nua_substate_terminated && sofia_private && sofia_private != &mod_sofia_globals.destroy_private && 
		sofia_private != &mod_sofia_globals.keep_private) {
		sofia_private->destroy_nh = 1;
		sofia_private->destroy_me = 1;
	}

}

void sofia_handle_sip_i_bye(switch_core_session_t *session, int status,
							char const *phrase,
							nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip, tagi_t tags[])
{
	const char *tmp;
	switch_channel_t *channel;
	private_object_t *tech_pvt;
	char *extra_headers;
#ifdef MANUAL_BYE
	int cause;
	char st[80] = "";
#endif

	if (!session)
		return;

	channel = switch_core_session_get_channel(session);
	tech_pvt = switch_core_session_get_private(session);


#ifdef MANUAL_BYE
	status = 200;
	phrase = "OK";
	
	sofia_set_flag_locked(tech_pvt, TFLAG_BYE);

	if (sip->sip_reason && sip->sip_reason->re_protocol &&
		(!strcasecmp(sip->sip_reason->re_protocol, "Q.850") 
			|| !strcasecmp(sip->sip_reason->re_protocol, "FreeSWITCH")
			|| !strcasecmp(sip->sip_reason->re_protocol, profile->username)) && sip->sip_reason->re_cause) {
		tech_pvt->q850_cause = atoi(sip->sip_reason->re_cause);
		cause = tech_pvt->q850_cause;
	} else {
		cause = sofia_glue_sip_cause_to_freeswitch(status);
	}

	switch_snprintf(st, sizeof(st), "%d", status);
	switch_channel_set_variable(channel, "sip_term_status", st);
	switch_snprintf(st, sizeof(st), "sip:%d", status);

	if (phrase) {
		switch_channel_set_variable_partner(channel, "sip_hangup_phrase", phrase);
	}

	switch_snprintf(st, sizeof(st), "%d", cause);
	switch_channel_set_variable(channel, "sip_term_cause", st);
	
	extra_headers = sofia_glue_get_extra_headers(channel, SOFIA_SIP_BYE_HEADER_PREFIX);
	sofia_glue_set_extra_headers(channel, sip, SOFIA_SIP_BYE_HEADER_PREFIX);
	
	switch_channel_hangup(channel, cause);
	nua_respond(nh, SIP_200_OK, NUTAG_WITH_THIS(nua), 
		TAG_IF(!switch_strlen_zero(extra_headers), SIPTAG_HEADER_STR(extra_headers)), TAG_END());
		
	switch_safe_free(extra_headers);

	if (sofia_private) {
        sofia_private->destroy_me = 1;
        sofia_private->destroy_nh = 1;
    }
#endif


	if (sip->sip_user_agent && !switch_strlen_zero(sip->sip_user_agent->g_string)) {
		switch_channel_set_variable(channel, "sip_user_agent", sip->sip_user_agent->g_string);
	} else if (sip->sip_server && !switch_strlen_zero(sip->sip_server->g_string)) {
		switch_channel_set_variable(channel, "sip_user_agent", sip->sip_server->g_string);
	}

	if ((tmp = sofia_glue_get_unknown_header(sip, "rtp-txstat"))) {
		switch_channel_set_variable(channel, "sip_rtp_txstat", tmp);
	}
	if ((tmp = sofia_glue_get_unknown_header(sip, "rtp-rxstat"))) {
		switch_channel_set_variable(channel, "sip_rtp_rxstat", tmp);
	}
	if ((tmp = sofia_glue_get_unknown_header(sip, "P-RTP-Stat"))) {
		switch_channel_set_variable(channel, "sip_p_rtp_stat", tmp);
	}
	
	tech_pvt->got_bye = 1;
	switch_channel_set_variable(channel, "sip_hangup_disposition", "recv_bye");	

	return;
}

void sofia_handle_sip_r_message(int status, sofia_profile_t *profile, nua_handle_t *nh, sip_t const *sip)
{
}

void sofia_wait_for_reply(struct private_object *tech_pvt, nua_event_t event, uint32_t timeout)
{
	time_t exp = switch_epoch_time_now(NULL) + timeout;
	
	tech_pvt->want_event = event;

	while(switch_channel_ready(tech_pvt->channel) && tech_pvt->want_event && switch_epoch_time_now(NULL) < exp) {
		switch_yield(100000);
	}
	
}

void sofia_event_callback(nua_event_t event,
						  int status,
						  char const *phrase,
						  nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip, tagi_t tags[])
{
	struct private_object *tech_pvt = NULL;
	auth_res_t auth_res = AUTH_FORBIDDEN;
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	sofia_gateway_t *gateway = NULL;
	int locked = 0;
	int check_destroy = 1;

	if (nh && sofia_private == &mod_sofia_globals.keep_private) {
		if (status >= 300) {
			nua_handle_bind(nh, NULL);
			nua_handle_destroy(nh);
			return;
		}
	}

	if (sofia_private && sofia_private != &mod_sofia_globals.destroy_private && sofia_private != &mod_sofia_globals.keep_private) {
		if ((gateway = sofia_private->gateway)) {
			if (switch_thread_rwlock_tryrdlock(gateway->profile->rwlock) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Profile %s is locked\n", gateway->profile->name);
				return;
			}
		} else if (!switch_strlen_zero(sofia_private->uuid)) {
			if ((session = switch_core_session_locate(sofia_private->uuid))) {
				tech_pvt = switch_core_session_get_private(session);
				channel = switch_core_session_get_channel(session);
				if (tech_pvt) {
					switch_mutex_lock(tech_pvt->sofia_mutex);
					locked = 1;										
				} else {
					switch_core_session_rwunlock(session);
					return;
				}

				if (status >= 180 && !*sofia_private->auth_gateway_name) {
					const char *gwname = switch_channel_get_variable(channel, "sip_use_gateway");
					if (!switch_strlen_zero(gwname)) {
						switch_set_string(sofia_private->auth_gateway_name, gwname);
					}
				}
				if (!tech_pvt->call_id && sip && sip->sip_call_id && sip->sip_call_id->i_id) {
					tech_pvt->call_id = switch_core_session_strdup(session, sip->sip_call_id->i_id);
					switch_channel_set_variable(channel, "sip_call_id", tech_pvt->call_id);
				}

				if (tech_pvt->gateway_name) {
					gateway = sofia_reg_find_gateway(tech_pvt->gateway_name);
				}

				if (channel && switch_channel_down(channel)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Channel is already hungup.\n");
					goto done;
				}
			} else {
				/* we can't find the session it must be hanging up or something else, its too late to do anything with it. */
				return;
			}
		}
	}

	if (sofia_test_pflag(profile, PFLAG_AUTH_ALL) && tech_pvt && tech_pvt->key && sip) {
		sip_authorization_t const *authorization = NULL;

		if (sip->sip_authorization) {
			authorization = sip->sip_authorization;
		} else if (sip->sip_proxy_authorization) {
			authorization = sip->sip_proxy_authorization;
		}

		if (authorization) {
			char network_ip[80];
			sofia_glue_get_addr(nua_current_request(nua), network_ip,  sizeof(network_ip), NULL);
			auth_res = sofia_reg_parse_auth(profile, authorization, sip,
											(char *) sip->sip_request->rq_method_name, tech_pvt->key, strlen(tech_pvt->key), network_ip, NULL, 0,
											REG_INVITE, NULL, NULL);
		}
		
		if (auth_res != AUTH_OK) {
			//switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
			nua_respond(nh, SIP_401_UNAUTHORIZED, TAG_END());
			goto done;
		}

		if (channel) {
			switch_channel_set_variable(channel, "sip_authorized", "true");
		}
	}

	if (sip && (status == 401 || status == 407)) {
		sofia_reg_handle_sip_r_challenge(status, phrase, nua, profile, nh, sofia_private, session, gateway, sip, tags);
		goto done;
	}

	switch (event) {
	case nua_r_get_params:
	case nua_i_fork:
	case nua_r_info:
		break;
	case nua_r_bye:
	case nua_r_unregister:
	case nua_r_unsubscribe:
	case nua_r_publish:
	case nua_i_cancel:
	case nua_r_cancel:
	case nua_i_error:
	case nua_i_active:
	case nua_i_ack:
	case nua_i_terminated:
	case nua_r_set_params:
	case nua_i_prack:
	case nua_r_prack:
		break;
	case nua_r_shutdown:
		if (status >= 200) su_root_break(profile->s_root);
		break;
	case nua_r_message:
		sofia_handle_sip_r_message(status, profile, nh, sip);
		break;
	case nua_r_invite:
		sofia_handle_sip_r_invite(session, status, phrase, nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_r_options:
		sofia_handle_sip_r_options(session, status, phrase, nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_i_bye:
		sofia_handle_sip_i_bye(session, status, phrase, nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_r_notify:
		sofia_handle_sip_r_notify(session, status, phrase, nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_i_notify:
		sofia_handle_sip_i_notify(session, status, phrase, nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_r_register:
		sofia_reg_handle_sip_r_register(status, phrase, nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_i_options:
		sofia_handle_sip_i_options(status, phrase, nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_i_invite:
		if (!session) sofia_handle_sip_i_invite(nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_i_publish:
		sofia_presence_handle_sip_i_publish(nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_i_register:
		//nua_respond(nh, SIP_200_OK, SIPTAG_CONTACT(sip->sip_contact), NUTAG_WITH_THIS(nua), TAG_END());
		//nua_handle_destroy(nh);
		sofia_reg_handle_sip_i_register(nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_i_state:
		sofia_handle_sip_i_state(session, status, phrase, nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_i_message:
		sofia_presence_handle_sip_i_message(status, phrase, nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_i_info:
		sofia_handle_sip_i_info(nua, profile, nh, session, sip, tags);
		break;
	case nua_r_update:
		break;
	case nua_r_refer:
		break;
	case nua_i_refer:
		if (session) sofia_handle_sip_i_refer(nua, profile, nh, session, sip, tags);
		break;
	case nua_r_subscribe:
		sofia_presence_handle_sip_r_subscribe(status, phrase, nua, profile, nh, sofia_private, sip, tags);
		break;
	case nua_i_subscribe:
		sofia_presence_handle_sip_i_subscribe(status, phrase, nua, profile, nh, sofia_private, sip, tags);
		break;
	default:
		if (status > 100) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s: unknown event %d: %03d %s\n", nua_event_name(event), event, status, phrase);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s: unknown event %d\n", nua_event_name(event), event);
		}
		break;
	}

  done:

	if (tech_pvt && tech_pvt->want_event && event == tech_pvt->want_event) {
		tech_pvt->want_event = 0;
	}

	switch (event) {
	case nua_i_subscribe:
	case nua_r_notify:
		check_destroy = 0;
		break;

	case nua_i_notify:
		
		if (sip && sip->sip_event && !strcmp(sip->sip_event->o_type, "dialog") && sip->sip_event->o_params && !strcmp(sip->sip_event->o_params[0], "sla")) {
			check_destroy = 0;
		}

		break;
	default:
		break;
	}

	if ((sofia_private && sofia_private == &mod_sofia_globals.destroy_private)) {
		nua_handle_bind(nh, NULL);
		nua_handle_destroy(nh);
		nh = NULL;
	}

	if (check_destroy) {
		if (nh && ((sofia_private && sofia_private->destroy_nh) || !nua_handle_magic(nh))) {
			if (sofia_private) {
				nua_handle_bind(nh, NULL);
			}
			nua_handle_destroy(nh);
			nh = NULL;
		}
	}
	
	if (sofia_private && sofia_private->destroy_me) {
		if (tech_pvt) {
            tech_pvt->sofia_private = NULL;
        }

		if (nh) {
			nua_handle_bind(nh, NULL);
		}
		sofia_private->destroy_me = 12;
		sofia_private_free(sofia_private);
	}

	if (gateway) {
		sofia_reg_release_gateway(gateway);
	}

	if (locked && tech_pvt) {
		switch_mutex_unlock(tech_pvt->sofia_mutex);
	}
	
	if (session) {
		switch_core_session_rwunlock(session);
	}
}

void event_handler(switch_event_t *event)
{
	char *subclass, *sql;

	if ((subclass = switch_event_get_header(event, "orig-event-subclass")) && !strcasecmp(subclass, MY_EVENT_REGISTER)) {
		char *from_user = switch_event_get_header(event, "orig-from-user");
		char *from_host = switch_event_get_header(event, "orig-from-host");
		char *to_host = switch_event_get_header(event, "orig-to-host");
		char *contact_str = switch_event_get_header(event, "orig-contact");
		char *exp_str = switch_event_get_header(event, "orig-expires");
		char *rpid = switch_event_get_header(event, "orig-rpid");
		char *call_id = switch_event_get_header(event, "orig-call-id");
		char *user_agent = switch_event_get_header(event, "orig-user-agent");
		long expires = (long) switch_epoch_time_now(NULL);
		char *profile_name = switch_event_get_header(event, "orig-profile-name");
		char *to_user = switch_event_get_header(event, "orig-to-user");
		char *presence_hosts = switch_event_get_header(event, "orig-presence-hosts");
		char *network_ip = switch_event_get_header(event, "orig-network-ip");
		char *network_port = switch_event_get_header(event, "orig-network-port");
		char *username = switch_event_get_header(event, "orig-username");
		char *realm = switch_event_get_header(event, "orig-realm");
		char *fixed_contact_str = NULL;

		sofia_profile_t *profile = NULL;

		char guess_ip4[256];

		if (exp_str) {
			expires += atol(exp_str);
		}

		if (!rpid) {
			rpid = "unknown";
		}

		if (!profile_name || !(profile = sofia_glue_find_profile(profile_name))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid Profile\n");
			return;
		}
		if (sofia_test_pflag(profile, PFLAG_MULTIREG)) {
			sql = switch_mprintf("delete from sip_registrations where call_id='%q'", call_id);
		} else {
			sql = switch_mprintf("delete from sip_registrations where sip_user='%q' and sip_host='%q'", from_user, from_host);
		}

		if (mod_sofia_globals.rewrite_multicasted_fs_path && contact_str) {
			const char *needle = ";fs_path=";
			char *sptr, *eptr = NULL;
			/* allocate enough room for worst-case scenario */
			size_t len = strlen(contact_str) + strlen(to_host) + 14;
			fixed_contact_str = malloc(len);
			switch_assert(fixed_contact_str);
			switch_copy_string(fixed_contact_str, contact_str, len);
	
			if ((sptr = strstr(fixed_contact_str, needle))) {
				char *origsptr = strstr(contact_str, needle);
				eptr = strchr(++origsptr, ';');
			} else {
				sptr = strchr(fixed_contact_str, '\0') - 1;
			}

			switch_snprintf(sptr, len - (sptr - fixed_contact_str), ";fs_path=sip:%s%s", to_host, eptr ? eptr : ">");

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Rewrote contact string from '%s' to '%s'\n", contact_str, fixed_contact_str);
			contact_str = fixed_contact_str;
		}

		switch_mutex_lock(profile->ireg_mutex);
		sofia_glue_execute_sql(profile, &sql, SWITCH_TRUE);
		
		switch_find_local_ip(guess_ip4, sizeof(guess_ip4), NULL, AF_INET);
		sql = switch_mprintf("insert into sip_registrations "
							 "(call_id, sip_user, sip_host, presence_hosts, contact, status, rpid, expires,"
							 "user_agent, server_user, server_host, profile_name, hostname, network_ip, network_port, sip_username, sip_realm) "
							 "values ('%q','%q','%q','%q','%q','Registered','%q',%ld, '%q','%q','%q','%q','%q','%q','%q','%q','%q')",
							 call_id, from_user, from_host, presence_hosts, contact_str, rpid, expires, user_agent, to_user, guess_ip4, 
							 profile_name, mod_sofia_globals.hostname, network_ip, network_port, username, realm);

		if (sql) {
			sofia_glue_execute_sql(profile, &sql, SWITCH_TRUE);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Propagating registration for %s@%s->%s\n", from_user, from_host, contact_str);
		}
		switch_mutex_unlock(profile->ireg_mutex);

		if (profile) {
			sofia_glue_release_profile(profile);
		}

		switch_safe_free(fixed_contact_str);
	}
}

void *SWITCH_THREAD_FUNC sofia_profile_worker_thread_run(switch_thread_t *thread, void *obj)
{
	sofia_profile_t *profile = (sofia_profile_t *) obj;
	uint32_t ireg_loops = 0;
	uint32_t gateway_loops = 0;
	int loops = 0;
	uint32_t qsize;
	void *pop;
	
	ireg_loops = IREG_SECONDS;
	gateway_loops = GATEWAY_SECONDS;

	sofia_set_pflag_locked(profile, PFLAG_WORKER_RUNNING);

	switch_queue_create(&profile->sql_queue, SOFIA_QUEUE_SIZE, profile->pool);

	qsize = switch_queue_size(profile->sql_queue);

	while ((mod_sofia_globals.running == 1 && sofia_test_pflag(profile, PFLAG_RUNNING)) || qsize) {
		if (qsize) {
			switch_mutex_lock(profile->ireg_mutex);
			while (switch_queue_trypop(profile->sql_queue, &pop) == SWITCH_STATUS_SUCCESS && pop) {
				sofia_glue_actually_execute_sql(profile, SWITCH_TRUE, (char *) pop, NULL);
				free(pop);
			}
			switch_mutex_unlock(profile->ireg_mutex);
		}

		if (++loops >= 100) {
			if (++ireg_loops >= IREG_SECONDS) {
				sofia_reg_check_expire(profile, switch_epoch_time_now(NULL), 0);
				ireg_loops = 0;
			}

			if (++gateway_loops >= GATEWAY_SECONDS) {
				sofia_reg_check_gateway(profile, switch_epoch_time_now(NULL));
				gateway_loops = 0;
			}
			sofia_sub_check_gateway(profile, time(NULL));
			loops = 0;
		}

		switch_cond_next();
		qsize = switch_queue_size(profile->sql_queue);
	}

	switch_mutex_lock(profile->ireg_mutex);
	while (switch_queue_trypop(profile->sql_queue, &pop) == SWITCH_STATUS_SUCCESS && pop) {
		sofia_glue_actually_execute_sql(profile, SWITCH_TRUE, (char *) pop, NULL);
		free(pop);
	}
	switch_mutex_unlock(profile->ireg_mutex);

	sofia_clear_pflag_locked(profile, PFLAG_WORKER_RUNNING);

	return NULL;
}

switch_thread_t *launch_sofia_worker_thread(sofia_profile_t *profile)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;
	int x = 0;

	switch_threadattr_create(&thd_attr, profile->pool);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_threadattr_priority_increase(thd_attr);
	switch_thread_create(&thread, thd_attr, sofia_profile_worker_thread_run, profile, profile->pool);

	while (!sofia_test_pflag(profile, PFLAG_WORKER_RUNNING)) {
		switch_yield(100000);
		if (++x >= 100) {
			break;
		}
	}

	return thread;
}

void *SWITCH_THREAD_FUNC sofia_profile_thread_run(switch_thread_t *thread, void *obj)
{
	sofia_profile_t *profile = (sofia_profile_t *) obj;
	switch_memory_pool_t *pool;
	sip_alias_node_t *node;
	switch_event_t *s_event;
	int use_100rel = !sofia_test_pflag(profile, PFLAG_DISABLE_100REL);
	int use_timer = !sofia_test_pflag(profile, PFLAG_DISABLE_TIMER);
	const char *supported = NULL;
	int sanity;
	switch_thread_t *worker_thread;
	switch_status_t st;

	

	switch_mutex_lock(mod_sofia_globals.mutex);
	mod_sofia_globals.threads++;
	switch_mutex_unlock(mod_sofia_globals.mutex);

	profile->s_root = su_root_create(NULL);
	profile->home = su_home_new(sizeof(*profile->home));

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Creating agent for %s\n", profile->name);

	if (!sofia_glue_init_sql(profile)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Cannot Open SQL Database [%s]!\n", profile->name);
		sofia_glue_del_profile(profile);
		goto end;
	}

	supported = switch_core_sprintf(profile->pool, "%s%sprecondition, path, replaces", 
									use_100rel ? "100rel, " : "",
									use_timer ? "timer, " : ""
									);

	if (sofia_test_pflag(profile, PFLAG_AUTO_NAT) && switch_core_get_variable("nat_type")) {
		if (switch_nat_add_mapping(profile->sip_port, SWITCH_NAT_UDP, NULL, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Created UDP nat mapping for %s port %d\n", profile->name, profile->sip_port);
		}
		if (switch_nat_add_mapping(profile->sip_port, SWITCH_NAT_TCP, NULL, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Created TCP nat mapping for %s port %d\n", profile->name, profile->sip_port);
		}
		if(sofia_test_pflag(profile, PFLAG_TLS) && switch_nat_add_mapping(profile->tls_sip_port, SWITCH_NAT_TCP, NULL, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Created TCP/TLS nat mapping for %s port %d\n", profile->name, profile->tls_sip_port);
		}
	}

	profile->nua = nua_create(profile->s_root,	/* Event loop */
							  sofia_event_callback,	/* Callback for processing events */
							  profile,	/* Additional data to pass to callback */
							  NUTAG_URL(profile->bindurl),
							  NTATAG_USER_VIA(1),
							  TAG_IF(!strchr(profile->sipip, ':'), SOATAG_AF(SOA_AF_IP4_ONLY)),
							  TAG_IF(strchr(profile->sipip, ':'), SOATAG_AF(SOA_AF_IP6_ONLY)),
							  TAG_IF(sofia_test_pflag(profile, PFLAG_TLS), NUTAG_SIPS_URL(profile->tls_bindurl)), 
							  TAG_IF(sofia_test_pflag(profile, PFLAG_TLS), NUTAG_CERTIFICATE_DIR(profile->tls_cert_dir)), 
							  TAG_IF(sofia_test_pflag(profile, PFLAG_TLS), TPTAG_TLS_VERIFY_POLICY(0)),
							  TAG_IF(sofia_test_pflag(profile, PFLAG_TLS), TPTAG_TLS_VERSION(profile->tls_version)), 
							  TAG_IF(sofia_test_pflag(profile, PFLAG_TLS), TPTAG_KEEPALIVE(20000)),
							  TAG_IF(!strchr(profile->sipip, ':'), NTATAG_UDP_MTU(65535)), 
 							  TAG_IF(sofia_test_pflag(profile, PFLAG_DISABLE_SRV), NTATAG_USE_SRV(0)),
 							  TAG_IF(sofia_test_pflag(profile, PFLAG_DISABLE_NAPTR), NTATAG_USE_NAPTR(0)),
							  NTATAG_DEFAULT_PROXY(profile->outbound_proxy),
							  NTATAG_SERVER_RPORT(profile->rport_level),
							  TPTAG_LOG(sofia_test_flag(profile, TFLAG_TPORT_LOG)), 
							  TAG_IF(profile->timer_t1, NTATAG_SIP_T1(profile->timer_t1)),
							  TAG_IF(profile->timer_t1x64, NTATAG_SIP_T1X64(profile->timer_t1x64)),
							  TAG_IF(profile->timer_t2, NTATAG_SIP_T2(profile->timer_t2)),
							  TAG_IF(profile->timer_t4, NTATAG_SIP_T4(profile->timer_t4)),
							  TAG_END());	/* Last tag should always finish the sequence */

	if (!profile->nua) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Creating SIP UA for profile: %s\n", profile->name);
		sofia_glue_del_profile(profile);
		goto end;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Created agent for %s\n", profile->name);

	nua_set_params(profile->nua,
				   NUTAG_APPL_METHOD("OPTIONS"),
				   NUTAG_APPL_METHOD("NOTIFY"),
				   NUTAG_APPL_METHOD("INFO"),
#ifdef MANUAL_BYE
				   NUTAG_APPL_METHOD("BYE"),
#endif
				   NUTAG_AUTOANSWER(0),
				   NUTAG_AUTOALERT(0),
				   NUTAG_ENABLEMESSENGER(1),
				   TAG_IF((profile->mflags & MFLAG_REGISTER), NUTAG_ALLOW("REGISTER")),
				   TAG_IF((profile->mflags & MFLAG_REFER), NUTAG_ALLOW("REFER")),
				   NUTAG_ALLOW("INFO"),
				   NUTAG_ALLOW("NOTIFY"),
				   NUTAG_ALLOW_EVENTS("talk"),
				   NUTAG_SESSION_TIMER(profile->session_timeout),
				   TAG_IF(profile->minimum_session_expires, NUTAG_MIN_SE(profile->minimum_session_expires)),
				   NTATAG_MAX_PROCEEDING(profile->max_proceeding),
				   TAG_IF(profile->pres_type, NUTAG_ALLOW("PUBLISH")),
				   TAG_IF(profile->pres_type, NUTAG_ALLOW("SUBSCRIBE")),
				   TAG_IF(profile->pres_type, NUTAG_ENABLEMESSAGE(1)),
				   TAG_IF(profile->pres_type, NUTAG_ALLOW_EVENTS("presence")),
				   TAG_IF((profile->pres_type || sofia_test_pflag(profile, PFLAG_MANAGE_SHARED_APPEARANCE)), NUTAG_ALLOW_EVENTS("dialog")),
				   TAG_IF(profile->pres_type, NUTAG_ALLOW_EVENTS("call-info")),
				   TAG_IF((profile->pres_type || sofia_test_pflag(profile, PFLAG_MANAGE_SHARED_APPEARANCE)), NUTAG_ALLOW_EVENTS("sla")),
				   TAG_IF(profile->pres_type, NUTAG_ALLOW_EVENTS("include-session-description")),
				   TAG_IF(profile->pres_type, NUTAG_ALLOW_EVENTS("presence.winfo")),
				   TAG_IF(profile->pres_type, NUTAG_ALLOW_EVENTS("message-summary")),
				   NUTAG_ALLOW_EVENTS("refer"),
				   SIPTAG_SUPPORTED_STR(supported), SIPTAG_USER_AGENT_STR(profile->user_agent), TAG_END());

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Set params for %s\n", profile->name);

	for (node = profile->aliases; node; node = node->next) {
		node->nua = nua_create(profile->s_root,	/* Event loop */
							   sofia_event_callback,	/* Callback for processing events */
							   profile,	/* Additional data to pass to callback */
							   NTATAG_SERVER_RPORT(profile->rport_level), NUTAG_URL(node->url), TAG_END());	/* Last tag should always finish the sequence */

		nua_set_params(node->nua,
					   NUTAG_APPL_METHOD("OPTIONS"),
					   NUTAG_AUTOANSWER(0),
					   NUTAG_AUTOALERT(0),
					   TAG_IF((profile->mflags & MFLAG_REGISTER), NUTAG_ALLOW("REGISTER")),
					   TAG_IF((profile->mflags & MFLAG_REFER), NUTAG_ALLOW("REFER")),
					   NUTAG_ALLOW("INFO"),
					   TAG_IF(profile->pres_type, NUTAG_ALLOW("PUBLISH")),
					   TAG_IF(profile->pres_type, NUTAG_ENABLEMESSAGE(1)),
					   SIPTAG_SUPPORTED_STR(supported), SIPTAG_USER_AGENT_STR(profile->user_agent), TAG_END());
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Activated db for %s\n", profile->name);

	switch_mutex_init(&profile->ireg_mutex, SWITCH_MUTEX_NESTED, profile->pool);
	switch_mutex_init(&profile->gateway_mutex, SWITCH_MUTEX_NESTED, profile->pool);

	if (switch_event_create(&s_event, SWITCH_EVENT_PUBLISH) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header(s_event, SWITCH_STACK_BOTTOM, "service", "_sip._udp,_sip._tcp,_sip._sctp%s",
								(sofia_test_pflag(profile, PFLAG_TLS)) ? ",_sips._tcp" : "");

		switch_event_add_header(s_event, SWITCH_STACK_BOTTOM, "port", "%d", profile->sip_port);
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "module_name", "mod_sofia");
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "profile_name", profile->name);
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "profile_uri", profile->url);

		if (sofia_test_pflag(profile, PFLAG_TLS)) {
			switch_event_add_header(s_event, SWITCH_STACK_BOTTOM, "tls_port", "%d", profile->tls_sip_port);
			switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "profile_tls_uri", profile->tls_url);
		}
		switch_event_fire(&s_event);
	}

	sofia_glue_add_profile(profile->name, profile);

	if (profile->pres_type) {
		sofia_presence_establish_presence(profile);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Starting thread for %s\n", profile->name);

	profile->started = switch_epoch_time_now(NULL);

	sofia_set_pflag_locked(profile, PFLAG_RUNNING);
	worker_thread = launch_sofia_worker_thread(profile);

	switch_yield(1000000);

	while (mod_sofia_globals.running == 1 && sofia_test_pflag(profile, PFLAG_RUNNING) && sofia_test_pflag(profile, PFLAG_WORKER_RUNNING)) {
		su_root_step(profile->s_root, 1000);
	}

	sofia_clear_pflag_locked(profile, PFLAG_RUNNING);

	switch_core_session_hupall_matching_var("sofia_profile_name", profile->name, SWITCH_CAUSE_MANAGER_REQUEST);
	sanity = 10;
	while (profile->inuse) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Waiting for %d session(s)\n", profile->inuse);
		su_root_step(profile->s_root, 1000);
		if (!--sanity) {
			break;
		} else if (sanity == 5) {
			switch_core_session_hupall_matching_var("sofia_profile_name", profile->name, SWITCH_CAUSE_MANAGER_REQUEST);
		}
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Write lock %s\n", profile->name);
	switch_thread_rwlock_wrlock(profile->rwlock);
	sofia_reg_unregister(profile);
	nua_shutdown(profile->nua);
	su_root_run(profile->s_root);
	
	sofia_clear_pflag_locked(profile, PFLAG_RUNNING);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Waiting for worker thread\n");

	switch_thread_join(&st, worker_thread);
	
	sanity = 4;
	while (profile->inuse) {
		switch_core_session_hupall_matching_var("sofia_profile_name", profile->name, SWITCH_CAUSE_MANAGER_REQUEST);
		switch_yield(5000000);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Waiting for %d session(s)\n", profile->inuse);
		if (!--sanity) {
			break;
		}
	}
	nua_destroy(profile->nua);
	
	switch_mutex_lock(profile->ireg_mutex);
	switch_mutex_unlock(profile->ireg_mutex);

	switch_mutex_lock(profile->flag_mutex);
	switch_mutex_unlock(profile->flag_mutex);

	if (switch_event_create(&s_event, SWITCH_EVENT_UNPUBLISH) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header(s_event, SWITCH_STACK_BOTTOM, "service", "_sip._udp,_sip._tcp,_sip._sctp%s",
								(sofia_test_pflag(profile, PFLAG_TLS)) ? ",_sips._tcp" : "");

		switch_event_add_header(s_event, SWITCH_STACK_BOTTOM, "port", "%d", profile->sip_port);
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "module_name", "mod_sofia");
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "profile_name", profile->name);
		switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "profile_uri", profile->url);

		if (sofia_test_pflag(profile, PFLAG_TLS)) {
			switch_event_add_header(s_event, SWITCH_STACK_BOTTOM, "tls_port", "%d", profile->tls_sip_port);
			switch_event_add_header_string(s_event, SWITCH_STACK_BOTTOM, "profile_tls_uri", profile->tls_url);
		}
		switch_event_fire(&s_event);
	}

	if (sofia_test_pflag(profile, PFLAG_AUTO_NAT) && switch_core_get_variable("nat_type")) {
		if (switch_nat_del_mapping(profile->sip_port, SWITCH_NAT_UDP) == SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Deleted UDP nat mapping for %s port %d\n", profile->name, profile->sip_port);
		}
		if (switch_nat_del_mapping(profile->sip_port, SWITCH_NAT_TCP) == SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Deleted TCP nat mapping for %s port %d\n", profile->name, profile->sip_port);
		}
		if(sofia_test_pflag(profile, PFLAG_TLS) && switch_nat_del_mapping(profile->tls_sip_port, SWITCH_NAT_TCP) == SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Deleted TCP/TLS nat mapping for %s port %d\n", profile->name, profile->tls_sip_port);
		}
	}

	sofia_glue_sql_close(profile);
	su_home_unref(profile->home);
	su_root_destroy(profile->s_root);
	pool = profile->pool;

	sofia_glue_del_profile(profile);
	switch_core_hash_destroy(&profile->chat_hash);

	switch_thread_rwlock_unlock(profile->rwlock);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Write unlock %s\n", profile->name);

	if (sofia_test_pflag(profile, PFLAG_RESPAWN)) {
		config_sofia(1, profile->name);
	}

	switch_core_destroy_memory_pool(&pool);

  end:
	switch_mutex_lock(mod_sofia_globals.mutex);
	mod_sofia_globals.threads--;
	switch_mutex_unlock(mod_sofia_globals.mutex);

	return NULL;
}

void launch_sofia_profile_thread(sofia_profile_t *profile)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;

	switch_threadattr_create(&thd_attr, profile->pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_threadattr_priority_increase(thd_attr);
	switch_thread_create(&thread, thd_attr, sofia_profile_thread_run, profile, profile->pool);
}

static void logger(void *logarg, char const *fmt, va_list ap)
{
	if (fmt && ap) {
		switch_log_vprintf(SWITCH_CHANNEL_LOG_CLEAN, mod_sofia_globals.tracelevel, fmt, ap);
	} else if (fmt && !ap) {
		switch_log_printf(SWITCH_CHANNEL_LOG_CLEAN, mod_sofia_globals.tracelevel, "%s", fmt);
	}
}

static su_log_t *sofia_get_logger(const char *name) 
{
	if (!strcasecmp(name, "tport")) {
		return tport_log;
	} else if (!strcasecmp(name, "iptsec")) {
		return iptsec_log;
	} else if (!strcasecmp(name, "nea")) {
		return nea_log;
	} else if (!strcasecmp(name, "nta")) {
		return nta_log;
	} else if (!strcasecmp(name, "nth_client")) {
		return nth_client_log;
	} else if (!strcasecmp(name, "nth_server")) {
		return nth_server_log;
	} else if (!strcasecmp(name, "nua")) {
		return nua_log;
	} else if (!strcasecmp(name, "sresolv")) {
		return sresolv_log;
	} else if (!strcasecmp(name, "stun")) {
		return stun_log;
	} else if (!strcasecmp(name, "default")) {
		return su_log_default;
	} else {
		return NULL;
	}
}

switch_status_t sofia_set_loglevel(const char *name, int level)
{
	su_log_t *log = NULL;
	
	if (level < 0 || level > 9) {
		return SWITCH_STATUS_FALSE;
	}
		
	if (!strcasecmp(name, "all")) {
		su_log_set_level(su_log_default, level);
		su_log_set_level(tport_log, level);
		su_log_set_level(iptsec_log, level);
		su_log_set_level(nea_log, level);
		su_log_set_level(nta_log, level);
		su_log_set_level(nth_client_log, level);
		su_log_set_level(nth_server_log, level);
		su_log_set_level(nua_log, level);
		su_log_set_level(soa_log, level);
		su_log_set_level(sresolv_log, level);
		su_log_set_level(stun_log, level);
		return SWITCH_STATUS_SUCCESS;
	}
	
	if (!(log = sofia_get_logger(name))) {
		return SWITCH_STATUS_FALSE;
	}
	
	su_log_set_level(log, level);
		
	return SWITCH_STATUS_SUCCESS;
}

int sofia_get_loglevel(const char *name)
{
	su_log_t *log = NULL;
	
	if ((log = sofia_get_logger(name))) {
		return log->log_level;
	} else {
		return -1;
	}
}

static void parse_gateway_subscriptions(sofia_profile_t *profile, sofia_gateway_t *gateway, switch_xml_t gw_subs_tag)
{
	switch_xml_t subscription_tag, param;

	for (subscription_tag = switch_xml_child(gw_subs_tag, "subscription"); subscription_tag; subscription_tag = subscription_tag->next) {
		sofia_gateway_subscription_t *gw_sub;

		if ((gw_sub = switch_core_alloc(profile->pool, sizeof(*gw_sub)))) {
			char *expire_seconds = "3600", *retry_seconds = "30", *content_type = "NO_CONTENT_TYPE";
			char *event = (char *) switch_xml_attr_soft(subscription_tag, "event");
			gw_sub->event = switch_core_strdup(gateway->pool, event);			
			gw_sub->gateway = gateway;
			gw_sub->next = NULL;
			
			for (param = switch_xml_child(subscription_tag, "param"); param; param = param->next) {
				char *var = (char *) switch_xml_attr_soft(param, "name");
				char *val = (char *) switch_xml_attr_soft(param, "value");
				if (!strcmp(var, "expire-seconds")) {
					expire_seconds = val;
				} else if (!strcmp(var, "retry-seconds")) {
					retry_seconds = val;
				} else if (!strcmp(var, "content-type")) {
					content_type = val;
				}
			}
			
			gw_sub->retry_seconds = atoi(retry_seconds);
			if (gw_sub->retry_seconds < 10) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "INVALID: retry_seconds correcting the value to 30\n");
				gw_sub->retry_seconds = 30;
			}
			
			gw_sub->expires_str = switch_core_strdup(gateway->pool, expire_seconds);  
			
			if ((gw_sub->freq = atoi(gw_sub->expires_str)) < 5) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"Invalid Freq: %d.  Setting Register-Frequency to 3600\n", gw_sub->freq);
				gw_sub->freq = 3600;
			}
			gw_sub->freq -= 2;
			gw_sub->content_type = switch_core_strdup(gateway->pool, content_type);
			gw_sub->next = gateway->subscriptions;		
		}
		gateway->subscriptions = gw_sub;
	}
}

static void parse_gateways(sofia_profile_t *profile, switch_xml_t gateways_tag)
{
	switch_xml_t gateway_tag, param = NULL, x_params, gw_subs_tag;
	sofia_gateway_t *gp;

	for (gateway_tag = switch_xml_child(gateways_tag, "gateway"); gateway_tag; gateway_tag = gateway_tag->next) {
		char *name = (char *) switch_xml_attr_soft(gateway_tag, "name");
		sofia_gateway_t *gateway;

		if (switch_strlen_zero(name)) {
			name = "anonymous";
		}

		switch_mutex_lock(mod_sofia_globals.hash_mutex);
		if ((gp = switch_core_hash_find(mod_sofia_globals.gateway_hash, name))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Ignoring duplicate gateway '%s'\n", name);
			switch_mutex_unlock(mod_sofia_globals.hash_mutex);
			goto skip;
		}
		switch_mutex_unlock(mod_sofia_globals.hash_mutex);

		if ((gateway = switch_core_alloc(profile->pool, sizeof(*gateway)))) {
			const char *sipip, *format;
			switch_uuid_t uuid;
			uint32_t ping_freq = 0, extension_in_contact = 0;
			char *register_str = "true", *scheme = "Digest",
				*realm = NULL,
				*username = NULL,
				*auth_username = NULL,
				*password = NULL,
				*caller_id_in_from = "false",
				*extension = NULL,
				*proxy = NULL,
				*context = profile->context,
				*expire_seconds = "3600",
				*retry_seconds = "30",
				*from_user = "", *from_domain = NULL, *outbound_proxy = NULL, *register_proxy = NULL, *contact_host = NULL,
				*contact_params = NULL, *params = NULL, *register_transport = NULL;
			
			if (!context) {
				context = "default";
			}
			
			switch_uuid_get(&uuid);
			switch_uuid_format(gateway->uuid_str, &uuid);
			
			gateway->register_transport = SOFIA_TRANSPORT_UDP;
			gateway->pool = profile->pool;
			gateway->profile = profile;
			gateway->name = switch_core_strdup(gateway->pool, name);
			gateway->freq = 0;
			gateway->next = NULL;
			gateway->ping = 0;
			gateway->ping_freq = 0;
			
			
			if ((x_params = switch_xml_child(gateway_tag, "variables"))) {
				param = switch_xml_child(x_params, "variable");
			} else {
				param = switch_xml_child(gateway_tag, "variable");
			}
			
			
			for (; param; param = param->next) {
				const char *var = switch_xml_attr(param, "name");
				const char *val = switch_xml_attr(param, "value");
				const char *direction = switch_xml_attr(param, "direction");
				int in = 0, out = 0;
				
				if (var && val) {
					if (direction) {
						if (!strcasecmp(direction, "inbound")) {
							in = 1;
						} else if (!strcasecmp(direction, "outbound")) {
							out = 1;
						}
					} else {
						in = out = 1;
					}

					if (in) {
						if (!gateway->ib_vars) {
							switch_event_create_plain(&gateway->ib_vars, SWITCH_EVENT_GENERAL);
						}
						switch_event_add_header_string(gateway->ib_vars, SWITCH_STACK_BOTTOM, var, val);
					}

					if (out) {
						if (!gateway->ob_vars) {
							switch_event_create_plain(&gateway->ob_vars, SWITCH_EVENT_GENERAL);
						}
						switch_event_add_header_string(gateway->ob_vars, SWITCH_STACK_BOTTOM, var, val);
					}
				}
			}

			if ((x_params = switch_xml_child(gateway_tag, "params"))) {
				param = switch_xml_child(x_params, "param");
			} else {
				param = switch_xml_child(gateway_tag, "param");
			}
			
			for (; param; param = param->next) {
				char *var = (char *) switch_xml_attr_soft(param, "name");
				char *val = (char *) switch_xml_attr_soft(param, "value");
				
				if (!strcmp(var, "register")) {
					register_str = val;
				} else if (!strcmp(var, "scheme")) {
					scheme = val;
				} else if (!strcmp(var, "realm")) {
					realm = val;
				} else if (!strcmp(var, "username")) {
					username = val;
				} else if (!strcmp(var, "extension-in-contact")) {
					extension_in_contact = switch_true(val);
				} else if (!strcmp(var, "auth-username")) {
					auth_username = val;
				} else if (!strcmp(var, "password")) {
					password = val;
				} else if (!strcmp(var, "caller-id-in-from")) {
					caller_id_in_from = val;
				} else if (!strcmp(var, "extension")) {
					extension = val;
				} else if (!strcmp(var, "ping")) {
					ping_freq = atoi(val);
				} else if (!strcmp(var, "proxy")) {
					proxy = val;
				} else if (!strcmp(var, "context")) {
					context = val;
				} else if (!strcmp(var, "expire-seconds")) {
					expire_seconds = val;
				} else if (!strcmp(var, "retry-seconds")) {
					retry_seconds = val;
				} else if (!strcmp(var, "retry_seconds")) { // support typo for back compat
					retry_seconds = val;
				} else if (!strcmp(var, "from-user")) {
					from_user = val;
				} else if (!strcmp(var, "from-domain")) {
					from_domain = val;
				} else if (!strcmp(var, "contact-host")) {
					contact_host = val;
				} else if (!strcmp(var, "register-proxy")) {
					register_proxy = val;
				} else if (!strcmp(var, "outbound-proxy")) {
					outbound_proxy = val;
				} else if (!strcmp(var, "contact-params")) {
					contact_params = val;
				} else if (!strcmp(var, "register-transport")) {
					sofia_transport_t transport = sofia_glue_str2transport(val);

					if (transport == SOFIA_TRANSPORT_UNKNOWN || (!sofia_test_pflag(profile, PFLAG_TLS) && sofia_glue_transport_has_tls(transport))) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ERROR: unsupported transport\n");
						goto skip;
					}

					gateway->register_transport = transport;
				}
			}

			if (ping_freq) {
				if (ping_freq >= 5) {
					gateway->ping_freq = ping_freq;
					gateway->ping = switch_epoch_time_now(NULL) + ping_freq;
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ERROR: invalid ping!\n");
				}
			}

			if ((gw_subs_tag = switch_xml_child(gateway_tag, "subscriptions"))) {
				parse_gateway_subscriptions(profile, gateway, gw_subs_tag);
			}
			
			if (switch_strlen_zero(realm)) {
				if (switch_strlen_zero(proxy)) {
					realm = name;
				} else {
					realm = proxy;
				}
			}

			if (switch_strlen_zero(username)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ERROR: username param is REQUIRED!\n");
				goto skip;
			}

			if (switch_strlen_zero(password)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ERROR: password param is REQUIRED!\n");
				goto skip;
			}

			if (switch_strlen_zero(from_user)) {
				from_user = username;
			}

			if (switch_strlen_zero(extension)) {
				extension = username;
			}

			if (switch_strlen_zero(proxy)) {
				proxy = realm;
			}

			if (!switch_true(register_str)) {
				gateway->state = REG_STATE_NOREG;
				gateway->status = SOFIA_GATEWAY_UP;
			}

			if (switch_strlen_zero(auth_username)) {
				auth_username = username;
			}
			
			if (!switch_strlen_zero(register_proxy)) {
				if (strncasecmp(register_proxy, "sip:", 4) && strncasecmp(register_proxy, "sips:", 5)) {
					gateway->register_sticky_proxy = switch_core_sprintf(gateway->pool, "sip:%s", register_proxy);
				} else {
					gateway->register_sticky_proxy = switch_core_strdup(gateway->pool, register_proxy);
				}
			}

			if (!switch_strlen_zero(outbound_proxy)) {
				if (strncasecmp(outbound_proxy, "sip:", 4) && strncasecmp(outbound_proxy, "sips:", 5)) {
					gateway->outbound_sticky_proxy = switch_core_sprintf(gateway->pool, "sip:%s", outbound_proxy);
				} else {
					gateway->outbound_sticky_proxy = switch_core_strdup(gateway->pool, outbound_proxy);
				}
			}

			gateway->retry_seconds = atoi(retry_seconds);

			if (gateway->retry_seconds < 5) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid retry-seconds of %d on gateway %s, using the value of 30 instead.\n",
								  gateway->retry_seconds, name);
				gateway->retry_seconds = 30;
			}

			gateway->register_scheme = switch_core_strdup(gateway->pool, scheme);
			gateway->register_context = switch_core_strdup(gateway->pool, context);
			gateway->register_realm = switch_core_strdup(gateway->pool, realm);
			gateway->register_username = switch_core_strdup(gateway->pool, username);
			gateway->auth_username = switch_core_strdup(gateway->pool, auth_username);
			gateway->register_password = switch_core_strdup(gateway->pool, password);

			if (switch_true(caller_id_in_from)) {
				sofia_set_flag(gateway, REG_FLAG_CALLERID);
			}

			register_transport = (char *) sofia_glue_transport2str(gateway->register_transport);

			if (contact_params) {
				if (*contact_params == ';') {
					params = switch_core_sprintf(gateway->pool, "%s;transport=%s", contact_params, register_transport);
				} else {
					params = switch_core_sprintf(gateway->pool, ";%s;transport=%s", contact_params, register_transport);
				}
			} else {
				params = switch_core_sprintf(gateway->pool, ";transport=%s", register_transport);
			}

			if (!switch_strlen_zero(from_domain)) {
				gateway->from_domain = switch_core_strdup(gateway->pool, from_domain);
			}

			gateway->register_url = switch_core_sprintf(gateway->pool, "sip:%s", proxy);
			gateway->register_from = switch_core_sprintf(gateway->pool, "<sip:%s@%s;transport=%s>", 
														 from_user, !switch_strlen_zero(from_domain) ? from_domain : proxy, register_transport);

			sipip = contact_host ? contact_host : profile->extsipip ?  profile->extsipip : profile->sipip;

			if (extension_in_contact) {
				format = strchr(sipip, ':') ? "<sip:%s@[%s]:%d%s>" : "<sip:%s@%s:%d%s>";
				gateway->register_contact = switch_core_sprintf(gateway->pool, format, extension,
																sipip,
																sofia_glue_transport_has_tls(gateway->register_transport) ?
																profile->tls_sip_port : profile->sip_port, params);
			} else {
				format = strchr(sipip, ':') ? "<sip:gw+%s@[%s]:%d%s>" : "<sip:gw+%s@%s:%d%s>";
				gateway->register_contact = switch_core_sprintf(gateway->pool, format, gateway->name,
																sipip,
																sofia_glue_transport_has_tls(gateway->register_transport) ?
																profile->tls_sip_port : profile->sip_port, params);
			}

			gateway->extension = switch_core_strdup(gateway->pool, extension);
			
			if (!strncasecmp(proxy, "sip:", 4)) {
				gateway->register_proxy = switch_core_strdup(gateway->pool, proxy);
				gateway->register_to = switch_core_sprintf(gateway->pool, "sip:%s@%s", username, proxy + 4);
			} else {
				gateway->register_proxy = switch_core_sprintf(gateway->pool, "sip:%s", proxy);
				gateway->register_to = switch_core_sprintf(gateway->pool, "sip:%s@%s", username, proxy);
			}

			gateway->expires_str = switch_core_strdup(gateway->pool, expire_seconds);

			if ((gateway->freq = atoi(gateway->expires_str)) < 5) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid register-frequency of %d on gateway %s, using the value of 3600 instead\n",
								  gateway->freq, name);
				gateway->freq = 3600;
			}

			gateway->next = profile->gateways;
			profile->gateways = gateway;
			sofia_reg_add_gateway(gateway->name, gateway);
		}

	  skip:
		switch_assert(gateway_tag);
	}
}

static void parse_domain_tag(sofia_profile_t *profile, switch_xml_t x_domain_tag, const char *dname, const char *parse, const char *alias)
{
	if (switch_true(alias)) {
		if (sofia_glue_add_profile(switch_core_strdup(profile->pool, dname), profile) == SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Adding Alias [%s] for profile [%s]\n", dname, profile->name);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Alias [%s] for profile [%s] (already exists)\n",                         
							  dname, profile->name);
		}
	}

	if (switch_true(parse)) {
		switch_xml_t gts, gt, uts, ut, gateways_tag;
		/* Backwards Compatibility */
		for (ut = switch_xml_child(x_domain_tag, "user"); ut; ut = ut->next) {
			if (((gateways_tag = switch_xml_child(ut, "gateways")))) {
				parse_gateways(profile, gateways_tag);
			}
		}
		/* New Method with <groups> tags and users are now inside a <users> tag */
		for (gts = switch_xml_child(x_domain_tag, "groups"); gts; gts = gts->next) {
			for (gt = switch_xml_child(gts, "group"); gt; gt = gt->next) {
				for (uts = switch_xml_child(gt, "users"); uts; uts = uts->next) {
					for (ut = switch_xml_child(uts, "user"); ut; ut = ut->next) {
						if (((gateways_tag = switch_xml_child(ut, "gateways")))) {
							parse_gateways(profile, gateways_tag);
						}
					}
				}
			}
		}
	}
}

static void parse_rtp_bugs(sofia_profile_t *profile, const char *str)
{
	if (switch_stristr("clear", str)) {
		profile->auto_rtp_bugs = 0;
	}

	if (switch_stristr("CISCO_SKIP_MARK_BIT_2833", str)) {
		profile->auto_rtp_bugs |= RTP_BUG_CISCO_SKIP_MARK_BIT_2833;
	}

	if (switch_stristr("~CISCO_SKIP_MARK_BIT_2833", str)) {
		profile->auto_rtp_bugs &= ~RTP_BUG_CISCO_SKIP_MARK_BIT_2833;
	}
	
	if (switch_stristr("SONUS_SEND_INVALID_TIMESTAMP_2833", str)) {
		profile->auto_rtp_bugs |= RTP_BUG_SONUS_SEND_INVALID_TIMESTAMP_2833;
	}

	if (switch_stristr("~SONUS_SEND_INVALID_TIMESTAMP_2833", str)) {
		profile->auto_rtp_bugs &= ~RTP_BUG_SONUS_SEND_INVALID_TIMESTAMP_2833;
	}


}

switch_status_t reconfig_sofia(sofia_profile_t *profile)
{
	switch_xml_t cfg, xml = NULL, xprofile, profiles, gateways_tag, domain_tag, domains_tag, aliases_tag, alias_tag, settings, param;
	char *cf = "sofia.conf";
	switch_event_t *params = NULL;
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_event_create(&params, SWITCH_EVENT_REQUEST_PARAMS);
	switch_assert(params);
	switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "profile", profile->name);
	switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "reconfig", "true");

	if (!(xml = switch_xml_open_cfg(cf, &cfg, params))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	
	if ((profiles = switch_xml_child(cfg, "profiles"))) {
		for (xprofile = switch_xml_child(profiles, "profile"); xprofile; xprofile = xprofile->next) {
			char *xprofilename = (char *) switch_xml_attr_soft(xprofile, "name");
			char *xprofiledomain = (char *) switch_xml_attr(xprofile, "domain");

			if (strcasecmp(profile->name, xprofilename)) {
				continue;
			}

			/* you could change profile->foo here if it was a minor change like context or dialplan ... */
			profile->rport_level = 1; /* default setting */
			profile->acl_count = 0;
			sofia_set_pflag(profile, PFLAG_STUN_ENABLED);
			profile->ib_calls = 0;
			profile->ob_calls = 0;
			profile->ib_failed_calls = 0;
			profile->ob_failed_calls = 0;

			if (xprofiledomain) {
				profile->domain_name = switch_core_strdup(profile->pool, xprofiledomain);
			}

			if ((settings = switch_xml_child(xprofile, "settings"))) {
				for (param = switch_xml_child(settings, "param"); param; param = param->next) {
					char *var = (char *) switch_xml_attr_soft(param, "name");
					char *val = (char *) switch_xml_attr_soft(param, "value");
					if (!strcasecmp(var, "debug")) {
						profile->debug = atoi(val);
					} else if (!strcasecmp(var, "tracelevel")) {
						mod_sofia_globals.tracelevel = switch_log_str2level(val);
					} else if (!strcasecmp(var, "sip-trace")) {
						if (switch_true(val)) {
							sofia_set_flag(profile, TFLAG_TPORT_LOG);
						} else {
							sofia_clear_flag(profile, TFLAG_TPORT_LOG);
						}
						nua_set_params(profile->nua, TPTAG_LOG(sofia_test_flag(profile, TFLAG_TPORT_LOG)), TAG_END());
					} else if (!strcasecmp(var, "send-message-query-on-register")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_MESSAGE_QUERY_ON_REGISTER);
						} else {
							sofia_clear_pflag(profile, PFLAG_MESSAGE_QUERY_ON_REGISTER);
						}
					} else if (!strcasecmp(var, "auto-rtp-bugs")) {
						parse_rtp_bugs(profile, val);
					} else if (!strcasecmp(var, "user-agent-string")) { 
						profile->user_agent = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "auto-restart")) {
						profile->auto_restart = switch_true(val);
					} else if (!strcasecmp(var, "dtmf-type")) {
						if (!strcasecmp(val, "rfc2833")) {
							profile->dtmf_type = DTMF_2833;
						} else if (!strcasecmp(val, "info")) {
							profile->dtmf_type = DTMF_INFO;
						} else {
							profile->dtmf_type = DTMF_NONE;
						}
					} else if (!strcasecmp(var, "NDLB-force-rport")) {
						if (switch_true(val)) {
							profile->rport_level = 2;
						}
					} else if (!strcasecmp(var, "caller-id-type")) {
						profile->cid_type = sofia_cid_name2type(val);
					} else if (!strcasecmp(var, "record-template")) {
						profile->record_template = switch_core_strdup(profile->pool, val);;
					} else if ((!strcasecmp(var, "inbound-no-media") || !strcasecmp(var, "inbound-bypass-media"))) {
						if (switch_true(val)) {
							sofia_set_flag(profile, TFLAG_INB_NOMEDIA);
						} else {
							sofia_clear_flag(profile, TFLAG_INB_NOMEDIA);
						}
					} else if (!strcasecmp(var, "force-subscription-expires")) {
						int tmp = atoi(val);
						if (tmp > 0) {
							profile->force_subscription_expires = tmp;
						}
					} else if (!strcasecmp(var, "inbound-late-negotiation")) {
						if (switch_true(val)) {
							sofia_set_flag(profile, TFLAG_LATE_NEGOTIATION);
						} else {
							sofia_clear_flag(profile, TFLAG_LATE_NEGOTIATION);
						}
					} else if (!strcasecmp(var, "inbound-proxy-media")) {
						if (switch_true(val)) { 
							sofia_set_flag(profile, TFLAG_PROXY_MEDIA);
						} else {
							sofia_clear_flag(profile, TFLAG_PROXY_MEDIA);
						}
					} else if (!strcasecmp(var, "inbound-use-callid-as-uuid")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_CALLID_AS_UUID);
						} else {
							sofia_clear_pflag(profile, PFLAG_CALLID_AS_UUID);
						}
					} else if (!strcasecmp(var, "rtp-autoflush-during-bridge")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_RTP_AUTOFLUSH_DURING_BRIDGE);
						} else {
							sofia_clear_pflag(profile, PFLAG_RTP_AUTOFLUSH_DURING_BRIDGE);
						}
					} else if (!strcasecmp(var, "manual-redirect")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_MANUAL_REDIRECT);
						} else {
							sofia_clear_pflag(profile, PFLAG_MANUAL_REDIRECT);
						}
					} else if (!strcasecmp(var, "outbound-use-uuid-as-callid")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_UUID_AS_CALLID);
						} else {
							sofia_clear_pflag(profile, PFLAG_UUID_AS_CALLID);
						}
					} else if (!strcasecmp(var, "NDLB-received-in-nat-reg-contact")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_RECIEVED_IN_NAT_REG_CONTACT);
						} else {
							sofia_clear_pflag(profile, PFLAG_RECIEVED_IN_NAT_REG_CONTACT);
						}
					} else if (!strcasecmp(var, "aggressive-nat-detection")) {
						if (switch_true(val)) { 
							sofia_set_pflag(profile, PFLAG_AGGRESSIVE_NAT_DETECTION);
						} else {
							sofia_clear_pflag(profile, PFLAG_AGGRESSIVE_NAT_DETECTION);
						}
					} else if (!strcasecmp(var, "disable-rtp-auto-adjust")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_DISABLE_RTP_AUTOADJ);
						} else {
							sofia_clear_pflag(profile, PFLAG_DISABLE_RTP_AUTOADJ);
						}
					} else if (!strcasecmp(var, "NDLB-support-asterisk-missing-srtp-auth")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_DISABLE_SRTP_AUTH); 
						} else {
							sofia_clear_pflag(profile, PFLAG_DISABLE_SRTP_AUTH); 
						}
					} else if (!strcasecmp(var, "NDLB-funny-stun")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_FUNNY_STUN); 
						} else {
							sofia_clear_pflag(profile, PFLAG_FUNNY_STUN); 
						}
					} else if (!strcasecmp(var, "stun-enabled")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_STUN_ENABLED);
						} else {
							sofia_clear_pflag(profile, PFLAG_STUN_ENABLED);
						}
					} else if (!strcasecmp(var, "stun-auto-disable")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_STUN_AUTO_DISABLE);
						} else {
							sofia_clear_pflag(profile, PFLAG_STUN_AUTO_DISABLE); 
						}
					} else if (!strcasecmp(var, "apply-nat-acl")) {
						if (profile->acl_count < SOFIA_MAX_ACL) {
							if (!profile->extsipip && switch_check_network_list_ip(profile->sipip, val)) {
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Not adding acl %s because it's the local network\n", val);
							} else {
								profile->nat_acl[profile->nat_acl_count++] = switch_core_strdup(profile->pool, val);
							}
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Max acl records of %d reached\n", SOFIA_MAX_ACL);
						}
					} else if (!strcasecmp(var, "apply-inbound-acl")) {
						if (profile->acl_count < SOFIA_MAX_ACL) {
							profile->acl[profile->acl_count++] = switch_core_strdup(profile->pool, val);
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Max acl records of %d reached\n", SOFIA_MAX_ACL);
						}
					} else if (!strcasecmp(var, "apply-register-acl")) {
						if (profile->reg_acl_count < SOFIA_MAX_ACL) {
							profile->reg_acl[profile->reg_acl_count++] = switch_core_strdup(profile->pool, val);
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Max acl records of %d reached\n", SOFIA_MAX_ACL);
						}
					} else if (!strcasecmp(var, "rfc2833-pt")) {
						profile->te = (switch_payload_t) atoi(val);
					} else if (!strcasecmp(var, "cng-pt") && !(sofia_test_pflag(profile, PFLAG_SUPPRESS_CNG))) {
						profile->cng_pt = (switch_payload_t) atoi(val);
					} else if (!strcasecmp(var, "vad")) {
						if (!strcasecmp(val, "in")) {
							sofia_set_flag(profile, TFLAG_VAD_IN);
						} else if (!strcasecmp(val, "out")) {
							sofia_set_flag(profile, TFLAG_VAD_OUT);
						} else if (!strcasecmp(val, "both")) {
							sofia_set_flag(profile, TFLAG_VAD_IN);
							sofia_set_flag(profile, TFLAG_VAD_OUT);
						} else if (strcasecmp(val, "none")) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid option %s for VAD\n", val);
						}
					} else if (!strcasecmp(var, "unregister-on-options-fail")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_UNREG_OPTIONS_FAIL);
						} else {
							sofia_clear_pflag(profile, PFLAG_UNREG_OPTIONS_FAIL);
						}
					} else if (!strcasecmp(var, "require-secure-rtp")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_SECURE);
						} else {
							sofia_clear_pflag(profile, PFLAG_SECURE);
						}
					} else if (!strcasecmp(var, "multiple-registrations")) {
						if (!strcasecmp(val, "call-id")) {
							sofia_set_pflag(profile, PFLAG_MULTIREG);
						} else if (!strcasecmp(val, "contact") || switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_MULTIREG);
							sofia_set_pflag(profile, PFLAG_MULTIREG_CONTACT);
						} else if (switch_true(val)) {
							sofia_clear_pflag(profile, PFLAG_MULTIREG);
							//sofia_clear_pflag(profile, PFLAG_MULTIREG_CONTACT);
						}
					} else if (!strcasecmp(var, "supress-cng") || !strcasecmp(var, "suppress-cng")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_SUPPRESS_CNG);
							profile->cng_pt = 0;
						} else {
							sofia_clear_pflag(profile, PFLAG_SUPPRESS_CNG);
						}
					} else if (!strcasecmp(var, "NDLB-broken-auth-hash")) {
						if (switch_true(val)) {
							profile->ndlb |= PFLAG_NDLB_BROKEN_AUTH_HASH;
						} else {
							profile->ndlb &= ~PFLAG_NDLB_BROKEN_AUTH_HASH;
						}
					} else if (!strcasecmp(var, "NDLB-sendrecv-in-session")) {
						if (switch_true(val)) {
							profile->ndlb |= PFLAG_NDLB_SENDRECV_IN_SESSION;
						} else {
							profile->ndlb &= ~PFLAG_NDLB_SENDRECV_IN_SESSION;
						}
					} else if (!strcasecmp(var, "pass-rfc2833")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_PASS_RFC2833);
						} else {
							sofia_clear_pflag(profile, PFLAG_PASS_RFC2833);
						}
					} else if (!strcasecmp(var, "rtp-autoflush")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_AUTOFLUSH);
						} else {
							sofia_clear_pflag(profile, PFLAG_AUTOFLUSH);
						}
					} else if (!strcasecmp(var, "rtp-autofix-timing")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_AUTOFIX_TIMING);
						} else {
							sofia_clear_pflag(profile, PFLAG_AUTOFIX_TIMING);
						}
					} else if (!strcasecmp(var, "nat-options-ping")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_NAT_OPTIONS_PING);
						} else {
							sofia_clear_pflag(profile, PFLAG_NAT_OPTIONS_PING);
						}
					} else if (!strcasecmp(var, "inbound-codec-negotiation")) {
						if (!strcasecmp(val, "greedy")) {
							sofia_set_pflag(profile, PFLAG_GREEDY);
						} else if (!strcasecmp(val, "scrooge")) {
							sofia_set_pflag(profile, PFLAG_GREEDY);
							sofia_set_pflag(profile, PFLAG_SCROOGE);
						} else {
							sofia_clear_pflag(profile, PFLAG_SCROOGE);
							sofia_clear_pflag(profile, PFLAG_GREEDY);
						}
					} else if (!strcasecmp(var, "disable-transcoding")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_DISABLE_TRANSCODING);
						} else {
							sofia_clear_pflag(profile, PFLAG_DISABLE_TRANSCODING);
						}
					} else if (!strcasecmp(var, "rtp-rewrite-timestamps")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_REWRITE_TIMESTAMPS);
						} else {
							sofia_clear_pflag(profile, PFLAG_REWRITE_TIMESTAMPS);
						}
					} else if (!strcasecmp(var, "auth-calls")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_AUTH_CALLS);
						} else {
							sofia_clear_pflag(profile, PFLAG_AUTH_CALLS);
						}
					} else if (!strcasecmp(var, "context")) {
						profile->context = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "local-network-acl")) {
						profile->local_network = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "force-register-domain")) {
						profile->reg_domain = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "force-register-db-domain")) {
						profile->reg_db_domain = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "hold-music")) {
						profile->hold_music = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "outbound-proxy")) {
						profile->outbound_proxy = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "session-timeout")) {
						int v_session_timeout = atoi(val);
						if (v_session_timeout >= 0) {
							profile->session_timeout = v_session_timeout;
						}
					} else if (!strcasecmp(var, "rtp-timeout-sec")) {
						int v = atoi(val);
						if (v >= 0) {
							profile->rtp_timeout_sec = v;
						}
					} else if (!strcasecmp(var, "rtp-hold-timeout-sec")) {
						int v = atoi(val);
						if (v >= 0) {
							profile->rtp_hold_timeout_sec = v;
						}
					} else if (!strcasecmp(var, "nonce-ttl")) {
						profile->nonce_ttl = atoi(val);
					} else if (!strcasecmp(var, "dialplan")) {
						profile->dialplan = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "max-calls")) {
						profile->max_calls = atoi(val);
					} else if (!strcasecmp(var, "codec-prefs")) {
						profile->codec_string = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "challenge-realm")) {
						profile->challenge_realm = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "dtmf-duration")) {
						int dur = atoi(val);
						if (dur > 10 && dur < 8000) {
							profile->dtmf_duration = dur;
						} else {
							profile->dtmf_duration = SWITCH_DEFAULT_DTMF_DURATION;
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Duration out of bounds, using default of %d!\n", SWITCH_DEFAULT_DTMF_DURATION);
						}
					} else if (!strcasecmp(var, "timer-T1")) {
						int v = atoi(val);
						if (v > 0) {
							profile->timer_t1 = v;
						} else {
							profile->timer_t1 = 500;
						}
						nua_set_params(profile->nua, NTATAG_SIP_T1(profile->timer_t1), TAG_END());
					} else if (!strcasecmp(var, "timer-T1X64")) {
						int v = atoi(val);
						if (v > 0) {
							profile->timer_t1x64 = v;
						} else {
							profile->timer_t1x64 = 32000;
						}
						nua_set_params(profile->nua, NTATAG_SIP_T1X64(profile->timer_t1x64), TAG_END());
					} else if (!strcasecmp(var, "timer-T2")) {
						int v = atoi(val);
						if (v > 0) {
							profile->timer_t2 = v;
						} else {
							profile->timer_t2 = 4000;
						}
						nua_set_params(profile->nua, NTATAG_SIP_T2(profile->timer_t2), TAG_END());
					} else if (!strcasecmp(var, "timer-T4")) {
						int v = atoi(val);
						if (v > 0) {
							profile->timer_t4 = v;
						} else {
							profile->timer_t4 = 4000;
						}
						nua_set_params(profile->nua, NTATAG_SIP_T4(profile->timer_t4), TAG_END());
					}
				}
			}

			if ((gateways_tag = switch_xml_child(xprofile, "gateways"))) {
				parse_gateways(profile, gateways_tag);
			}
			
			status = SWITCH_STATUS_SUCCESS;

			if ((domains_tag = switch_xml_child(xprofile, "domains"))) {
				switch_event_t *xml_params;
				switch_event_create(&xml_params, SWITCH_EVENT_REQUEST_PARAMS);
				switch_assert(xml_params);
				switch_event_add_header_string(xml_params, SWITCH_STACK_BOTTOM, "purpose", "gateways");
				switch_event_add_header_string(xml_params, SWITCH_STACK_BOTTOM, "profile", profile->name);
				
				for (domain_tag = switch_xml_child(domains_tag, "domain"); domain_tag; domain_tag = domain_tag->next) {
					switch_xml_t droot, x_domain_tag;
					const char *dname = switch_xml_attr_soft(domain_tag, "name");
					const char *parse = switch_xml_attr_soft(domain_tag, "parse");
					const char *alias = switch_xml_attr_soft(domain_tag, "alias");

					if (!switch_strlen_zero(dname)) {
						if (!strcasecmp(dname, "all")) {
							switch_xml_t xml_root, x_domains;
							if (switch_xml_locate("directory", NULL, NULL, NULL, &xml_root, &x_domains, xml_params, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
								for (x_domain_tag = switch_xml_child(x_domains, "domain"); x_domain_tag; x_domain_tag = x_domain_tag->next) {
									dname = switch_xml_attr_soft(x_domain_tag, "name");
									parse_domain_tag(profile, x_domain_tag, dname, parse, alias);
								}
								switch_xml_free(xml_root);
							}
						} else if (switch_xml_locate_domain(dname, xml_params, &droot, &x_domain_tag) == SWITCH_STATUS_SUCCESS) {
							parse_domain_tag(profile, x_domain_tag, dname, parse, alias);
							switch_xml_free(droot);
						}
					}
				}
				
				switch_event_destroy(&xml_params);
			}

			if ((aliases_tag = switch_xml_child(xprofile, "aliases"))) {
				for (alias_tag = switch_xml_child(aliases_tag, "alias"); alias_tag; alias_tag = alias_tag->next) {
					char *aname = (char *) switch_xml_attr_soft(alias_tag, "name");
					if (!switch_strlen_zero(aname)) {
						
						if (sofia_glue_add_profile(switch_core_strdup(profile->pool, aname), profile) == SWITCH_STATUS_SUCCESS) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Adding Alias [%s] for profile [%s]\n", aname, profile->name);
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Alias [%s] for profile [%s] (already exists)\n",
											  aname, profile->name);
						}
					}
				}
			}
		}
	}

 done:

	if (xml) {
        switch_xml_free(xml);
    }

	switch_event_destroy(&params);

	return status;
}

switch_status_t config_sofia(int reload, char *profile_name)
{
	char *cf = "sofia.conf";
	switch_xml_t cfg, xml = NULL, xprofile, param, settings, profiles, gateways_tag, domain_tag, domains_tag;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	sofia_profile_t *profile = NULL;
	char url[512] = "";
	int profile_found = 0;
	switch_event_t *params = NULL;;

	if (!reload) {
		su_init();
		if (sip_update_default_mclass(sip_extend_mclass(NULL)) < 0) {
			su_deinit();
			return SWITCH_STATUS_FALSE;
		}
		
		/* Redirect loggers in sofia */
		su_log_redirect(su_log_default, logger, NULL);
		su_log_redirect(tport_log, logger, NULL);
		su_log_redirect(iptsec_log, logger, NULL);
		su_log_redirect(nea_log, logger, NULL);
		su_log_redirect(nta_log, logger, NULL);
		su_log_redirect(nth_client_log, logger, NULL);
		su_log_redirect(nth_server_log, logger, NULL);
		su_log_redirect(nua_log, logger, NULL);
		su_log_redirect(soa_log, logger, NULL);
		su_log_redirect(sresolv_log, logger, NULL);
		su_log_redirect(stun_log, logger, NULL);
	}

	if (!switch_strlen_zero(profile_name) && (profile = sofia_glue_find_profile(profile_name))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Profile [%s] Already exists.\n", switch_str_nil(profile_name));
		status = SWITCH_STATUS_FALSE;
		sofia_glue_release_profile(profile);
		return status;
	}

	switch_event_create(&params, SWITCH_EVENT_REQUEST_PARAMS);
	switch_assert(params);
	switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "profile", profile_name);

	if (!(xml = switch_xml_open_cfg(cf, &cfg, params))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	mod_sofia_globals.auto_restart = SWITCH_TRUE;
	mod_sofia_globals.rewrite_multicasted_fs_path = SWITCH_FALSE;

	if ((settings = switch_xml_child(cfg, "global_settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = (char *) switch_xml_attr_soft(param, "name");
			char *val = (char *) switch_xml_attr_soft(param, "value");
			if (!strcasecmp(var, "log-level")) {
				su_log_set_level(NULL, atoi(val));
			} else if (!strcasecmp(var, "debug-presence")) {
				mod_sofia_globals.debug_presence = atoi(val);
			} else if (!strcasecmp(var, "auto-restart")) {
				mod_sofia_globals.auto_restart = switch_true(val);
			} else if (!strcasecmp(var, "rewrite-multicasted-fs-path")) {
				mod_sofia_globals.rewrite_multicasted_fs_path = switch_true(val);
			}
		}
	}

	if ((profiles = switch_xml_child(cfg, "profiles"))) {
		for (xprofile = switch_xml_child(profiles, "profile"); xprofile; xprofile = xprofile->next) {
			if (!(settings = switch_xml_child(xprofile, "settings"))) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No Settings, check the new config!\n");
			} else {
				char *xprofilename = (char *) switch_xml_attr_soft(xprofile, "name");
				char *xprofiledomain = (char *) switch_xml_attr(xprofile, "domain");
				switch_memory_pool_t *pool = NULL;

				if (!xprofilename) {
					xprofilename = "unnamed";
				}

				if (profile_name) {
					if (strcasecmp(profile_name, xprofilename)) {
						continue;
					} else {
						profile_found = 1;
					}
				}

				/* Setup the pool */
				if ((status = switch_core_new_memory_pool(&pool)) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Memory Error!\n");
					goto done;
				}

				if (!(profile = (sofia_profile_t *) switch_core_alloc(pool, sizeof(*profile)))) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Memory Error!\n");
					goto done;
				}

				profile->auto_rtp_bugs = RTP_BUG_CISCO_SKIP_MARK_BIT_2833 | RTP_BUG_SONUS_SEND_INVALID_TIMESTAMP_2833;

				profile->pool = pool;
				profile->user_agent = SOFIA_USER_AGENT;

				profile->name = switch_core_strdup(profile->pool, xprofilename);
				switch_snprintf(url, sizeof(url), "sofia_reg_%s", xprofilename);

				if (xprofiledomain) {
					profile->domain_name = switch_core_strdup(profile->pool, xprofiledomain);
				}

				profile->dbname = switch_core_strdup(profile->pool, url);
				switch_core_hash_init(&profile->chat_hash, profile->pool);
				switch_thread_rwlock_create(&profile->rwlock, profile->pool);
				switch_mutex_init(&profile->flag_mutex, SWITCH_MUTEX_NESTED, profile->pool);
				profile->dtmf_duration = 100;
				profile->tls_version = 0;
				profile->mflags = MFLAG_REFER | MFLAG_REGISTER;
				profile->rport_level = 1;
				sofia_set_pflag(profile, PFLAG_STUN_ENABLED);
				sofia_set_pflag(profile, PFLAG_DISABLE_100REL);
				profile->auto_restart = 1;
				sofia_set_pflag(profile, PFLAG_AUTOFIX_TIMING);
				sofia_set_pflag(profile, PFLAG_MESSAGE_QUERY_ON_REGISTER);
				sofia_set_pflag(profile, PFLAG_RTP_AUTOFLUSH_DURING_BRIDGE);
				profile->contact_user = SOFIA_DEFAULT_CONTACT_USER;

				for (param = switch_xml_child(settings, "param"); param; param = param->next) {
					char *var = (char *) switch_xml_attr_soft(param, "name");
					char *val = (char *) switch_xml_attr_soft(param, "value");

					if (!strcasecmp(var, "debug")) {
						profile->debug = atoi(val);
					} else if (!strcasecmp(var, "sip-trace") && switch_true(val)) {
						sofia_set_flag(profile, TFLAG_TPORT_LOG);
					} else if (!strcasecmp(var, "odbc-dsn") && !switch_strlen_zero(val)) {
						if (switch_odbc_available()) {
							profile->odbc_dsn = switch_core_strdup(profile->pool, val);
							if ((profile->odbc_user = strchr(profile->odbc_dsn, ':'))) {
								*profile->odbc_user++ = '\0';
								if ((profile->odbc_pass = strchr(profile->odbc_user, ':'))) {
									*profile->odbc_pass++ = '\0';
								}
							}
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ODBC IS NOT AVAILABLE!\n");
						}
					} else if (!strcasecmp(var, "user-agent-string")) {
						profile->user_agent = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "auto-restart")) {
						profile->auto_restart = switch_true(val);
					} else if (!strcasecmp(var, "dtmf-type")) {
						if (!strcasecmp(val, "rfc2833")) {
							profile->dtmf_type = DTMF_2833;
						} else if (!strcasecmp(val, "info")) {
							profile->dtmf_type = DTMF_INFO;
						} else {
							profile->dtmf_type = DTMF_NONE;
						}
					} else if (!strcasecmp(var, "NDLB-force-rport")) {
						if (switch_true(val)) {
							profile->rport_level = 2;
						}
					} else if (!strcasecmp(var, "auto-rtp-bugs")) {
						parse_rtp_bugs(profile, val);
					} else if (!strcasecmp(var, "dbname")) {
						profile->dbname = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "presence-hosts")) {
						profile->presence_hosts = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "caller-id-type")) {
						profile->cid_type = sofia_cid_name2type(val);
					} else if (!strcasecmp(var, "record-template")) {
						profile->record_template = switch_core_strdup(profile->pool, val);
					} else if ((!strcasecmp(var, "inbound-no-media") || !strcasecmp(var, "inbound-bypass-media")) && switch_true(val)) {
						sofia_set_flag(profile, TFLAG_INB_NOMEDIA);
					} else if (!strcasecmp(var, "inbound-late-negotiation") && switch_true(val)) {
						sofia_set_flag(profile, TFLAG_LATE_NEGOTIATION);
					} else if (!strcasecmp(var, "rtp-autoflush-during-bridge")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_RTP_AUTOFLUSH_DURING_BRIDGE);
						} else {
							sofia_clear_pflag(profile, PFLAG_RTP_AUTOFLUSH_DURING_BRIDGE);
						}
					} else if (!strcasecmp(var, "manual-redirect")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_MANUAL_REDIRECT);
						} else {
							sofia_clear_pflag(profile, PFLAG_MANUAL_REDIRECT);
						}
					} else if (!strcasecmp(var, "inbound-proxy-media") && switch_true(val)) {
						sofia_set_flag(profile, TFLAG_PROXY_MEDIA);
					} else if (!strcasecmp(var, "force-subscription-expires")) {
						int tmp = atoi(val);
						if (tmp > 0) {
							profile->force_subscription_expires = tmp;
						}
					} else if (!strcasecmp(var, "send-message-query-on-register")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_MESSAGE_QUERY_ON_REGISTER);
						} else {
							sofia_clear_pflag(profile, PFLAG_MESSAGE_QUERY_ON_REGISTER);
						}
					} else if (!strcasecmp(var, "inbound-use-callid-as-uuid")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_CALLID_AS_UUID);
						} else {
							sofia_clear_pflag(profile, PFLAG_CALLID_AS_UUID);
						}
					} else if (!strcasecmp(var, "outbound-use-uuid-as-callid")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_UUID_AS_CALLID);
						} else {
							sofia_clear_pflag(profile, PFLAG_UUID_AS_CALLID);
						}
					} else if (!strcasecmp(var, "NDLB-received-in-nat-reg-contact") && switch_true(val)) {
						sofia_set_pflag(profile, PFLAG_RECIEVED_IN_NAT_REG_CONTACT);
					} else if (!strcasecmp(var, "aggressive-nat-detection") && switch_true(val)) {
						sofia_set_pflag(profile, PFLAG_AGGRESSIVE_NAT_DETECTION);
					} else if (!strcasecmp(var, "disable-rtp-auto-adjust") && switch_true(val)) {
						sofia_set_pflag(profile, PFLAG_DISABLE_RTP_AUTOADJ);
					} else if (!strcasecmp(var, "NDLB-support-asterisk-missing-srtp-auth") && switch_true(val)) {
						sofia_set_pflag(profile, PFLAG_DISABLE_SRTP_AUTH);
					} else if (!strcasecmp(var, "NDLB-funny-stun")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_FUNNY_STUN); 
						} else {
							sofia_clear_pflag(profile, PFLAG_FUNNY_STUN); 
						}
					} else if (!strcasecmp(var, "stun-enabled")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_STUN_ENABLED);
						} else {
							sofia_clear_pflag(profile, PFLAG_STUN_ENABLED); 
						}
					} else if (!strcasecmp(var, "stun-auto-disable")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_STUN_AUTO_DISABLE);
						} else {
							sofia_clear_pflag(profile, PFLAG_STUN_AUTO_DISABLE); 
						}
					} else if (!strcasecmp(var, "rfc2833-pt")) {
						profile->te = (switch_payload_t) atoi(val);
					} else if (!strcasecmp(var, "cng-pt") && !sofia_test_pflag(profile, PFLAG_SUPPRESS_CNG)) {
						profile->cng_pt = (switch_payload_t) atoi(val);
					} else if (!strcasecmp(var, "sip-port")) {
						profile->sip_port = (switch_port_t)atoi(val);
					} else if (!strcasecmp(var, "vad")) {
						if (!strcasecmp(val, "in")) {
							sofia_set_flag(profile, TFLAG_VAD_IN);
						} else if (!strcasecmp(val, "out")) {
							sofia_set_flag(profile, TFLAG_VAD_OUT);
						} else if (!strcasecmp(val, "both")) {
							sofia_set_flag(profile, TFLAG_VAD_IN);
							sofia_set_flag(profile, TFLAG_VAD_OUT);
						} else if (strcasecmp(val, "none")) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid option %s for VAD\n", val);
						}
					} else if (!strcasecmp(var, "ext-rtp-ip")) {
						if (!switch_strlen_zero(val)) {
							char *ip = mod_sofia_globals.guess_ip;
							
							if (!strcmp(val, "0.0.0.0")) {
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid IP 0.0.0.0 replaced with %s\n", mod_sofia_globals.guess_ip);
							} else if (!strcasecmp(val, "auto-nat")) {
								ip = mod_sofia_globals.auto_nat ? switch_core_get_variable("nat_public_addr") : mod_sofia_globals.guess_ip; 
							} else {
								ip = strcasecmp(val, "auto") ? val : mod_sofia_globals.guess_ip;
							}
							sofia_set_pflag(profile, PFLAG_AUTO_NAT);
							profile->extrtpip = switch_core_strdup(profile->pool, ip);
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid ext-rtp-ip\n");
						}
					} else if (!strcasecmp(var, "rtp-ip")) {
						char *ip = mod_sofia_globals.guess_ip;

						if (!strcmp(val, "0.0.0.0")) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid IP 0.0.0.0 replaced with %s\n", mod_sofia_globals.guess_ip);
						} else {
							ip = strcasecmp(val, "auto") ? val : mod_sofia_globals.guess_ip;
						}
						profile->rtpip = switch_core_strdup(profile->pool, ip);
					} else if (!strcasecmp(var, "sip-ip")) {
						char *ip = mod_sofia_globals.guess_ip;

						if (!strcmp(val, "0.0.0.0")) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid IP 0.0.0.0 replaced with %s\n", mod_sofia_globals.guess_ip);
						} else {
							ip = strcasecmp(val, "auto") ? val : mod_sofia_globals.guess_ip;
						}
						profile->sipip = switch_core_strdup(profile->pool, ip);
					} else if (!strcasecmp(var, "ext-sip-ip")) {
						if (!switch_strlen_zero(val)) {
							char *ip = mod_sofia_globals.guess_ip;
							char stun_ip[50] = "";
							char *myip = stun_ip;
							
							switch_copy_string(stun_ip, ip, sizeof(stun_ip));
							
							if (!strcasecmp(val, "0.0.0.0")) {
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid IP 0.0.0.0 replaced with %s\n", mod_sofia_globals.guess_ip);
							} else if (!strcasecmp(val, "auto-nat")) {
								ip = mod_sofia_globals.auto_nat ? switch_core_get_variable("nat_public_addr") : mod_sofia_globals.guess_ip;
							} else if (strcasecmp(val, "auto")) {
								switch_port_t port = 0;
								if (sofia_glue_ext_address_lookup(profile, NULL, &myip, &port, val, profile->pool) == SWITCH_STATUS_SUCCESS) {
									ip = myip;
								} else {
									switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to get external ip.\n");
								}
							}
							sofia_set_pflag(profile, PFLAG_AUTO_NAT);
							profile->extsipip = switch_core_strdup(profile->pool, ip);
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid ext-sip-ip\n");
						}
					} else if (!strcasecmp(var, "local-network-acl")) {
						profile->local_network = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "force-register-domain")) {
						profile->reg_domain = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "force-register-db-domain")) {
						profile->reg_db_domain = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "bind-params")) {
						profile->bind_params = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "sip-domain")) {
						profile->sipdomain = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "rtp-timer-name")) {
						profile->timer_name = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "hold-music")) {
						profile->hold_music = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "outbound-proxy")) {
						profile->outbound_proxy = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "session-timeout")) {
						int v_session_timeout = atoi(val);
						if (v_session_timeout >= 0) {
							profile->session_timeout = v_session_timeout;
						}
					} else if (!strcasecmp(var, "max-proceeding")) {
						int v_max_proceeding = atoi(val);
						if (v_max_proceeding >= 0) {
							profile->max_proceeding = v_max_proceeding;
						}
					} else if (!strcasecmp(var, "rtp-timeout-sec")) {
						int v = atoi(val);
						if (v >= 0) {
							profile->rtp_timeout_sec = v;
						}
					} else if (!strcasecmp(var, "rtp-hold-timeout-sec")) {
						int v = atoi(val);
						if (v >= 0) {
							profile->rtp_hold_timeout_sec = v;
						}
					} else if (!strcasecmp(var, "disable-transfer") && switch_true(val)) {
						profile->mflags &= ~MFLAG_REFER;
					} else if (!strcasecmp(var, "disable-register") && switch_true(val)) {
						profile->mflags &= ~MFLAG_REGISTER;
					} else if (!strcasecmp(var, "media-option")) {
						if (!strcasecmp(val, "resume-media-on-hold")) {
							profile->media_options |= MEDIA_OPT_MEDIA_ON_HOLD;
						} else if (!strcasecmp(val, "bypass-media-after-att-xfer")) {
							profile->media_options |= MEDIA_OPT_BYPASS_AFTER_ATT_XFER;
						}
					} else if (!strcasecmp(var, "manage-presence")) {
						if (!strcasecmp(val, "passive")) {
							profile->pres_type = PRES_TYPE_PASSIVE;
						
						} else if (switch_true(val)) {
							profile->pres_type = PRES_TYPE_FULL;
						} 
					} else if (!strcasecmp(var, "manage-shared-appearance")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_MANAGE_SHARED_APPEARANCE);
							profile->sla_contact = switch_core_sprintf(profile->pool, "sla-agent");
						}
					} else if (!strcasecmp(var, "disable-srv")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_DISABLE_SRV);
						}
					} else if (!strcasecmp(var, "disable-naptr")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_DISABLE_NAPTR);
						}
					} else if (!strcasecmp(var, "unregister-on-options-fail")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_UNREG_OPTIONS_FAIL);
						}
					} else if (!strcasecmp(var, "require-secure-rtp")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_SECURE);
						}
					} else if (!strcasecmp(var, "multiple-registrations")) {
						if (!strcasecmp(val, "call-id")) {
							sofia_set_pflag(profile, PFLAG_MULTIREG);
						} else if (!strcasecmp(val, "contact") || switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_MULTIREG);
							sofia_set_pflag(profile, PFLAG_MULTIREG_CONTACT);
						} else if (switch_true(val)) {
							sofia_clear_pflag(profile, PFLAG_MULTIREG);
							//sofia_clear_pflag(profile, PFLAG_MULTIREG_CONTACT);
						}
					} else if (!strcasecmp(var, "supress-cng") || !strcasecmp(var, "suppress-cng")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_SUPPRESS_CNG);
							profile->cng_pt = 0;
						}
					} else if (!strcasecmp(var, "NDLB-broken-auth-hash")) {
						if (switch_true(val)) {
							profile->ndlb |= PFLAG_NDLB_BROKEN_AUTH_HASH;
						}
					} else if (!strcasecmp(var, "NDLB-sendrecv-in-session")) {
						if (switch_true(val)) {
							profile->ndlb |= PFLAG_NDLB_SENDRECV_IN_SESSION;
						} else {
							profile->ndlb &= ~PFLAG_NDLB_SENDRECV_IN_SESSION;
						}
					} else if (!strcasecmp(var, "pass-rfc2833")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_PASS_RFC2833);
						}
					} else if (!strcasecmp(var, "rtp-autoflush")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_AUTOFLUSH);
						} else {
							sofia_clear_pflag(profile, PFLAG_AUTOFLUSH);
						}
					} else if (!strcasecmp(var, "rtp-autofix-timing")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_AUTOFIX_TIMING);
						} else {
							sofia_clear_pflag(profile, PFLAG_AUTOFIX_TIMING);
						}
					} else if (!strcasecmp(var, "contact-user")) {
						profile->contact_user = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "nat-options-ping")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_NAT_OPTIONS_PING);
						} else {
							sofia_clear_pflag(profile, PFLAG_NAT_OPTIONS_PING);
						}
					} else if (!strcasecmp(var, "inbound-codec-negotiation")) {
						if (!strcasecmp(val, "greedy")) {
							sofia_set_pflag(profile, PFLAG_GREEDY);
						} else if (!strcasecmp(val, "scrooge")) {
							sofia_set_pflag(profile, PFLAG_GREEDY);
							sofia_set_pflag(profile, PFLAG_SCROOGE);
						} else {
							sofia_clear_pflag(profile, PFLAG_SCROOGE);
							sofia_clear_pflag(profile, PFLAG_GREEDY);
						}
					} else if (!strcasecmp(var, "disable-transcoding")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_DISABLE_TRANSCODING);
						}
					} else if (!strcasecmp(var, "rtp-rewrite-timestamps")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_REWRITE_TIMESTAMPS);
						}
					} else if (!strcasecmp(var, "auth-calls")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_AUTH_CALLS);
						}
					} else if (!strcasecmp(var, "nonce-ttl")) {
						profile->nonce_ttl = atoi(val);
					} else if (!strcasecmp(var, "accept-blind-reg")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_BLIND_REG);
						}
					} else if (!strcasecmp(var, "enable-3pcc")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_3PCC);
						}
						else if (!strcasecmp(val, "proxy")){
							sofia_set_pflag(profile, PFLAG_3PCC_PROXY);
						}
					} else if (!strcasecmp(var, "accept-blind-auth")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_BLIND_AUTH);
						}
					} else if (!strcasecmp(var, "auth-all-packets")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_AUTH_ALL);
						}
					} else if (!strcasecmp(var, "full-id-in-dialplan")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_FULL_ID);
						}
					} else if (!strcasecmp(var, "inbound-reg-force-matching-username")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_CHECKUSER);
						}
					} else if (!strcasecmp(var, "enable-timer")) {
						if (!switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_DISABLE_TIMER);
						}
					} else if (!strcasecmp(var, "minimum-session-expires")) {
						profile->minimum_session_expires = atoi(val);
						/* per RFC 4028: minimum_session_expires must be > 90 */
						if (profile->minimum_session_expires < 90) {
							profile->minimum_session_expires = 90;
						}
					} else if (!strcasecmp(var, "enable-100rel")) {
						if (switch_true(val)) {
							sofia_clear_pflag(profile, PFLAG_DISABLE_100REL);
						}
					} else if (!strcasecmp(var, "bitpacking")) {
						if (!strcasecmp(val, "aal2")) {
							profile->codec_flags = SWITCH_CODEC_FLAG_AAL2;
						}
					} else if (!strcasecmp(var, "username")) {
						profile->username = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "context")) {
						profile->context = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "apply-nat-acl")) {
						if (profile->acl_count < SOFIA_MAX_ACL) {
							if (!profile->extsipip && profile->sipip && switch_check_network_list_ip(profile->sipip, val)) {
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Not adding acl %s because it's the local network\n", val);
							} else {
								profile->nat_acl[profile->nat_acl_count++] = switch_core_strdup(profile->pool, val);
							}
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Max acl records of %d reached\n", SOFIA_MAX_ACL);
						}
					} else if (!strcasecmp(var, "apply-inbound-acl")) {
						if (profile->acl_count < SOFIA_MAX_ACL) {
							profile->acl[profile->acl_count++] = switch_core_strdup(profile->pool, val);
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Max acl records of %d reached\n", SOFIA_MAX_ACL);
						}
					} else if (!strcasecmp(var, "apply-register-acl")) {
						if (profile->reg_acl_count < SOFIA_MAX_ACL) {
							profile->reg_acl[profile->reg_acl_count++] = switch_core_strdup(profile->pool, val);
						} else {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Max acl records of %d reached\n", SOFIA_MAX_ACL);
						}
					} else if (!strcasecmp(var, "alias")) {
						sip_alias_node_t *node;
						if ((node = switch_core_alloc(profile->pool, sizeof(*node)))) {
							if ((node->url = switch_core_strdup(profile->pool, val))) {
								node->next = profile->aliases;
								profile->aliases = node;
							}
						}
					} else if (!strcasecmp(var, "dialplan")) {
						profile->dialplan = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "max-calls")) {
						profile->max_calls = atoi(val);
					} else if (!strcasecmp(var, "codec-prefs")) {
						profile->codec_string = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "challenge-realm")) {
						profile->challenge_realm = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "dtmf-duration")) {
						int dur = atoi(val);
						if (dur > 10 && dur < 8000) {
							profile->dtmf_duration = dur;
						} else {
							profile->dtmf_duration = SWITCH_DEFAULT_DTMF_DURATION;
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Duration out of bounds, using default of %d!\n", 
											  SWITCH_DEFAULT_DTMF_DURATION);
						}

						/*
						 * handle TLS params #1
						 */
					} else if (!strcasecmp(var, "tls")) {
						if (switch_true(val)) {
							sofia_set_pflag(profile, PFLAG_TLS);
						}
					} else if (!strcasecmp(var, "tls-bind-params")) {
						profile->tls_bind_params = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "tls-sip-port")) {
						profile->tls_sip_port = (switch_port_t)atoi(val);
					} else if (!strcasecmp(var, "tls-cert-dir")) {
						profile->tls_cert_dir = switch_core_strdup(profile->pool, val);
					} else if (!strcasecmp(var, "tls-version")) {

						if (!strcasecmp(val, "tlsv1")) {
							profile->tls_version = 1;
						} else {
							profile->tls_version = 0;
						}
					} else if (!strcasecmp(var, "timer-T1")) {
						int v = atoi(val);
						if (v > 0) {
							profile->timer_t1 = v;
						} else {
							profile->timer_t1 = 500;
						}
					} else if (!strcasecmp(var, "timer-T1X64")) {
						int v = atoi(val);
						if (v > 0) {
							profile->timer_t1x64 = v;
						} else {
							profile->timer_t1x64 = 32000;
						}
					} else if (!strcasecmp(var, "timer-T2")) {
						int v = atoi(val);
						if (v > 0) {
							profile->timer_t2 = v;
						} else {
							profile->timer_t2 = 4000;
						}
					} else if (!strcasecmp(var, "timer-T4")) {
						int v = atoi(val);
						if (v > 0) {
							profile->timer_t4 = v;
						} else {
							profile->timer_t4 = 4000;
						}
					}
				}

				if ((!profile->cng_pt) && (!sofia_test_pflag(profile, PFLAG_SUPPRESS_CNG))) {
					profile->cng_pt = SWITCH_RTP_CNG_PAYLOAD;
				}

				if (!profile->sipip) {
					profile->sipip = switch_core_strdup(profile->pool, mod_sofia_globals.guess_ip);
				}

				if (!profile->rtpip) {
					profile->rtpip = switch_core_strdup(profile->pool, mod_sofia_globals.guess_ip);
				}

				if (profile->nonce_ttl < 60) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Setting nonce TTL to 60 seconds\n");
					profile->nonce_ttl = 60;
				}

				if (!profile->username) {
					profile->username = switch_core_strdup(profile->pool, "FreeSWITCH");
				}

				if (!profile->rtpip) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Setting ip to '127.0.0.1'\n");
					profile->rtpip = switch_core_strdup(profile->pool, "127.0.0.1");
				}

				if (!profile->sip_port) {
					profile->sip_port = (switch_port_t)atoi(SOFIA_DEFAULT_PORT);
				}

				if (!profile->dialplan) {
					profile->dialplan = switch_core_strdup(profile->pool, "XML");
				}

				if (!profile->context) {
					profile->context = switch_core_strdup(profile->pool, "default");
				}

				if (!profile->sipdomain) {
					profile->sipdomain = switch_core_strdup(profile->pool, profile->sipip);
				}
				if (profile->extsipip && sofia_test_pflag(profile, PFLAG_AUTO_NAT)) {
					char *ipv6 = strchr(profile->extsipip, ':');
					profile->public_url = switch_core_sprintf(profile->pool,
															  "sip:%s@%s%s%s:%d",
															  profile->contact_user,
															  ipv6 ? "[" : "",
															  profile->extsipip,
															  ipv6 ? "]" : "",
															  profile->sip_port);
				}

				if (profile->extsipip && !sofia_test_pflag(profile, PFLAG_AUTO_NAT)) {
					char *ipv6 = strchr(profile->extsipip, ':');
					profile->url = switch_core_sprintf(profile->pool,
														"sip:%s@%s%s%s:%d",
														profile->contact_user,
														ipv6 ? "[" : "",
														profile->extsipip,
														ipv6 ? "]" : "",
														profile->sip_port);
					profile->bindurl = switch_core_sprintf(profile->pool, "%s;maddr=%s", profile->url, profile->sipip);
				} else {
					char *ipv6 = strchr(profile->sipip, ':');
					profile->url = switch_core_sprintf(profile->pool,
														"sip:%s@%s%s%s:%d",
														profile->contact_user,
														ipv6 ? "[" : "",
														profile->sipip,
														ipv6 ? "]" : "",
														profile->sip_port);
					profile->bindurl = profile->url;
				}

				profile->tcp_contact = switch_core_sprintf(profile->pool, "%s;transport=tcp", profile->url);

				if (sofia_test_pflag(profile, PFLAG_AUTO_NAT)) {
					profile->tcp_public_contact = switch_core_sprintf(profile->pool, "%s;transport=tcp", profile->public_url);
				}

				if (profile->bind_params) {
					char *bindurl = profile->bindurl;
					profile->bindurl = switch_core_sprintf(profile->pool, "%s;%s", bindurl, profile->bind_params);
				}
				
				/*
				 * handle TLS params #2
				 */
				if (sofia_test_pflag(profile, PFLAG_TLS)) {
					if (!profile->tls_sip_port) {
						profile->tls_sip_port = (switch_port_t)atoi(SOFIA_DEFAULT_TLS_PORT);
					}

					if (profile->extsipip && sofia_test_pflag(profile, PFLAG_AUTO_NAT)) {
						char *ipv6 = strchr(profile->extsipip, ':');
						profile->tls_public_url = switch_core_sprintf(profile->pool,
																  "sip:%s@%s%s%s:%d",
																  profile->contact_user,
																  ipv6 ? "[" : "",
																  profile->extsipip,
																  ipv6 ? "]" : "",
																  profile->tls_sip_port);
					}
					
					if (profile->extsipip && !sofia_test_pflag(profile, PFLAG_AUTO_NAT)) {
						char *ipv6 = strchr(profile->extsipip, ':');
						profile->tls_url = 
							switch_core_sprintf(profile->pool,
												"sip:%s@%s%s%s:%d",
												profile->contact_user,
												ipv6 ? "[" : "",
												profile->extsipip, ipv6 ? "]" : "",
												profile->tls_sip_port);
						profile->tls_bindurl =
							switch_core_sprintf(profile->pool,
												"sips:%s@%s%s%s:%d;maddr=%s",
												profile->contact_user,
												ipv6 ? "[" : "",
												profile->extsipip,
												ipv6 ? "]" : "",
												profile->tls_sip_port,
												profile->sipip);
					} else {
						char *ipv6 = strchr(profile->sipip, ':');
						profile->tls_url = 
							switch_core_sprintf(profile->pool,
												"sip:%s@%s%s%s:%d",
												profile->contact_user,
												ipv6 ? "[" : "",
												profile->sipip,
												ipv6 ? "]" : "",
												profile->tls_sip_port);
						profile->tls_bindurl =
							switch_core_sprintf(profile->pool,
												"sips:%s@%s%s%s:%d",
												profile->contact_user,
												ipv6 ? "[" : "",
												profile->sipip,
												ipv6 ? "]" : "",
												profile->tls_sip_port);
					}

					if (profile->tls_bind_params) {
						char *tls_bindurl = profile->tls_bindurl;
						profile->tls_bindurl = switch_core_sprintf(profile->pool, "%s;%s", tls_bindurl, profile->tls_bind_params);
					}

					if (!profile->tls_cert_dir) {
						profile->tls_cert_dir = switch_core_sprintf(profile->pool, "%s/ssl", SWITCH_GLOBAL_dirs.conf_dir);
					}
					profile->tls_contact = switch_core_sprintf(profile->pool, "%s;transport=tls", profile->tls_url);
					if (sofia_test_pflag(profile, PFLAG_AUTO_NAT)) {
						profile->tls_public_contact = switch_core_sprintf(profile->pool, "%s;transport=tls", profile->tls_public_url);
					}
				}
			}
			if (profile) {
				switch_xml_t aliases_tag, alias_tag;

				if ((gateways_tag = switch_xml_child(xprofile, "gateways"))) {
					parse_gateways(profile, gateways_tag);
				}

				if ((domains_tag = switch_xml_child(xprofile, "domains"))) {
					switch_event_t *xml_params;
					switch_event_create(&xml_params, SWITCH_EVENT_REQUEST_PARAMS);
					switch_assert(xml_params);
					switch_event_add_header_string(xml_params, SWITCH_STACK_BOTTOM, "purpose", "gateways");
					switch_event_add_header_string(xml_params, SWITCH_STACK_BOTTOM, "profile", profile->name);
					
					for (domain_tag = switch_xml_child(domains_tag, "domain"); domain_tag; domain_tag = domain_tag->next) {
						switch_xml_t droot, x_domain_tag;
						const char *dname = switch_xml_attr_soft(domain_tag, "name");
						const char *parse = switch_xml_attr_soft(domain_tag, "parse");
						const char *alias = switch_xml_attr_soft(domain_tag, "alias");

						if (!switch_strlen_zero(dname)) {
							if (!strcasecmp(dname, "all")) {
								switch_xml_t xml_root, x_domains;
								if (switch_xml_locate("directory", NULL, NULL, NULL, &xml_root, &x_domains, xml_params, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
									for (x_domain_tag = switch_xml_child(x_domains, "domain"); x_domain_tag; x_domain_tag = x_domain_tag->next) {
										dname = switch_xml_attr_soft(x_domain_tag, "name");
										parse_domain_tag(profile, x_domain_tag, dname, parse, alias);
									}
									switch_xml_free(xml_root);
								}
							} else if (switch_xml_locate_domain(dname, xml_params, &droot, &x_domain_tag) == SWITCH_STATUS_SUCCESS) {
								parse_domain_tag(profile, x_domain_tag, dname, parse, alias);
								switch_xml_free(droot);
							}
						}
					}
					
					switch_event_destroy(&xml_params);
				}

				if ((aliases_tag = switch_xml_child(xprofile, "aliases"))) {
					for (alias_tag = switch_xml_child(aliases_tag, "alias"); alias_tag; alias_tag = alias_tag->next) {
						char *aname = (char *) switch_xml_attr_soft(alias_tag, "name");
						if (!switch_strlen_zero(aname)) {

							if (sofia_glue_add_profile(switch_core_strdup(profile->pool, aname), profile) == SWITCH_STATUS_SUCCESS) {
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Adding Alias [%s] for profile [%s]\n", aname, profile->name);
							} else {
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Adding Alias [%s] for profile [%s] (name in use)\n",
												  aname, profile->name);
							}
						}
					}
				}

				if (profile->sipip) {
					launch_sofia_profile_thread(profile);
					if (profile->odbc_dsn) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Connecting ODBC Profile %s [%s]\n", profile->name, url);
						switch_yield(1000000);
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Started Profile %s [%s]\n", profile->name, url);
					}
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Unable to start Profile %s due to no configured sip-ip\n", profile->name);
				}
				profile = NULL;
			}
			if (profile_found) {
				break;
			}
		}
	}
  done:

	switch_event_destroy(&params);

	if (xml) {
		switch_xml_free(xml);
	}

	if (profile_name && !profile_found) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No Such Profile '%s'\n", profile_name);
		status = SWITCH_STATUS_FALSE;
	}

	return status;
}

static void sofia_handle_sip_r_options(switch_core_session_t *session, int status,
									   char const *phrase,
									   nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip,
									   tagi_t tags[])
{
	sofia_gateway_t *gateway = NULL;

	if (sofia_private && !switch_strlen_zero(sofia_private->gateway_name)) {
		gateway = sofia_reg_find_gateway(sofia_private->gateway_name);
		sofia_private->destroy_me = 1;
	}

	if (gateway) {
		if (status >= 200 && status < 600) {
			if (gateway->state == REG_STATE_FAILED) {
				gateway->state = REG_STATE_UNREGED;
			}
			gateway->status = SOFIA_GATEWAY_UP;
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Ping failed %s\n", gateway->name);
			gateway->status = SOFIA_GATEWAY_DOWN;
			if (gateway->state == REG_STATE_REGED) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Unregister %s\n", gateway->name);
				gateway->state = REG_STATE_FAILED;
			}
		}
		gateway->ping = switch_epoch_time_now(NULL) + gateway->ping_freq;
		sofia_reg_release_gateway(gateway);
		gateway->pinging = 0;
	} else if (sofia_test_pflag(profile, PFLAG_UNREG_OPTIONS_FAIL) && status != 200 && sip && sip->sip_to) {
		char *sql;
		time_t now = switch_epoch_time_now(NULL);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Expire registration '%s@%s' due to options failure\n",
						  sip->sip_to->a_url->url_user, sip->sip_to->a_url->url_host);

		sql = switch_mprintf("update sip_registrations set expires=%ld where sip_user='%s' and sip_host='%s'",
							 (long) now, sip->sip_to->a_url->url_user, sip->sip_to->a_url->url_host);
		sofia_glue_execute_sql(profile, &sql, SWITCH_TRUE);
	}
}

static void sofia_handle_sip_r_invite(switch_core_session_t *session, int status,
									  char const *phrase,
									  nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip,
									  tagi_t tags[])
{
	if (sip && session) {
		switch_channel_t *channel = switch_core_session_get_channel(session);
		const char *uuid;
		switch_core_session_t *other_session;
		private_object_t *tech_pvt = switch_core_session_get_private(session);
		char network_ip[80];
		int network_port = 0;
		switch_caller_profile_t *caller_profile = NULL;

		sofia_glue_get_addr(nua_current_request(nua), network_ip,  sizeof(network_ip), &network_port);

		switch_channel_set_variable(channel, "sip_reply_host", network_ip);
		switch_channel_set_variable_printf(channel, "sip_reply_port", "%d", network_port);
		
		if ((caller_profile = switch_channel_get_caller_profile(channel))) {
			caller_profile->network_addr = switch_core_strdup(caller_profile->pool, network_ip);
		}

		switch_channel_clear_flag(channel, CF_REQ_MEDIA);
		
		if ((status == 180 || status == 183 || status == 200)) { 
			if (sip->sip_user_agent && sip->sip_user_agent->g_string) {
				switch_channel_set_variable(channel, "sip_user_agent", sip->sip_user_agent->g_string);
			} else if (sip->sip_server && sip->sip_server->g_string) {
				switch_channel_set_variable(channel, "sip_user_agent", sip->sip_server->g_string);
			}

			sofia_glue_set_extra_headers(channel, sip, SOFIA_SIP_PROGRESS_HEADER_PREFIX);
		}

		if (switch_channel_test_flag(channel, CF_PROXY_MODE) || switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {

			if (!sofia_test_flag(tech_pvt, TFLAG_SENT_UPDATE)) {
				return;
			}

			sofia_clear_flag_locked(tech_pvt, TFLAG_SENT_UPDATE);

			if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE)) && (other_session = switch_core_session_locate(uuid))) {
				const char *r_sdp = NULL;
				switch_core_session_message_t msg = { 0 };

				if (sip->sip_payload && sip->sip_payload->pl_data &&
					sip->sip_content_type && sip->sip_content_type->c_subtype && switch_stristr("sdp", sip->sip_content_type->c_subtype)) {
					tech_pvt->remote_sdp_str = switch_core_session_strdup(tech_pvt->session, sip->sip_payload->pl_data);
					r_sdp = tech_pvt->remote_sdp_str;
					sofia_glue_tech_proxy_remote_addr(tech_pvt);
				}

				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Passing %d %s to other leg\n", status, phrase);

				msg.message_id = SWITCH_MESSAGE_INDICATE_RESPOND;
				msg.from = __FILE__;
				msg.numeric_arg = status;
				msg.string_arg = switch_core_session_strdup(other_session, phrase);
				if (r_sdp) {
					msg.pointer_arg = switch_core_session_strdup(other_session, r_sdp);
					msg.pointer_arg_size = strlen(r_sdp);
				}
				if (switch_core_session_receive_message(other_session, &msg) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Other leg is not available\n");
					nua_respond(tech_pvt->nh, 403, "Hangup in progress", TAG_END());
				}
				switch_core_session_rwunlock(other_session);
			}
			return;
		}

		if ((status == 180 || status == 183 || status == 200)) {
			const char *astate = "early";
			url_t *from = NULL, *to = NULL, *contact = NULL;

			if (sip->sip_to) {
				to = sip->sip_to->a_url;
			}
			if (sip->sip_from) {
				from = sip->sip_from->a_url;
			}
			if (sip->sip_contact) {
				contact = sip->sip_contact->m_url;
			}

			if (status == 200) {
				astate = "confirmed";
			}

			if (!switch_channel_test_flag(channel, CF_EARLY_MEDIA) && !switch_channel_test_flag(channel, CF_ANSWERED) &&
				!switch_channel_test_flag(channel, CF_RING_READY)) {
				const char *from_user = "", *from_host = "", *to_user = "", *to_host = "", *contact_user = "", *contact_host = "";
				const char *user_agent = "", *call_id = "";
				char *sql = NULL;

				if (sip->sip_user_agent) {
					user_agent = switch_str_nil(sip->sip_user_agent->g_string);
				}

				if (sip->sip_call_id) {
					call_id = switch_str_nil(sip->sip_call_id->i_id);
				}

				if (to) {
					from_user = switch_str_nil(to->url_user);
				}

				if (from) {
					from_host = switch_str_nil(from->url_host);
					to_user = switch_str_nil(from->url_user);
					to_host = switch_str_nil(from->url_host);
				}

				if (contact) {
					contact_user = switch_str_nil(contact->url_user);
					contact_host = switch_str_nil(contact->url_host);
				}

				if (profile->pres_type) {
					sql = switch_mprintf("insert into sip_dialogs "
										 "(call_id,uuid,sip_to_user,sip_to_host,sip_from_user,sip_from_host,contact_user,"
										 "contact_host,state,direction,user_agent,profile_name,hostname) "
										 "values('%q','%q','%q','%q','%q','%q','%q','%q','%q','%q','%q','%q','%q')",
										 call_id,
										 switch_core_session_get_uuid(session),
										 to_user, to_host, from_user, from_host, contact_user, 
										 contact_host, astate, "outbound", user_agent,
										 profile->name, mod_sofia_globals.hostname);

					switch_assert(sql);

					sofia_glue_execute_sql(profile, &sql, SWITCH_TRUE);
				}
			} else if (status == 200 && (profile->pres_type)) {
				char *sql = NULL;
				sql = switch_mprintf("update sip_dialogs set state='%s' where uuid='%s';\n", astate, switch_core_session_get_uuid(session));
				switch_assert(sql);
				sofia_glue_execute_sql(profile, &sql, SWITCH_TRUE);
			}
		}

		if (channel && sip && (status == 300 || status == 302 || status == 305) && switch_channel_test_flag(channel, CF_OUTBOUND)) {
			sip_contact_t * p_contact = sip->sip_contact;
			int i = 0;
			char var_name[80];	
			const char *diversion_header;
			char *full_contact = NULL;
			char *invite_contact;
			const char *br;
			
			if ((br = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))) {
				switch_xml_t root = NULL, domain = NULL;
				switch_core_session_t *a_session;
				switch_channel_t *a_channel;

				const char *sip_redirect_profile, *sip_redirect_context, *sip_redirect_dialplan, *sip_redirect_fork;

				if ((a_session = switch_core_session_locate(br)) && (a_channel = switch_core_session_get_channel(a_session))) { 
					switch_stream_handle_t stream = { 0 };
					char separator[2] = "|";
					char *redirect_dialstring;
					su_home_t *home = su_home_new(sizeof(*home));
					switch_assert(home != NULL);

					SWITCH_STANDARD_STREAM(stream);

					if (!(sip_redirect_profile = switch_channel_get_variable(channel, "sip_redirect_profile"))) {
						sip_redirect_profile = profile->name;
					}
					if (!(sip_redirect_context = switch_channel_get_variable(channel, "sip_redirect_context"))) {
						sip_redirect_context = "redirected";
					}
					if (!(sip_redirect_dialplan = switch_channel_get_variable(channel, "sip_redirect_dialplan"))) {
						sip_redirect_dialplan = "XML";
					}

					sip_redirect_fork = switch_channel_get_variable(channel, "sip_redirect_fork");
					
					if (switch_true(sip_redirect_fork)) {
						*separator = ',';
					}

					for (p_contact = sip->sip_contact; p_contact; p_contact = p_contact->m_next) {
						if (p_contact->m_url) {
							full_contact = sip_header_as_string(home, (void *) sip->sip_contact);
							invite_contact = sofia_glue_strip_uri(full_contact);
							
							switch_snprintf(var_name, sizeof(var_name), "sip_redirect_contact_%d", i);
							switch_channel_set_variable(a_channel, var_name, full_contact);
							
							if (i == 0) {
								switch_channel_set_variable(channel, "sip_redirected_to", full_contact);
								switch_channel_set_variable(a_channel, "sip_redirected_to", full_contact);
							}
							
							if (p_contact->m_url->url_user) {
								switch_snprintf(var_name, sizeof(var_name), "sip_redirect_contact_user_%d", i);
								switch_channel_set_variable(channel, var_name, p_contact->m_url->url_user);
								switch_channel_set_variable(a_channel, var_name, p_contact->m_url->url_user);
							}
							if (p_contact->m_url->url_host) {
								switch_snprintf(var_name, sizeof(var_name), "sip_redirect_contact_host_%d", i);
								switch_channel_set_variable(channel, var_name, p_contact->m_url->url_host);
								switch_channel_set_variable(a_channel, var_name, p_contact->m_url->url_host);
							}
							if (p_contact->m_url->url_params) {
								switch_snprintf(var_name, sizeof(var_name), "sip_redirect_contact_params_%d", i);
								switch_channel_set_variable(channel, var_name, p_contact->m_url->url_params);
								switch_channel_set_variable(a_channel, var_name, p_contact->m_url->url_params);
							}
							
							switch_snprintf(var_name, sizeof(var_name), "sip_redirect_dialstring_%d", i);
							switch_channel_set_variable_printf(channel, var_name, "sofia/%s/%s", sip_redirect_profile, invite_contact);
							switch_channel_set_variable_printf(a_channel, var_name, "sofia/%s/%s", sip_redirect_profile, invite_contact);
							stream.write_function(&stream, "%ssofia/%s/%s", i ? separator : "", sip_redirect_profile, invite_contact);
							free(invite_contact);
							i++;
						}
					}
					
					redirect_dialstring = stream.data;
					
					switch_channel_set_variable(channel, "sip_redirect_dialstring", redirect_dialstring);
					switch_channel_set_variable(a_channel, "sip_redirect_dialstring", redirect_dialstring);
					
					p_contact = sip->sip_contact;
					full_contact = sip_header_as_string(home, (void *) sip->sip_contact);
					
					if ((diversion_header = sofia_glue_get_unknown_header(sip, "diversion"))) {
						switch_channel_set_variable(channel, "sip_redirected_by", diversion_header);
						switch_channel_set_variable(a_channel, "sip_redirected_by", diversion_header);
					}
					
					if (sofia_test_pflag(profile, PFLAG_MANUAL_REDIRECT)) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Redirect: Transfering to %s %s %s\n", 
										  p_contact->m_url->url_user, sip_redirect_dialplan, sip_redirect_context);
						switch_ivr_session_transfer(a_session, p_contact->m_url->url_user, sip_redirect_dialplan, sip_redirect_context);					
					} else if ((!strcmp(profile->sipip, p_contact->m_url->url_host))
							   || (profile->extsipip && !strcmp(profile->extsipip, p_contact->m_url->url_host))
							   || (switch_xml_locate_domain(p_contact->m_url->url_host, NULL, &root, &domain) == SWITCH_STATUS_SUCCESS)) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Redirect: Transfering to %s\n", p_contact->m_url->url_user);
						switch_ivr_session_transfer(a_session, p_contact->m_url->url_user, NULL, NULL);
						switch_xml_free(root);
					} else {
						invite_contact = sofia_glue_strip_uri(full_contact);
						tech_pvt->redirected = switch_core_session_strdup(session, invite_contact);
						free(invite_contact);
					}
					
					if (home) {
						su_home_unref(home);
						home = NULL;
					}

					free(stream.data);
					switch_core_session_rwunlock(a_session);
				}
			} else {
				su_home_t *home = su_home_new(sizeof(*home));
				switch_assert(home != NULL);
				full_contact = sip_header_as_string(home, (void *) sip->sip_contact);
				invite_contact = sofia_glue_strip_uri(full_contact);

				tech_pvt->redirected = switch_core_session_strdup(session, invite_contact);

				free(invite_contact);

				if (home) {
					su_home_unref(home);
					home = NULL;
				}
			}
		}
	}

	if (!session && (status == 180 || status == 183 || status == 200)) {
		/* nevermind */
		nua_handle_bind(nh, NULL);
		nua_handle_destroy(nh);
	}
}


/* Pure black magic, if you can't understand this code you are lucky.........*/
void *SWITCH_THREAD_FUNC media_on_hold_thread_run(switch_thread_t *thread, void *obj)
{
	switch_core_session_t *other_session = NULL, *session = (switch_core_session_t *) obj;
	const char *uuid;
	
	if (switch_core_session_read_lock(session) == SWITCH_STATUS_SUCCESS) {
		switch_channel_t *channel = switch_core_session_get_channel(session);
		private_object_t *tech_pvt = switch_core_session_get_private(session);
		
		if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE)) && (other_session = switch_core_session_locate(uuid))) {
			if (switch_core_session_compare(session, other_session)) {
				sofia_set_flag_locked(tech_pvt, TFLAG_HOLD_LOCK);
				switch_ivr_media(switch_core_session_get_uuid(other_session), SMF_REBRIDGE);
			
				if (tech_pvt->rtp_session) {
					switch_rtp_clear_flag(tech_pvt->rtp_session, SWITCH_RTP_FLAG_AUTOADJ);
				}

				sofia_glue_toggle_hold(tech_pvt, 1);
			}
			switch_core_session_rwunlock(other_session);
		}

		switch_core_session_rwunlock(session);
	}
	
	return NULL;
}

static void launch_media_on_hold(switch_core_session_t *session)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;

	switch_threadattr_create(&thd_attr, switch_core_session_get_pool(session));
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_threadattr_priority_increase(thd_attr);
	switch_thread_create(&thread, thd_attr, media_on_hold_thread_run, session, switch_core_session_get_pool(session));
}

static void sofia_handle_sip_i_state(switch_core_session_t *session, int status,
									 char const *phrase,
									 nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip,
									 tagi_t tags[])
{
	const char *l_sdp = NULL, *r_sdp = NULL;
	int offer_recv = 0, answer_recv = 0, offer_sent = 0, answer_sent = 0;
	int ss_state = nua_callstate_init;
	switch_channel_t *channel = NULL;
	private_object_t *tech_pvt = NULL;
	const char *replaces_str = NULL;
	const char *uuid;
	switch_core_session_t *other_session = NULL;
	switch_channel_t *other_channel = NULL;
	char st[80] = "";

	tl_gets(tags,
			NUTAG_CALLSTATE_REF(ss_state),
			NUTAG_OFFER_RECV_REF(offer_recv),
			NUTAG_ANSWER_RECV_REF(answer_recv),
			NUTAG_OFFER_SENT_REF(offer_sent),
			NUTAG_ANSWER_SENT_REF(answer_sent),
			SIPTAG_REPLACES_STR_REF(replaces_str), SOATAG_LOCAL_SDP_STR_REF(l_sdp), SOATAG_REMOTE_SDP_STR_REF(r_sdp), TAG_END());
	
	if (ss_state == nua_callstate_terminated) {

		if ((status == 300 || status == 302 || status == 305) && session) {
			channel = switch_core_session_get_channel(session);
			tech_pvt = switch_core_session_get_private(session);
			
			if (!tech_pvt || !tech_pvt->nh) {
				goto done;
			}
			

			if (tech_pvt->redirected) {
				sofia_glue_do_invite(session);
				goto done;
			}
		}

		if (sofia_private) {
			sofia_private->destroy_me = 1;
		}
	}

	if (session) {
		channel = switch_core_session_get_channel(session);
		tech_pvt = switch_core_session_get_private(session);

		if (!tech_pvt || !tech_pvt->nh) {
			goto done;
		}

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Channel %s entering state [%s][%d]\n",
						  switch_channel_get_name(channel), nua_callstate_name(ss_state), status);
		
		if (r_sdp) {
			sdp_parser_t *parser;
			sdp_session_t *sdp;

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Remote SDP:\n%s\n", r_sdp);
			tech_pvt->remote_sdp_str = switch_core_session_strdup(session, r_sdp);
			switch_channel_set_variable(channel, SWITCH_R_SDP_VARIABLE, r_sdp);

			if ( sofia_test_flag(tech_pvt, TFLAG_LATE_NEGOTIATION) && (parser = sdp_parse(NULL, r_sdp, (int) strlen(r_sdp), 0))) {
				if ((sdp = sdp_session(parser))) {
					sofia_glue_set_r_sdp_codec_string(channel, (tech_pvt->profile?tech_pvt->profile->codec_string:NULL), sdp);
				}
				sdp_parser_free(parser);
			}
			sofia_glue_pass_sdp(tech_pvt, (char *) r_sdp);
		}
	}

	if (status == 988) {
		goto done;
	}

	if (status == 183 && !r_sdp) {
		status = 180;
	} 
	
	if (status == 180 && r_sdp) {
		status = 183;
	}
	
	if (channel && (status == 180 || status == 183) && switch_channel_test_flag(channel, CF_OUTBOUND)) {
		const char *val;
		if ((val = switch_channel_get_variable(channel, "sip_auto_answer")) && switch_true(val)) {
			nua_notify(nh, NUTAG_NEWSUB(1), NUTAG_SUBSTATE(nua_substate_active), SIPTAG_EVENT_STR("talk"), TAG_END());
		}
	}

  state_process:

	switch ((enum nua_callstate) ss_state) {
	case nua_callstate_terminated:
	case nua_callstate_terminating:
	case nua_callstate_ready:
	case nua_callstate_completed:
	case nua_callstate_received:
	case nua_callstate_proceeding:
		if (!(session && channel && tech_pvt)) goto done;
		break;
	default:
		break;
	}

	switch ((enum nua_callstate) ss_state) {
	case nua_callstate_init:
		break;
	case nua_callstate_authenticating:
		break;
	case nua_callstate_calling:
		break;
	case nua_callstate_proceeding:
		if (status == 180) {
			switch_channel_mark_ring_ready(channel);
			if (!switch_channel_test_flag(channel, CF_GEN_RINGBACK)) {
				if (switch_channel_test_flag(channel, CF_PROXY_MODE)) {
					if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))
						&& (other_session = switch_core_session_locate(uuid))) {
						switch_core_session_message_t msg;
						msg.message_id = SWITCH_MESSAGE_INDICATE_RINGING;
						msg.from = __FILE__;
						switch_core_session_receive_message(other_session, &msg);
						switch_core_session_rwunlock(other_session);
					}

				} else {
					switch_core_session_queue_indication(session, SWITCH_MESSAGE_INDICATE_RINGING);
				}
			}
		}
			
		if (r_sdp) {
			if (switch_channel_test_flag(channel, CF_PROXY_MODE) || switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {
				if (switch_channel_test_flag(channel, CF_PROXY_MEDIA) && !switch_channel_test_flag(tech_pvt->channel, CF_OUTBOUND)) {
					switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "PROXY MEDIA");
				}
				sofia_set_flag_locked(tech_pvt, TFLAG_EARLY_MEDIA);
				switch_channel_mark_pre_answered(channel);
				sofia_set_flag(tech_pvt, TFLAG_SDP);
				if (switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {
					if (sofia_glue_activate_rtp(tech_pvt, 0) != SWITCH_STATUS_SUCCESS) {
						goto done;
					}
				}
				if (!switch_channel_test_flag(channel, CF_GEN_RINGBACK) && (uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))
					&& (other_session = switch_core_session_locate(uuid))) {
					other_channel = switch_core_session_get_channel(other_session);
					if (!switch_channel_get_variable(other_channel, SWITCH_B_SDP_VARIABLE)) {
						switch_channel_set_variable(other_channel, SWITCH_B_SDP_VARIABLE, r_sdp);
					}
					switch_channel_pre_answer(other_channel);
					switch_core_session_rwunlock(other_session);
				}
				goto done;
			} else {
				if (sofia_test_flag(tech_pvt, TFLAG_LATE_NEGOTIATION) && !switch_channel_test_flag(tech_pvt->channel, CF_OUTBOUND)) {
					switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "DELAYED NEGOTIATION");
				} else {
					if (sofia_glue_tech_media(tech_pvt, (char *) r_sdp) != SWITCH_STATUS_SUCCESS) {
						switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "CODEC NEGOTIATION ERROR");
						nua_respond(nh, SIP_488_NOT_ACCEPTABLE, TAG_END());
						switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
					}
				}
				goto done;
			}
		}
		break;
	case nua_callstate_completing:
		nua_ack(nh, TAG_END());
		break;
	case nua_callstate_received:
		if (!sofia_test_flag(tech_pvt, TFLAG_SDP)) {
			if (r_sdp && !sofia_test_flag(tech_pvt, TFLAG_SDP)) {
				if (switch_channel_test_flag(channel, CF_PROXY_MODE)) {
					switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "RECEIVED_NOMEDIA");
					sofia_set_flag_locked(tech_pvt, TFLAG_READY);
					if (switch_channel_get_state(channel) == CS_NEW) {
						switch_channel_set_state(channel, CS_INIT);
					}
					sofia_set_flag(tech_pvt, TFLAG_SDP);
					goto done;
				} else if (switch_channel_test_flag(tech_pvt->channel, CF_PROXY_MEDIA)) {
					switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "PROXY MEDIA");
					sofia_set_flag_locked(tech_pvt, TFLAG_READY);
					if (switch_channel_get_state(channel) == CS_NEW) {
						switch_channel_set_state(channel, CS_INIT);
					}
				} else if (sofia_test_flag(tech_pvt, TFLAG_LATE_NEGOTIATION)) {
					switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "DELAYED NEGOTIATION");
					sofia_set_flag_locked(tech_pvt, TFLAG_READY);
					if (switch_channel_get_state(channel) == CS_NEW) {
						switch_channel_set_state(channel, CS_INIT);
					}
				} else {
					sdp_parser_t *parser;
					sdp_session_t *sdp;
					uint8_t match = 0;

					if (tech_pvt->num_codecs) {
						if ((parser = sdp_parse(NULL, r_sdp, (int) strlen(r_sdp), 0))) {
							if ((sdp = sdp_session(parser))) {
								match = sofia_glue_negotiate_sdp(session, sdp);
							}
							sdp_parser_free(parser);
						}
					}

					if (match) {
						nua_handle_t *bnh;
						sip_replaces_t *replaces;
						su_home_t *home = NULL;
						switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "RECEIVED");
						sofia_set_flag_locked(tech_pvt, TFLAG_READY);
						if (switch_channel_get_state(channel) == CS_NEW) {
							switch_channel_set_state(channel, CS_INIT);
						}
						sofia_set_flag(tech_pvt, TFLAG_SDP);
						if (replaces_str) {
							home = su_home_new(sizeof(*home));
							switch_assert(home != NULL);
							if ((replaces = sip_replaces_make(home, replaces_str))
								&& (bnh = nua_handle_by_replaces(nua, replaces))) {
								sofia_private_t *b_private;

								switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Processing Replaces Attended Transfer\n");
								while (switch_channel_get_state(channel) < CS_EXECUTE) {
									switch_yield(10000);
								}

								if ((b_private = nua_handle_magic(bnh))) {
									const char *br_b = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE);
									char *br_a = b_private->uuid;

									if (br_b) {
										switch_ivr_uuid_bridge(br_a, br_b);
										switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "ATTENDED_TRANSFER");
										sofia_clear_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
										sofia_clear_flag_locked(tech_pvt, TFLAG_HOLD_LOCK);
										switch_channel_hangup(channel, SWITCH_CAUSE_ATTENDED_TRANSFER);
									} else {
										switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "ATTENDED_TRANSFER_ERROR");
										switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
									}
								} else {
									switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "ATTENDED_TRANSFER_ERROR");
									switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
								}
								nua_handle_unref(bnh);
							}
							su_home_unref(home);
							home = NULL;
						}

						goto done;
					}

					switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "NO CODECS");
					switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
				}
			} else {
				if (switch_channel_test_flag(channel, CF_PROXY_MODE) || switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {
					goto done;
				} else {
					if (sofia_test_pflag(profile, PFLAG_3PCC)) {
						switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "RECEIVED_NOSDP");
						sofia_glue_tech_choose_port(tech_pvt, 0);
						sofia_glue_set_local_sdp(tech_pvt, NULL, 0, NULL, 0);
						sofia_set_flag_locked(tech_pvt, TFLAG_3PCC);
						switch_channel_set_state(channel, CS_HIBERNATE);
						nua_respond(tech_pvt->nh, SIP_200_OK,
									SIPTAG_CONTACT_STR(tech_pvt->profile->url),
									SOATAG_USER_SDP_STR(tech_pvt->local_sdp_str),
									SOATAG_REUSE_REJECTED(1),
									SOATAG_ORDERED_USER(1), SOATAG_AUDIO_AUX("cn telephone-event"), 
									TAG_IF(sofia_test_pflag(profile, PFLAG_DISABLE_100REL), NUTAG_INCLUDE_EXTRA_SDP(1)), TAG_END());
					} else if (sofia_test_pflag(profile, PFLAG_3PCC_PROXY)) {
						//3PCC proxy mode delays the 200 OK until the call is answered
						switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "RECEIVED_NOSDP");
						sofia_set_flag_locked(tech_pvt, TFLAG_3PCC);
						sofia_glue_tech_choose_port(tech_pvt, 0);
						sofia_glue_set_local_sdp(tech_pvt, NULL, 0, NULL, 0);
						switch_channel_set_flag(channel, TFLAG_LATE_NEGOTIATION);
						//Moves into CS_INIT so call moves forward into the dialplan
						switch_channel_set_state(channel, CS_INIT);
					} else {
						switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "3PCC DISABLED");
						switch_channel_hangup(channel, SWITCH_CAUSE_MANDATORY_IE_MISSING);
					}
					goto done;
				}
			}

		} else if (tech_pvt && sofia_test_flag(tech_pvt, TFLAG_SDP) && !r_sdp) {
			nua_respond(tech_pvt->nh, SIP_200_OK, TAG_END());
			sofia_set_flag_locked(tech_pvt, TFLAG_NOSDP_REINVITE);
			goto done;
		} else {
			ss_state = nua_callstate_completed;
			goto state_process;
		}

		break;
	case nua_callstate_early:
		break;
	case nua_callstate_completed:
		if (r_sdp) {
			sdp_parser_t *parser;
			sdp_session_t *sdp;
			uint8_t match = 0, is_ok = 1;
			tech_pvt->hold_laps = 0;

			if (r_sdp) {
				const char *var;
				
				if ((var = switch_channel_get_variable(channel, "sip_ignore_reinvites")) && switch_true(var)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Ignoring Re-invite\n");
					nua_respond(tech_pvt->nh, SIP_200_OK, TAG_END());
					goto done;
				}

				if (switch_channel_test_flag(channel, CF_PROXY_MODE) || switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {
					
					if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))
						&& (other_session = switch_core_session_locate(uuid))) {
						switch_core_session_message_t msg = { 0 };
						
						if (profile->media_options & MEDIA_OPT_MEDIA_ON_HOLD) {
							tech_pvt->hold_laps = 1;
							switch_channel_set_variable(channel, SWITCH_R_SDP_VARIABLE, r_sdp);
							switch_channel_clear_flag(channel, CF_PROXY_MODE);
							sofia_glue_tech_set_local_sdp(tech_pvt, NULL, SWITCH_FALSE);
							
							if (!switch_channel_media_ready(channel)) {
								if (!switch_channel_test_flag(tech_pvt->channel, CF_OUTBOUND)) {
									//const char *r_sdp = switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);

									tech_pvt->num_codecs = 0;
									sofia_glue_tech_prepare_codecs(tech_pvt);
									if (sofia_glue_tech_media(tech_pvt, r_sdp) != SWITCH_STATUS_SUCCESS) {
										switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "CODEC NEGOTIATION ERROR");
										status = SWITCH_STATUS_FALSE;
										goto done;
									}
								}
							}

							if (!switch_rtp_ready(tech_pvt->rtp_session)) {
								sofia_glue_tech_prepare_codecs(tech_pvt);
								if ((status = sofia_glue_tech_choose_port(tech_pvt, 0)) != SWITCH_STATUS_SUCCESS) {
									switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
									goto done;
								}
							}
							sofia_glue_set_local_sdp(tech_pvt, NULL, 0, NULL, 1);
							
							nua_respond(tech_pvt->nh, SIP_200_OK,
										SIPTAG_CONTACT_STR(tech_pvt->reply_contact),
										SOATAG_USER_SDP_STR(tech_pvt->local_sdp_str),
										SOATAG_REUSE_REJECTED(1),
										SOATAG_ORDERED_USER(1), SOATAG_AUDIO_AUX("cn telephone-event"), 
										TAG_IF(sofia_test_pflag(profile, PFLAG_DISABLE_100REL), NUTAG_INCLUDE_EXTRA_SDP(1)), TAG_END());
							launch_media_on_hold(session);

							switch_core_session_rwunlock(other_session);
							goto done;
						}
						
						msg.message_id = SWITCH_MESSAGE_INDICATE_MEDIA_REDIRECT;
						msg.from = __FILE__;
						msg.string_arg = (char *) r_sdp;
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Passing SDP to other leg.\n%s\n", r_sdp);
						
						if (sofia_test_flag(tech_pvt, TFLAG_SIP_HOLD)) {
							if (!switch_stristr("sendonly", r_sdp)) {
								sofia_clear_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
								switch_channel_presence(tech_pvt->channel, "unknown", "unhold", NULL);
							}
						} else if (switch_stristr("sendonly", r_sdp)) {
							sofia_set_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
							switch_channel_presence(tech_pvt->channel, "unknown", "hold", NULL);
						}
					

						if (switch_core_session_receive_message(other_session, &msg) != SWITCH_STATUS_SUCCESS) {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Other leg is not available\n");
							nua_respond(tech_pvt->nh, 403, "Hangup in progress", TAG_END());
						}
						switch_core_session_rwunlock(other_session);
					} else {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Re-INVITE to a no-media channel that is not in a bridge.\n");
						is_ok = 0;
						switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
					}
					goto done;
				} else {
					if (tech_pvt->num_codecs) {
						if ((parser = sdp_parse(NULL, r_sdp, (int) strlen(r_sdp), 0))) {
							if ((sdp = sdp_session(parser))) {
								match = sofia_glue_negotiate_sdp(session, sdp);
							}
							sdp_parser_free(parser);
						}
					}
					if (match) {
						if (sofia_glue_tech_choose_port(tech_pvt, 0) != SWITCH_STATUS_SUCCESS) {
							goto done;
						}
						sofia_glue_set_local_sdp(tech_pvt, NULL, 0, NULL, 0);
						sofia_set_flag_locked(tech_pvt, TFLAG_REINVITE);
						if (sofia_glue_activate_rtp(tech_pvt, 0) != SWITCH_STATUS_SUCCESS) {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Reinvite RTP Error!\n");
							is_ok = 0;
							switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
						}
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Processing Reinvite\n");
					} else {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Reinvite Codec Error!\n");
						is_ok = 0;
					}
				}
				
				if (is_ok) {
					if (tech_pvt->local_crypto_key) {
						sofia_glue_set_local_sdp(tech_pvt, NULL, 0, NULL, 0);
					}
					nua_respond(tech_pvt->nh, SIP_200_OK,
								SIPTAG_CONTACT_STR(tech_pvt->reply_contact),
								SOATAG_USER_SDP_STR(tech_pvt->local_sdp_str),
								SOATAG_REUSE_REJECTED(1),
								SOATAG_ORDERED_USER(1), SOATAG_AUDIO_AUX("cn telephone-event"), 
								TAG_IF(sofia_test_pflag(profile, PFLAG_DISABLE_100REL), NUTAG_INCLUDE_EXTRA_SDP(1)), TAG_END());
				} else {
					nua_respond(tech_pvt->nh, SIP_488_NOT_ACCEPTABLE, TAG_END());
				}
			}
		}
		break;
	case nua_callstate_ready:

		if (r_sdp && sofia_test_flag(tech_pvt, TFLAG_NOSDP_REINVITE)) {
			sdp_parser_t *parser;
			sdp_session_t *sdp;
			uint8_t match = 0;
			int is_ok = 1;

			sofia_clear_flag_locked(tech_pvt, TFLAG_NOSDP_REINVITE);

			if (tech_pvt->num_codecs) {
				if ((parser = sdp_parse(NULL, r_sdp, (int) strlen(r_sdp), 0))) {
					if ((sdp = sdp_session(parser))) {
						match = sofia_glue_negotiate_sdp(session, sdp);
					}
					sdp_parser_free(parser);
				}
			}

			if (match) {
				sofia_set_flag_locked(tech_pvt, TFLAG_REINVITE);
				if (sofia_glue_activate_rtp(tech_pvt, 0) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "RTP Error!\n");
					switch_channel_set_variable(tech_pvt->channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "RTP ERROR");
					is_ok = 0;
				}
				sofia_clear_flag_locked(tech_pvt, TFLAG_REINVITE);
			} else {
				switch_channel_set_variable(tech_pvt->channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "CODEC NEGOTIATION ERROR");
				is_ok = 0;
			}

			if (!is_ok) {
				nua_respond(nh, SIP_488_NOT_ACCEPTABLE, TAG_END());
				switch_channel_hangup(tech_pvt->channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
			}

			goto done;
		}

		if (channel) {
			switch_channel_clear_flag(channel, CF_REQ_MEDIA);
		}
		if (tech_pvt && nh == tech_pvt->nh2) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Cheater Reinvite!\n");
			sofia_set_flag_locked(tech_pvt, TFLAG_REINVITE);
			tech_pvt->nh = tech_pvt->nh2;
			tech_pvt->nh2 = NULL;
			if (sofia_glue_tech_choose_port(tech_pvt, 0) == SWITCH_STATUS_SUCCESS) {
				if (sofia_glue_activate_rtp(tech_pvt, 0) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cheater Reinvite RTP Error!\n");
					switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
				}
			}
			goto done;
		}
		
		if (channel) {
            if (sofia_test_flag(tech_pvt, TFLAG_EARLY_MEDIA)) {
                sofia_set_flag_locked(tech_pvt, TFLAG_ANS);
                sofia_set_flag(tech_pvt, TFLAG_SDP);
                switch_channel_mark_answered(channel);
				if (switch_channel_test_flag(channel, CF_PROXY_MODE) || switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {
					if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))
						&& (other_session = switch_core_session_locate(uuid))) {
						other_channel = switch_core_session_get_channel(other_session);
						switch_channel_answer(other_channel);
						switch_core_session_rwunlock(other_session);
					}
				}
                goto done;
            }
			
			if (!r_sdp && !sofia_test_flag(tech_pvt, TFLAG_SDP)) {
				r_sdp = (const char *) switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);
			}

			if (r_sdp && !sofia_test_flag(tech_pvt, TFLAG_SDP)) {
				if (switch_channel_test_flag(channel, CF_PROXY_MODE) || switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {
					sofia_set_flag_locked(tech_pvt, TFLAG_ANS);
					sofia_set_flag_locked(tech_pvt, TFLAG_SDP);
					switch_channel_mark_answered(channel);
					if (switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {
						if (sofia_glue_activate_rtp(tech_pvt, 0) != SWITCH_STATUS_SUCCESS) {
							goto done;
						}
					}

					if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))
						&& (other_session = switch_core_session_locate(uuid))) {
						other_channel = switch_core_session_get_channel(other_session);
						if (!switch_channel_get_variable(other_channel, SWITCH_B_SDP_VARIABLE)) {
							switch_channel_set_variable(other_channel, SWITCH_B_SDP_VARIABLE, r_sdp);
						}
						switch_channel_answer(other_channel);
						switch_core_session_rwunlock(other_session);
					}
					goto done;
				} else {
					sdp_parser_t *parser;
					sdp_session_t *sdp;
					uint8_t match = 0;

					if (tech_pvt->num_codecs) {
						if ((parser = sdp_parse(NULL, r_sdp, (int) strlen(r_sdp), 0))) {
							if ((sdp = sdp_session(parser))) {
								match = sofia_glue_negotiate_sdp(session, sdp);
							}
							sdp_parser_free(parser);
						}
					}

					if (match) {
						sofia_set_flag_locked(tech_pvt, TFLAG_ANS);
						if (sofia_glue_tech_choose_port(tech_pvt, 0) == SWITCH_STATUS_SUCCESS) {
							if (sofia_glue_activate_rtp(tech_pvt, 0) == SWITCH_STATUS_SUCCESS) {
								switch_channel_mark_answered(channel);
							} else {
								switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "RTP Error!\n");
								switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
							}

							if (sofia_test_flag(tech_pvt, TFLAG_3PCC)) {
								/* Check if we are in 3PCC proxy mode, if so then set the flag to indicate we received the ack */
								if (sofia_test_pflag(profile, PFLAG_3PCC_PROXY )) {
									switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "3PCC-PROXY, Got my ACK\n");
									sofia_set_flag(tech_pvt, TFLAG_3PCC_HAS_ACK);
								} else if (switch_channel_get_state(channel) == CS_HIBERNATE) {
									sofia_set_flag_locked(tech_pvt, TFLAG_READY);
									switch_channel_set_state(channel, CS_INIT);
									sofia_set_flag(tech_pvt, TFLAG_SDP);
								}
							}
							goto done;
						}
					}

					switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "NO CODECS");
					switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
				}
			}
		}

		break;
	case nua_callstate_terminating:
		if (status == 488 || switch_channel_get_state(channel) == CS_HIBERNATE) {
			tech_pvt->q850_cause = SWITCH_CAUSE_MANDATORY_IE_MISSING;
		}
	case nua_callstate_terminated:
		sofia_set_flag_locked(tech_pvt, TFLAG_BYE);
		if (sofia_test_flag(tech_pvt, TFLAG_NOHUP)) {
			sofia_clear_flag_locked(tech_pvt, TFLAG_NOHUP);
		} else if (switch_channel_up(channel)) {
			int cause;
			if (tech_pvt->q850_cause) {
				cause = tech_pvt->q850_cause;
			} else {
				cause = sofia_glue_sip_cause_to_freeswitch(status);
			}
			if (status) {
				switch_snprintf(st, sizeof(st), "%d", status);
				switch_channel_set_variable(channel, "sip_term_status", st);
				switch_snprintf(st, sizeof(st), "sip:%d", status);
				switch_channel_set_variable_partner(channel, SWITCH_PROTO_SPECIFIC_HANGUP_CAUSE_VARIABLE, st);
				switch_channel_set_variable(channel, SWITCH_PROTO_SPECIFIC_HANGUP_CAUSE_VARIABLE, st);
				if (phrase) {
					switch_channel_set_variable_partner(channel, "sip_hangup_phrase", phrase);
				}
				sofia_glue_set_extra_headers(channel, sip, SOFIA_SIP_BYE_HEADER_PREFIX);
			}
			switch_snprintf(st, sizeof(st), "%d", cause);
			switch_channel_set_variable(channel, "sip_term_cause", st);
			switch_channel_hangup(channel, cause);
		}
		

		if (ss_state == nua_callstate_terminated) {
			if (tech_pvt->sofia_private) {
				tech_pvt->sofia_private = NULL;
			}
			
			tech_pvt->nh = NULL;
	
			if (nh) {
				nua_handle_bind(nh, NULL);
				nua_handle_destroy(nh);
			}
		}
		
		break;
	}

  done:
	return;
}

typedef struct {
	char *exten;
	char *event;
	char *reply_uuid;
	char *bridge_to_uuid;
	switch_memory_pool_t *pool;
} nightmare_xfer_helper_t;

void *SWITCH_THREAD_FUNC nightmare_xfer_thread_run(switch_thread_t *thread, void *obj)
{
	nightmare_xfer_helper_t *nhelper = (nightmare_xfer_helper_t *) obj;
	switch_memory_pool_t *pool;
	switch_status_t status;
	switch_core_session_t *session, *a_session;
	
	if ((a_session = switch_core_session_locate(nhelper->bridge_to_uuid))) {
		switch_core_session_t *tsession;
		switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
		uint32_t timeout = 60;
		char *tuuid_str;

		if ((session = switch_core_session_locate(nhelper->reply_uuid))) {
			private_object_t *tech_pvt = switch_core_session_get_private(session);
			switch_channel_t *channel_a = switch_core_session_get_channel(session);

			status = switch_ivr_originate(a_session, &tsession, &cause, nhelper->exten, timeout, NULL, NULL, NULL, NULL, NULL, SOF_NONE);
							
			if ((switch_channel_up(channel_a))) {
				
				if (status != SWITCH_STATUS_SUCCESS || cause != SWITCH_CAUSE_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Cannot Create Outgoing Channel! [%s]\n", nhelper->exten);
					nua_notify(tech_pvt->nh, NUTAG_NEWSUB(1), SIPTAG_CONTENT_TYPE_STR("messsage/sipfrag"),
							   NUTAG_SUBSTATE(nua_substate_terminated),
							   SIPTAG_PAYLOAD_STR("SIP/2.0 403 Forbidden"), SIPTAG_EVENT_STR(nhelper->event), TAG_END());
					status = SWITCH_STATUS_FALSE;
				} else {
					tuuid_str = switch_core_session_get_uuid(tsession);
					switch_ivr_uuid_bridge(nhelper->bridge_to_uuid, tuuid_str);
					switch_channel_set_variable(channel_a, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "ATTENDED_TRANSFER");
					sofia_set_flag_locked(tech_pvt, TFLAG_BYE);
					nua_notify(tech_pvt->nh, NUTAG_NEWSUB(1), SIPTAG_CONTENT_TYPE_STR("message/sipfrag"),
							   NUTAG_SUBSTATE(nua_substate_terminated), SIPTAG_PAYLOAD_STR("SIP/2.0 200 OK"), SIPTAG_EVENT_STR(nhelper->event), TAG_END());
					switch_core_session_rwunlock(tsession);
				}
			}
			switch_core_session_rwunlock(session);
		}

		switch_core_session_rwunlock(a_session);
	}

	pool = nhelper->pool;
	switch_core_destroy_memory_pool(&pool);

	return NULL;
}

static void launch_nightmare_xfer(nightmare_xfer_helper_t *nhelper)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;

	switch_threadattr_create(&thd_attr, nhelper->pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_threadattr_priority_increase(thd_attr);
	switch_thread_create(&thread, thd_attr, nightmare_xfer_thread_run, nhelper, nhelper->pool);
}

/*---------------------------------------*/

void sofia_handle_sip_i_refer(nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, switch_core_session_t *session, sip_t const *sip, tagi_t tags[])
{
	/* Incoming refer */
	sip_from_t const *from;
	sip_to_t const *to;
	sip_refer_to_t const *refer_to;
	private_object_t *tech_pvt = switch_core_session_get_private(session);
	char *etmp = NULL, *exten = NULL;
	switch_channel_t *channel_a = switch_core_session_get_channel(session);
	switch_channel_t *channel_b = NULL;
	su_home_t *home = NULL;
	char *full_ref_by = NULL;
	char *full_ref_to = NULL;
	nightmare_xfer_helper_t *nightmare_xfer_helper;
	switch_memory_pool_t *npool;

	if (!(profile->mflags & MFLAG_REFER)) {
		nua_respond(nh, SIP_403_FORBIDDEN, NUTAG_WITH_THIS(nua), TAG_END());
		goto done;
	}

	if (!sip->sip_cseq || !(etmp = switch_mprintf("refer;id=%u", sip->sip_cseq->cs_seq))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Memory Error!\n");
		goto done;
	}

	from = sip->sip_from;
	to = sip->sip_to;

	home = su_home_new(sizeof(*home));
	switch_assert(home != NULL);

	if (sip->sip_referred_by) {
		full_ref_by = sip_header_as_string(home, (void *) sip->sip_referred_by);
	}

	if ((refer_to = sip->sip_refer_to)) {
		char *rep;
		full_ref_to = sip_header_as_string(home, (void *) sip->sip_refer_to);

		if (sofia_test_pflag(profile, PFLAG_FULL_ID)) {
			exten = switch_core_session_sprintf(session, "%s@%s", (char *) refer_to->r_url->url_user, (char *) refer_to->r_url->url_host);
		} else {
			exten = (char *) refer_to->r_url->url_user;
		}

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Process REFER to [%s@%s]\n", exten, (char *) refer_to->r_url->url_host);

		if (refer_to->r_url->url_headers && (rep = (char *)switch_stristr("Replaces=", refer_to->r_url->url_headers))) {
			sip_replaces_t *replaces;
			nua_handle_t *bnh;

			if (switch_channel_test_flag(channel_a, CF_PROXY_MODE)) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot Attended Transfer BYPASS MEDIA CALLS!\n");
				switch_channel_set_variable(channel_a, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "ATTENDED_TRANSFER_ERROR");
				nua_notify(tech_pvt->nh, NUTAG_NEWSUB(1), SIPTAG_CONTENT_TYPE_STR("message/sipfrag"),
						   NUTAG_SUBSTATE(nua_substate_terminated), SIPTAG_PAYLOAD_STR("SIP/2.0 403 Forbidden"), SIPTAG_EVENT_STR(etmp), TAG_END());
				goto done;
			}
			
			if (rep) {
				const char *br_a = NULL, *br_b = NULL;
				char *buf;
				char *p;

				rep = switch_core_session_strdup(session, rep + 9);

				if ((p = strchr(rep, ';'))) {
					*p = '\0';
				}
				
				if ((buf = switch_core_session_alloc(session, strlen(rep) + 1))) {
					rep = url_unescape(buf, (const char *) rep);
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Replaces: [%s]\n", rep);
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Memory Error!\n");
					goto done;
				}

				if ((replaces = sip_replaces_make(home, rep))
					&& (bnh = nua_handle_by_replaces(nua, replaces))) {
					sofia_private_t *b_private = NULL;
					private_object_t *b_tech_pvt = NULL;
					switch_core_session_t *b_session = NULL;


					switch_channel_set_variable(channel_a, SOFIA_REPLACES_HEADER, rep);
					if ((b_private = nua_handle_magic(bnh))) {
						if (!(b_session = switch_core_session_locate(b_private->uuid))) {
							goto done;
						}
						b_tech_pvt = (private_object_t *) switch_core_session_get_private(b_session);
						channel_b = switch_core_session_get_channel(b_session);

						br_a = switch_channel_get_variable(channel_a, SWITCH_SIGNAL_BOND_VARIABLE);
						br_b = switch_channel_get_variable(channel_b, SWITCH_SIGNAL_BOND_VARIABLE);

						if (br_a && br_b) {
							switch_core_session_t *new_b_session = NULL, *a_session = NULL, *tmp = NULL;
						
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Attended Transfer [%s][%s]\n", 
											  switch_str_nil(br_a),
											  switch_str_nil(br_b));

							if ((profile->media_options & MEDIA_OPT_BYPASS_AFTER_ATT_XFER) && (tmp = switch_core_session_locate(br_b))) {
								switch_channel_t *tchannel = switch_core_session_get_channel(tmp);
								switch_channel_set_variable(tchannel, SWITCH_BYPASS_MEDIA_AFTER_BRIDGE_VARIABLE, "true");
								switch_core_session_rwunlock(tmp);
							}
							
							switch_ivr_uuid_bridge(br_b, br_a);
							switch_channel_set_variable(channel_b, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "ATTENDED_TRANSFER");
							nua_notify(tech_pvt->nh, NUTAG_NEWSUB(1), SIPTAG_CONTENT_TYPE_STR("message/sipfrag"),
									   NUTAG_SUBSTATE(nua_substate_terminated), SIPTAG_PAYLOAD_STR("SIP/2.0 200 OK"), SIPTAG_EVENT_STR(etmp), TAG_END());

							sofia_clear_flag_locked(b_tech_pvt, TFLAG_SIP_HOLD);
							sofia_clear_flag_locked(tech_pvt, TFLAG_HOLD_LOCK);
							switch_channel_set_variable(switch_core_session_get_channel(b_session), "park_timeout", "2");
							switch_channel_set_state(switch_core_session_get_channel(b_session), CS_PARK);

							new_b_session = switch_core_session_locate(br_b);
							a_session = switch_core_session_locate(br_a);
							sofia_info_send_sipfrag(a_session, new_b_session);

							if (new_b_session) {
								switch_core_session_rwunlock(new_b_session);
							}

							if (a_session) {
								switch_core_session_rwunlock(a_session);
							}
						} else {
							if (!br_a && !br_b) {
								switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Cannot transfer channels that are not in a bridge.\n");
								nua_notify(tech_pvt->nh, NUTAG_NEWSUB(1), SIPTAG_CONTENT_TYPE_STR("message/sipfrag"),
										   NUTAG_SUBSTATE(nua_substate_terminated),
										   SIPTAG_PAYLOAD_STR("SIP/2.0 403 Forbidden"), SIPTAG_EVENT_STR(etmp), TAG_END());
							} else {
								switch_core_session_t *t_session;
								switch_channel_t *hup_channel;
								const char *ext;

								if (br_a && !br_b) {
									t_session = switch_core_session_locate(br_a);
									hup_channel = channel_b;
								} else {
									private_object_t *h_tech_pvt = (private_object_t *) switch_core_session_get_private(b_session);
									t_session = switch_core_session_locate(br_b);
									hup_channel = channel_a;
									sofia_clear_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
									sofia_clear_flag_locked(h_tech_pvt, TFLAG_SIP_HOLD);
									switch_channel_hangup(channel_b, SWITCH_CAUSE_ATTENDED_TRANSFER);
								}

								if (t_session) {
									switch_channel_t *t_channel = switch_core_session_get_channel(t_session);
									ext = switch_channel_get_variable(hup_channel, "destination_number");

									if (!switch_strlen_zero(full_ref_by)) {
										switch_channel_set_variable(t_channel, SOFIA_SIP_HEADER_PREFIX "Referred-By", full_ref_by);
									}

									if (!switch_strlen_zero(full_ref_to)) {
										switch_channel_set_variable(t_channel, SOFIA_REFER_TO_VARIABLE, full_ref_to);
									}

									switch_ivr_session_transfer(t_session, ext, NULL, NULL);
									nua_notify(tech_pvt->nh,
											   NUTAG_NEWSUB(1),
											   SIPTAG_CONTENT_TYPE_STR("message/sipfrag"),
											   NUTAG_SUBSTATE(nua_substate_terminated),
											   SIPTAG_PAYLOAD_STR("SIP/2.0 200 OK"), SIPTAG_EVENT_STR(etmp), TAG_END());
									switch_core_session_rwunlock(t_session);
									switch_channel_hangup(hup_channel, SWITCH_CAUSE_ATTENDED_TRANSFER);
								} else {
									switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Session to transfer to not found.\n");
									nua_notify(tech_pvt->nh, NUTAG_NEWSUB(1), SIPTAG_CONTENT_TYPE_STR("message/sipfrag"),
											   NUTAG_SUBSTATE(nua_substate_terminated),
											   SIPTAG_PAYLOAD_STR("SIP/2.0 403 Forbidden"), SIPTAG_EVENT_STR(etmp), TAG_END());
								}
							}
						}
						if (b_session) {
							switch_core_session_rwunlock(b_session);
						}
					}
					nua_handle_unref(bnh);
				} else {		/* the other channel is on a different box, we have to go find them */
					if (exten && (br_a = switch_channel_get_variable(channel_a, SWITCH_SIGNAL_BOND_VARIABLE))) {
						switch_core_session_t *a_session;
						switch_channel_t *channel = switch_core_session_get_channel(session);

						if ((a_session = switch_core_session_locate(br_a))) {
							const char *port = NULL;
							char *param_string = "";
							int count = 0, bytes = 0;

							if (refer_to && refer_to->r_url && refer_to->r_url->url_port) {
								port = refer_to->r_url->url_port;
							}

							if (switch_strlen_zero(port)) {
								port = "5060";
							}
							
							channel = switch_core_session_get_channel(a_session);
							
							if (refer_to->r_params) {
								for (count = 0; refer_to->r_params[count] ; count++) {
									bytes += strlen(refer_to->r_params[count]) + 1;
								}

								if (bytes) {
									bytes += 2;

									param_string = switch_core_session_alloc(session, bytes);
									*param_string = ';';
									for (count = 0; refer_to->r_params[count] ; count++) {
										switch_snprintf(param_string + strlen(param_string), bytes - strlen(param_string), "%s;", refer_to->r_params[count]);
									}
								
									if (end_of(param_string) == ';') {
										end_of(param_string) = '\0';
									}
								}
							}

							exten = switch_core_session_sprintf(session, "sofia/%s/sip:%s@%s", 
																profile->name, refer_to->r_url->url_user, 
																refer_to->r_url->url_host);

							switch_core_new_memory_pool(&npool);
							nightmare_xfer_helper = switch_core_alloc(npool, sizeof(*nightmare_xfer_helper));
							nightmare_xfer_helper->exten = switch_core_strdup(npool, exten);
							nightmare_xfer_helper->event = switch_core_strdup(npool, etmp);
							nightmare_xfer_helper->reply_uuid = switch_core_strdup(npool, switch_core_session_get_uuid(session));
							nightmare_xfer_helper->bridge_to_uuid = switch_core_strdup(npool, br_a);
							nightmare_xfer_helper->pool = npool;

							switch_channel_set_variable(channel, SOFIA_REPLACES_HEADER, rep);

							if (!switch_strlen_zero(full_ref_by)) {
								switch_channel_set_variable(channel, SOFIA_SIP_HEADER_PREFIX "Referred-By", full_ref_by);
							}

							if (!switch_strlen_zero(full_ref_to)) {
								switch_channel_set_variable(channel, SOFIA_REFER_TO_VARIABLE, full_ref_to);
							}

							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Good Luck, you'll need it......\n");
							launch_nightmare_xfer(nightmare_xfer_helper);
							
							switch_core_session_rwunlock(a_session);

						} else {
							goto error;
						}

					} else {
					  error:
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid Transfer! [%s]\n", br_a);
						switch_channel_set_variable(channel_a, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "ATTENDED_TRANSFER_ERROR");
						nua_notify(tech_pvt->nh, NUTAG_NEWSUB(1), SIPTAG_CONTENT_TYPE_STR("message/sipfrag"),
								   NUTAG_SUBSTATE(nua_substate_terminated), SIPTAG_PAYLOAD_STR("SIP/2.0 403 Forbidden"), SIPTAG_EVENT_STR(etmp),
								   TAG_END());
					}
				}
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot parse Replaces!\n");
			}
			goto done;
		}

	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Missing Refer-To\n");
		goto done;
	}

	if (exten) {
		switch_channel_t *channel = switch_core_session_get_channel(session);
		const char *br;

		if ((br = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))) {
			switch_core_session_t *b_session;

			if ((b_session = switch_core_session_locate(br))) {
				switch_channel_t *b_channel = switch_core_session_get_channel(b_session);
				switch_channel_set_variable(channel, "transfer_fallback_extension", from->a_user);
				if (!switch_strlen_zero(full_ref_by)) {
					switch_channel_set_variable(b_channel, SOFIA_SIP_HEADER_PREFIX "Referred-By", full_ref_by);
				}
				if (!switch_strlen_zero(full_ref_to)) {
					switch_channel_set_variable(b_channel, SOFIA_REFER_TO_VARIABLE, full_ref_to);
				}

				switch_ivr_session_transfer(b_session, exten, NULL, NULL);
				switch_core_session_rwunlock(b_session);
			}

			switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "BLIND_TRANSFER");
			nua_notify(tech_pvt->nh, NUTAG_NEWSUB(1), SIPTAG_CONTENT_TYPE_STR("message/sipfrag"),
					   NUTAG_SUBSTATE(nua_substate_terminated), SIPTAG_PAYLOAD_STR("SIP/2.0 200 OK"), SIPTAG_EVENT_STR(etmp), TAG_END());

		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot Blind Transfer 1 Legged calls\n");
			switch_channel_set_variable(channel_a, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "ATTENDED_TRANSFER_ERROR");
			nua_notify(tech_pvt->nh, NUTAG_NEWSUB(1), SIPTAG_CONTENT_TYPE_STR("message/sipfrag"),
					   NUTAG_SUBSTATE(nua_substate_terminated), SIPTAG_PAYLOAD_STR("SIP/2.0 403 Forbidden"), SIPTAG_EVENT_STR(etmp), TAG_END());
		}
	}

  done:
	if (home) {
		su_home_unref(home);
		home = NULL;
	}

	if (etmp) {
		switch_safe_free(etmp);
	}
}

void sofia_handle_sip_i_info(nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, switch_core_session_t *session, sip_t const *sip, tagi_t tags[])
{
	/* placeholder for string searching */
	const char *signal_ptr;
	const char *rec_header;
	const char *clientcode_header;
	switch_dtmf_t dtmf = { 0, switch_core_default_dtmf_duration(0) };
	switch_event_t *event;

	if (session) {
		/* Get the channel */
		switch_channel_t *channel = switch_core_session_get_channel(session);

		/* Barf if we didn't get our private */
		assert(switch_core_session_get_private(session));

		if (sip && sip->sip_content_type && sip->sip_content_type->c_type && sip->sip_content_type->c_subtype &&
			sip->sip_payload && sip->sip_payload->pl_data) {
			if (!strncasecmp(sip->sip_content_type->c_type, "application", 11) && !strcasecmp(sip->sip_content_type->c_subtype, "dtmf-relay")) {
				/* Try and find signal information in the payload */
				if ((signal_ptr = switch_stristr("Signal=", sip->sip_payload->pl_data))) {
					int tmp;
					/* move signal_ptr where we need it (right past Signal=) */
					signal_ptr = signal_ptr + 7;

					/* handle broken devices with spaces after the = (cough) VegaStream (cough) */
					while (*signal_ptr && *signal_ptr == ' ') signal_ptr++;

					if (*signal_ptr && (*signal_ptr == '*' || *signal_ptr == '#' || *signal_ptr == 'A' || *signal_ptr == 'B' || *signal_ptr == 'C' || *signal_ptr == 'D')) {
						dtmf.digit = *signal_ptr;
					} else {
						tmp = atoi(signal_ptr);
						dtmf.digit = switch_rfc2833_to_char(tmp);
					}
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Bad signal\n");
					goto end;
				}

				if ((signal_ptr = switch_stristr("Duration=", sip->sip_payload->pl_data))) {
					int tmp;
					signal_ptr += 9;

					/* handle broken devices with spaces after the = (cough) VegaStream (cough) */
					while (*signal_ptr && *signal_ptr == ' ') signal_ptr++;
					
					if ((tmp = atoi(signal_ptr)) <= 0) {
						tmp = switch_core_default_dtmf_duration(0);
					}
					dtmf.duration = tmp * 8;
				}
			} else if (!strncasecmp(sip->sip_content_type->c_type, "application", 11) && !strcasecmp(sip->sip_content_type->c_subtype, "dtmf")) {
				int tmp = atoi(sip->sip_payload->pl_data);
				dtmf.digit = switch_rfc2833_to_char(tmp);
			} else {
				goto end;
			}

			if (dtmf.digit) {
				/* queue it up */
				switch_channel_queue_dtmf(channel, &dtmf);

				/* print debug info */
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "INFO DTMF(%c)\n", dtmf.digit);
				
				if (switch_channel_test_flag(channel, CF_PROXY_MODE)) {
					const char *uuid;
					switch_core_session_t *session_b;
					
					if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE)) && (session_b = switch_core_session_locate(uuid))) {
						while (switch_channel_has_dtmf(channel)) {
							switch_dtmf_t idtmf = { 0, 0 };
							if (switch_channel_dequeue_dtmf(channel, &idtmf) == SWITCH_STATUS_SUCCESS) {
								switch_core_session_send_dtmf(session_b, &idtmf);
							}
						}

						switch_core_session_rwunlock(session_b);
					}
				}

				/* Send 200 OK response */
				nua_respond(nh, SIP_200_OK, NUTAG_WITH_THIS(nua), TAG_END());
			}
			goto end;
		}

		if ((clientcode_header = sofia_glue_get_unknown_header(sip, "x-clientcode"))) {
			if (!switch_strlen_zero(clientcode_header)) {
				switch_channel_set_variable(channel, "call_clientcode", clientcode_header);
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Setting CMC to %s\n", clientcode_header);
				nua_respond(nh, SIP_200_OK, NUTAG_WITH_THIS(nua), TAG_END());
			}
			goto end;
		}

		if ((rec_header = sofia_glue_get_unknown_header(sip, "record"))) {
			if (switch_strlen_zero(profile->record_template)) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Record attempted but no template defined.\n");
				nua_respond(nh, 488, "Recording not enabled", NUTAG_WITH_THIS(nua), TAG_END());
			} else {
				if (!strcasecmp(rec_header, "on")) {
					char *file;

					file = switch_channel_expand_variables(channel, profile->record_template);
					switch_ivr_record_session(session, file, 0, NULL);
					switch_channel_set_variable(channel, "sofia_record_file", file);
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Recording %s to %s\n", switch_channel_get_name(channel), file);
					nua_respond(nh, SIP_200_OK, NUTAG_WITH_THIS(nua), TAG_END());
					if (file != profile->record_template) {
						free(file);
						file = NULL;
					}
				} else {
					const char *file;

					if ((file = switch_channel_get_variable(channel, "sofia_record_file"))) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Done recording %s to %s\n", switch_channel_get_name(channel), file);
						switch_ivr_stop_record_session(session, file);
						nua_respond(nh, SIP_200_OK, NUTAG_WITH_THIS(nua), TAG_END());
					} else {
						nua_respond(nh, 488, "Nothing to stop", NUTAG_WITH_THIS(nua), TAG_END());
					}
				}
			}
		}
	}

 end:


	if (sip && switch_event_create(&event, SWITCH_EVENT_RECV_INFO) == SWITCH_STATUS_SUCCESS) {
		sip_alert_info_t *alert_info = sip_alert_info(sip);

		if (sip && sip->sip_content_type) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "SIP-Content-Type", sip->sip_content_type->c_type);
		}

		if (sip->sip_from && sip->sip_from->a_url) {
			if (sip->sip_from->a_url->url_user) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "SIP-From-User", sip->sip_from->a_url->url_user);
			}

			if (sip->sip_from->a_url->url_host) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "SIP-From-Host", sip->sip_from->a_url->url_host);
			}
		}

		if (sip->sip_to && sip->sip_to->a_url) {
			if (sip->sip_to->a_url->url_user) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "SIP-To-User", sip->sip_to->a_url->url_user);
			}

			if (sip->sip_to->a_url->url_host) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "SIP-To-Host", sip->sip_to->a_url->url_host);
			}
		}


		if (sip->sip_contact && sip->sip_contact->m_url) {
			if (sip->sip_contact->m_url->url_user) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "SIP-Contact-User", sip->sip_contact->m_url->url_user);
			}

			if (sip->sip_contact->m_url->url_host) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "SIP-Contact-Host", sip->sip_contact->m_url->url_host);
			}
		}


		if (sip->sip_call_info) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Call-Info", sip_header_as_string(nua_handle_home(nh), (void *) sip->sip_call_info));
		}

		if (alert_info) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Alert-Info", sip_header_as_string(nua_handle_home(nh), (void *) alert_info));
		}


		if (sip->sip_payload && sip->sip_payload->pl_data) {
			switch_event_add_body(event, "%s", sip->sip_payload->pl_data);
		}

		switch_event_fire(&event);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "dispatched freeswitch event for INFO\n");
	}

	nua_respond(nh, SIP_200_OK, NUTAG_WITH_THIS(nua), TAG_END());

	return;

}

#define url_set_chanvars(session, url, varprefix) _url_set_chanvars(session, url, #varprefix "_user", #varprefix "_host", #varprefix "_port", #varprefix "_uri", #varprefix "_params")
const char *_url_set_chanvars(switch_core_session_t *session, url_t *url, const char *user_var,
							  const char *host_var, const char *port_var, const char *uri_var, const char *params_var)
{
	const char *user = NULL, *host = NULL, *port = NULL;
	char *uri = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	char new_port[25] = "";

	if (url) {
		user = url->url_user;
		host = url->url_host;
		port = url->url_port;
		if (!switch_strlen_zero(url->url_params)) {
			switch_channel_set_variable(channel, params_var, url->url_params);
		}
	}

	if (switch_strlen_zero(user)) {
		user = "nobody";
	}

	if (switch_strlen_zero(host)) {
		host = "nowhere";
	}

	check_decode(user, session);

	if (user) {
		switch_channel_set_variable(channel, user_var, user);
	}


	if (port) {
		switch_snprintf(new_port, sizeof(new_port), ":%s", port);
	}

	switch_channel_set_variable(channel, port_var, port);
	if (host) {
		if (user) {
			uri = switch_core_session_sprintf(session, "%s@%s%s", user, host, new_port);
		} else {
			uri = switch_core_session_sprintf(session, "%s%s", host, new_port);
		}
		switch_channel_set_variable(channel, uri_var, uri);
		switch_channel_set_variable(channel, host_var, host);
	}

	return uri;
}

void sofia_handle_sip_i_invite(nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip, tagi_t tags[])
{
	switch_core_session_t *session = NULL;
	char key[128] = "";
	sip_unknown_t *un;
	sip_remote_party_id_t *rpid = NULL;
	sip_p_asserted_identity_t *passerted = NULL;
	sip_p_preferred_identity_t *ppreferred = NULL;
	sip_privacy_t *privacy = NULL;
	sip_alert_info_t *alert_info = NULL;
	sip_call_info_t *call_info = NULL;
	private_object_t *tech_pvt = NULL;
	switch_channel_t *channel = NULL;
	const char *channel_name = NULL;
	const char *displayname = NULL;
	const char *destination_number = NULL;
	const char *from_user = NULL, *from_host = NULL;
	const char *referred_by_user = NULL, *referred_by_host = NULL;
	const char *context = NULL;
	const char *dialplan = NULL;
	char network_ip[80];
	switch_event_t *v_event = NULL;
	uint32_t sess_count = switch_core_session_count();
	uint32_t sess_max = switch_core_session_limit(0);
	int is_auth = 0, calling_myself = 0;
	int network_port = 0;
	char *is_nat = NULL;
	char acl_token[512] = "";
	sofia_transport_t transport;

	profile->ib_calls++;

	if (sess_count >= sess_max || !sofia_test_pflag(profile, PFLAG_RUNNING)) {
		nua_respond(nh, 503, "Maximum Calls In Progress", SIPTAG_RETRY_AFTER_STR("300"), TAG_END());
		goto fail;
	}

	if (!sip || !sip->sip_request || !sip->sip_request->rq_method_name) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Received an invalid packet!\n");
		nua_respond(nh, SIP_503_SERVICE_UNAVAILABLE, TAG_END());
		goto fail;
	}

	if (!(sip->sip_contact && sip->sip_contact->m_url)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "NO CONTACT!\n");
		nua_respond(nh, 400, "Missing Contact Header", TAG_END());
		goto fail;
	}

	sofia_glue_get_addr(nua_current_request(nua), network_ip,  sizeof(network_ip), &network_port);

	if (sofia_test_pflag(profile, PFLAG_AGGRESSIVE_NAT_DETECTION)) {
		if (sip && sip->sip_via) {
			const char *port = sip->sip_via->v_port;
			const char *host = sip->sip_via->v_host;

			if (host && sip->sip_via->v_received) {
				is_nat = "via received";
			} else if (host && strcmp(network_ip, host)) {
				is_nat = "via host";
			} else if (port && atoi(port) != network_port) {
				is_nat = "via port";
			}
		}
	}

	if (!is_nat && profile->nat_acl_count) {
		uint32_t x = 0;
		int ok = 1;
		char *last_acl = NULL;
		const char *contact_host = NULL;

		if (sip && sip->sip_contact && sip->sip_contact->m_url) {
			contact_host = sip->sip_contact->m_url->url_host;
		}

		if (!switch_strlen_zero(contact_host)) {
			for (x = 0; x < profile->nat_acl_count; x++) {
				last_acl = profile->nat_acl[x];
				if (!(ok = switch_check_network_list_ip(contact_host, last_acl))) {
					break;
				}
			}

			if (ok) {
				is_nat = last_acl;
			}
		}
	}
	
	if (profile->acl_count) {
		uint32_t x = 0;
		int ok = 1;
		char *last_acl = NULL;
		const char *token = NULL;

		for (x = 0; x < profile->acl_count; x++) {
			last_acl = profile->acl[x];
			if (!(ok = switch_check_network_list_ip_token(network_ip, last_acl, &token))) {
				break;
			}
		}

		if (ok) {
			if (token) {
				switch_set_string(acl_token, token);
			}
			if (sofia_test_pflag(profile, PFLAG_AUTH_CALLS)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "IP %s Approved by acl \"%s[%s]\". Access Granted.\n",
								  network_ip, switch_str_nil(last_acl), acl_token);
				is_auth = 1;
			}
		} else {
			if (!sofia_test_pflag(profile, PFLAG_AUTH_CALLS)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "IP %s Rejected by acl \"%s\"\n", network_ip, switch_str_nil(last_acl));
				nua_respond(nh, SIP_403_FORBIDDEN, TAG_END());
				goto fail;
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "IP %s Rejected by acl \"%s\". Falling back to Digest auth.\n",
								  network_ip, switch_str_nil(last_acl));
			}
		}
	}

	if (!is_auth &&
		(sofia_test_pflag(profile, PFLAG_AUTH_CALLS) || (!sofia_test_pflag(profile, PFLAG_BLIND_AUTH) && (sip->sip_proxy_authorization || sip->sip_authorization)))) {
		if (!strcmp(network_ip, profile->sipip) && network_port == profile->sip_port) {
			calling_myself++;
		} else {
			if (sofia_reg_handle_register(nua, profile, nh, sip, REG_INVITE, key, sizeof(key), &v_event, NULL)) {
				if (v_event) {
					switch_event_destroy(&v_event);
				}
				
				if (sip->sip_authorization || sip->sip_proxy_authorization) {
					goto fail;
				}

				return;
			}
		}
		is_auth++;
	}

	if (sofia_endpoint_interface) {
		if (sofia_test_pflag(profile, PFLAG_CALLID_AS_UUID)) {
			session = switch_core_session_request_uuid(sofia_endpoint_interface, SWITCH_CALL_DIRECTION_INBOUND, NULL, sip->sip_call_id->i_id);
		} else {
			session = switch_core_session_request(sofia_endpoint_interface, SWITCH_CALL_DIRECTION_INBOUND, NULL);
		}
	}

	if (!session) {
		nua_respond(nh, 503, "Maximum Calls In Progress", SIPTAG_RETRY_AFTER_STR("300"), TAG_END());
		goto fail;
	}

	if (!(tech_pvt = (private_object_t *) switch_core_session_alloc(session, sizeof(private_object_t)))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Hey where is my memory pool?\n");
		nua_respond(nh, SIP_503_SERVICE_UNAVAILABLE, TAG_END());
		switch_core_session_destroy(&session);
		goto fail;
	}


	switch_mutex_init(&tech_pvt->flag_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
	switch_mutex_init(&tech_pvt->sofia_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

	tech_pvt->remote_ip = switch_core_session_strdup(session, network_ip);
	tech_pvt->remote_port = network_port;

	channel = tech_pvt->channel = switch_core_session_get_channel(session);

	if (*acl_token) {
		switch_channel_set_variable(channel, "acl_token", acl_token);
		if (strchr(acl_token, '@')) {
			if (switch_ivr_set_user(session, acl_token) == SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Authenticating user %s\n", acl_token);
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Error Authenticating user %s\n", acl_token);
			}
		}
	}

	if (sip->sip_contact && sip->sip_contact->m_url) {
		char tmp[35] = "";
		const char *ipv6 = strchr(tech_pvt->remote_ip, ':');

		transport = sofia_glue_url2transport(sip->sip_contact->m_url);

		tech_pvt->record_route =
			switch_core_session_sprintf(session,
			"sip:%s@%s%s%s:%d;transport=%s",
			sip->sip_contact->m_url->url_user,
			ipv6 ? "[" : "",
			tech_pvt->remote_ip,
			ipv6 ? "]" : "",
			tech_pvt->remote_port,
			sofia_glue_transport2str(transport));

		switch_channel_set_variable(channel, "sip_received_ip", tech_pvt->remote_ip);
		snprintf(tmp, sizeof(tmp), "%d", tech_pvt->remote_port);
		switch_channel_set_variable(channel, "sip_received_port", tmp);
	}

	if (sip->sip_via) {
		switch_channel_set_variable(channel, "sip_via_protocol", sofia_glue_transport2str(sofia_glue_via2transport(sip->sip_via)));
	}

	if (*key != '\0') {
		tech_pvt->key = switch_core_session_strdup(session, key);
	}


	if (is_auth) {
		switch_channel_set_variable(channel, "sip_authorized", "true");
	}

	if (calling_myself) {
		switch_channel_set_variable(channel, "sip_looped_call", "true");
	}

	if (v_event) {
		switch_event_header_t *hp;

		for (hp = v_event->headers; hp; hp = hp->next) {
			switch_channel_set_variable(channel, hp->name, hp->value);
		}
		switch_event_destroy(&v_event);
	}

	if (sip->sip_from && sip->sip_from->a_url) {
		from_user = sip->sip_from->a_url->url_user;
		from_host = sip->sip_from->a_url->url_host;
		channel_name = url_set_chanvars(session, sip->sip_from->a_url, sip_from);

		if (!switch_strlen_zero(from_user)) {
			if (*from_user == '+') {
				switch_channel_set_variable(channel, "sip_from_user_stripped", (const char *) (from_user + 1));
			} else {
				switch_channel_set_variable(channel, "sip_from_user_stripped", from_user);
			}
		}

		switch_channel_set_variable(channel, "sip_from_comment", sip->sip_from->a_comment);

		if (sip->sip_from->a_params) {
			set_variable_sip_param(channel, "from", sip->sip_from->a_params);
		}

		switch_channel_set_variable(channel, "sofia_profile_name", profile->name);
		switch_channel_set_variable(channel, "sofia_profile_domain_name", profile->domain_name);

		if (!switch_strlen_zero(sip->sip_from->a_display)) {
			displayname = sip->sip_from->a_display;
		} else {
			displayname = switch_strlen_zero(from_user) ? "unknown" : from_user;
		}
	}

	if ((rpid = sip_remote_party_id(sip))) {
		if (rpid->rpid_url && rpid->rpid_url->url_user) {
			char *full_rpid_header = sip_header_as_string(nh->nh_home, (void *) rpid); 
			from_user = rpid->rpid_url->url_user;
			if (!switch_strlen_zero(full_rpid_header)) {
				switch_channel_set_variable(channel, "sip_Remote-Party-ID", full_rpid_header);
			}

		}
		if (!switch_strlen_zero(rpid->rpid_display)) {
			displayname = rpid->rpid_display;
		}
		switch_channel_set_variable(channel, "sip_cid_type", "rpid");
	}

	if ((passerted = sip_p_asserted_identity(sip))) {
		if (passerted->paid_url && passerted->paid_url->url_user) {
			char *full_paid_header = sip_header_as_string(nh->nh_home, (void *) passerted);
			from_user = passerted->paid_url->url_user;
			if (!switch_strlen_zero(full_paid_header)) {
				switch_channel_set_variable(channel, "sip_P-Asserted-Identity", from_user);
			}
		}
		if (!switch_strlen_zero(passerted->paid_display)) {
			displayname = passerted->paid_display;
		}
		switch_channel_set_variable(channel, "sip_cid_type", "pid");
	}

	if ((ppreferred = sip_p_preferred_identity(sip))) {
		if (ppreferred->ppid_url && ppreferred->ppid_url->url_user) {
			char *full_ppid_header = sip_header_as_string(nh->nh_home, (void *) ppreferred);
			from_user = ppreferred->ppid_url->url_user;
			if (!switch_strlen_zero(full_ppid_header)) {
				switch_channel_set_variable(channel, "sip_P-Preferred-Identity", full_ppid_header);
			}
			
		}
		if (!switch_strlen_zero(ppreferred->ppid_display)) {
			displayname = ppreferred->ppid_display;
		}
		switch_channel_set_variable(channel, "sip_cid_type", "pid");
	}

	if (from_user) {
		check_decode(from_user, session);
	}

	if (sip->sip_request->rq_url) {
		const char *req_uri = url_set_chanvars(session, sip->sip_request->rq_url, sip_req);
		if (sofia_test_pflag(profile, PFLAG_FULL_ID)) {
			destination_number = req_uri;
		} else {
			destination_number = sip->sip_request->rq_url->url_user;
		}
		if (sip->sip_request->rq_url->url_params && (sofia_glue_find_parameter(sip->sip_request->rq_url->url_params, "intercom=true"))) {
			switch_channel_set_variable(channel, "sip_auto_answer_detected", "true");
		}
	}

	if (!destination_number && sip->sip_to && sip->sip_to->a_url) {
		destination_number = sip->sip_to->a_url->url_user;
	}
	
	if (destination_number) {
		check_decode(destination_number, session);
	} else {
		destination_number = "service";
	}
	
	if (sip->sip_to && sip->sip_to->a_url) {
		const char *host, *user;
		int port;
		url_t *transport_url;

		if (sip->sip_record_route && sip->sip_record_route->r_url) {
			transport_url = sip->sip_record_route->r_url;
		} else {
			transport_url = sip->sip_contact->m_url;
		}

		transport = sofia_glue_url2transport(transport_url);
		tech_pvt->transport = transport;

		url_set_chanvars(session, sip->sip_to->a_url, sip_to);
		if (switch_channel_get_variable(channel, "sip_to_uri")) {
			const char *ipv6;
			const char *tmp, *at, *url = NULL;
			
			host = switch_channel_get_variable(channel, "sip_to_host");
			user = switch_channel_get_variable(channel, "sip_to_user");

			switch_channel_set_variable(channel, "sip_to_comment", sip->sip_to->a_comment);

			if (sip->sip_to->a_params) {
				set_variable_sip_param(channel, "to", sip->sip_to->a_params);
			}

			if (sip->sip_contact->m_url->url_port) {
				port = atoi(sip->sip_contact->m_url->url_port);
			} else {
				port = sofia_glue_transport_has_tls(transport) ? profile->tls_sip_port : profile->sip_port;
			}

			ipv6 = strchr(host, ':');
			tech_pvt->to_uri =
				switch_core_session_sprintf(session,
					"sip:%s@%s%s%s:%d;transport=%s",
					user, ipv6 ? "[" : "",
					host, ipv6 ? "]" : "",
					port,
					sofia_glue_transport2str(transport));

			if (sofia_glue_check_nat(profile, tech_pvt->remote_ip)) {
				url = (sofia_glue_transport_has_tls(transport)) ? profile->tls_public_url : profile->public_url;
			} else { 
				url = (sofia_glue_transport_has_tls(transport)) ? profile->tls_url : profile->url;
			}			
			
			tmp = sofia_overcome_sip_uri_weakness(session, url, transport, SWITCH_TRUE, NULL);
	
			if ((at = strchr(tmp, '@'))) {
				url = switch_core_session_sprintf(session, "sip:%s%s", user, at);
			}

			if (url) {
				const char *brackets = NULL;
				const char *proto = NULL;
				
				brackets = strchr(url, '>');
				proto = switch_stristr("transport=", url);
				tech_pvt->reply_contact = switch_core_session_sprintf(session, "%s%s%s%s%s",
																	  brackets ? "" : "<", url,
																	  proto ? "" : ";transport=",
																	  proto ? "" : sofia_glue_transport2str(transport),
																	  brackets ? "" : ">");
			} else {
				switch_channel_hangup(tech_pvt->channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
			}
			
		} else {
			const char *url = NULL;
			if (sofia_glue_check_nat(profile, tech_pvt->remote_ip)) {
			url = (sofia_glue_transport_has_tls(transport)) ? profile->tls_public_url : profile->public_url;
			} else { 
				url = (sofia_glue_transport_has_tls(transport)) ? profile->tls_url : profile->url;
			}			
			
			if (url) {
				const char *brackets = NULL;
				const char *proto = NULL;
				
				brackets = strchr(url, '>');
				proto = switch_stristr("transport=", url);
				tech_pvt->reply_contact = switch_core_session_sprintf(session, "%s%s%s%s%s",
																	  brackets ? "" : "<", url,
																	  proto ? "" : ";transport=",
																	  proto ? "" : sofia_glue_transport2str(transport),
																	  brackets ? "" : ">");
			} else {
				switch_channel_hangup(tech_pvt->channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
			}
		}
	}
	
	if (sofia_glue_check_nat(profile, tech_pvt->remote_ip)) {
		tech_pvt->user_via = sofia_glue_create_external_via(session, profile, tech_pvt->transport);
	}

	if (sip->sip_contact && sip->sip_contact->m_url) {
		const char *contact_uri = url_set_chanvars(session, sip->sip_contact->m_url, sip_contact);
		if (!channel_name) {
			channel_name = contact_uri;
		}
	}

	if (sip->sip_referred_by) {
		referred_by_user = sip->sip_referred_by->b_url->url_user;
		referred_by_host = sip->sip_referred_by->b_url->url_host;
		channel_name = url_set_chanvars(session, sip->sip_referred_by->b_url, sip_referred_by);

		check_decode(referred_by_user, session);

		if (!switch_strlen_zero(referred_by_user)) {
			if (*referred_by_user == '+') {
				switch_channel_set_variable(channel, "sip_referred_by_user_stripped", (const char *) (referred_by_user + 1));
			} else {
				switch_channel_set_variable(channel, "sip_referred_by_user_stripped", referred_by_user);
			}
		}

		switch_channel_set_variable(channel, "sip_referred_by_cid", sip->sip_referred_by->b_cid);

		if (sip->sip_referred_by->b_params) {
			set_variable_sip_param(channel, "referred_by", sip->sip_referred_by->b_params);
		}
	}

	sofia_glue_attach_private(session, profile, tech_pvt, channel_name);
	sofia_glue_tech_prepare_codecs(tech_pvt);

	switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "INBOUND CALL");

	if (sofia_test_flag(tech_pvt, TFLAG_INB_NOMEDIA)) {
		switch_channel_set_flag(channel, CF_PROXY_MODE);
	}

	if (sofia_test_flag(tech_pvt, TFLAG_PROXY_MEDIA)) {
		switch_channel_set_flag(channel, CF_PROXY_MEDIA);
	}

	if (!tech_pvt->call_id && sip->sip_call_id && sip->sip_call_id->i_id) {
		tech_pvt->call_id = switch_core_session_strdup(session, sip->sip_call_id->i_id);
		switch_channel_set_variable(channel, "sip_call_id", tech_pvt->call_id);
	}

	if (sip->sip_subject && sip->sip_subject->g_string) {
		switch_channel_set_variable(channel, "sip_subject", sip->sip_subject->g_string);
	}

	if (sip->sip_user_agent && !switch_strlen_zero(sip->sip_user_agent->g_string)) {
		switch_channel_set_variable(channel, "sip_user_agent", sip->sip_user_agent->g_string);
	}

	if (sip->sip_via) {
		if (sip->sip_via->v_host) {
			switch_channel_set_variable(channel, "sip_via_host", sip->sip_via->v_host);
		}
		if (sip->sip_via->v_port) {
			switch_channel_set_variable(channel, "sip_via_port", sip->sip_via->v_port);
		}
		if (sip->sip_via->v_rport) {
			switch_channel_set_variable(channel, "sip_via_rport", sip->sip_via->v_rport);
		}
	}

	if (sip->sip_max_forwards) {
		char max_forwards[32];
		switch_snprintf(max_forwards, sizeof(max_forwards), "%lu", sip->sip_max_forwards->mf_count);
		switch_channel_set_variable(channel, SWITCH_MAX_FORWARDS_VARIABLE, max_forwards);
	}

	if (!context) {
		context = switch_channel_get_variable(channel, "user_context");
	}

	if (!context) {
		if (profile->context && !strcasecmp(profile->context, "_domain_")) {
			context = from_host;
		} else {
			context = profile->context;
		}
	}

	if (!(dialplan = switch_channel_get_variable(channel, "inbound_dialplan"))) {
		dialplan = profile->dialplan;
	}

	if ((alert_info = sip_alert_info(sip))) {
		char *tmp = sip_header_as_string(profile->home, (void *) alert_info);
		switch_channel_set_variable(channel, "alert_info", tmp);
		su_free(profile->home, tmp);
	}

	if ((call_info = sip_call_info(sip))) {
		char *tmp = sip_header_as_string(profile->home, (void *) call_info);
		if (call_info->ci_params && (msg_params_find(call_info->ci_params , "answer-after=0"))) {
			switch_channel_set_variable(channel, "sip_auto_answer_detected", "true");
		}
		switch_channel_set_variable(channel, "sip_call_info", tmp);
		su_free(profile->home, tmp);
	}

	if (profile->pres_type) {
		const char *user = switch_str_nil(sip->sip_from->a_url->url_user);
		const char *host = switch_str_nil(sip->sip_from->a_url->url_host);

		char *tmp = switch_mprintf("%s@%s", user, host);
		switch_assert(tmp);
		switch_channel_set_variable(channel, "presence_id", tmp);
		free(tmp);
	}


	if (strstr(destination_number, "gw+")) {
		const char *gw_name = destination_number + 3;
		sofia_gateway_t *gateway;
		if (gw_name && (gateway = sofia_reg_find_gateway(gw_name))) {
			context = switch_core_session_strdup(session, gateway->register_context);
			switch_channel_set_variable(channel, "sip_gateway", gateway->name);

			if (gateway->extension) {
				destination_number = switch_core_session_strdup(session, gateway->extension);
			}

			gateway->ib_calls++;

			if (gateway->ib_vars) {
				switch_event_header_t *hp;
				for(hp = gateway->ib_vars->headers; hp; hp = hp->next) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s setting variable [%s]=[%s]\n",
									  switch_channel_get_name(channel), hp->name, hp->value);
					switch_channel_set_variable(channel, hp->name, hp->value);
				}
			}

			sofia_reg_release_gateway(gateway);
		}
	}

	if (sip->sip_replaces) {
		nua_handle_t *bnh;
		if ((bnh = nua_handle_by_replaces(nua, sip->sip_replaces))) {
			sofia_private_t *b_private = NULL;
			if ((b_private = nua_handle_magic(bnh))) {
				switch_core_session_t *b_session = NULL;
				if ((b_session = switch_core_session_locate(b_private->uuid))) {
					switch_channel_t *b_channel = switch_core_session_get_channel(b_session);
					const char *uuid;
					int one_leg = 1;
					private_object_t *b_tech_pvt = NULL;
					const char *app = switch_channel_get_variable(b_channel, SWITCH_CURRENT_APPLICATION_VARIABLE);
					const char *data = switch_channel_get_variable(b_channel, SWITCH_CURRENT_APPLICATION_DATA_VARIABLE);
					
					if (app && data && !strcasecmp(app, "conference")) {
						destination_number = switch_core_session_sprintf(b_session, "answer,conference:%s", data);
						dialplan = "inline";
					} else {
						if (switch_core_session_check_interface(b_session, sofia_endpoint_interface)) {
							b_tech_pvt = switch_core_session_get_private(b_session);
						}

						if ((uuid = switch_channel_get_variable(b_channel, SWITCH_SIGNAL_BOND_VARIABLE))) {
							one_leg = 0;
						} else {
							uuid = switch_core_session_get_uuid(b_session);
						}
					
						if (uuid) {
							switch_core_session_t *c_session = NULL;
							int do_conf = 0;					
						
							uuid = switch_core_session_strdup(b_session, uuid);

							if ((c_session = switch_core_session_locate(uuid))) {
								switch_channel_t *c_channel = switch_core_session_get_channel(c_session);
								private_object_t *c_tech_pvt = NULL;
					
								if (switch_core_session_check_interface(c_session, sofia_endpoint_interface)) {
									c_tech_pvt = switch_core_session_get_private(c_session);
								}
								

								if (!one_leg && 
									(!b_tech_pvt || !sofia_test_flag(b_tech_pvt, TFLAG_SIP_HOLD)) && 
									(!c_tech_pvt || !sofia_test_flag(c_tech_pvt, TFLAG_SIP_HOLD))) {
									char *ext = switch_core_session_sprintf(b_session, "conference:%s@sla+flags{mintwo}", uuid);
								
									switch_channel_set_flag(c_channel, CF_REDIRECT);
									switch_ivr_session_transfer(b_session, ext, "inline", NULL);
									switch_ivr_session_transfer(c_session, ext, "inline", NULL);
									switch_channel_clear_flag(c_channel, CF_REDIRECT);
									do_conf = 1;
								}
								switch_core_session_rwunlock(c_session);
							}
						
							if (do_conf) {
								destination_number = switch_core_session_sprintf(b_session, "answer,conference:%s@sla+flags{mintwo}", uuid);
							} else {
								destination_number = switch_core_session_sprintf(b_session, "answer,intercept:%s", uuid);
							}

							dialplan = "inline";
						}
					}
					switch_core_session_rwunlock(b_session);
				}
			}
			nua_handle_unref(bnh);
		}
	}

	check_decode(displayname, session);
	tech_pvt->caller_profile = switch_caller_profile_new(switch_core_session_get_pool(session),
														 from_user,
														 dialplan,
														 displayname, from_user, network_ip, NULL, NULL, NULL, MODNAME, context, destination_number);

	if (tech_pvt->caller_profile) {

		if (rpid) {
			if (rpid->rpid_privacy) {
				if (!strcasecmp(rpid->rpid_privacy, "yes")) {
					switch_set_flag(tech_pvt->caller_profile, SWITCH_CPF_HIDE_NAME | SWITCH_CPF_HIDE_NUMBER);
				} else if (!strcasecmp(rpid->rpid_privacy, "full")) {
					switch_set_flag(tech_pvt->caller_profile, SWITCH_CPF_HIDE_NAME | SWITCH_CPF_HIDE_NUMBER);
				} else if (!strcasecmp(rpid->rpid_privacy, "name")) {
					switch_set_flag(tech_pvt->caller_profile, SWITCH_CPF_HIDE_NAME);
				} else if (!strcasecmp(rpid->rpid_privacy, "number")) {
					switch_set_flag(tech_pvt->caller_profile, SWITCH_CPF_HIDE_NUMBER);
				} else {
					switch_clear_flag(tech_pvt->caller_profile, SWITCH_CPF_HIDE_NAME);
					switch_clear_flag(tech_pvt->caller_profile, SWITCH_CPF_HIDE_NUMBER);
				}
			}

			if (rpid->rpid_screen && !strcasecmp(rpid->rpid_screen, "no")) {
				switch_clear_flag(tech_pvt->caller_profile, SWITCH_CPF_SCREEN);
			}
		}

		if ((privacy = sip_privacy(sip))) {
			char *full_priv_header = sip_header_as_string(nh->nh_home, (void *) privacy); 
			if (!switch_strlen_zero(full_priv_header)) {
				switch_channel_set_variable(channel, "sip_Privacy", full_priv_header);
			}
			if (msg_params_find(privacy->priv_values, "id")) {
				switch_set_flag(tech_pvt->caller_profile, SWITCH_CPF_HIDE_NAME | SWITCH_CPF_HIDE_NUMBER);
			}
		}

		/* Loop thru unknown Headers Here so we can do something with them */
		for (un = sip->sip_unknown; un; un = un->un_next) {
			if (!strncasecmp(un->un_name, "Diversion", 9)) {
				/* Basic Diversion Support for Diversion Indication in SIP */
				/* draft-levy-sip-diversion-08 */
				if (!switch_strlen_zero(un->un_value)) {
					char *tmp_name;
					if ((tmp_name = switch_mprintf("%s%s", SOFIA_SIP_HEADER_PREFIX, un->un_name))) {
						switch_channel_set_variable(channel, tmp_name, un->un_value);
						free(tmp_name);
					}
				}
			} else if (!strncasecmp(un->un_name, "History-Info", 12)) {
				switch_channel_set_variable(channel, "sip_history_info", un->un_value);
			} else if (!strncasecmp(un->un_name, "X-", 2) || !strncasecmp(un->un_name, "P-", 2)) {
				if (!switch_strlen_zero(un->un_value)) {
					char new_name[512] = "";
					int reps  = 0;
					for(;;) {
						char postfix[25] = "";
						if (reps > 0) {
							switch_snprintf(postfix, sizeof(postfix), "-%d", reps);
						}
						reps++;
						switch_snprintf(new_name, sizeof(new_name), "%s%s%s", SOFIA_SIP_HEADER_PREFIX, un->un_name, postfix);

						if (switch_channel_get_variable(channel, new_name)) {
							continue;
						}

						switch_channel_set_variable(channel, new_name, un->un_value);
						break;
					}
				}
			}
		}
		
		switch_channel_set_caller_profile(channel, tech_pvt->caller_profile);
	}
	
	if (!(sofia_private = malloc(sizeof(*sofia_private)))) {
		abort();
	}

	memset(sofia_private, 0, sizeof(*sofia_private));
	sofia_private->is_call++;
	tech_pvt->sofia_private = sofia_private;
	
	if ((profile->pres_type)) {
		sofia_presence_set_chat_hash(tech_pvt, sip);
	}
	switch_copy_string(tech_pvt->sofia_private->uuid, switch_core_session_get_uuid(session), sizeof(tech_pvt->sofia_private->uuid));
	nua_handle_bind(nh, tech_pvt->sofia_private);
	tech_pvt->nh = nh;

	if (sip && switch_core_session_thread_launch(session) == SWITCH_STATUS_SUCCESS) {
		const char *dialog_from_user = "", *dialog_from_host = "", *to_user = "", *to_host = "", *contact_user = "", *contact_host = "";
		const char *user_agent = "", *call_id = "";
		url_t *from = NULL, *to = NULL, *contact = NULL;
		char *sql = NULL;

		if (sip->sip_to) {
			to = sip->sip_to->a_url;
		}
		if (sip->sip_from) {
			from = sip->sip_from->a_url;
		}
		if (sip->sip_contact) {
			contact = sip->sip_contact->m_url;
		}

		if (sip->sip_user_agent) {
			user_agent = switch_str_nil(sip->sip_user_agent->g_string);
		}

		if (sip->sip_call_id) {
			call_id = switch_str_nil(sip->sip_call_id->i_id);
		}

		if (to) {
			to_user = switch_str_nil(to->url_user);
			to_host = switch_str_nil(to->url_host);
		}

		if (from) {
			dialog_from_user = switch_str_nil(from->url_user);
			dialog_from_host = switch_str_nil(from->url_host);
		}

		if (contact) {
			contact_user = switch_str_nil(contact->url_user);
			contact_host = switch_str_nil(contact->url_host);
		}

		if (profile->pres_type) {
			sql = switch_mprintf("insert into sip_dialogs "
								 "(call_id,uuid,sip_to_user,sip_to_host,sip_from_user,sip_from_host,contact_user,"
								 "contact_host,state,direction,user_agent,profile_name,hostname) "
								 "values('%q','%q','%q','%q','%q','%q','%q','%q','%q','%q','%q','%q','%q')",
								 call_id,
								 tech_pvt->sofia_private->uuid,
								 to_user, to_host, dialog_from_user, dialog_from_host, 
								 contact_user, contact_host, "confirmed", "inbound", user_agent,
								 profile->name, mod_sofia_globals.hostname);
			switch_assert(sql);
			sofia_glue_execute_sql(profile, &sql, SWITCH_TRUE);
		}

		if (is_nat) {
			sofia_set_flag(tech_pvt, TFLAG_NAT);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Setting NAT mode based on %s\n", is_nat);
			switch_channel_set_variable(channel, "sip_nat_detected", "true");
		}
		return;
	}

	if (sess_count > 110) {
		switch_mutex_lock(profile->flag_mutex);
		switch_core_session_limit(sess_count - 10);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "LUKE: I'm hit, but not bad.\n");
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "LUKE'S VOICE: Artoo, see what you can do with it. Hang on back there....\n"
						  "Green laserfire moves past the beeping little robot as his head turns.  "
						  "After a few beeps and a twist of his mechanical arm,\n"
						  "Artoo reduces the max sessions to %d thus, saving the switch from certain doom.\n", sess_count - 10);
		switch_mutex_unlock(profile->flag_mutex);
	}

	if (tech_pvt->hash_key) {
		switch_mutex_lock(tech_pvt->profile->flag_mutex);
		switch_core_hash_delete(tech_pvt->profile->chat_hash, tech_pvt->hash_key);
		switch_mutex_unlock(tech_pvt->profile->flag_mutex);
	}

	nua_handle_bind(nh, NULL);
	sofia_private_free(sofia_private);
	switch_core_session_destroy(&session);
	nua_respond(nh, 503, "Maximum Calls In Progress", SIPTAG_RETRY_AFTER_STR("300"), TAG_END());
	return;

 fail:
	profile->ib_failed_calls++;
	return;

}

void sofia_handle_sip_i_options(int status,
								char const *phrase,
								nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh, sofia_private_t *sofia_private, sip_t const *sip, tagi_t tags[])
{
	nua_respond(nh, SIP_200_OK, NUTAG_WITH_THIS(nua), TAG_END());
}

static void sofia_info_send_sipfrag(switch_core_session_t *aleg, switch_core_session_t *bleg)
{
	private_object_t *b_tech_pvt = NULL, *a_tech_pvt = NULL;
	char message[256] = "";

	if (aleg && bleg && switch_core_session_compare(aleg, bleg)) {
		switch_channel_t *channel = switch_core_session_get_channel(bleg);
		const char *ua = switch_channel_get_variable(channel, "sip_user_agent");
		
		a_tech_pvt = (private_object_t *) switch_core_session_get_private(aleg);
		b_tech_pvt = (private_object_t *) switch_core_session_get_private(bleg);

		if (b_tech_pvt && a_tech_pvt && a_tech_pvt->caller_profile) {
			switch_caller_profile_t *acp = a_tech_pvt->caller_profile;
			
			if (ua && switch_stristr("snom", ua)) {
				if (switch_strlen_zero(acp->caller_id_name)) {
					snprintf(message, sizeof(message), "From:\r\nTo: %s\r\n", acp->caller_id_number);
				} else {
					snprintf(message, sizeof(message), "From:\r\nTo: \"%s\" %s\r\n", acp->caller_id_name, acp->caller_id_number);
				}
				nua_info(b_tech_pvt->nh, SIPTAG_CONTENT_TYPE_STR("message/sipfrag"), SIPTAG_PAYLOAD_STR(message), TAG_END());
			} else if (ua && switch_stristr("polycom", ua)) {
				if (switch_strlen_zero(acp->caller_id_name)) {
					snprintf(message, sizeof(message), "P-Asserted-Identity: \"%s\" <%s>", acp->caller_id_number, acp->caller_id_number);
				} else {
					snprintf(message, sizeof(message), "P-Asserted-Identity: \"%s\" <%s>", acp->caller_id_name, acp->caller_id_number);
				}
				nua_update(b_tech_pvt->nh,
						   TAG_IF(!switch_strlen_zero_buf(message), SIPTAG_HEADER_STR(message)),
						   TAG_END());
			}
		}
	}
}

/*
 * This subroutine will take the a_params of a sip_addr_s structure and spin through them.
 * Each param will be used to create a channel variable.
 * In the SIP RFC's, this data is called generic-param.
 * Note that the tag-param is also included in the a_params list.
 *
 * From: "John Doe" <sip:5551212@1.2.3.4>;tag=ed23266b52cbb17eo2;ref=101;mbid=201
 *
 * For example, the header above will produce an a_params list with three entries
 *    tag=ed23266b52cbb17eo2
 *    ref=101
 *    mbid=201
 *
 * The a_params list is parsed and the lvalue is used to create the channel variable name while the
 * rvalue is used to create the channel variable value. 
 *
 * If no equal (=) sign is found during parsing, a channel variable name is created with the param and
 * the value is set to NULL.
 *
 * Pointers are used for copying the sip_header_name for performance reasons.  There are no calls to
 * any string functions and no memory is allocated/dealocated.  The only limiter is the size of the
 * sip_header_name array. 
*/
static void set_variable_sip_param(switch_channel_t *channel, char *header_type, sip_param_t const *params)
{
	char sip_header_name[128] = "";
	char var1[] = "sip_";
	char *cp, *sh, *sh_end, *sh_save;

	/* Build the static part of the sip_header_name variable from   */
	/* the header_type. If the header type is "referred_by" then    */
	/* sip_header_name = "sip_referred_by_".                        */
	sh = sip_header_name;
	sh_end = sh + sizeof(sip_header_name) - 1;
	for (cp = var1; *cp; cp++, sh++) {
		*sh = *cp;
	}
	*sh = '\0';

	/* Copy the header_type to the sip_header_name. Before copying  */
	/* each character, check that we aren't going to overflow the   */
	/* the sip_header_name buffer.  We have to account for the      */
	/* trailing underscore and NULL that will be added to the end.  */
	for (cp = header_type; (*cp && (sh < (sh_end - 1))); cp++, sh++) {
		*sh = *cp;
	}
	*sh++ = '_';
	*sh = '\0';

	/* sh now points to the NULL at the end of the partially built  */
	/* sip_header_name variable.  This is also the start of the     */
	/* variable part of the sip_header_name built from the lvalue   */
	/* of the params data.                                          */
	sh_save = sh;

	while (params && params[0]) {

		/* Copy the params data to the sip_header_name variable until   */
		/* the end of the params string is reached, an '=' is detected  */
		/* or until the sip_header_name buffer has been exhausted.      */
		for (cp = (char *) (*params); ((*cp != '=') && *cp && (sh < sh_end)); cp++, sh++) {
			*sh = *cp;
		}

		/* cp now points to either the end of the params data or the */
		/* equal (=) sign separating the lvalue and rvalue.          */
		if (*cp == '=')
			cp++;
		*sh = '\0';
		switch_channel_set_variable(channel, sip_header_name, cp);

		/* Bump pointer to next param in the list.  Also reset the      */
		/* sip_header_name pointer to the beginning of the dynamic area */
		params++;
		sh = sh_save;
	}
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */
