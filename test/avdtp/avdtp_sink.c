/*
 * Copyright (C) 2016 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */


#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btstack.h"
#include "avdtp.h"
#include "avdtp_sink.h"
#include "avdtp_util.h"
#include "avdtp_initiator.h"
#include "avdtp_acceptor.h"

static const char * default_avdtp_sink_service_name = "BTstack AVDTP Sink Service";
static const char * default_avdtp_sink_service_provider_name = "BTstack AVDTP Sink Service Provider";

static avdtp_device_t avdtp_sink;

static void avdtp_state_init(avdtp_device_t * device){
    device->stream_endpoints = NULL;
    device->service_mode = AVDTP_BASIC_SERVICE_MODE;
    device->state = AVDTP_DEVICE_IDLE;
    device->stream_endpoints_id_counter = 0;
    device->disconnect = 0;
}

static btstack_packet_handler_t avdtp_sink_callback;
static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avdtp_sink_run(void);

static void (*handle_media_data)(avdtp_stream_endpoint_t * stream_endpoint, uint8_t *packet, uint16_t size);    

void a2dp_sink_create_sdp_record(uint8_t * service,  uint32_t service_record_handle, uint16_t supported_features, const char * service_name, const char * service_provider_name){
    uint8_t* attribute;
    de_create_sequence(service);

    // 0x0000 "Service Record Handle"
    de_add_number(service, DE_UINT, DE_SIZE_16, SDP_ServiceRecordHandle);
    de_add_number(service, DE_UINT, DE_SIZE_32, service_record_handle);

    // 0x0001 "Service Class ID List"
    de_add_number(service,  DE_UINT, DE_SIZE_16, SDP_ServiceClassIDList);
    attribute = de_push_sequence(service);
    {
        de_add_number(attribute, DE_UUID, DE_SIZE_16, AUDIO_SINK_GROUP); 
    }
    de_pop_sequence(service, attribute);

    // 0x0004 "Protocol Descriptor List"
    de_add_number(service,  DE_UINT, DE_SIZE_16, SDP_ProtocolDescriptorList);
    attribute = de_push_sequence(service);
    {
        uint8_t* l2cpProtocol = de_push_sequence(attribute);
        {
            de_add_number(l2cpProtocol,  DE_UUID, DE_SIZE_16, SDP_L2CAPProtocol);
            de_add_number(l2cpProtocol,  DE_UINT, DE_SIZE_16, PSM_AVDTP);  
        }
        de_pop_sequence(attribute, l2cpProtocol);
        
        uint8_t* avProtocol = de_push_sequence(attribute);
        {
            de_add_number(avProtocol,  DE_UUID, DE_SIZE_16, PSM_AVDTP);  // avProtocol_service
            de_add_number(avProtocol,  DE_UINT, DE_SIZE_16,  0x0103);  // version
        }
        de_pop_sequence(attribute, avProtocol);
    }
    de_pop_sequence(service, attribute);

    // 0x0005 "Public Browse Group"
    de_add_number(service,  DE_UINT, DE_SIZE_16, SDP_BrowseGroupList); // public browse group
    attribute = de_push_sequence(service);
    {
        de_add_number(attribute,  DE_UUID, DE_SIZE_16, SDP_PublicBrowseGroup);
    }
    de_pop_sequence(service, attribute);

    // 0x0009 "Bluetooth Profile Descriptor List"
    de_add_number(service,  DE_UINT, DE_SIZE_16, SDP_BluetoothProfileDescriptorList);
    attribute = de_push_sequence(service);
    {
        uint8_t *a2dProfile = de_push_sequence(attribute);
        {
            de_add_number(a2dProfile,  DE_UUID, DE_SIZE_16, ADVANCED_AUDIO_DISTRIBUTION); 
            de_add_number(a2dProfile,  DE_UINT, DE_SIZE_16, 0x0103); 
        }
        de_pop_sequence(attribute, a2dProfile);
    }
    de_pop_sequence(service, attribute);


    // 0x0100 "Service Name"
    de_add_number(service,  DE_UINT, DE_SIZE_16, 0x0100);
    if (service_name){
        de_add_data(service,  DE_STRING, strlen(service_name), (uint8_t *) service_name);
    } else {
        de_add_data(service,  DE_STRING, strlen(default_avdtp_sink_service_name), (uint8_t *) default_avdtp_sink_service_name);
    }

    // 0x0100 "Provider Name"
    de_add_number(service,  DE_UINT, DE_SIZE_16, 0x0102);
    if (service_provider_name){
        de_add_data(service,  DE_STRING, strlen(service_provider_name), (uint8_t *) service_provider_name);
    } else {
        de_add_data(service,  DE_STRING, strlen(default_avdtp_sink_service_provider_name), (uint8_t *) default_avdtp_sink_service_provider_name);
    }

    // 0x0311 "Supported Features"
    de_add_number(service, DE_UINT, DE_SIZE_16, 0x0311);
    de_add_number(service, DE_UINT, DE_SIZE_16, supported_features);
}

