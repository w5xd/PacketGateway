#include <RFM69.h>
#include <SPI.h>
#include <RadioConfiguration.h>


#define VERSION_ID_STR "Gateway V4"

// For a default wiring configuration, define USE_DEFAULT_CPU_ASSIGNMENTS 1
// Otherwise, spell out the arguments to the RFM69 constructor
#define USE_DEFAULT_CPU_ASSIGNMENTS 0
#define USE_TEENSY_ASSIGNMENTS 1 

#if USE_DEFAULT_CPU_ASSIGNMENTS == 0
#if USE_TEENSY_ASSIGNMENTS > 0
const int RFM69_NSS_PIN = 8;
const int RFM69_INT_PIN = 9;
#define STATIC_QUEUE_BYTE_LENGTH 0x8000
#define MESSAGE_ID_TYPE uint16_t
#endif
#endif


#include "StaticQueue.h"
using namespace StaticQueue;

/* The gateway does store and forward in both directions:
* 		network -> host (that is, incoming RF becomes outgoing serial)
* 		host -> network (that, the serial port sends us commands for a node that we send later)
*
* There is a queue in the gateway for store and forward. And there is only one,
* and it is used for messages going in both directions.
*
* The longest message that can be sent is 61 characters, per the RFM69 limit
*
* The messages that are forwarded are text. A message does not the contain newline character
* as that is used to separate messages.
* The delimiter between fields in a command is a single ASCII space character.
* <nodeId> is ASCII digits in decimal from 2 through 255 (nodeId 1 is permanently ours)
* A command is sent by the Serial port to this gateway, separated by ASCII <CR>
* There are four:
*
* GetMessages
* DeleteMessages
* ForwardMessageToNode
* SendMessageToNode
*
* GetMessages
* Messages from network to host are sent to the host in response to this host string (command)
*
*  The response to that command is a series of lines like this:
* QueueBegin
* Queue <ID> <age-seconds> <REC rssi or ACK or NAC or QUE> <nodeId> <text as received or sent>
* QueueEnd
* QueueBytesFree nnnn
*
* REC rssi  is the string REC followed by the RSSI signal strength, followed by the message text
* The remainder are messages to send, or have been sent, to nodes:
* ACK means it was forwarded and ACK was received
* NAK means it was forwarded and NACK was received.
* QUE means we haven't heard from the node yet.
*
* QueueBytesFree is free bytes in the queue. This should be kept above about 70
*
* The list is in order oldest first
*
* DeleteMessagesFromId <id>
* deletes the message <id> and all older messages from the store-and-forward queue.
*
* Note that the host may find itself forced to delete an earlier
* ForwardMessageToNode is still in QUE because its been there long enough that
* REC messages have filled the queue. The solution is for the host to delete
* and then send another ForwareMessageToNode to put the command back in the queue.
*
* ForwardMessageToNode <nodeId> <text to be sent>
* Stores the message text to be sent to <nodeId> when we next hear from it.
* This handles the battery operated remotes that don't listen continuously.
*
* SendMessageToNode <nodeId> <text to be sent>
* Sends the message now. A result is sent immediately back to the host,
* and without using the queue. The ACK or NACK keyword is added to
* tell the host the result.
* SendMessageToNode <ACK or NACK> <text as sent>
*
*/

enum {
    RFM69_FRAME_LIMIT = 61,	// radio packet/encrypted mode max message length.
    LONGEST_COMMAND_NAME = 20,	// our command table goes up to this size
    CHAR_IN_NODEID = 3,	// extra characters due to NodeId
    NUMBER_OF_SPACES_AND_CR = 3,	// whitespace on the line
    HOST_BUFFER_LENGTH = RFM69_FRAME_LIMIT + LONGEST_COMMAND_NAME + CHAR_IN_NODEID + NUMBER_OF_SPACES_AND_CR
};

static uint16_t Startup_delay_msec;

enum class EepromAddresses {GATEWAY_START = (~0x7u & (7 + RadioConfiguration::EepromAddresses::TOTAL_EEPROM_USED)),
       STARTUP_DELAY_MSEC = GATEWAY_START,
        TOTAL_EEPROM_USED = STARTUP_DELAY_MSEC + sizeof(Startup_delay_msec),
    };

