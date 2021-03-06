/*
 * File:   Gps.c
 * Author: David Goodman
 *
 * Created on February 4, 2013, 9:19 PM
 */

#include <xc.h>
#include <stdio.h>
#include <plib.h>
#include "Serial.h"
#include "Timer.h"
#include "Board.h"
#include "Uart.h"
#include "Gps.h"



/***********************************************************************
 * PRIVATE DEFINITIONS                                                 *
 ***********************************************************************/
//#define DEBUG
//#define DEBUG_STATE


#define START_BAUDRATE      38400 // (baud)
#define DESIRED_BAUDRATE    START_BAUDRATE

#define GPS_UART_ID         UART2_ID
#define GPS_UART_BAUDRATE   START_BAUDRATE


#define RAW_BUFFER_SIZE         255 // (bytes) maximum gps message size
#define CHECKSUM_BYTES          2 // (bytes) number in checksum

// Field indexes ( see pg. 60 in ublox UBX protocol specifications)
#define SYNC1_INDEX             0 // index of first sync byte
#define SYNC2_INDEX             1 // index of second sync byte
#define CLASS_INDEX             2 // index of class byte
#define ID_INDEX                3 // index of id byte
#define LENGTH1_INDEX           4 // start of length field (2 bytes long)
#define LENGTH2_INDEX           5 // end of length field (2 bytes long)
#define PAYLOAD_INDEX           6 // start of the payload

// Field constants
#define SYNC1_CHAR              0xB5 // first byte in message
#define SYNC2_CHAR              0x62 // second byte in message
#define NAV_CLASS               0x01 // navigation message class
#define NAV_POSLLH_ID           0x02 // geodetic postion message id+
#define NAV_STATUS_ID           0x03 // receiver navigation status (fix/nofix)
#define NAV_VELOCITY_ID         0x12 // velocity NED navigation message

#define NOFIX_STATUS            0x00

// GPS connection timeout for packet not seen
#define DELAY_TIMEOUT           5000

// unpacking functions
#define UNPACK_LITTLE_ENDIAN_32(data, start) \
    ((uint32_t)(data[start] + ((uint32_t)data[start+1] << 8) \
    + ((uint32_t)data[start+2] << 16) + ((uint32_t)data[start+3] << 24)))

#define MM_TO_M(unit)                   ((float)unit/1000)
#define CM_TO_M(unit)                   ((float)unit/100)
#define COORDINATE_TO_DECIMAL(coord)    ((float)coord/10000000)
#define ALTITUDE_TO_DECIMAL(alt)        (MM_TO_M(alt))
#define HEADING_TO_DEGREE(heading)      ((float)heading/100000)


/**********************************************************************
 * PRIVATE VARIABLES                                                  *
 **********************************************************************/
BOOL gpsInitialized = FALSE;
static enum {
    STATE_IDLE      = 0x0,
    STATE_READ      = 0x1, // Reading GPS packet from UART
    STATE_PARSE     = 0x2, // Parsing a received GPS packet
} state;

uint8_t rawMessage[RAW_BUFFER_SIZE];
uint8_t byteIndex = 0, messageLength = LENGTH2_INDEX + 1,
        messageClass = 0, messageId = 0, gpsStatus = NOFIX_STATUS;


BOOL hasNewMessage = FALSE, isConnected = FALSE, isUsingError = FALSE,
    hasPosition = FALSE;

// Variables read from the GPS
int32_t heading;

struct {
    int32_t latitude, longitude, altitude;
} geodetic;

struct {
    int32_t north, east;
} velocity;

// TODO shrink these variables if possible
struct {
    int32_t latitude, longitude;
} error;




/**********************************************************************
 * PRIVATE PROTOTYPES                                                 *
 **********************************************************************/

BOOL hasNewByte();
void startReadState();
void startIdleState();
void startParseState();
int8_t readMessageByte();
int8_t parseMessage();
void parsePayloadField();

/**********************************************************************
 * PUBLIC FUNCTIONS                                                   *
 **********************************************************************/

BOOL GPS_init(uint8_t options) {
#ifdef DEBUG
    printf("Intializing the GPS on uart %d.\n", GPS_UART_ID);
#endif

    UART_init(GPS_UART_ID,GPS_UART_BAUDRATE);

    startIdleState();
    gpsInitialized = TRUE;
    return SUCCESS;
}

