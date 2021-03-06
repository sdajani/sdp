#include <xc.h>
#include "Mavlink.h"
#include "Uart.h"
#include "Board.h"
#include "Xbee.h"
#include "Compas.h"

static int packet_drops = 0;
static mavlink_message_t msg;
static mavlink_status_t status;

#define MAV_NUMBER 15 // defines the MAV number, arbitrary
#define COMP_ID 15

void Mavlink_recieve(uint8_t uart_id){
    while(UART_isReceiveEmpty(uart_id) == FALSE){
        uint8_t c = UART_getChar(uart_id);
        //if a message can be deciphered
        if(mavlink_parse_char(MAVLINK_COMM_0, c, &msg, &status)) {
            switch(msg.msgid){
                case MAVLINK_MSG_ID_XBEE_HEARTBEAT:
                {
                    mavlink_xbee_heartbeat_t data;
                    mavlink_msg_xbee_heartbeat_decode(&msg, &data);
                    //call outside function to handle data
                    Xbee_recieved_message_heartbeat(&data);
                }break;
#ifdef XBEE_TEST
                case MAVLINK_MSG_ID_TEST_DATA:
                {
                    mavlink_test_data_t data;
                    mavlink_msg_test_data_decode(&msg, &data);
                    //call outside function to handle data
                    Xbee_message_data_test(&data);
                }break;
#endif
                case MAVLINK_MSG_ID_START_RESCUE:
                {
                    mavlink_start_rescue_t data;
                    mavlink_msg_start_rescue_decode(&msg, &data);
                    if(data.ack == TRUE){
                        Mavlink_send_ACK(XBEE_UART_ID, messageName_start_rescue);
                    }
                    Compas_recieve_start_rescue(&data);
                }break;
                case MAVLINK_MSG_ID_MAVLINK_ACK:
                {
                    mavlink_mavlink_ack_t data;
                    mavlink_msg_mavlink_ack_decode(&msg, &data);
                    Mavlink_recieve_ACK(&data);
                }break;
            }
        }
    }
    packet_drops += status.packet_rx_drop_count;
}
/*************************************************************************
 * SEND FUNCTIONS                                                        *
 *************************************************************************/

void Mavlink_send_ACK(uint8_t uart_id, uint8_t Message_Name){
    mavlink_message_t msg;
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    mavlink_msg_mavlink_ack_pack(MAV_NUMBER, COMP_ID, &msg, Message_Name);
    uint16_t length = mavlink_msg_to_send_buffer(buf, &msg);
    UART_putString(uart_id, buf, length);
}
void Mavlink_send_xbee_heartbeat(uint8_t uart_id, uint8_t data){
    mavlink_message_t msg;
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    mavlink_msg_xbee_heartbeat_pack(MAV_NUMBER, COMP_ID, &msg, TRUE, data);
    uint16_t length = mavlink_msg_to_send_buffer(buf, &msg);
    UART_putString(uart_id, buf, length);
}

void Mavlink_send_start_rescue(uint8_t uart_id, uint8_t ack, uint8_t status, float latitude, float longitude){
    mavlink_message_t msg;
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    mavlink_msg_start_rescue_pack(MAV_NUMBER, COMP_ID, &msg, ack, status, latitude, longitude);
    uint16_t length = mavlink_msg_to_send_buffer(buf, &msg);
    UART_putString(uart_id, buf, length);
    if(ack == TRUE){
        start_rescue.ACK_status = ACK_STATUS_WAIT;
        int x;
        for(x = 0; x <=MAVLINK_MAX_PACKET_LEN;x++){
            start_rescue.last_buf[x] = buf[x];
        }
        start_rescue.last_length = length;
        start_rescue.last_uart_id = uart_id;
    }
}

#ifdef XBEE_TEST
void Mavlink_send_Test_data(uint8_t uart_id, uint8_t data){
    mavlink_message_t msg;
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    mavlink_msg_test_data_pack(MAV_NUMBER, COMP_ID, &msg, data);
    uint16_t length = mavlink_msg_to_send_buffer(buf, &msg);
    UART_putString(uart_id, buf, length);
}
#endif


/*************************************************************************
 * RECIEVE FUNCTIONS                                                     *
 *************************************************************************/

void Mavlink_recieve_ACK(mavlink_mavlink_ack_t* packet){
    switch(packet->Message_Name){
        case messageName_start_rescue:
        {
            start_rescue.ACK_status = ACK_STATUS_RECIEVED;
        }break;
    }
}

void Compas_recieve_start_rescue(mavlink_start_rescue_t* packet){
    printf("Lat: %d Long: %d\n",packet->latitude,packet->longitude);
}

/*************************************************************************
 * PUBLIC FUNCTIONS                                                      *
 *************************************************************************/

void Mavlink_resend_message(ACK *message){
    UART_putString(message->last_uart_id, message->last_buf, message->last_length);
    message->ACK_status = ACK_STATUS_WAIT;
}