// Create a library object for our RFM69HCW module:
#if USE_DEFAULT_CPU_ASSIGNMENTS > 0
RFM69 radio;
#elif USE_TEENSY_ASSIGNMENTS > 0
class RFM69delayCanSend : public RFM69
{
    public:
        RFM69delayCanSend(int nss, int irq) : RFM69(nss, irq, true)
        {}
        
        bool canSend() override 
        {       
            /* this delay(1) is inspired by the  #ifdef ESP8266 in the library code
                https://github.com/LowPowerLab/RFM69
            */
            auto ret = RFM69::canSend();
            if (!ret)
                delay(1);
            return ret;
        }
};
RFM69delayCanSend radio(RFM69_NSS_PIN,RFM69_INT_PIN);
#endif
RadioConfiguration radioConfiguration;
static const int AM_GATEWAY_NODEID = 1;   // My node ID
static    bool radioOK = false;

void setup()
{
    EEPROM.get(static_cast<uint16_t>(EepromAddresses::STARTUP_DELAY_MSEC), Startup_delay_msec);
    if (Startup_delay_msec == 0xFFFF)
        Startup_delay_msec = 0;
    delay(Startup_delay_msec);

    uint16_t nodeId = AM_GATEWAY_NODEID;
    // Initialize the RFM69HCW:
    if (radioConfiguration.NetworkId() != 0xff)
    {
        auto EepromNodeId = radioConfiguration.NodeId();
        if (EepromNodeId != 0xff)
            nodeId = EepromNodeId;
        radioOK = radio.initialize(radioConfiguration.FrequencyBandId(),
            nodeId, radioConfiguration.NetworkId());
        if (radioOK)
        {
            uint32_t freq;
            if (radioConfiguration.FrequencyKHz(freq))
                radio.setFrequency(freq*1000);
            radio.spyMode(true);
            radioOK = radio.getFrequency() != 0;
            if (radioOK)
            {
                radio.setHighPower(); // Always use this for RFM69HCW
                const char *key = radioConfiguration.EncryptionKey();
                if (radioConfiguration.encrypted())
                    radio.encrypt(key);
            }
        }
    }

    Serial.begin(9600);
    Serial.print(F(VERSION_ID_STR));
    if (radioOK)
    {
        Serial.print(" Node ");
        Serial.print(nodeId, DEC);
        Serial.print(" on network ");
        Serial.print(radioConfiguration.NetworkId(), DEC);
        Serial.print(" band ");
        Serial.print(radioConfiguration.FrequencyBandId(), DEC);
        Serial.print(" key ");
        radioConfiguration.printEncryptionKey(Serial);
        Serial.println();
        Serial.print("Freq= "); Serial.print(radio.getFrequency() / 1000);
        Serial.println(" KHz");
    }
    else
        Serial.println(F("Radio not initialized"));
    Serial.print(F("StartupDelayMsec=")); Serial.println(Startup_delay_msec);
    Serial.println(F(" ready"));
}

