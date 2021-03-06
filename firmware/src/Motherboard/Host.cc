/*
 * Copyright 2010 by Adam Mayer	 <adam@makerbot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include "Host.hh"
#include "Command.hh"
#include <string.h>
#include "Commands.hh"
#include "Steppers.hh"
#include "Configuration.hh"
#if defined(HONOR_DEBUG_PACKETS) && (HONOR_DEBUG_PACKETS == 1)
	#include "DebugPacketProcessor.hh"
#endif
#include "Tool.hh"
#include "Timeout.hh"
#include "Version.hh"
#include <util/atomic.h>
#include <avr/eeprom.h>
#include <avr/pgmspace.h>
#include <util/delay.h>
#include <avr/wdt.h>
#include "Main.hh"
#include "Errors.hh"
#include "Eeprom.hh"
#include "EepromMap.hh"
#include "EepromDefaults.hh"
#include "stdio.h"

namespace host {

/// Identify a command packet, and process it.  If the packet is a command
/// packet, return true, indicating that the packet has been queued and no
/// other processing needs to be done. Otherwise, processing of this packet
/// should drop through to the next processing level.
bool processCommandPacket(const InPacket& from_host, OutPacket& to_host);
bool processQueryPacket(const InPacket& from_host, OutPacket& to_host);

// Timeout from time first bit recieved until we abort packet reception
Timeout packet_in_timeout;
Timeout cancel_timeout;
Timeout do_host_reset_timeout;

#define HOST_PACKET_TIMEOUT_MS 200
#define HOST_PACKET_TIMEOUT_MICROS (1000L*HOST_PACKET_TIMEOUT_MS)

#define HOST_TOOL_RESPONSE_TIMEOUT_MS 50
#define HOST_TOOL_RESPONSE_TIMEOUT_MICROS (1000L*HOST_TOOL_RESPONSE_TIMEOUT_MS)

char machineName[MAX_MACHINE_NAME_LEN + 1];

char buildName[MAX_FILE_LEN];

uint32_t buildSteps;

/// Used to indicate what the UI should do, and used by
/// host process to know what state it's in for error/command allowed.
/// doesn't change state machine per-se, but sets context for other cmds.
HostState currentState = HOST_STATE_READY;

/// Used to indicate the status of the current or last finished print
/// is queryable by repG and by the stats screen during builds
BuildState buildState = BUILD_NONE;

/// queryable time for last print
uint8_t last_print_hours = 0;
uint8_t last_print_minutes = 0;

uint32_t last_print_line = 0;

/// counter for current print time
uint8_t print_time_hours = 0;
Timeout print_time;

const static uint32_t ONE_HOUR = 3600000000U;


bool do_host_reset = false;
bool hard_reset = false;
bool cancelBuild = false;

void runHostSlice() {
	// If we're cancelling the build, and we have completed pausing,
	// then we cancel the build
	if (( buildState == BUILD_CANCELLING ) && ( command::pauseState() == PAUSE_STATE_PAUSED )) {
		stopBuildNow();
	}

        InPacket& in = UART::getHostUART().in;
        OutPacket& out = UART::getHostUART().out;
	if (out.isSending() &&
	    (( ! do_host_reset) || (do_host_reset && (! do_host_reset_timeout.hasElapsed())))) {
		return;
	}

	// soft reset the machine unless waiting to notify repG that a cancel has occured
	if (do_host_reset && (!cancelBuild || cancel_timeout.hasElapsed())){

		if((buildState == BUILD_RUNNING) || (buildState == BUILD_PAUSED)){
			stopBuild();
		}
		do_host_reset = false;

		// reset local board
		reset(hard_reset);

        // hard_reset can be called, but is not called by any
        // a hard reset calls the start up sound and resets heater errors
		hard_reset = false;
		packet_in_timeout.abort();

		// Clear the machine and build names
		machineName[0] = 0;
		buildName[0] = 0;
		currentState = HOST_STATE_READY;

		return;
	}
	// new packet coming in
	if (in.isStarted() && !in.isFinished()) {
		if (!packet_in_timeout.isActive()) {
			// initiate timeout
			packet_in_timeout.start(HOST_PACKET_TIMEOUT_MICROS);
		} else if (packet_in_timeout.hasElapsed()) {
			in.timeout();
		}

	}
	if (in.hasError()) {
		// Reset packet quickly and start handling the next packet.
		packet_in_timeout.abort();
		out.reset();

		// Report error code.
		switch (in.getErrorCode()){
			case PacketError::PACKET_TIMEOUT:
				out.append8(RC_PACKET_TIMEOUT);
				break;
			case PacketError::BAD_CRC:
				out.append8(RC_CRC_MISMATCH);
				break;
			case PacketError::EXCEEDED_MAX_LENGTH:
				out.append8(RC_PACKET_LENGTH);
				break;
			default:
				//PacketError::NOISE_BYTE and PacketError::APPEND_BUFFER_OVERFLOW
				out.append8(RC_PACKET_ERROR);
				break;
		}

		in.reset();
		UART::getHostUART().beginSend();
		Motherboard::getBoard().indicateError(ERR_HOST_PACKET_MISC);

	}
	else if (in.isFinished() == 1) {
		packet_in_timeout.abort();
		out.reset();
		if(cancelBuild){
			out.append8(RC_CANCEL_BUILD);
			cancelBuild = false;
			Motherboard::getBoard().indicateError(ERR_CANCEL_BUILD);
		} else
#if defined(HONOR_DEBUG_PACKETS) && (HONOR_DEBUG_PACKETS == 1)
		if (processDebugPacket(in, out)) {
			// okay, processed
		} else
#endif
		if (processCommandPacket(in, out)) {
			// okay, processed
		} else if (processQueryPacket(in, out)) {
			// okay, processed
		} else {
			// Unrecognized command
			out.append8(RC_CMD_UNSUPPORTED);
		}
		in.reset();
                UART::getHostUART().beginSend();
	}
	/// mark new state as ready if done building from SD
	if(currentState==HOST_STATE_BUILDING_FROM_SD)
	{
		if(!sdcard::isPlaying())
			currentState = HOST_STATE_READY;
	}
	// mark new state as ready if done buiding onboard script
	managePrintTime();
}

/** Identify a command packet, and process it.  If the packet is a command
 * packet, return true, indicating that the packet has been queued and no
 * other processing needs to be done. Otherwise, processing of this packet
 * should drop through to the next processing level.
 */