static avdtp_stream_endpoint_t * get_avdtp_stream_endpoint_for_seid(uint16_t seid){
    btstack_linked_list_iterator_t it;    
    btstack_linked_list_iterator_init(&it, (btstack_linked_list_t *) &avdtp_sink.stream_endpoints);
    while (btstack_linked_list_iterator_has_next(&it)){
        avdtp_stream_endpoint_t * stream_endpoint = (avdtp_stream_endpoint_t *)btstack_linked_list_iterator_next(&it);
        if (stream_endpoint->sep.seid == seid){
            return stream_endpoint;
        }
    }
    return NULL;
}

uint8_t avdtp_sink_register_stream_endpoint(avdtp_sep_type_t sep_type, avdtp_media_type_t media_type){
    avdtp_stream_endpoint_t * stream_endpoint = btstack_memory_avdtp_stream_endpoint_get();
    avdtp_sink.stream_endpoints_id_counter++;
    stream_endpoint->sep.seid = avdtp_sink.stream_endpoints_id_counter;
    stream_endpoint->sep.in_use = 0;
    stream_endpoint->sep.media_type = media_type;
    stream_endpoint->sep.type = sep_type;
    btstack_linked_list_add(&avdtp_sink.stream_endpoints, (btstack_linked_item_t *) stream_endpoint);
    return stream_endpoint->sep.seid;
}


void avdtp_sink_register_media_transport_category(uint8_t seid){
    avdtp_stream_endpoint_t * stream_endpoint = get_avdtp_stream_endpoint_for_seid(seid);
    if (!stream_endpoint){
        log_error("avdtp_sink_register_media_transport_category: stream endpoint with seid %d is not registered", seid);
        return;
    }
    uint16_t bitmap = store_bit16(stream_endpoint->sep.registered_service_categories, AVDTP_MEDIA_TRANSPORT, 1);
    stream_endpoint->sep.registered_service_categories = bitmap;
    printf("registered services AVDTP_MEDIA_TRANSPORT(%d) %02x\n", AVDTP_MEDIA_TRANSPORT, stream_endpoint->sep.registered_service_categories);
}

void avdtp_sink_register_reporting_category(uint8_t seid){
    avdtp_stream_endpoint_t * stream_endpoint = get_avdtp_stream_endpoint_for_seid(seid);
    if (!stream_endpoint){
        log_error("avdtp_sink_register_media_transport_category: stream endpoint with seid %d is not registered", seid);
        return;
    }
    uint16_t bitmap = store_bit16(stream_endpoint->sep.registered_service_categories, AVDTP_REPORTING, 1);
    stream_endpoint->sep.registered_service_categories = bitmap;
}

void avdtp_sink_register_delay_reporting_category(uint8_t seid){
    avdtp_stream_endpoint_t * stream_endpoint = get_avdtp_stream_endpoint_for_seid(seid);
    if (!stream_endpoint){
        log_error("avdtp_sink_register_media_transport_category: stream endpoint with seid %d is not registered", seid);
        return;
    }
    uint16_t bitmap = store_bit16(stream_endpoint->sep.registered_service_categories, AVDTP_DELAY_REPORTING, 1);
    stream_endpoint->sep.registered_service_categories = bitmap;
}

void avdtp_sink_register_recovery_category(uint8_t seid, uint8_t maximum_recovery_window_size, uint8_t maximum_number_media_packets){
    avdtp_stream_endpoint_t * stream_endpoint = get_avdtp_stream_endpoint_for_seid(seid);
    if (!stream_endpoint){
        log_error("avdtp_sink_register_media_transport_category: stream endpoint with seid %d is not registered", seid);
        return;
    }
    uint16_t bitmap = store_bit16(stream_endpoint->sep.registered_service_categories, AVDTP_RECOVERY, 1);
    stream_endpoint->sep.registered_service_categories = bitmap;
    stream_endpoint->sep.capabilities.recovery.recovery_type = 0x01; // 0x01 = RFC2733
    stream_endpoint->sep.capabilities.recovery.maximum_recovery_window_size = maximum_recovery_window_size;
    stream_endpoint->sep.capabilities.recovery.maximum_number_media_packets = maximum_number_media_packets;
}

