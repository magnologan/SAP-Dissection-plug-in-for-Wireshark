/*
## ===========
## SAP Dissector Plugin for Wireshark
##
## Copyright (C) 2014 Core Security Technologies
##
## The plugin was designed and developed by Martin Gallo from the Security
## Consulting Services team of Core Security Technologies.
##
## This program is free software; you can redistribute it and/or
## modify it under the terms of the GNU General Public License
## as published by the Free Software Foundation; either version 2
## of the License, or (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##==============
*/

#include "config.h"

#include <epan/packet.h>
#include <epan/prefs.h>
#include <epan/expert.h>
#include <epan/dissectors/packet-tcp.h>
#include <epan/next_tvb.h>
#include <epan/conversation.h>

#include "packet-sapprotocol.h"


/* Define default ports */
#define SAP_PROTOCOL_PORT_RANGE "3200-3999"

/*
 * Length of the frame header
 */
#define SAP_PROTOCOL_HEADER_LEN			4

static int proto_sap_protocol = -1;

static int hf_sap_protocol_length = -1;
static int hf_sap_protocol_payload = -1;

static int hf_sap_protocol_ping = -1;
static int hf_sap_protocol_pong = -1;

static gint ett_sap_protocol = -1;

/* Global port preference */
static range_t *global_sap_protocol_port_range;

/* Global reassemble preference */
static gboolean global_sap_protocol_desegment = TRUE;

/* Protocol handle */
static dissector_handle_t sap_protocol_handle;

/* Sub-dissectors table */
static dissector_table_t sub_dissectors_table;
static heur_dissector_list_t heur_subdissector_list;

/*
 *
 */
void proto_reg_handoff_sap_protocol(void);

/*
 * Get the SAPNI pdu length
 */
static guint
get_sap_protocol_pdu_len(packet_info *pinfo _U_, tvbuff_t *tvb, int offset)
{
	return ((guint)tvb_get_ntohl(tvb, offset - offset) + 4);
}


/*
 * Dissect the payload of a packet using a registered SAP protocol. It uses
 * heuristics as a first try as some protocols uses the same TCP ports
 * (e.g. 3200/tcp for Enqueue Server and Diag).
 */
void
dissect_sap_protocol_payload(tvbuff_t *tvb, guint32 offset, packet_info *pinfo, proto_tree *tree, guint16 sport, guint16 dport){
	guint16 low_port = 0, high_port = 0;
	tvbuff_t *next_tvb = NULL;

	/* Set the new tvb for further dissection of the payload */
	next_tvb = tvb_new_subset_remaining(tvb, offset);

	/* Determine if this packet is part of a conversation and call dissector
	 * for the conversation if available.
	 * TODO: Check for building with conversation.h include */
	/*
	if (try_conversation_dissector(&pinfo->dst, &pinfo->src, PT_TCP,
			dport, sport, next_tvb, pinfo, tree)) {
		return;
	}
	 */

	/* Try with the heuristic dissectors first */
	/* TODO: When the protocol is guessed via heuristic dissector (Enqueue
	 * Server), the NI Protocol tree is missed. */
	if (dissector_try_heuristic(heur_subdissector_list, next_tvb, pinfo, tree, NULL)) {
		return;
	}

	/* Call the dissector in the subdissectors table according to the port number */
	if (sport > dport) {
		low_port = dport; high_port = sport;
	} else {
		low_port = sport; high_port = dport;
	}
	if ((low_port != 0 && dissector_try_uint(sub_dissectors_table, low_port, next_tvb, pinfo, tree)) ||
		(high_port != 0 && dissector_try_uint(sub_dissectors_table, high_port, next_tvb, pinfo, tree))){
		return;
	}
}


/*
 * Dissect a SAPNI packet, adding the length field to the protocol tree and
 * calling the sub-dissector according to the port number. It also identifies
 * PING/PONG packets at the SAPNI layer.
 */