bool processCommandPacket(const InPacket& from_host, OutPacket& to_host) {
	if (from_host.getLength() >= 1) {
		uint8_t command = from_host.read8(0);
		if ((command & 0x80) != 0) {
			// If we're capturing a file to an SD card, we send it to the sdcard module
			// for processing.
			if (sdcard::isCapturing()) {
				sdcard::capturePacket(from_host);
				to_host.append8(RC_OK);
				return true;
                        }
			if(sdcard::isPlaying()){
				// ignore action commands if SD card build is playing
				to_host.append8(RC_BOT_BUILDING);
				return true;
			}
			// Queue command, if there's room.
			// Turn off interrupts while querying or manipulating the queue!
			ATOMIC_BLOCK(ATOMIC_FORCEON) {
				const uint8_t command_length = from_host.getLength();
				if (command::getRemainingCapacity() >= command_length) {
					// Append command to buffer
					for (int i = 0; i < command_length; i++) {
						command::push(from_host.read8(i));
					}
					to_host.append8(RC_OK);
				} else {
					to_host.append8(RC_BUFFER_OVERFLOW);
				}
			}
			return true;
		}
	}
	return false;
}

// Received driver version info, and request for fw version info.
// puts fw version into a reply packet, and send it back
inline void handleVersion(const InPacket& from_host, OutPacket& to_host) {

    // Give an error on Replicator G versions older than 0039
    //   HOWEVER, allow RepG 29 for purposes of setting up a second
    //   extruders tool head index
    int16_t rv = from_host.read16(1);
    if( (rv != 29) && (rv < 39) ) {
        to_host.append8(RC_OK);
        to_host.append16(0x0000);
    }
    else  {
        to_host.append8(RC_OK);
        to_host.append16(firmware_version);
    }

}

// Received driver version info, and request for fw version info.
// puts fw version into a reply packet, and send it back
inline void handleGetAdvancedVersion(const InPacket& from_host, OutPacket& to_host) {

	// we're not doing anything with the host version at the moment
	from_host.read16(1);	//uint16_t host_version

	to_host.append8(RC_OK);
	to_host.append16(firmware_version);
	to_host.append16(internal_version);
	to_host.append8(SOFTWARE_VARIANT_ID);
	to_host.append8(0);
	to_host.append16(0);

}

    // return build name
