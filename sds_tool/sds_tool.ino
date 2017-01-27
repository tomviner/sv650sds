#include <Arduino.h>
#include "sds_tool.h"
#include "utils.h"
#include "PROGMEM_readAnything.h"

bool process_message(uint8_t *message, uint8_t msglen, uint32_t delay);
void diff_sensors(uint8_t msglen);
char *calc_rpm(char *str, int len, uint8_t a, uint8_t b);
char *calc_tps(char *str, int len, uint8_t a, uint8_t b);
char *calc_temp(char *str, int len, uint8_t a, uint8_t b);
char *calc_stva(char *str, int len, uint8_t a, uint8_t b);
char *calc_hex(char *str, int len, uint8_t a, uint8_t b);
char *calc_decimal(char *str, int len, uint8_t a, uint8_t b);



#define ECU_CODES_LEN 14  // number of ecu codes we support below
const ECU_CODE ecu_codes[ECU_CODES_LEN] PROGMEM = {
    { 25 , 26 , "RPM"            , &calc_rpm }     , 
    { 27 , -1 , "TPS"            , &calc_tps  }    , 
    { 29 , -1 , "ECT"            , &calc_temp  }   , 
    { 30 , -1 , "IAT"            , &calc_temp }    , 
    { 31 , -1 , "IAP"            , &calc_hex }     , 
    { 34 , -1 , "GPS"            , &calc_decimal } , 
    { 40 , -1 , "Fuel1"          , &calc_decimal } , 
    { 42 , -1 , "Fuel2"          , &calc_decimal } , 
    { 49 , -1 , "Ign1"           , &calc_decimal } , 
    { 50 , -1 , "Ign2"           , &calc_decimal } , 
    { 54 , -1 , "STVA"           , &calc_decimal } , 
    { 51 , -1 , "PAIR"           , &calc_decimal } , 
    { 52 , -1 , "Clutch/FuelMap" , &calc_hex }     , 
    { 53 , -1 , "Neutral"        , &calc_hex }
};
// number of items in array.  Don't think we actually need this?
// template< typename T, size_t N > size_t ArraySize (T (&) [N]) { return N; }

uint8_t last_message[MAX_MSG_LEN];
uint8_t message[MAX_MSG_LEN];


void
setup() {
    uint32_t last = 0, now;

    // Init the pins
    pinMode(LED_PIN, OUTPUT);
    pinMode(TX_PIN, OUTPUT);
    pinMode(RX_PIN, INPUT);
    Console.begin(CONSOLE_BAUD);
    digitalWrite(LED_PIN, HIGH);
    memset(message, 0, MAX_MSG_LEN);
    memset(last_message, 0, MAX_MSG_LEN);

    // Wait until we see some indication that someone is talking.
    Console.print("Waiting for someone to talk (RX goes low)...\n");
    while (digitalRead(RX_PIN) == HIGH) {
        now = millis();
        if (last + 500 < now) {
            last = now;
            toggle_led();
        }
    }
    SDSSerial.begin(SDS_BAUD);
    Console.print("staring loop()\n");
}



void
loop() {
    static bool overflow = false;
    static uint8_t msglen = 0;
    static uint32_t lastms = 0;
    static uint32_t force_blink = 0;
    uint8_t byte;
    uint32_t currentms, deltams;

    currentms = millis();
    if (SDSSerial.available()) {
        byte = SDSSerial.read();
        toggle_led();
        deltams = currentms - lastms;
        lastms = currentms;


        if (deltams > MINIMUM_MESSAGE_GAP) {
            if (process_message(message, msglen, deltams)) {
                // replace last_message if CSUM is valid
                memcpy(last_message, message, msglen);
            }
            msglen = 0;
            memset(message, 0, MAX_MSG_LEN);
            overflow = false;
        } else if (msglen == MAX_MSG_LEN) {
            // Only print the warning once per read() until we reach the message gap
            if (!overflow) {
                overflow = true;
                Console.printf("\n******\nERROR: ECU Message is larger then %u"
                       " bytes! Unable to process!\n******", MAX_MSG_LEN);
            }
            return;  // return now so we don't increment msglen
        }

        message[msglen] = byte;
        msglen++;
    } else {
        if (force_blink + 1000 < currentms) {
            toggle_led();
            force_blink = currentms;
            Console.printf("Waiting...\n");
        }
    }
}

/*
 * Processes & prints a message to the console 
 * Returns true if message is valid
 */