void avdtp_sink_register_content_protection_category(uint8_t seid, uint8_t cp_type_lsb,  uint8_t cp_type_msb, const uint8_t * cp_type_value, uint8_t cp_type_value_len){
    avdtp_stream_endpoint_t * stream_endpoint = get_avdtp_stream_endpoint_for_seid(seid);
    if (!stream_endpoint){
        log_error("avdtp_sink_register_media_transport_category: stream endpoint with seid %d is not registered", seid);
        return;
    }
    uint16_t bitmap = store_bit16(stream_endpoint->sep.registered_service_categories, AVDTP_CONTENT_PROTECTION, 1);
    stream_endpoint->sep.registered_service_categories = bitmap;
    stream_endpoint->sep.capabilities.content_protection.cp_type_lsb = cp_type_lsb;
    stream_endpoint->sep.capabilities.content_protection.cp_type_msb = cp_type_msb;
    stream_endpoint->sep.capabilities.content_protection.cp_type_value = cp_type_value;
    stream_endpoint->sep.capabilities.content_protection.cp_type_value_len = cp_type_value_len;
}

void avdtp_sink_register_header_compression_category(uint8_t seid, uint8_t back_ch, uint8_t media, uint8_t recovery){
    avdtp_stream_endpoint_t * stream_endpoint = get_avdtp_stream_endpoint_for_seid(seid);
    if (!stream_endpoint){
        log_error("avdtp_sink_register_media_transport_category: stream endpoint with seid %d is not registered", seid);
        return;
    }
    uint16_t bitmap = store_bit16(stream_endpoint->sep.registered_service_categories, AVDTP_HEADER_COMPRESSION, 1);
    stream_endpoint->sep.registered_service_categories = bitmap;
    stream_endpoint->sep.capabilities.header_compression.back_ch = back_ch;
    stream_endpoint->sep.capabilities.header_compression.media = media;
    stream_endpoint->sep.capabilities.header_compression.recovery = recovery;
}

void avdtp_sink_register_media_codec_category(uint8_t seid, avdtp_media_type_t media_type, avdtp_media_codec_type_t media_codec_type, const uint8_t * media_codec_info, uint16_t media_codec_info_len){
    avdtp_stream_endpoint_t * stream_endpoint = get_avdtp_stream_endpoint_for_seid(seid);
    if (!stream_endpoint){
        log_error("avdtp_sink_register_media_transport_category: stream endpoint with seid %d is not registered", seid);
        return;
    }
    uint16_t bitmap = store_bit16(stream_endpoint->sep.registered_service_categories, AVDTP_MEDIA_CODEC, 1);
    stream_endpoint->sep.registered_service_categories = bitmap;
    printf("registered services AVDTP_MEDIA_CODEC(%d) %02x\n", AVDTP_MEDIA_CODEC, stream_endpoint->sep.registered_service_categories);
    stream_endpoint->sep.capabilities.media_codec.media_type = media_type;
    stream_endpoint->sep.capabilities.media_codec.media_codec_type = media_codec_type;
    stream_endpoint->sep.capabilities.media_codec.media_codec_information = media_codec_info;
    stream_endpoint->sep.capabilities.media_codec.media_codec_information_len = media_codec_info_len;
}

void avdtp_sink_register_multiplexing_category(uint8_t seid, uint8_t fragmentation){
    avdtp_stream_endpoint_t * stream_endpoint = get_avdtp_stream_endpoint_for_seid(seid);
    if (!stream_endpoint){
        log_error("avdtp_sink_register_media_transport_category: stream endpoint with seid %d is not registered", seid);
        return;
    }
    uint16_t bitmap = store_bit16(stream_endpoint->sep.registered_service_categories, AVDTP_MULTIPLEXING, 1);
    stream_endpoint->sep.registered_service_categories = bitmap;
    stream_endpoint->sep.capabilities.multiplexing_mode.fragmentation = fragmentation;
}

// // media, reporting. recovery
// void avdtp_sink_register_media_transport_identifier_for_multiplexing_category(uint8_t seid, uint8_t fragmentation){

// }