inline void handleGetBuildName(const InPacket& from_host, OutPacket& to_host) {
	to_host.append8(RC_OK);
	for (uint8_t idx = 0; idx < sizeof(buildName); idx++) {
	  to_host.append8(buildName[idx]);
	  if (buildName[idx] == '\0') break;
	}
}

inline void handleGetBufferSize(const InPacket& from_host, OutPacket& to_host) {
	to_host.append8(RC_OK);
	to_host.append32(command::getRemainingCapacity());
}

inline void handleGetPosition(const InPacket& from_host, OutPacket& to_host) {
	ATOMIC_BLOCK(ATOMIC_FORCEON) {
		const Point p = steppers::getStepperPosition();
		to_host.append8(RC_OK);
		to_host.append32(p[0]);
		to_host.append32(p[1]);
		to_host.append32(p[2]);
		// From spec:
		// endstop status bits: (7-0) : | N/A | N/A | z max | z min | y max | y min | x max | x min |
		uint8_t endstop_status = steppers::getEndstopStatus();
		to_host.append8(endstop_status);
	}
}

inline void handleGetPositionExt(const InPacket& from_host, OutPacket& to_host) {
	ATOMIC_BLOCK(ATOMIC_FORCEON) {
		const Point p = steppers::getStepperPosition();
		to_host.append8(RC_OK);
		to_host.append32(p[0]);
		to_host.append32(p[1]);
		to_host.append32(p[2]);
#if STEPPER_COUNT > 3
		to_host.append32(p[3]);
		to_host.append32(p[4]);
#else
		to_host.append32(0);
		to_host.append32(0);
#endif
		// From spec:
		// endstop status bits: (15-0) : | b max | b min | a max | a min | z max | z min | y max | y min | x max | x min |
		uint8_t endstop_status = steppers::getEndstopStatus();

		to_host.append16((uint16_t)endstop_status);
	}
}

    // capture to SD
inline void handleCaptureToFile(const InPacket& from_host, OutPacket& to_host) {
	// Drop the file into the current working directory
	// To instead put it into root, uncomment the next line
	// sdcard::forceReinit();

	char *p = (char*)from_host.getData() + 1;
	to_host.append8(RC_OK);
	to_host.append8((uint8_t)sdcard::startCapture(p));
}

    // stop capture to SD
inline void handleEndCapture(const InPacket& from_host, OutPacket& to_host) {
	to_host.append8(RC_OK);
	to_host.append32(sdcard::finishCapture());
	sdcard::reset();
}

    // playback from SD
inline void handlePlayback(const InPacket& from_host, OutPacket& to_host) {
	// Drop the file into the current working directory
	// To instead put it into root, uncomment the next line
	// sdcard::forceReinit();

	to_host.append8(RC_OK);
	for (uint8_t idx = 1; (idx < from_host.getLength()) && (idx < sizeof(buildName)); idx++) {
		buildName[idx-1] = from_host.read8(idx);
	}
	buildName[sizeof(buildName)-1] = '\0';

	to_host.append8((uint8_t)startBuildFromSD(0));
}

    // retrieve SD file names
void handleNextFilename(const InPacket& from_host, OutPacket& to_host) {
	to_host.append8(RC_OK);
	uint8_t resetFlag = from_host.read8(1);
	if (resetFlag != 0) {
		// force the filesystem back to root
		// sdcard::forceReinit();
		sdcard::SdErrorCode e = sdcard::directoryReset();
		if (e != sdcard::SD_SUCCESS && e != sdcard::SD_ERR_CARD_LOCKED) {
			to_host.append8(e);
			to_host.append8(0);
			return;
		}
	}
	char fnbuf[MAX_FILE_LEN];
	bool isdir;
	// Ignore dot-files
	do {
		sdcard::directoryNextEntry(fnbuf,sizeof(fnbuf),&isdir);
		if (fnbuf[0] == '\0') break;
		else if ( (fnbuf[0] != '.') ||
			  ( isdir && fnbuf[1] == '.' && fnbuf[2] == 0) ) break;
	} while (true);
	// Note that the old directoryNextEntry() always returned SD_SUCCESS
	to_host.append8(sdcard::SD_SUCCESS);
	uint8_t idx;
	for (idx = 0; (idx < sizeof(fnbuf)) && (fnbuf[idx] != 0); idx++)
		to_host.append8(fnbuf[idx]);
	to_host.append8(0);
}