bool
process_message(uint8_t *message, uint8_t msglen, uint32_t delay) {
    uint8_t i, csum;
    bool unknown = false;
    uint8_t verify = 0;

    if (msglen < 5) {
        Console.printf("Wow!  Short message.  %d bytes long\n", msglen);
        return false;
    }

    // Figure out if message is from the ECU or Diag tool
    if (message[1] == ECU_ID) {
        verify += ECU_ID;
    } else {
        verify += SDT_ID;
    }

    // Valid checksum?
    csum = calc_checksum(message, msglen - 1);
    if (csum == message[msglen - 1]) {
        verify += 1;
    } else {
        verify += 2;
    }

    // Print the header based on the above checks
    switch (verify) {
        case (ECU_ID + 1):
            console_printf(F("OK ToECU [%03lums] %d: "), delay, msglen);
            break;
        case (SDT_ID + 1):
            console_printf(F("OK FromECU [%03lums] %d: "), delay, msglen);
            break;
        case (ECU_ID + 2):
            console_printf(F("BAD ToECU [%03lums] %d: "), delay, msglen);
            break;
        case (SDT_ID + 2):
            console_printf(F("BAD FromECU [%03lums] %d: "), delay, msglen);
            break;
        default:
            console_printf(F("Uknown message [%03lums] %d\n"), delay, msglen);
            unknown = true;
    }

    // print the message
    for (i = 0; i < (msglen - 1); i++) {
        Console.printf("%02x,", message[i]);
    }
    // last/checksum byte has a newline instead of a comma after it
    Console.printf("%02x\n", message[i]);

    // Don't diff the sensors if we have an unknown message
    if (unknown)
        return false;

    // Diff sensors if checksum is OK
    if (csum == message[msglen - 1]) {
        diff_sensors(msglen);
        return true;
    }
    return false;
}

/*
 * Calculates the RPM
 */
char *
calc_rpm(char *str, int len, uint8_t a, uint8_t b) {
    snprintf(str, len, "[0x%02x%02x] %lu", 
            a, b, (unsigned long)(a * 100) + b);
    return str;
}

/*
 * Calculates the Throttle Position Sensor
 */
char *
calc_tps(char *str, int len, uint8_t a, uint8_t b) {
    char temp[10];
    ftoa(temp, (float)((a - 55) * 100) / 169.0, 2);
    snprintf(str, len, "[0x%02x] %s", a, temp);
    return str;
}

/*
 * Calculates the Temp (C) for Air and Water
 */
char *
calc_temp(char *str, int len, uint8_t a, uint8_t b) {
    char temp[10];
    ftoa(temp, (float)(a - 48) * 0.625, 1);
    snprintf(str, len, "[0x%02x] %s", a, temp);
    return str;
}

/*
 * Calculates the Secondary Throttle Valve Acutator
 */
char *
calc_stva(char *str, int len, uint8_t a, uint8_t b) {
    char temp[10];
    ftoa(temp, (float)(a * 100) / 255.0, 2);
    snprintf(str, len, "[0x%02x] %s", a, temp);
    return str;
}

/*
 * Generic Hex output
 */
char *
calc_hex(char *str, int len, uint8_t a, uint8_t b) {
    snprintf(str, len, "0x%02x", a);
    return str;
}

/*
 * Generic decimal output
 */
char *
calc_decimal(char *str, int len, uint8_t a, uint8_t b) {
    snprintf(str, len, "[0x%02x] %u", a, a);
    return str;
}

/*
 * Prints the diff of two messages from the ECU
 */
void
diff_sensors(uint8_t msglen) {
    int i;
    uint8_t a_sensor, b_sensor;
#define TEMP_STR_LEN 15
    char last_str[TEMP_STR_LEN], current_str[TEMP_STR_LEN];
    ECU_CODE ecu_code;

    for (i = 0; i < ECU_CODES_LEN; i++) {
        PROGMEM_readAnything(&ecu_codes[i], ecu_code);
        a_sensor = ecu_code.a_index;
        b_sensor = ecu_code.b_index;
        if (a_sensor > msglen || b_sensor > msglen) {
            Console.printf("Invalid message length (%u). Unable to lookup %s at %d/%d\n",
                    msglen, ecu_code.name, a_sensor, b_sensor);
            continue;

        }
        if (b_sensor != -1) {
            // needs two bytes
            if ((last_message[a_sensor] != message[a_sensor]) &&
                    (last_message[b_sensor] != message[b_sensor])) {
                ecu_code.formatter(last_str, TEMP_STR_LEN, last_message[a_sensor], last_message[b_sensor]);
                ecu_code.formatter(current_str, TEMP_STR_LEN, message[a_sensor], message[b_sensor]);
                Console.printf("%s: %s -> %s\n", ecu_code.name, last_str, current_str);
            }
        } else {
            // single byte value
            if (last_message[a_sensor] != message[a_sensor]) {
                ecu_code.formatter(last_str, TEMP_STR_LEN, last_message[a_sensor], 0);
                ecu_code.formatter(current_str, TEMP_STR_LEN, message[a_sensor], 0);
                Console.printf("%s: %s -> %s\n", ecu_code.name, last_str, current_str);
            }
        }
        i++;
    }

}