static bool processHostCommand(const char *pHost, unsigned short count)
{
    static const char GETMESSAGES[] = "GetMessages";
    static const char DELETEMESSAGES[] = "DeleteMessagesFromId";
    static const char FORWARDMESSAGETONODE[] = "ForwardMessageToNode";
    static const char SENDMESSAGETONODE[] = "SendMessageToNode";
    static const char SETSTARTUPDELAYMSEC[] = "SetStartupDelayMsec";

    if (strncmp(pHost, GETMESSAGES, sizeof(GETMESSAGES) - 1) == 0)
    {	// print the message queue
        Serial.println(F("QueueBegin"));
        if (!QueueManager::empty())
        {
            QueueEntry qe = QueueManager::first();
            unsigned long nowSec = millis() / 1000ul;
            for (;;)
            {
                // Queue <REC or ACK or NAC or QUE> <nodeId> <text as received or sent>
                Serial.print(F("Queue "));
                Serial.print((int)qe.MessageId(), DEC);
                Serial.print(F(" "));
                unsigned short seconds = nowSec - qe.getTime();
                Serial.print(seconds, DEC);
                if (qe.isRx())
                {
                    Serial.print(F(" REC "));
                    Serial.print(qe.getRSSI(), DEC);
                    Serial.print(F(" "));
                }
                else
                {
                    if (qe.isWaiting())
                        Serial.print(F(" QUE "));
                    else if (qe.AckedOK())
                        Serial.print(F(" ACK "));
                    else
                        Serial.print(F(" NAC "));
                }
                Serial.print(qe.NodeId(), DEC);
                Serial.print(F(" "));
                qe.SerialPrint();
                Serial.println();
                if (qe.AmLast())
                    break;
                qe = QueueManager::next(qe);
            }
        }
        Serial.println(F("QueueEnd "));
        Serial.print(F("QueueBytesFree "));
        Serial.println(QueueManager::QueueBytesFree(), DEC);
    }
    else if (strncmp(pHost, DELETEMESSAGES, sizeof(DELETEMESSAGES) - 1) == 0)
    {	// delete oldest messages
        pHost += sizeof(DELETEMESSAGES) - 1;
        if (*pHost++ == ' ')
        {
            unsigned deleteId = 0;
            bool found = false;
            for (;; pHost++)
            {
                if (*pHost >= '0' && *pHost <= '9')
                {
                    deleteId *= 10;
                    deleteId += *pHost - '0';
                    count -= 1;
                    found = true;
                }
                else
                    break;
            }
            unsigned deleted = 0;
            if (found)
            {
                // to avoid looking really stupid, first check that
                // the given id is currently in the queue
                if (!QueueManager::empty())
                {
                    // At most one waiting-to-forward message will be put back into queue
                    unsigned char TxNodeId = 0;
                    unsigned char RadioMessageBuffer[RFM69_FRAME_LIMIT];
                    unsigned char len = 0;

                    QueueEntry qe = QueueManager::first();
                    for (;;)
                    {
                        if (qe.MessageId() == deleteId)
                        {
                            found = true;
                            break;
                        } else if (qe.isTx() && qe.isWaiting() && TxNodeId == 0)
                        {   // forward-to-node message that will be deleted. Save most recent one
                            TxNodeId = qe.NodeId();
                            len = qe.CopyMessage(RadioMessageBuffer, RFM69_FRAME_LIMIT);
                        } else if (qe.AmLast())
                        {
                            found = false;
                            break;
                        }
                        qe = QueueManager::next(qe);
                    }

                    if (found) 
                    {
                        while (!QueueManager::empty() )
                        {       
                            QueueEntry qe = QueueManager::first();
                            auto id = qe.MessageId();
                            QueueManager::pop();
                            deleted += 1;
                            if (id == deleteId)
                                break;
                        }
                        if (TxNodeId > AM_GATEWAY_NODEID) // put forward-to-node back. but just one
                            QueueManager::push(TxNodeId, true, (const unsigned char*)RadioMessageBuffer, len);
                    }
                }
            }
            Serial.print(DELETEMESSAGES);
            Serial.print(F(" count:"));
            Serial.print(deleted, DEC);
            Serial.print(F(" "));
            Serial.print(F("QueueBytesFree "));
            Serial.println(QueueManager::QueueBytesFree(), DEC);
        }
    }
    else if (strncmp(pHost, FORWARDMESSAGETONODE, sizeof(FORWARDMESSAGETONODE) - 1) == 0)
    {	// queue a message to be sent on receipt of a message from a nodeId
        pHost += sizeof(FORWARDMESSAGETONODE) - 1;
        count -= sizeof(FORWARDMESSAGETONODE);
        if (*pHost++ == ' ')
        {
            unsigned nodeId = 0;
            for (;; pHost++)
            {
                if (*pHost >= '0' && *pHost <= '9')
                {
                    nodeId *= 10;
                    nodeId += *pHost - '0';
                    count -= 1;
                }
                else
                    break;
            }
            if ((nodeId >= 0) && *pHost++ == ' ')
            {
                count -= 1;
                while (!QueueManager::isRoomFor(count))
                    QueueManager::pop();
                QueueManager::push(nodeId, true, (const unsigned char *)pHost, count);
            }
        }
    }
    else if (strncmp(pHost, SENDMESSAGETONODE, sizeof(SENDMESSAGETONODE) - 1) == 0)
    {	// send a message to a node now.
        pHost += sizeof(SENDMESSAGETONODE) - 1;
        count -= sizeof(SENDMESSAGETONODE);
        if (*pHost++ == ' ')
        {
            unsigned nodeId = 0;
            for (;; pHost++)
            {
                if (*pHost >= '0' && *pHost <= '9')
                {
                    nodeId *= 10;
                    nodeId += *pHost - '0';
                    count -= 1;
                }
                else
                    break;
            }
            if ((nodeId >= 0) && *pHost++ == ' ')
            {
                bool acked = radio.sendWithRetry(nodeId, pHost, --count);
                Serial.print(SENDMESSAGETONODE);
                Serial.print(F(" "));
                Serial.print(nodeId, DEC);
                Serial.println(acked? F(" ACK") : F(" NAC"));
            }
        }
    }
    else if (strncmp(pHost, SETSTARTUPDELAYMSEC, sizeof(SETSTARTUPDELAYMSEC) - 1) == 0)
    {
        pHost += sizeof(SETSTARTUPDELAYMSEC);
        Startup_delay_msec = atoi(pHost);
        EEPROM.put(static_cast<uint16_t>(EepromAddresses::STARTUP_DELAY_MSEC), Startup_delay_msec);
    }
    else
        return false;
    return true;
}