void doToolPause(OutPacket& to_host) {
	Timeout acquire_lock_timeout;
	acquire_lock_timeout.start(HOST_TOOL_RESPONSE_TIMEOUT_MICROS);
	while (!tool::getLock()) {
		if (acquire_lock_timeout.hasElapsed()) {
			to_host.append8(RC_DOWNSTREAM_TIMEOUT);
                        Motherboard::getBoard().indicateError(ERR_SLAVE_LOCK_TIMEOUT);
			return;
		}
	}
	OutPacket& out = tool::getOutPacket();
	InPacket& in = tool::getInPacket();
	out.reset();
	out.append8(tool::getCurrentToolheadIndex());
	out.append8(SLAVE_CMD_PAUSE_UNPAUSE);
	// Timeouts are handled inside the toolslice code; there's no need
	// to check for timeouts on this loop.
	tool::startTransaction();
	tool::releaseLock();
	// WHILE: bounded by tool timeout in runToolSlice
	while (!tool::isTransactionDone()) {
		tool::runToolSlice();
	}
	if (in.getErrorCode() == PacketError::PACKET_TIMEOUT) {
		to_host.append8(RC_DOWNSTREAM_TIMEOUT);
	} else {
		// Copy payload back. Start from 0-- we need the response code.
		for (int i = 0; i < in.getLength(); i++) {
			to_host.append8(in.read8(i));
		}
	}
}

void handleToolQuery(const InPacket& from_host, OutPacket& to_host) {
	// Quick sanity assert: ensure that host packet length >= 2
	// (Payload must contain toolhead address and at least one byte)
	if (from_host.getLength() < 2) {
		to_host.append8(RC_PACKET_ERROR);
                Motherboard::getBoard().indicateError(ERR_HOST_TRUNCATED_CMD);
		return;
	}
	Timeout acquire_lock_timeout;
	acquire_lock_timeout.start(HOST_TOOL_RESPONSE_TIMEOUT_MICROS);
	while (!tool::getLock()) {
		if (acquire_lock_timeout.hasElapsed()) {
			to_host.append8(RC_DOWNSTREAM_TIMEOUT);
                        Motherboard::getBoard().indicateError(ERR_SLAVE_LOCK_TIMEOUT);
			return;
		}
	}
	OutPacket& out = tool::getOutPacket();
	InPacket& in = tool::getInPacket();
	out.reset();
	for (int i = 1; i < from_host.getLength(); i++) {
		out.append8(from_host.read8(i));
	}
	// Timeouts are handled inside the toolslice code; there's no need
	// to check for timeouts on this loop.
	tool::startTransaction();
	tool::releaseLock();
	// WHILE: bounded by tool timeout in runToolSlice
	while (!tool::isTransactionDone()) {
		tool::runToolSlice();
	}
	if (in.getErrorCode() == PacketError::PACKET_TIMEOUT) {
		to_host.append8(RC_DOWNSTREAM_TIMEOUT);
	} else {
		// Copy payload back. Start from 0-- we need the response code.
		for (int i = 0; i < in.getLength(); i++) {
			to_host.append8(in.read8(i));
		}
	}
}

inline void handlePause(const InPacket& from_host, OutPacket& to_host) {
	//If we're either pausing or unpausing, but we haven't completed
	//the operation yet, we ignore this request
	if (!command::pauseIntermediateState()) {
		/// this command also calls the host::pauseBuild() command
		pauseBuild(!command::isPaused(), PAUSE_EXT_OFF | PAUSE_HBP_OFF);
		doToolPause(to_host);
	}

	to_host.append8(RC_OK);
}

    // check if steppers are still executing a command
inline void handleIsFinished(const InPacket& from_host, OutPacket& to_host) {
	to_host.append8(RC_OK);
	ATOMIC_BLOCK(ATOMIC_FORCEON) {
		bool done = !steppers::isRunning() && command::isEmpty();
		to_host.append8(done?1:0);
	}
}

    // read value from eeprom