/**********************************************************************
 * Function: GPS_isInitialized()
 * @return Whether the GPS was initialized.
 * @remark none
 **********************************************************************/
BOOL GPS_isInitialized() {
    return gpsInitialized;
}

/**********************************************************************
 * Function: GPS_runSM()
 * @return None
 * @remark Executes the GPS's currently running state.
 **********************************************************************/
void GPS_runSM() {
    switch (state) {
        // Waiting for data
        case STATE_IDLE:
            // check for a new packet to start reading
            if (hasNewByte())
                startReadState();
            break;
        // Reading the message in and verifying sync, length, and checksum
        case STATE_READ:
            if (hasNewByte() && (readMessageByte() != SUCCESS)) {
                #ifdef DEBUG
                printf("Failed reading GPS message at byte %d.\n", byteIndex);
                while (!Serial_isTransmitEmpty()) { asm("nop"); }
                #endif
                startIdleState();
            }
            if (hasNewMessage)
                startParseState(); // finished reading, start parsing the payload
            break;
        // Parsing the new message's payload
        case STATE_PARSE:
            if (hasNewMessage)
                parseMessage();
            else
                startIdleState();
           break;
            // Should not be here!
    } // switch

    // Update connected variable
    if (Timer_isExpired(TIMER_GPS))
        isConnected = FALSE;
}

/**********************************************************************
 * Function: GPS_hasFix
 * @return TRUE if a lock has been obtained.
 * @remark
 **********************************************************************/
BOOL GPS_hasFix() {
    return gpsStatus != NOFIX_STATUS;
}

/**********************************************************************
 * Function: GPS_hasPosition
 * @return TRUE if a position has been obtained.
 * @remark
 **********************************************************************/
BOOL GPS_hasPosition() {
    return hasPosition;
}


/**********************************************************************
 * Function: GPS_getLatitude
 * @return The GPS's latitude value (N/S) scaled 1e7.
 * @remark
 **********************************************************************/
float GPS_getLatitude() {
    float lat =  (isUsingError)?
        geodetic.latitude - error.latitude
        :
        geodetic.latitude;

    return COORDINATE_TO_DECIMAL(lat);
}

/**********************************************************************
 * Function: GPS_getLongitude
 * @return The GPS's longitude value (E/W) scaled 1e7.
 * @remark
 **********************************************************************/
float GPS_getLongitude() {
    float lon =  (isUsingError)?
        (geodetic.longitude - error.longitude)
        :
        geodetic.longitude;

    return COORDINATE_TO_DECIMAL(lon);
}

/**********************************************************************
 * Function: GPS_getAltitudse
 * @return The GPS's altitude value in milimeters.
 * @remark
 **********************************************************************/
float GPS_getAltitude() {
    return ALTITUDE_TO_DECIMAL(geodetic.altitude);
}

/**********************************************************************
 * Function: GPS_setLongitudeError
 * @return None
 * @remark Sets the longitudal error for error corrections.
 **********************************************************************/
void GPS_setLongitudeError(int32_t lonError) {
    error.longitude = lonError;
}

/**********************************************************************
 * Function: GPS_setLatitudeError
 * @return None
 * @remark Sets the latitudal error for error corrections.
 **********************************************************************/
void GPS_setLatitudeError(int32_t latError) {
    error.latitude = latError;
}

/**********************************************************************
 * Function: GPS_enableErrorCorrection
 * @return None
 * @remark Enables error correction for retreived coordinates.
 **********************************************************************/
void GPS_enableErrorCorrection() {
    isUsingError = TRUE;
}

/**********************************************************************
 * Function: GPS_disableErrorCorrection
 * @return None
 * @remark Disables error correction for retreived coordinates.
 **********************************************************************/
void GPS_disableErrorCorrection() {
    isUsingError = FALSE;
}

/**********************************************************************
 * Function: GPS_getNorthVelocity
 * @return Returns the current velocity in the north direction.
 * @remark Centimeters per second in the north direction.
 **********************************************************************/
int32_t GPS_getNorthVelocity() {
    return velocity.north;
}