static avdtp_stream_endpoint_t * get_avdtp_stream_endpoint_for_l2cap_cid(uint16_t l2cap_cid){
    btstack_linked_list_iterator_t it;    
    btstack_linked_list_iterator_init(&it, (btstack_linked_list_t *) &avdtp_sink.stream_endpoints);
    while (btstack_linked_list_iterator_has_next(&it)){
        avdtp_stream_endpoint_t * stream_endpoint = (avdtp_stream_endpoint_t *)btstack_linked_list_iterator_next(&it);
        if (stream_endpoint->l2cap_media_cid == l2cap_cid){
            return stream_endpoint;
        }
        if (stream_endpoint->l2cap_reporting_cid == l2cap_cid){
            return stream_endpoint;
        }   
    }
    return NULL;
}

static void avdtp_sink_remove_connection_context(avdtp_stream_endpoint_t * stream_endpoint){
    btstack_linked_list_remove(&avdtp_sink.stream_endpoints, (btstack_linked_item_t*) stream_endpoint);   
}

static void handle_l2cap_signaling_data_packet(uint8_t *packet, uint16_t size){
    if (size < 2) {
        log_error("l2cap data packet too small");
        return;
    }
    
    avdtp_signaling_packet_header_t signaling_header;
    avdtp_read_signaling_header(&signaling_header, packet, size);

    // printf("SIGNALING HEADER: tr_label %d, packet_type %d, msg_type %d, signal_identifier %02x\n", 
    //     signaling_header.transaction_label, signaling_header.packet_type, signaling_header.message_type, signaling_header.signal_identifier);

    btstack_linked_list_iterator_t it;    
    btstack_linked_list_iterator_init(&it, (btstack_linked_list_t *) &avdtp_sink.stream_endpoints);
    
    int request_to_send = 0;
    while (btstack_linked_list_iterator_has_next(&it) && !request_to_send){
        avdtp_stream_endpoint_t * stream_endpoint = (avdtp_stream_endpoint_t *)btstack_linked_list_iterator_next(&it);
        if (stream_endpoint->state == AVDTP_CONFIGURATION_SUBSTATEMACHINE){
            if (avdtp_initiator_stream_config_subsm_is_done(stream_endpoint) || avdtp_acceptor_stream_config_subsm_is_done(stream_endpoint)){
                printf("AVDTP_CONFIGURATION_SUBSTATEMACHINE -> AVDTP_CONFIGURED\n");
                stream_endpoint->state = AVDTP_CONFIGURED;
            }
        }
        request_to_send = 1;
        switch (stream_endpoint->state){
            case AVDTP_CONFIGURATION_SUBSTATEMACHINE:
                if (signaling_header.message_type == AVDTP_CMD_MSG){
                    request_to_send = avdtp_acceptor_stream_config_subsm(&avdtp_sink, stream_endpoint, packet, size);
                    break;
                } 
                request_to_send = avdtp_initiator_stream_config_subsm(&avdtp_sink, stream_endpoint, packet, size);
                break;
            case AVDTP_CONFIGURED:
                switch (signaling_header.signal_identifier){
                    case AVDTP_SI_OPEN:
                        printf("AVDTP_CONFIGURED -> AVDTP_W2_ANSWER_OPEN_STREAM %d\n", signaling_header.transaction_label);
                        if (stream_endpoint->sep.seid != packet[2] >> 2) return;
                        stream_endpoint->state = AVDTP_W2_ANSWER_OPEN_STREAM;
                        avdtp_sink.acceptor_transaction_label = signaling_header.transaction_label;
                        l2cap_request_can_send_now_event(avdtp_sink.l2cap_signaling_cid);
                        break;
                    default:
                        request_to_send = 0;
                        printf("AVDTP_CONFIGURED -> NOT IMPLEMENTED signal_identifier %d, state %d\n", signaling_header.signal_identifier, stream_endpoint->state);
                        break;
                }
                break;
            case AVDTP_OPEN:
                switch (signaling_header.signal_identifier){
                    case AVDTP_SI_START:
                        printf("AVDTP_OPEN -> AVDTP_W2_ANSWER_START_SINGLE_STREAM\n");
                        if (stream_endpoint->sep.seid != packet[2] >> 2) return;
                        stream_endpoint->sep.in_use  = 1;
                        stream_endpoint->state = AVDTP_W2_ANSWER_START_SINGLE_STREAM;
                        avdtp_sink.acceptor_transaction_label = signaling_header.transaction_label;
                        l2cap_request_can_send_now_event(avdtp_sink.l2cap_signaling_cid);
                        break;
                    default:
                        request_to_send = 0;
                        printf("AVDTP_OPEN -> NOT IMPLEMENTED signal_identifier %d\n", signaling_header.signal_identifier);
                        break;
                }
                break;
            default:
                request_to_send = 0;
                printf("handle_l2cap_signaling_data_packet: state %d -> NOT IMPLEMENTED signal_identifier %d\n", stream_endpoint->state, signaling_header.signal_identifier);
                // printf_hexdump( packet, size );
                break;
        }

    }
    if (request_to_send){
        printf("request_to_send cid %02x\n", avdtp_sink.l2cap_signaling_cid);
        l2cap_request_can_send_now_event(avdtp_sink.l2cap_signaling_cid);
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    bd_addr_t event_addr;
    hci_con_handle_t con_handle;
    uint16_t psm;
    uint16_t remote_cid;
    uint16_t local_cid;
    avdtp_stream_endpoint_t * stream_endpoint = NULL;

    switch (packet_type) {
        case L2CAP_DATA_PACKET:
            if (channel == avdtp_sink.l2cap_signaling_cid){
                handle_l2cap_signaling_data_packet(packet, size);
                break;
            } 
            stream_endpoint = get_avdtp_stream_endpoint_for_l2cap_cid(channel);
            if (!stream_endpoint){
                log_error("avdtp packet handler L2CAP_DATA_PACKET: connection for local cid 0x%02x not found", channel);
                break;
            }

            if (channel == stream_endpoint->l2cap_media_cid){
                (*handle_media_data)(stream_endpoint, packet, size);
            } else if (channel == stream_endpoint->l2cap_reporting_cid){
                // TODO
                printf("L2CAP_DATA_PACKET for reporting: NOT IMPLEMENTED\n");
            } else if (channel == stream_endpoint->l2cap_recovery_cid){
                // TODO
                printf("L2CAP_DATA_PACKET for recovery: NOT IMPLEMENTED\n");
            } else {
                log_error("avdtp packet handler L2CAP_DATA_PACKET: local cid 0x%02x not found", channel);
            }
            break;
            
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(packet)) {
                case L2CAP_EVENT_INCOMING_CONNECTION:
                    l2cap_event_incoming_connection_get_address(packet, event_addr);
                    local_cid  = l2cap_event_incoming_connection_get_local_cid(packet); 

                    if (avdtp_sink.state == AVDTP_DEVICE_IDLE){
                        // accept signaling channel
                        memcpy(avdtp_sink.remote_addr, event_addr, 6);
                        avdtp_sink.state = AVDTP_W4_L2CAP_FOR_SIGNALING_CONNECTED;
                        l2cap_accept_connection(local_cid);
                        break;
                    } 
                    printf("L2CAP_EVENT_INCOMING_CONNECTION \n");
                    con_handle = l2cap_event_incoming_connection_get_handle(packet); 
                    psm        = l2cap_event_incoming_connection_get_psm(packet); 
                    remote_cid = l2cap_event_incoming_connection_get_remote_cid(packet); 
                    
                    printf("L2CAP_EVENT_INCOMING_CONNECTION %s, handle 0x%02x, psm 0x%02x, local cid 0x%02x, remote cid 0x%02x\n",
                        bd_addr_to_str(event_addr), con_handle, psm, local_cid, remote_cid);
                    
                    switch (avdtp_sink.service_mode){
                        case AVDTP_MULTIPLEXING_SERVICE_MODE:
                            printf("AVDTP_MULTIPLEXING_SERVICE_MODE not implemented\n");
                            return;
                        case AVDTP_BASIC_SERVICE_MODE:
                            break;
                    } 

                    // if multiplexing os not used, multiple L2CAP channels are created in a fix order to the same PSM.
                    // They have different usages:
                    // 1. signaling
                    // 2. media transport
                    // 3. reporting
                    // 4. recovery
                    
                    if (avdtp_sink.l2cap_signaling_cid == 0){
                        avdtp_sink.state = AVDTP_W4_L2CAP_FOR_SIGNALING_CONNECTED;
                        l2cap_accept_connection(local_cid);
                        return;
                    }

                    stream_endpoint = get_avdtp_stream_endpoint_for_l2cap_cid(local_cid);
                    if (!stream_endpoint){
                        log_error("avdtp sink: L2CAP_EVENT_CHANNEL_OPENED, no stream endpoint is associated with cid %02x", local_cid);
                        return;
                    }

                    if (stream_endpoint->l2cap_media_cid == 0){
                         // if (stream_endpoint->state != AVDTP_W2_ANSWER_OPEN_STREAM) break;
                        printf("incoming %d: AVDTP_W2_ANSWER_OPEN_STREAM\n", stream_endpoint->state);
                        stream_endpoint->state = AVDTP_W4_L2CAP_FOR_MEDIA_CONNECTED;
                        l2cap_accept_connection(local_cid);
                        return;
                    }
                    
                    if (stream_endpoint->l2cap_reporting_cid == 0){
                        stream_endpoint->l2cap_reporting_cid = local_cid;
                        printf("TODO enable reporting, set state\n");
                        return;
                    }   
                    
                    if (stream_endpoint->l2cap_recovery_cid == 0){
                        stream_endpoint->l2cap_recovery_cid = local_cid;
                        printf("TODO enable recovery, set state\n");
                        return;
                    }
                    break;
                    
                case L2CAP_EVENT_CHANNEL_OPENED:
                    // inform about new l2cap connection
                    l2cap_event_channel_opened_get_address(packet, event_addr);
                    
                    if (l2cap_event_channel_opened_get_status(packet)){
                        log_error("L2CAP connection to device %s failed. status code 0x%02x", 
                            bd_addr_to_str(event_addr), l2cap_event_channel_opened_get_status(packet));
                        break;
                    }
                    psm = l2cap_event_channel_opened_get_psm(packet); 
                    if (psm != PSM_AVDTP){
                        log_error("unexpected PSM - Not implemented yet, avdtp sink: L2CAP_EVENT_CHANNEL_OPENED");
                        return;
                    }
                    
                    con_handle = l2cap_event_channel_opened_get_handle(packet);
                    local_cid = l2cap_event_channel_opened_get_local_cid(packet);

                    printf("L2CAP_EVENT_CHANNEL_OPENED: Channel successfully opened: %s, handle 0x%02x, psm 0x%02x, local cid 0x%02x, remote cid 0x%02x\n",
                           bd_addr_to_str(event_addr), con_handle, psm, local_cid,  l2cap_event_channel_opened_get_remote_cid(packet));

                    if (psm != PSM_AVDTP) break;

                    // if multiplexing os not used, multiple L2CAP channels are created in a fix order to the same PSM.
                    // They have different usages:
                    // 1. signaling
                    // 2. media transport
                    // 3. reporting
                    // 4. recovery
                    if (avdtp_sink.state == AVDTP_W4_L2CAP_FOR_SIGNALING_CONNECTED){
                        avdtp_sink.state = AVDTP_DEVICE_CONNECTED;
                        printf("AVDTP_DEVICE_CONNECTED, signaling %02x\n", local_cid);
                        avdtp_sink.l2cap_signaling_cid = local_cid; 
                        btstack_linked_list_iterator_t it;    
                        btstack_linked_list_iterator_init(&it, (btstack_linked_list_t *) &avdtp_sink.stream_endpoints);
                        while (btstack_linked_list_iterator_has_next(&it)){
                            stream_endpoint = (avdtp_stream_endpoint_t *)btstack_linked_list_iterator_next(&it);
                            stream_endpoint->state = AVDTP_CONFIGURATION_SUBSTATEMACHINE;
                            avdtp_initiator_stream_config_subsm_init(stream_endpoint);
                            avdtp_acceptor_stream_config_subsm_init(stream_endpoint);
                        }
                        avdtp_sink.initiator_transaction_label++;
                        l2cap_request_can_send_now_event(avdtp_sink.l2cap_signaling_cid);
                        break;
                    }

                    stream_endpoint = get_avdtp_stream_endpoint_for_l2cap_cid(local_cid);
                    if (!stream_endpoint){
                        printf("L2CAP_EVENT_CHANNEL_OPENED, no stream endpoint is associated with cid %02x\n", local_cid);
                        return;
                    }
                    
                    if (stream_endpoint->l2cap_media_cid == 0 || stream_endpoint->l2cap_media_cid == local_cid){
                        printf("L2CAP_EVENT_CHANNEL_OPENED Media, state %d \n", stream_endpoint->state);
                        if (stream_endpoint->state != AVDTP_W4_L2CAP_FOR_MEDIA_CONNECTED) break;
                        stream_endpoint->l2cap_media_cid = local_cid;
                        stream_endpoint->state = AVDTP_OPEN;
                        break;
                    }

                    if (stream_endpoint->l2cap_reporting_cid == 0 || stream_endpoint->l2cap_reporting_cid == local_cid){
                        printf("L2CAP_EVENT_CHANNEL_OPENED reporting, state %d \n", stream_endpoint->state);
                        stream_endpoint->l2cap_reporting_cid = local_cid;
                        break;
                    }   

                    if (stream_endpoint->l2cap_recovery_cid == 0 || stream_endpoint->l2cap_recovery_cid == local_cid){
                        printf("L2CAP_EVENT_CHANNEL_OPENED recovery, state %d \n", stream_endpoint->state);
                        stream_endpoint->l2cap_recovery_cid = local_cid;
                        break;
                    }  
                    break;
                
                case L2CAP_EVENT_CHANNEL_CLOSED:
                    // data: event (8), len(8), channel (16)
                    local_cid = l2cap_event_channel_closed_get_local_cid(packet);
                    stream_endpoint = get_avdtp_stream_endpoint_for_l2cap_cid(local_cid);
                    if (!stream_endpoint) return;

                    if (stream_endpoint->l2cap_recovery_cid == local_cid){
                        log_info("L2CAP_EVENT_CHANNEL_CLOSED recovery cid 0x%0x", local_cid);
                        stream_endpoint->l2cap_recovery_cid = 0;
                        break;
                    }
                    
                    if (stream_endpoint->l2cap_reporting_cid == local_cid){
                        log_info("L2CAP_EVENT_CHANNEL_CLOSED reporting cid 0x%0x", local_cid);
                        stream_endpoint->l2cap_reporting_cid = 0;
                        break;
                    }

                    if (stream_endpoint->l2cap_media_cid == local_cid){
                        log_info("L2CAP_EVENT_CHANNEL_CLOSED media cid 0x%0x", local_cid);
                        stream_endpoint->state = AVDTP_CONFIGURED;
                        stream_endpoint->l2cap_media_cid = 0;
                        break;
                    }

                    if (avdtp_sink.l2cap_signaling_cid == local_cid){
                        log_info("L2CAP_EVENT_CHANNEL_CLOSED signaling cid 0x%0x", local_cid);
                        stream_endpoint->state = AVDTP_IDLE;
                        avdtp_sink_remove_connection_context(stream_endpoint);
                        break;
                    }
                    break;

                case HCI_EVENT_DISCONNECTION_COMPLETE:
                    break;
                case L2CAP_EVENT_CAN_SEND_NOW:
                    break;
                default:
                    printf("unknown HCI event type %02x\n", hci_event_packet_get_type(packet));
                    break;
            }
            break;
            
        default:
            // other packet type
            break;
    }
    avdtp_sink_run();

}

