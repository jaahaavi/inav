/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "common/crc.h"
#include "common/maths.h"
#include "common/streambuf.h"
#include "common/utils.h"

#include "drivers/time.h"

#include "io/serial.h"

#include "rcdevice.h"

#ifdef USE_RCDEVICE

typedef enum {
    RCDP_SETTING_PARSE_WAITING_ID,
    RCDP_SETTING_PARSE_WAITING_NAME,
    RCDP_SETTING_PARSE_WAITING_VALUE,
} runcamDeviceSettingParseStep_e;

// return 0xFF if expected resonse data length is variable
static uint8_t runcamDeviceGetResponseLength(uint8_t command)
{
    switch (command) {
        case RCDEVICE_PROTOCOL_COMMAND_GET_DEVICE_INFO:
            return 5;
        case RCDEVICE_PROTOCOL_COMMAND_5KEY_SIMULATION_PRESS:
        case RCDEVICE_PROTOCOL_COMMAND_5KEY_SIMULATION_RELEASE:
            return 2;
        case RCDEVICE_PROTOCOL_COMMAND_5KEY_CONNECTION:
            return 3;
        default:
            return 0;
    }
}

// Parse the variable length response, e.g the response of settings data and the detail of setting
static uint8_t runcamDeviceIsResponseReceiveDone(uint8_t command, uint8_t *data, uint8_t dataLen, bool *isDone)
{
    if (isDone == NULL) {
        return false;
    }

    uint8_t expectedResponseDataLength = runcamDeviceGetResponseLength(command);
    if (expectedResponseDataLength == 0xFF) {
        uint8_t settingDataLength = 0x00;
        // get setting datalen first
        if (dataLen >= 3) {
            settingDataLength = data[2];
            if (dataLen >= (settingDataLength + 4)) {
                *isDone = true;
                return true;
            }
        }

        if (settingDataLength > 60) {
            return false;
        }
    } else if (dataLen >= expectedResponseDataLength) {
        *isDone = true;
        return true;
    }

    return true;
}

// a common way to receive packet and verify it
static uint8_t runcamDeviceReceivePacket(runcamDevice_t *device, uint8_t command, uint8_t *data)
{
    uint8_t dataPos = 0;
    uint8_t crc = 0;
    uint8_t responseDataLen = 0;

    // wait 500ms for reply
    timeMs_t timeout = millis() + 500;
    bool isWaitingHeader = true;
    while (millis() < timeout) {
        if (serialRxBytesWaiting(device->serialPort) > 0) {
            uint8_t c = serialRead(device->serialPort);
            crc = crc8_dvb_s2(crc, c);

            if (data) {
                data[dataPos] = c;
            }
            dataPos++;

            if (isWaitingHeader) {
                if (c == RCDEVICE_PROTOCOL_HEADER) {
                    isWaitingHeader = false;
                }
            } else {
                bool isDone = false;
                if (!runcamDeviceIsResponseReceiveDone(command, data, dataPos, &isDone)) {
                    return 0;
                }
    
                if (isDone) {
                    responseDataLen = dataPos;
                    break;
                }
            }
        }
    }

    // check crc
    if (crc != 0) {
        return 0;
    }

    return responseDataLen;
}

// every time send packet to device, and want to get something from device,
// it'd better call the method to clear the rx buffer before the packet send,
// else may be the useless data in rx buffer will cause the response decoding
// failed.
static void runcamDeviceFlushRxBuffer(runcamDevice_t *device)
{
    while (serialRxBytesWaiting(device->serialPort) > 0) {
        serialRead(device->serialPort);
    }
}

// a common way to send packet to device
static void runcamDeviceSendPacket(runcamDevice_t *device, uint8_t command, uint8_t *paramData, int paramDataLen)
{
    // is this device open?
    if (!device->serialPort) {
        return;
    }

    sbuf_t buf;
    // prepare pointer
    buf.ptr = device->buffer;
    buf.end = ARRAYEND(device->buffer);

    sbufWriteU8(&buf, RCDEVICE_PROTOCOL_HEADER);
    sbufWriteU8(&buf, command);

    if (paramData) {
        sbufWriteData(&buf, paramData, paramDataLen);
    }

    // add crc over (all) data
    crc8_dvb_s2_sbuf_append(&buf, device->buffer);

    // switch to reader
    sbufSwitchToReader(&buf, device->buffer);

    // send data if possible
    serialWriteBuf(device->serialPort, sbufPtr(&buf), sbufBytesRemaining(&buf));
}