/**********************************************************************
 * Function: GPS_getEsatVelocity
 * @return Returns the current velocity in the east direction.
 * @remark Centimeters per second in the east direction.
 **********************************************************************/
int32_t GPS_getEastVelocity() {
    return velocity.east;
}


/**********************************************************************
 * Function: GPS_getHeading
 * @return Returns the current heading in degrees scaled 1e-5.
 * @remark In degrees scaled by 1e-5.
 **********************************************************************/
int32_t GPS_getHeading() {
    return heading;
}


/**********************************************************************
 * Function: GPS_isConnected
 * @return Returns true if GPS data seen in last 5 seconds.
 * @remark In degrees scaled by 1e-5.
 **********************************************************************/
int32_t GPS_isConnected() {
    return isConnected;
}

/**********************************************************************
 * PRIVATE FUNCTIONS                                                  *
 **********************************************************************/


/**********************************************************************
 * Function: hasNewMessage
 * @return Returns true if a new message is ready to be read
 * @remark 
 **********************************************************************/
BOOL hasNewByte() {
    return !UART_isReceiveEmpty(GPS_UART_ID);
}

/**********************************************************************
 * Function: startIdleState
 * @return None
 * @remark Switches into the GPS idle state.
 **********************************************************************/
void startIdleState() {
    state = STATE_IDLE;
#ifdef DEBUG_STATE
    printf("Entered idle state.\n");
    while (!Serial_isTransmitEmpty()) { asm("nop"); }
#endif

}

/**********************************************************************
 * Function: startReadState
 * @return None
 * @remark Switches into the read state for the GPS.
 **********************************************************************/
void startReadState() {
    state = STATE_READ;
    byteIndex = 0;
    messageLength = PAYLOAD_INDEX;
    hasNewMessage = FALSE;
    
#ifdef DEBUG_STATE
    printf("Entered read state.\n");
    while (!Serial_isTransmitEmpty()) { asm("nop"); }
#endif
}

/**********************************************************************
 * Function: startParseState
 * @return None
 * @remark Switches into the GPS parse state.
 **********************************************************************/
void startParseState() {
    state = STATE_PARSE;
    byteIndex = PAYLOAD_INDEX;
    
#ifdef DEBUG_STATE
    printf("Entered parse state.\n");
    while (!Serial_isTransmitEmpty()) { asm("nop"); }
#endif
}

/**********************************************************************
 * Function: setConnected
 * @return None
 * @remark Sets the GPS connected state to TRUE and starts a timeout timer.
 **********************************************************************/
void setConnected() {
    isConnected = TRUE;
    Timer_new(TIMER_GPS,DELAY_TIMEOUT);

#ifdef DEBUG
    printf("Connected to GPS.\n");
    while (!Serial_isTransmitEmpty()) { asm("nop"); }
#endif
}

/**********************************************************************
 * Function: parseMessageByte
 * @return SUCCESS, FAILURE, or ERROR.
 * @remark Reads a GPS packet from the UART. This function will return
 *  SUCCESS every time a valid byte is read (interpets the sync, length,
 *  and checksum fields). The hasNewMessage field will be set to TRUE
 *  when a new message is received and ready for parsing.
 **********************************************************************/
int8_t readMessageByte() {
    // Read a new byte from the UART or return FAILURE
    if (hasNewByte() && !hasNewMessage)
        rawMessage[byteIndex] = UART_getChar(GPS_UART_ID);
    else
        return FAILURE;

    // Look at the new byte
    switch(byteIndex) {
        case SYNC1_INDEX:
            if (rawMessage[byteIndex] != SYNC1_CHAR)
                return FAILURE;
            break;
        case SYNC2_INDEX:
            if (rawMessage[byteIndex] != SYNC2_CHAR)
                return FAILURE;
            // Two sync packets mean we see the GPS
            setConnected();
            break;
        case CLASS_INDEX:
            messageClass = rawMessage[byteIndex];
            break;
        case ID_INDEX:
            messageId = rawMessage[byteIndex];
            break;
        case LENGTH1_INDEX:
            messageLength = rawMessage[byteIndex];
            break;
        case LENGTH2_INDEX:
            messageLength += rawMessage[byteIndex] << 8;
            // Make length total for whole message
            messageLength += PAYLOAD_INDEX + CHECKSUM_BYTES;
            break;
        default:
            // Just reading payload and checksum (look these later)
            if (byteIndex >= (messageLength - 1)) {
                hasNewMessage = TRUE;
            }
    } // switch

    byteIndex++;
    return SUCCESS;
}