// TODO: find out which security level is needed, and replace LEVEL_0 in avdtp_sink_init
void avdtp_sink_init(void){
    avdtp_state_init(&avdtp_sink);
    l2cap_register_service(&packet_handler, PSM_AVDTP, 0xffff, LEVEL_0);
}

void avdtp_sink_register_media_handler(void (*callback)(avdtp_stream_endpoint_t * stream_endpoint, uint8_t *packet, uint16_t size)){
    if (callback == NULL){
        log_error("avdtp_sink_register_media_handler called with NULL callback");
        return;
    }
    handle_media_data = callback;
}

void avdtp_sink_register_packet_handler(btstack_packet_handler_t callback){
    if (callback == NULL){
        log_error("avdtp_sink_register_packet_handler called with NULL callback");
        return;
    }
    avdtp_sink_callback = callback;
}

static void avdtp_sink_run_for_stream_endpoint(avdtp_stream_endpoint_t * stream_endpoint){
    if (stream_endpoint->state < AVDTP_OPEN){
        if (stream_endpoint->state == AVDTP_CONFIGURATION_SUBSTATEMACHINE){
            if (avdtp_initiator_stream_config_subsm_is_done(stream_endpoint) || avdtp_acceptor_stream_config_subsm_is_done(stream_endpoint)){
                printf("AVDTP_CONFIGURATION_SUBSTATEMACHINE -> AVDTP_CONFIGURED\n");
                stream_endpoint->state = AVDTP_CONFIGURED;
            }
        }
    
        // signaling
        if (!l2cap_can_send_packet_now(avdtp_sink.l2cap_signaling_cid)) {
            //printf("avdtp_sink_run: request cannot send for 0x%02x\n", avdtp_sink.l2cap_signaling_cid);
            return;
        }
    
        switch (stream_endpoint->state){
            case AVDTP_CONFIGURATION_SUBSTATEMACHINE:
                if (!avdtp_acceptor_stream_config_subsm_run(&avdtp_sink, stream_endpoint)) {
                    if (!avdtp_initiator_stream_config_subsm_run(&avdtp_sink, stream_endpoint)) break;
                }
                return;
            case AVDTP_W2_ANSWER_OPEN_STREAM:
                printf("AVDTP_W2_ANSWER_OPEN_STREAM -> AVDTP_OPEN\n");
                stream_endpoint->state = AVDTP_W4_L2CAP_FOR_MEDIA_CONNECTED;
                avdtp_acceptor_send_accept_response(avdtp_sink.l2cap_signaling_cid, AVDTP_SI_OPEN, avdtp_sink.acceptor_transaction_label);
                break;
            case AVDTP_W2_ANSWER_START_SINGLE_STREAM:
                printf("AVDTP_W2_ANSWER_START_SINGLE_STREAM -> AVDTP_W4_STREAMING_CONNECTION_OPEN\n");
                stream_endpoint->state = AVDTP_W4_STREAMING_CONNECTION_OPEN;
                avdtp_acceptor_send_accept_response(avdtp_sink.l2cap_signaling_cid, AVDTP_SI_START, avdtp_sink.acceptor_transaction_label);
                break;
            default:
                printf("avdtp_sink_run: state %d -> NOT IMPLEMENTED\n", stream_endpoint->state);
                break;
        }
    }
}