void handleReadEeprom(const InPacket& from_host, OutPacket& to_host) {

    uint16_t offset = from_host.read16(1);
    uint8_t length = from_host.read8(3);
    uint8_t data[length];
    eeprom_read_block(data, (const void*) offset, length);
    to_host.append8(RC_OK);
    for (int i = 0; i < length; i++) {
        to_host.append8(data[i]);
    }
}

/**
 * writes a chunk of data from a input packet to eeprom
 */
void handleWriteEeprom(const InPacket& from_host, OutPacket& to_host) {
    uint16_t offset = from_host.read16(1);
    uint8_t length = from_host.read8(3);
    uint8_t data[length];
    eeprom_read_block(data, (const void*) offset, length);
    for (int i = 0; i < length; i++) {
        data[i] = from_host.read8(i + 4);
    }
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE){
		eeprom_write_block(data, (void*) offset, length);
	}
    to_host.append8(RC_OK);
    to_host.append8(length);
}

enum { // bit assignments
	ES_STEPPERS = 0, // stop steppers
	ES_COMMANDS = 1  // clean queue
};

    // stop steppers and command execution
inline void handleExtendedStop(const InPacket& from_host, OutPacket& to_host) {
	uint8_t flags = from_host.read8(1);
	if (flags & _BV(ES_STEPPERS)) {
		steppers::abort();
	}
	if (flags & _BV(ES_COMMANDS)) {
		command::reset();
	}

	to_host.append8(RC_OK);
	to_host.append8(0);
}

    //set build name and build state
void handleBuildStartNotification(CircularBuffer& buf) {

	uint8_t idx = 0;
	switch (currentState){
		case HOST_STATE_BUILDING_FROM_SD:
			while ((uint8_t)buf.pop() != 0) ;
			break;
		case HOST_STATE_READY:
#ifdef PSTOP_SUPPORT
		        command::pstop_triggered = false;
		        command::pstop_okay = false;
#endif
			currentState = HOST_STATE_BUILDING;
		case HOST_STATE_BUILDING:
			do {
				buildName[idx++] = buf.pop();
                        } while ((buildName[idx-1] != '\0') && (idx < sizeof(buildName)));
			break;
		default:
			break;
	}
	Motherboard::getBoard().resetCurrentSeconds();
	startPrintTime();
	command::clearLineNumber();
	buildState = BUILD_RUNNING;
}

    // set build state to ready
void handleBuildStopNotification(uint8_t stopFlags) {
	if ( command::copiesToPrint == 0 || command::copiesPrinted >= (command::copiesToPrint - 1)) {
		stopPrintTime();
		last_print_line = command::getLineNumber();
		command::pauseHeaters(PAUSE_EXT_OFF | PAUSE_HBP_OFF);
		buildState = BUILD_FINISHED_NORMALLY;
		currentState = HOST_STATE_READY;
	}
}

/// get current print stats if printing, or last print stats if not printing
inline void handleGetBuildStats(OutPacket& to_host) {
        to_host.append8(RC_OK);

		uint8_t hours;
		uint8_t minutes;

		getPrintTime(hours, minutes);

        to_host.append8(buildState);
        to_host.append8(hours);
        to_host.append8(minutes);
        if((buildState == BUILD_RUNNING) || (buildState == BUILD_PAUSED)){
			to_host.append32(command::getLineNumber());
		} else {
			to_host.append32(last_print_line);
		}
        to_host.append32(0);// open spot for filament detect info
}
/// get current print stats if printing, or last print stats if not printing
inline void handleGetBoardStatus(OutPacket& to_host) {
	to_host.append8(RC_OK);
	//Return STATUS_NONE for now
	to_host.append8(0);
}