/**********************************************************************
 * Function: parseMessageByte()
 * @return SUCCESS, FAILURE, or ERROR.
 * @remark Parses the payload fields of a newly received GPS message.
 **********************************************************************/
int8_t parseMessage() {
    //for (byteIndex = 0; byteIndex < RAW_BUFFER_SIZE; byteIndex++) {

    // interpret message by parsing payload fields
    if (byteIndex < (messageLength - CHECKSUM_BYTES)) {
        parsePayloadField(); // Processing payload field by field
    }
    /*
    else if (byteIndex > (messageLength - CHECKSUM_BYTES) < messageLength) {
        //if (verifyChecksum() != SUCCESS) return FAILURE;
        startIdleState();
        return SUCCESS;
    }*/
    else {
        // Done parsing the message
        hasNewMessage = FALSE;
    }

    return SUCCESS; // ready to parse next byte
}


/**********************************************************************
 * Function: parsePayloadField()
 * @return None
 * @remark Parses one field of the payload for the new GPS message.
 **********************************************************************/
void parsePayloadField() {
    switch (messageClass) {
        // #################### Navigation Message #######################
        case NAV_CLASS:
            switch (messageId) {
                // ------------- NAV-POSLLH (0x01 0x02) --------------
                case NAV_POSLLH_ID:
                    switch (byteIndex - PAYLOAD_INDEX) {
                        case 0: // iTow (not implemented)
                            byteIndex += sizeof(uint32_t);
                            break;
                        case 4: // lon
                            geodetic.longitude = (int32_t)(rawMessage[byteIndex]
                                    + ((int32_t)rawMessage[byteIndex + 1] << 8)
                                    + ((int32_t)rawMessage[byteIndex + 2] << 16)
                                    + ((int32_t)rawMessage[byteIndex + 3] << 24));
                            //(int32_t)UNPACK_LITTLE_ENDIAN_32(rawMessage,byteIndex);
                            byteIndex += sizeof(int32_t);
                            break;
                        case 8: // lat
                            geodetic.latitude = (int32_t)(rawMessage[byteIndex]
                                    + ((int32_t)rawMessage[byteIndex + 1] << 8)
                                    + ((int32_t)rawMessage[byteIndex + 2] << 16)
                                    + ((int32_t)rawMessage[byteIndex + 3] << 24));
                            byteIndex += sizeof(int32_t);
                            break;
                        case 12: // height (not implemented)
                            byteIndex += sizeof(int32_t);
                            break;
                        case 16: // hMSL
                            geodetic.altitude = (int32_t)(rawMessage[byteIndex]
                                    + ((int32_t)rawMessage[byteIndex + 1] << 8)
                                    + ((int32_t)rawMessage[byteIndex + 2] << 16)
                                    + ((int32_t)rawMessage[byteIndex + 3] << 24));
                            hasPosition = TRUE;
                            byteIndex += sizeof(int32_t);
                            break;
                        case 20: // hAcc (not implemented)
                            byteIndex += sizeof(uint32_t);
                            break;
                        case 24: // vAcc (not implemented)
                            byteIndex += sizeof(uint32_t);
                    } // NAV_POSLLH_ID byte
                    break;
                // ------------- NAV-STATUS (0x01 0x03) --------------
                case NAV_STATUS_ID:
                    switch (byteIndex - PAYLOAD_INDEX) {
                        case 0: // iTow (not implemented)
                            byteIndex += sizeof(uint32_t);
                            break;
                        case 4: // gpsFix
                            gpsStatus = rawMessage[byteIndex];
                            byteIndex += sizeof(uint8_t);
                            break;
                        case 5: // flags (not implemented)
                            byteIndex += sizeof(uint8_t);
                            break;
                        case 6: // diffStat (not implemented)
                            byteIndex += sizeof(uint8_t);
                            break;
                        case 7: // res (not implemented)
                            byteIndex += sizeof(uint8_t);
                            break;
                        case 8: // ttff (not implemented)
                            byteIndex += sizeof(uint32_t);
                            break;
                        case 12: // msss (not implemented)
                            byteIndex += sizeof(uint32_t);
                    } //  NAV_STATUS_ID byte
                    break;
                // ------------- NAV-VELNED (0x01 0x12) --------------
                case NAV_VELOCITY_ID:
                    switch (byteIndex - PAYLOAD_INDEX) {
                        case 0: // iTow (not implemented)
                            byteIndex += sizeof(uint32_t);
                            break;
                        case 4: // velN
                            velocity.north = (int32_t)(rawMessage[byteIndex]
                                    + ((int32_t)rawMessage[byteIndex + 1] << 8)
                                    + ((int32_t)rawMessage[byteIndex + 2] << 16)
                                    + ((int32_t)rawMessage[byteIndex + 3] << 24));
                            byteIndex += sizeof(int32_t);
                            break;
                        case 8: // velE
                            velocity.east = (int32_t)(rawMessage[byteIndex]
                                    + ((int32_t)rawMessage[byteIndex + 1] << 8)
                                    + ((int32_t)rawMessage[byteIndex + 2] << 16)
                                    + ((int32_t)rawMessage[byteIndex + 3] << 24));
                            byteIndex += sizeof(int32_t);
                            break;
                        case 12: // velD
                            byteIndex += sizeof(int32_t);
                            break;
                        case 16: // speed
                            byteIndex += sizeof(uint32_t);
                            break;
                        case 20: // gSpeed
                            byteIndex += sizeof(uint32_t);
                            break;
                        case 24: // heading
                            heading = (int32_t)(rawMessage[byteIndex]
                                    + ((int32_t)rawMessage[byteIndex + 1] << 8)
                                    + ((int32_t)rawMessage[byteIndex + 2] << 16)
                                    + ((int32_t)rawMessage[byteIndex + 3] << 24));
                            byteIndex += sizeof(int32_t);
                            break;
                        case 28: // sAcc
                            byteIndex += sizeof(uint32_t);
                        case 32: // cAcc
                            byteIndex += sizeof(uint32_t);
                    } //  NAV_STATUS_ID byte
                    break;// ------------- End of Valid NAV message IDs --------------
                default:
                    byteIndex = messageLength; // skip rest of message
                #ifdef DEBUG
                    printf("Received unhandled navigation message id: 0x%X.\n", messageId);
                    while (!Serial_isTransmitEmpty()) { asm("nop"); }
                #endif
            } // switch messageID = NAV_CLASS
            break;
        // #################### End of NAV messages #######################
        default:
            byteIndex = messageLength; // skip rest of message
        #ifdef DEBUG
            printf("Received unhandled message class: 0x%X.\n", messageClass);
            while (!Serial_isTransmitEmpty()) { asm("nop"); }
        #endif
    } // switch messageClass
}