static void avdtp_sink_run(void){
    btstack_linked_list_iterator_t it;    
    btstack_linked_list_iterator_init(&it, (btstack_linked_list_t *) &avdtp_sink.stream_endpoints);
    while (btstack_linked_list_iterator_has_next(&it)){
        avdtp_stream_endpoint_t * stream_endpoint = (avdtp_stream_endpoint_t *)btstack_linked_list_iterator_next(&it);
        
        if (stream_endpoint->disconnect){
            switch (stream_endpoint->state){
                case AVDTP_IDLE:
                case AVDTP_CONFIGURATION_SUBSTATEMACHINE:
                case AVDTP_CONFIGURED:
                case AVDTP_W4_L2CAP_FOR_MEDIA_DISCONNECTED:
                    stream_endpoint->disconnect = 0;
                    break;
                case AVDTP_W2_ANSWER_OPEN_STREAM:
                    stream_endpoint->disconnect = 0;
                    stream_endpoint->state = AVDTP_CONFIGURED;
                    break;
                case AVDTP_W4_L2CAP_FOR_MEDIA_CONNECTED:
                    break;
                default:
                    stream_endpoint->disconnect = 0;
                    stream_endpoint->state = AVDTP_W4_L2CAP_FOR_MEDIA_DISCONNECTED;
                    l2cap_disconnect(stream_endpoint->l2cap_media_cid, 0);
                    return;
            }
        }
    }

    if (avdtp_sink.disconnect == 1){
        avdtp_sink.disconnect = 0;
        avdtp_sink.state = AVDTP_W4_L2CAP_FOR_SIGNALING_DISCONNECTED;
        l2cap_disconnect(avdtp_sink.l2cap_signaling_cid, 0);
        return;
    }
    
    btstack_linked_list_iterator_init(&it, (btstack_linked_list_t *) &avdtp_sink.stream_endpoints);
    while (btstack_linked_list_iterator_has_next(&it)){
        avdtp_stream_endpoint_t * stream_endpoint = (avdtp_stream_endpoint_t *)btstack_linked_list_iterator_next(&it);
        avdtp_sink_run_for_stream_endpoint(stream_endpoint);
        // l2cap_request_can_send_now_event(avdtp_sink.l2cap_signaling_cid);
    }
}