// a common way to send a packet to device, and get response from the device.
static bool runcamDeviceSendRequestAndWaitingResp(runcamDevice_t *device, uint8_t commandID, uint8_t *paramData, uint8_t paramDataLen, uint8_t *outputBuffer, uint8_t *outputBufferLen)
{
    uint32_t max_retries = 3;
    
    while (max_retries--) {
        // flush rx buffer
        runcamDeviceFlushRxBuffer(device);

        // send packet
        runcamDeviceSendPacket(device, commandID, paramData, paramDataLen);

        // waiting response
        uint8_t responseLength = runcamDeviceReceivePacket(device, commandID, outputBuffer);
        if (responseLength) {
            if (outputBufferLen) {
                *outputBufferLen = responseLength;
            }

            return true;
        }
    }

    return false;
}

static uint8_t crc_high_first(uint8_t *ptr, uint8_t len)
{
    uint8_t i;
    uint8_t crc=0x00;
    while (len--) {
        crc ^= *ptr++;
        for (i=8; i>0; --i) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x31;
            else
                crc = (crc << 1);
        }
    }
    return (crc);
}

static void sendCtrlCommand(runcamDevice_t *device, rcsplit_ctrl_argument_e argument)
{
    if (!device->serialPort)
        return ;

    uint8_t uart_buffer[5] = {0};
    uint8_t crc = 0;

    uart_buffer[0] = RCSPLIT_PACKET_HEADER;
    uart_buffer[1] = RCSPLIT_PACKET_CMD_CTRL;
    uart_buffer[2] = argument;
    uart_buffer[3] = RCSPLIT_PACKET_TAIL;
    crc = crc_high_first(uart_buffer, 4);

    // build up a full request [header]+[command]+[argument]+[crc]+[tail]
    uart_buffer[3] = crc;
    uart_buffer[4] = RCSPLIT_PACKET_TAIL;

    // write to device
    serialWriteBuf(device->serialPort, uart_buffer, 5);
}

// get the device info(firmware version, protocol version and features, see the
// definition of runcamDeviceInfo_t to know more)
static bool runcamDeviceGetDeviceInfo(runcamDevice_t *device, uint8_t *outputBuffer) 
{
    // Send "who are you" command to device to detect the device whether is running RCSplit FW1.0 or RCSplit FW1.1
    uint32_t max_retries = 2;
    while (max_retries--) {
        runcamDeviceFlushRxBuffer(device);
        sendCtrlCommand(device, RCSPLIT_CTRL_ARGU_WHO_ARE_YOU);

        timeMs_t timeout = millis() + 500;
        uint8_t response[5] = { 0 };
        while (millis() < timeout) {
            if (serialRxBytesWaiting(device->serialPort) >= 5) {
                response[0] = serialRead(device->serialPort);
                response[1] = serialRead(device->serialPort);
                response[2] = serialRead(device->serialPort);
                response[3] = serialRead(device->serialPort);
                response[4] = serialRead(device->serialPort);
                if (response[0] != RCSPLIT_PACKET_HEADER || response[1] != RCSPLIT_PACKET_CMD_CTRL || response[2] != RCSPLIT_CTRL_ARGU_WHO_ARE_YOU || response[4] != RCSPLIT_PACKET_TAIL) {
                    break;
                }

                uint8_t crcFromPacket = response[3];
                response[3] = response[4]; // move packet tail field to crc field, and calc crc with first 4 bytes
                uint8_t crc = crc_high_first(response, 4);
                if (crc != crcFromPacket) {
                    break;
                }

                // generate response for RCSplit FW 1.0 and FW 1.1
                outputBuffer[0] = RCDEVICE_PROTOCOL_HEADER;
                // protocol version
                outputBuffer[1] = RCDEVICE_PROTOCOL_RCSPLIT_VERSION;
                // features
                outputBuffer[2] = RCDEVICE_PROTOCOL_FEATURE_SIMULATE_POWER_BUTTON | RCDEVICE_PROTOCOL_FEATURE_SIMULATE_WIFI_BUTTON | RCDEVICE_PROTOCOL_FEATURE_CHANGE_MODE;
                outputBuffer[3] = 0;

                crc = 0;
                const uint8_t * const end = outputBuffer + 4;
                for (const uint8_t *ptr = outputBuffer; ptr < end; ++ptr) {
                    crc = crc8_dvb_s2(crc, *ptr);
                }
                outputBuffer[4] = crc;
                return true;
            }
        }
    }

    return runcamDeviceSendRequestAndWaitingResp(device, RCDEVICE_PROTOCOL_COMMAND_GET_DEVICE_INFO, NULL, 0, outputBuffer, NULL); 
}