/****************************** TESTS ************************************/
// Test harness that spits out GPS packets over the serial port
//#define GPS_TEST
#ifdef GPS_TEST
int main() {
    uint8_t options = 0x0;
    Board_init(); // initalize interrupts
    Serial_init(); // init serial port for printing messages
    GPS_init(options);
    Timer_init();
    printf("GPS initialized.\n");

    Timer_new(TIMER_TEST,1000);
    while(1) {
        if (Timer_isExpired(TIMER_TEST)) {
            if (!GPS_isConnected()) {
                printf("GPS not connected.\n");
            }
            else if (GPS_hasFix()) {
                /*printf("Lat:%ld, Lon: %ld, Alt: %ld (m)\n",lat,lon,alt);*/
                printf("Lat:%.6f, Lon: %.6f, Alt: %.2f (m)\n",GPS_getLatitude(),
                        GPS_getLongitude(), GPS_getAltitude() );
                printf("Velocity N:%.2f, E: %.2f (m/s), Heading: %.2f (deg)\n",CM_TO_M(GPS_getNorthVelocity()),
                        CM_TO_M(GPS_getEastVelocity()), HEADING_TO_DEGREE(GPS_getHeading()) );
            }
            else {
                printf("No fix!\n");
            }

            Timer_new(TIMER_TEST,1000);
        }
        GPS_runSM();

    }
}

#endif