void loop()
{
    // Set up a "buffer" for characters that we'll send:
    static char fromHostbuffer[HOST_BUFFER_LENGTH + 1]; // +1 so we can add trailing null
    static int charsInBuf = 0;

    while (Serial.available() > 0)
    {
        char input = Serial.read();
        bool isRet = (input == (char)'\r') || (input == (char)'\n');
        if (!isRet) 
        {
            fromHostbuffer[charsInBuf] = input;
            charsInBuf++;
        }

        // If the input is a carriage return, or the buffer is full:

        if (isRet || (charsInBuf == sizeof(fromHostbuffer) - 1)) // CR or buffer full
        {
            fromHostbuffer[charsInBuf] = 0;
            if (radioConfiguration.ApplyCommand(fromHostbuffer))
            {
                Serial.print(fromHostbuffer);
                Serial.println(" command accepted for radio");
            }
            else if (processHostCommand(fromHostbuffer, charsInBuf))
            {
            }
            charsInBuf = 0; // reset the packet
        }
    }

    // RECEIVING

    // In this section, we'll check with the RFM69HCW to see
    // if it has received any packets:

    if (radioOK && radio.receiveDone()) // Got a packet over the radio
    {
        // make room to store the incoming packet
        while (!QueueManager::isRoomFor(radio.DATALEN))
            QueueManager::pop();

        QueueEntry qe = QueueManager::push(
            radio.SENDERID, false,
            (const unsigned char *)&radio.DATA[0], radio.DATALEN);
        qe.setRSSI(radio.RSSI);

        if (radio.ACKRequested())
            radio.sendACK();
 
        if (!QueueManager::empty())
        {	// on receipt of a packet search queue for ForwardMessageToNode items
            QueueEntry qe = QueueManager::first();
            for (;;)
            {
                if (qe.isTx() && qe.NodeId() == radio.SENDERID && qe.isWaiting())
                {
                    qe.clrWaiting();
                    unsigned char RadioMessageBuffer[RFM69_FRAME_LIMIT];
                    unsigned char len = qe.CopyMessage(RadioMessageBuffer, RFM69_FRAME_LIMIT);
                    if (radio.sendWithRetry(qe.NodeId(), RadioMessageBuffer, len))
                        qe.setAckedOK();
                }
                if (qe.AmLast())
                    break;
                qe = QueueManager::next(qe);
            }
        }
    }
}