// query packets (non action, not queued)
bool processQueryPacket(const InPacket& from_host, OutPacket& to_host) {
	if (from_host.getLength() >= 1) {
		uint8_t command = from_host.read8(0);
		if ((command & 0x80) == 0) {
			// Is query command.
			switch (command) {
			case HOST_CMD_VERSION:
				handleVersion(from_host,to_host);
				return true;
			case HOST_CMD_GET_BUILD_NAME:
				handleGetBuildName(from_host,to_host);
				return true;
			case HOST_CMD_INIT:
				// There's really nothing we want to do here; we don't want to
				// interrupt a running build, for example.
				to_host.append8(RC_OK);
				return true;
			case HOST_CMD_CLEAR_BUFFER: // equivalent at current time
			case HOST_CMD_ABORT: // equivalent at current time
			case HOST_CMD_RESET:
			{
			        bool resetMe = true;
#ifdef HAS_FILAMENT_COUNTER
				command::addFilamentUsed();
#endif
				if (currentState == HOST_STATE_BUILDING ||
				    currentState == HOST_STATE_BUILDING_FROM_SD) {
				     if (1 == eeprom::getEeprom8(eeprom::CLEAR_FOR_ESTOP, 0)) {
					  buildState = BUILD_CANCELED;
					  resetMe = false;
					  stopBuild();
				     }
				     Motherboard::getBoard().indicateError(ERR_RESET_DURING_BUILD);
				}
				if ( resetMe ) {
				     do_host_reset = true; // indicate reset after response has been sent
				     do_host_reset_timeout.start(200000);	//Protection against the firmware sending to a down host
				}
				to_host.append8(RC_OK);
				return true;
			}
			case HOST_CMD_GET_BUFFER_SIZE:
				handleGetBufferSize(from_host,to_host);
				return true;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winline"
			case HOST_CMD_GET_POSITION:
				handleGetPosition(from_host,to_host);
				return true;
			case HOST_CMD_GET_POSITION_EXT:
				handleGetPositionExt(from_host,to_host);
				return true;
#pragma GCC diagnostic pop
			case HOST_CMD_CAPTURE_TO_FILE:
				handleCaptureToFile(from_host,to_host);
				return true;
			case HOST_CMD_END_CAPTURE:
				handleEndCapture(from_host,to_host);
				return true;
			case HOST_CMD_PLAYBACK_CAPTURE:
				handlePlayback(from_host,to_host);
				return true;
			case HOST_CMD_NEXT_FILENAME:
				handleNextFilename(from_host,to_host);
				return true;
			case HOST_CMD_PAUSE:
				handlePause(from_host,to_host);
				return true;
			case HOST_CMD_TOOL_QUERY:
				handleToolQuery(from_host,to_host);
				return true;
			case HOST_CMD_IS_FINISHED:
				handleIsFinished(from_host,to_host);
				return true;
			case HOST_CMD_READ_EEPROM:
				handleReadEeprom(from_host,to_host);
				return true;
			case HOST_CMD_WRITE_EEPROM:
				handleWriteEeprom(from_host,to_host);
				return true;
			case HOST_CMD_EXTENDED_STOP:
				handleExtendedStop(from_host,to_host);
				return true;
			case HOST_CMD_BOARD_STATUS:
				handleGetBoardStatus(to_host);
				return true;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winline"
			case HOST_CMD_GET_BUILD_STATS:
				handleGetBuildStats(to_host);
				return true;
#pragma GCC diagnostic pop
			case HOST_CMD_ADVANCED_VERSION:
				handleGetAdvancedVersion(from_host, to_host);
				return true;
			}
		}
	}
	return false;
}

char* getMachineName() {
	// If the machine name hasn't been loaded, load it
	if (machineName[0] == 0) {
		// WARNING: Owing to a bug in SanguinoDriver.java and
		//    MightyBoard.java, all versions of RepG up to and
		//    including RepG 0040 would NOT NUL terminate the
		//    string they sent to the bot's EEPROM if it had
		//    length >= 16.  As such this string can NOT be assumed
		//    to be NUL terminated.
		//
		//  This was fixed in RepG 40r4 Sailfish on 1 Feb 2013
		for(uint8_t i = 0; i < MAX_MACHINE_NAME_LEN; i++) {
			machineName[i] = eeprom::getEeprom8(eeprom::MACHINE_NAME+i, EEPROM_DEFAULT_MACHINE_NAME);
		}
		machineName[MAX_MACHINE_NAME_LEN] = 0;
	}

	// If it's still zero, load in a default.
	const static PROGMEM prog_uchar defaultMachineName[] = "Thing-O-Matic";

	if (machineName[0] == 0) {
		for(uint8_t i = 0; i < 13; i++) {
			machineName[i] = pgm_read_byte_near(defaultMachineName+i);
		}
		machineName[13] = '\0';
	}

	return machineName;
}

char* getBuildName() {
	return buildName;
}

HostState getHostState() {
	return currentState;
}

BuildState getBuildState() {
	return buildState;
}