static bool runcamDeviceSend5KeyOSDCableConnectionEvent(runcamDevice_t *device, uint8_t operation, uint8_t *outActionID, uint8_t *outErrorCode)
{
    uint8_t outputDataLen = RCDEVICE_PROTOCOL_MAX_PACKET_SIZE;
    uint8_t respBuf[RCDEVICE_PROTOCOL_MAX_PACKET_SIZE];
    if (!runcamDeviceSendRequestAndWaitingResp(device, RCDEVICE_PROTOCOL_COMMAND_5KEY_CONNECTION, &operation, sizeof(uint8_t), respBuf, &outputDataLen)) {
        return false;
    }

    // the high 4 bits is the operationID that we sent
    // the low 4 bits is the result code
    uint8_t operationID = (respBuf[1] & 0xF0) >> 4;
    bool errorCode = (respBuf[1] & 0x0F);
    if (outActionID) {
        *outActionID = operationID;
    }

    if (outErrorCode) {
        *outErrorCode = errorCode;
    }

    return true;
}

// init the runcam device, it'll search the UART port with FUNCTION_RCDEVICE id
// this function will delay 400ms in the first loop to wait the device prepared,
// as we know, there are has some camera need about 200~400ms to initialization,
// and then we can send/receive from it.
bool runcamDeviceInit(runcamDevice_t *device)
{
    serialPortFunction_e portID = FUNCTION_RCDEVICE;
    serialPortConfig_t *portConfig = findSerialPortConfig(portID);
    if (portConfig != NULL) {
        device->serialPort = openSerialPort(portConfig->identifier, portID, NULL, 115200, MODE_RXTX, SERIAL_NOT_INVERTED);

        if (device->serialPort != NULL) {
            // send RCDEVICE_PROTOCOL_COMMAND_GET_DEVICE_INFO to device to retrive
            // device info, e.g protocol version, supported features
            uint8_t respBuf[RCDEVICE_PROTOCOL_MAX_PACKET_SIZE];
            if (runcamDeviceGetDeviceInfo(device, respBuf)) {
                device->info.protocolVersion = respBuf[1];

                uint8_t featureLowBits = respBuf[2];
                uint8_t featureHighBits = respBuf[3];
                device->info.features = (featureHighBits << 8) | featureLowBits;
                return true;
            }

            closeSerialPort(device->serialPort);
        }
    }

    device->serialPort = NULL;
    return false;
}

bool runcamDeviceSimulateCameraButton(runcamDevice_t *device, uint8_t operation)
{
    if (device->info.protocolVersion == RCDEVICE_PROTOCOL_RCSPLIT_VERSION) {
        sendCtrlCommand(device, operation + 1);
    } else if (device->info.protocolVersion == RCDEVICE_PROTOCOL_VERSION_1_0) {
        runcamDeviceSendPacket(device, RCDEVICE_PROTOCOL_COMMAND_CAMERA_CONTROL, &operation, sizeof(operation));
    } else {
        return false;
    }

    return true;
}

// every time start to control the OSD menu of camera, must call this method to
// camera
bool runcamDeviceOpen5KeyOSDCableConnection(runcamDevice_t *device)
{
    uint8_t actionID = 0xFF;
    uint8_t code = 0xFF;
    bool r = runcamDeviceSend5KeyOSDCableConnectionEvent(device, RCDEVICE_PROTOCOL_5KEY_CONNECTION_OPEN, &actionID, &code);
    return r && (code == 1) && (actionID == RCDEVICE_PROTOCOL_5KEY_CONNECTION_OPEN);
}

// when the control was stop, must call this method to the camera to disconnect
// with camera.
bool runcamDeviceClose5KeyOSDCableConnection(runcamDevice_t *device)
{
    uint8_t actionID = 0xFF;
    uint8_t code = 0xFF;
    bool r = runcamDeviceSend5KeyOSDCableConnectionEvent(device, RCDEVICE_PROTOCOL_5KEY_CONNECTION_CLOSE, &actionID, &code);

    return r && (code == 1) && (actionID == RCDEVICE_PROTOCOL_5KEY_CONNECTION_CLOSE);
}

// simulate button press event of 5 key osd cable with special button
bool runcamDeviceSimulate5KeyOSDCableButtonPress(runcamDevice_t *device, uint8_t operation)
{
    if (operation == RCDEVICE_PROTOCOL_5KEY_SIMULATION_NONE) {
        return false;
    }

    if (runcamDeviceSendRequestAndWaitingResp(device, RCDEVICE_PROTOCOL_COMMAND_5KEY_SIMULATION_PRESS, &operation, sizeof(uint8_t), NULL, NULL)) {
        return true;
    }

    return false;
}

// simulate button release event of 5 key osd cable
bool runcamDeviceSimulate5KeyOSDCableButtonRelease(runcamDevice_t *device)
{
    return runcamDeviceSendRequestAndWaitingResp(device, RCDEVICE_PROTOCOL_COMMAND_5KEY_SIMULATION_RELEASE, NULL, 0, NULL, NULL);
}

#endif