void avdtp_sink_connect(bd_addr_t bd_addr){
    if (avdtp_sink.state != AVDTP_DEVICE_IDLE) return;
    memcpy(avdtp_sink.remote_addr, bd_addr, 6);
    avdtp_sink.state = AVDTP_W4_L2CAP_FOR_SIGNALING_CONNECTED;
    l2cap_create_channel(packet_handler, avdtp_sink.remote_addr, PSM_AVDTP, 0xffff, NULL);
}

void avdtp_sink_disconnect(uint16_t con_handle){
    if (avdtp_sink.state == AVDTP_DEVICE_IDLE) return;
    if (avdtp_sink.state == AVDTP_W4_L2CAP_FOR_SIGNALING_DISCONNECTED) return;
    
    avdtp_sink.disconnect = 1;

    btstack_linked_list_iterator_t it;    
    btstack_linked_list_iterator_init(&it, (btstack_linked_list_t *) &avdtp_sink.stream_endpoints);
    while (btstack_linked_list_iterator_has_next(&it)){
        avdtp_stream_endpoint_t * stream_endpoint = (avdtp_stream_endpoint_t *)btstack_linked_list_iterator_next(&it);
        stream_endpoint->disconnect = stream_endpoint->state != AVDTP_IDLE;
    }
    avdtp_sink_run();
}