sdcard::SdErrorCode startBuildFromSD(char *fname) {
	sdcard::SdErrorCode e;

	// See if we should copy the file name to the build name
	if ( !fname )
		// The filename is already stored in the build name
		fname = buildName;
	else if (fname != buildName ) {
		// Copy a possibly truncated version of the file name to the build name
		uint8_t i = 0;
		while ( fname[i] )
		{
			buildName[i] = fname[i];
			if ( ++i >= (sizeof(buildName) - 1) ) break;
		}
		buildName[i] = 0;
	}

	// Attempt to start build
	e = sdcard::startPlayback(fname);
	if (e == sdcard::SD_CWD) return sdcard::SD_SUCCESS;
	else if (e != sdcard::SD_SUCCESS) {
		// TODO: report error
		return e;
	}

	command::reset();
	steppers::reset();
	steppers::abort();

	// Must be done after command::reset();
	command::copiesToPrint = eeprom::getEeprom8(eeprom::ABP_COPIES, EEPROM_DEFAULT_ABP_COPIES);
	currentState = HOST_STATE_BUILDING_FROM_SD;

	return e;
}

// Stop the current build, if any
void stopBuildNow() {
    // if building from repG, try to send a cancel msg to repG before reseting
	if(currentState == HOST_STATE_BUILDING)
	{
		currentState = HOST_STATE_CANCEL_BUILD;
		cancel_timeout.start(1000000); //look for commands from repG for one second before resetting
		cancelBuild = true;
	}
	last_print_line = command::getLineNumber();
	stopPrintTime();
	do_host_reset = true; // indicate reset after response has been sent
	do_host_reset_timeout.start(200000);	//Protection against the firmware sending to a down host
	buildState = BUILD_CANCELED;
}

// Stop the current build, if any via an intermediate state (BUILD_CANCELLING),
// where we pause first and when that's complete we call stopBuildNow to cancel the
// print.  The purpose of the pause is to move the build away from the tool head.
void stopBuild() {
	buildState = BUILD_CANCELLING;

	steppers::abort();

	//If we're already paused, we stop the print now, otherwise we pause
	//The runSlice picks up this pause later when completed, then calls stopBuildNow
	if (( command::isPaused() ) || ( command::pauseIntermediateState() )) {
	    stopBuildNow();
	} else {
	    command::pause(true, 0);
	}
}

/// update state variables if print is paused
void pauseBuild(bool pause, uint8_t heaterControl) {

	/// don't update time or state if we are already in the desired state
	if (!(pause == command::isPaused())){

		//If we're either pausing or unpausing, but we haven't completed
		//the operation yet, we ignore this request
		if (command::pauseIntermediateState())
			return;

		command::pause(pause, heaterControl);
		if(pause){
			buildState = BUILD_PAUSED;
			print_time.pause(true);
		}else{
			buildState = BUILD_RUNNING;
			print_time.pause(false);
		}
	}
}

void startPrintTime(){
	print_time.start(ONE_HOUR);
	print_time_hours = 0;
}

void stopPrintTime(){

	getPrintTime(last_print_hours, last_print_minutes);
	print_time = Timeout();
	print_time_hours = 0;
}

void managePrintTime(){

	/// print time is precise to the host loop frequency
	if (print_time.hasElapsed()){
		print_time.start(ONE_HOUR);
		print_time_hours++;
	}
}

/// returns time hours and minutes since the start of the print
void getPrintTime(uint8_t& hours, uint8_t& minutes){

	hours = print_time_hours;
	minutes = print_time.getCurrentElapsed() / 60000000;
	return;
}

// Reset the current build, used for ATX power on reset
void resetBuild() {
	machineName[0] = 0;
	buildName[0] = 0;
	currentState = HOST_STATE_READY;
}

bool isBuildComplete() {
	if (( command::isEmpty() ) && ( ! sdcard::playbackHasNext() ))	return true;
	return false;
}

}

/* footnote 1: due to a protocol change, replicatiorG 0026 and newer can ONLY work with
 * firmware 3.00 and newer. Because replicatorG handles version mismatches poorly,
 * if our firmware is 3.0 or newer, *AND* the connecting replicatorG is 25 or older, we
 * lie, and reply with firmware 0.00 to case ReplicatorG to display a 'null version' error
 * so users will know to upgrade.
 */