static void
dissect_sap_protocol_message(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
	guint32 length = 0;
	proto_item *ti = NULL, *sap_protocol_length = NULL;
	proto_tree *sap_protocol_tree = NULL;

	/* Add the protocol to the column */
	col_set_str(pinfo->cinfo, COL_PROTOCOL, "SAPNI");
	/* Clear out stuff in the info column */
	col_clear(pinfo->cinfo,COL_INFO);

	/* Get the length field */
	length = tvb_get_ntohl(tvb, 0);

	/* Add the payload length to the info column */
	col_add_fstr(pinfo->cinfo, COL_INFO, "Length=%d ", length);

	/* We are being asked for details */
	if (tree) {  
		/* Add the main SAP Protocol subtree */
		ti = proto_tree_add_item(tree, proto_sap_protocol, tvb, 0, -1, FALSE);
		sap_protocol_tree = proto_item_add_subtree(ti, ett_sap_protocol);

		/* Add the length item */
		proto_item_append_text(ti, ", Len: %u", length);
		sap_protocol_length = proto_tree_add_item(sap_protocol_tree, hf_sap_protocol_length, tvb, 0, 4, FALSE);

		/* Add expert info in case of no match between the given length and the actual one */
		if (tvb_reported_length(tvb) != length + 4) {  
			expert_add_info_format(pinfo, sap_protocol_length, PI_MALFORMED, PI_WARN, "The reported length is incorrect");
		}

		/* Add the payload subtree */
		if (length > 0){
			proto_tree_add_item(sap_protocol_tree, hf_sap_protocol_payload, tvb, 4, -1, FALSE);
		}
	}

	/* Check for NI_PING/NI_PONG values */
	if ((length == 8)&&(tvb_strneql(tvb, 4, "NI_PING\00", 8) == 0)){
		col_set_str(pinfo->cinfo, COL_INFO, "Ping Message");
		if (tree){
			proto_item_append_text(ti, ", Ping Message (keep-alive request)");
            proto_tree_add_item(sap_protocol_tree, hf_sap_protocol_ping, tvb, 4, -1, FALSE);
		}
	} else if ((length == 8)&&(tvb_strneql(tvb, 4, "NI_PONG\00", 8) == 0)){
		col_set_str(pinfo->cinfo, COL_INFO, "Pong Message");
		if (tree){
			proto_item_append_text(ti, ", Pong Message (keep-alive response / route accepted)");
            proto_tree_add_item(sap_protocol_tree, hf_sap_protocol_pong, tvb, 4, -1, FALSE);
		}

	/* Dissect the payload */
	} else if (length > 0){
		dissect_sap_protocol_payload(tvb, 4, pinfo, tree, pinfo->srcport, pinfo->destport);
	}
}

/*
 * Performs the TCP reassembling and dissects the packet.
 */
static void
dissect_sap_protocol(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
	tcp_dissect_pdus(tvb, pinfo, tree, global_sap_protocol_desegment, SAP_PROTOCOL_HEADER_LEN,
		get_sap_protocol_pdu_len, dissect_sap_protocol_message);
}

void
proto_register_sap_protocol(void)
{
	static hf_register_info hf[] = {
		{ &hf_sap_protocol_length,
			{ "Length", "sapni.length", FT_UINT32, BASE_DEC, NULL, 0x0, "SAP NI Protocol Message Length", HFILL }},
		{ &hf_sap_protocol_payload,
			{ "Payload", "sapni.payload", FT_NONE, BASE_NONE, NULL, 0x0, "SAP NI Protocol Payload", HFILL }},
        { &hf_sap_protocol_ping,
            { "Ping", "sapni.ping", FT_NONE, BASE_NONE, NULL, 0x0, "SAP NI Ping Message", HFILL }},
        { &hf_sap_protocol_pong,
            { "Pong", "sapni.pong", FT_NONE, BASE_NONE, NULL, 0x0, "SAP NI Pong Message", HFILL }},
	};

	/* Setup protocol subtree array */
	static gint *ett[] = {
		&ett_sap_protocol
	};

	module_t *sap_protocol_module;

	/* Register the protocol */
	proto_sap_protocol = proto_register_protocol (
		"SAP NI Protocol",	/* name       */
		"SAPNI",	/* short name */
		"sapni"	/* abbrev     */
	);

	proto_register_field_array(proto_sap_protocol, hf, array_length(hf));
	proto_register_subtree_array(ett, array_length(ett));

	register_dissector("sapni", dissect_sap_protocol, proto_sap_protocol);

	/* Sub dissector code */
	sub_dissectors_table = register_dissector_table("sapni.port", "SAP Protocol Port", FT_UINT16, BASE_DEC);
	register_heur_dissector_list("sapni", &heur_subdissector_list);

	/* Register the preferences */
	sap_protocol_module = prefs_register_protocol(proto_sap_protocol, proto_reg_handoff_sap_protocol);

	range_convert_str(&global_sap_protocol_port_range, SAP_PROTOCOL_PORT_RANGE, MAX_TCP_PORT);
	prefs_register_range_preference(sap_protocol_module, "tcp_ports", "SAP NI Protocol TCP port numbers", "Port numbers used for SAP NI Protocol (default " SAP_PROTOCOL_PORT_RANGE ")", &global_sap_protocol_port_range, MAX_TCP_PORT);

	prefs_register_bool_preference(sap_protocol_module, "desegment", "Reassemble SAP NI Protocol messages spanning multiple TCP segments", "Whether the SAP NI Protocol dissector should reassemble messages spanning multiple TCP segments.", &global_sap_protocol_desegment);
}

/**
 * Helpers for dealing with the port range
 */
static void range_delete_callback (guint32 port)
{
	dissector_delete_uint("tcp.port", port, sap_protocol_handle);
}

static void range_add_callback (guint32 port)
{
	dissector_add_uint("tcp.port", port, sap_protocol_handle);
}

/**
 * Register Hand off for the SAP NI Protocol
 */
void
proto_reg_handoff_sap_protocol(void)
{
	static range_t *sap_protocol_port_range;
	static gboolean initialized = FALSE;

	if (!initialized) {
		sap_protocol_handle = find_dissector("sapni");
		initialized = TRUE;
	} else {
		range_foreach(sap_protocol_port_range, range_delete_callback);
		g_free(sap_protocol_port_range);
	}

	sap_protocol_port_range = range_copy(global_sap_protocol_port_range);
	range_foreach(sap_protocol_port_range, range_add_callback);
}