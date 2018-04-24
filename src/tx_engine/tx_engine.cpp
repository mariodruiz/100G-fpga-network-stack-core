/************************************************
Copyright (c) 2016, Xilinx, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.// Copyright (c) 2015 Xilinx, Inc.
************************************************/

#include "tx_engine.hpp"

using namespace hls;

/** @ingroup tx_engine
 *  @name metaLoader
 *  The metaLoader reads the Events from the EventEngine then it loads all the necessary MetaData from the data
 *  structures (RX & TX Sar Table). Depending on the Event type it generates the necessary MetaData for the
 *  ipHeaderConstruction and the pseudoHeaderConstruction.
 *  Additionally it requests the IP Tuples from the Session. In some special cases the IP Tuple is delivered directly
 *  from @ref rx_engine and does not have to be loaded from the Session Table. The isLookUpFifo indicates this special cases.
 *  Lookup Table for the current session.
 *  Depending on the Event Type the retransmit or/and probe Timer is set.
 *  @param[in]		eventEng2txEng_event
 *  @param[in]		rxSar2txEng_upd_rsp
 *  @param[in]		txSar2txEng_upd_rsp
 *  @param[out]		txEng2rxSar_upd_req
 *  @param[out]		txEng2txSar_upd_req
 *  @param[out]		txEng2timer_setRetransmitTimer
 *  @param[out]		txEng2timer_setProbeTimer
 *  @param[out]		txEng_ipMetaFifoOut
 *  @param[out]		txEng_tcpMetaFifoOut
 *  @param[out]		txBufferReadCmd
 *  @param[out]		txEng2sLookup_rev_req
 *  @param[out]		txEng_isLookUpFifoOut
 *  @param[out]		txEng_tupleShortCutFifoOut
 */
void metaLoader(stream<extendedEvent>&				eventEng2txEng_event,
				stream<rxSarEntry>&					rxSar2txEng_rsp,
				stream<txTxSarReply>&				txSar2txEng_upd_rsp,
				stream<ap_uint<16> >&				txEng2rxSar_req,
				stream<txTxSarQuery>&				txEng2txSar_upd_req,
				stream<txRetransmitTimerSet>&		txEng2timer_setRetransmitTimer,
				stream<ap_uint<16> >&				txEng2timer_setProbeTimer,
				stream<ap_uint<16> >&				txEng_ipMetaFifoOut,
				stream<tx_engine_meta>&				txEng_tcpMetaFifoOut,
				stream<mmCmd>&						txBufferReadCmd,
				stream<ap_uint<16> >&				txEng2sLookup_rev_req,
				stream<bool>&						txEng_isLookUpFifoOut,
#if (TCP_NODELAY)
				stream<bool>&						txEng_isDDRbypass,
#endif
				stream<fourTuple>&					txEng_tupleShortCutFifoOut,
				stream<ap_uint<1> >&				readCountFifo)
{
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	static ap_uint<1> ml_FsmState = 0;
	static bool ml_sarLoaded = false;
	static extendedEvent ml_curEvent;
	static ap_uint<32> ml_randomValue= 0x562301af; //Random seed initialization

	static ap_uint<2> ml_segmentCount = 0;
	static rxSarEntry	rxSar;
	static txTxSarReply	txSar;
	ap_uint<16> windowSize;
	ap_uint<16> currLength;
	ap_uint<16> usableWindow;
	ap_uint<16> slowstart_threshold;
	static tx_engine_meta meta;
	rstEvent resetEvent;

	switch (ml_FsmState) {
		case 0:
			if (!eventEng2txEng_event.empty()) {
				eventEng2txEng_event.read(ml_curEvent);
				readCountFifo.write(1);
				ml_sarLoaded = false;
				//NOT necessary for SYN/SYN_ACK only needs one
				switch (ml_curEvent.type) {
					case RT:
						txEng2rxSar_req.write(ml_curEvent.sessionID);
						txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID));
						break;
					case TX:
						txEng2rxSar_req.write(ml_curEvent.sessionID);
						txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID));
						break;
					case SYN_ACK:
						txEng2rxSar_req.write(ml_curEvent.sessionID);
						txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID));
						break;
					case FIN:
						txEng2rxSar_req.write(ml_curEvent.sessionID);
						txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID));
						break;
					case RST:
						// Get txSar for SEQ numb
						resetEvent = ml_curEvent;
						if (resetEvent.hasSessionID()) {
							txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID));
						}
						break;
					case ACK_NODELAY:
					case ACK:
						txEng2rxSar_req.write(ml_curEvent.sessionID);
						txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID));
						break;
					case SYN:
						if (ml_curEvent.rt_count != 0) {
							txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID));
						}
						break;
					default:
						break;
				}
				ml_FsmState = 1;
				ml_randomValue++; //make sure it doesn't become zero TODO move this out of if, but breaks my testsuite
			} //if not empty
			ml_segmentCount = 0;
			break;
		case 1:
			switch(ml_curEvent.type)
			{
			// When Nagle's algorithm disabled
			// Can bypass DDR
#if (TCP_NODELAY)
			case TX:
				if ((!rxSar2txEng_rsp.empty() && !txSar2txEng_upd_rsp.empty()) || ml_sarLoaded) {
					if (!ml_sarLoaded) {
						rxSar2txEng_rsp.read(rxSar);
						txSar2txEng_upd_rsp.read(txSar);
					}

					//Compute our space, Advertise at least a quarter/half, otherwise 0

					windowSize = (rxSar.appd - ((ap_uint<16>)rxSar.recvd)) - 1; // This works even for wrap around
					meta.ackNumb = rxSar.recvd;
					meta.seqNumb = txSar.not_ackd;
					meta.window_size = windowSize;
					meta.ack = 1; // ACK is always set when established
					meta.rst = 0;
					meta.syn = 0;
					meta.fin = 0;
					//meta.length = 0;

					currLength = ml_curEvent.length;
					ap_uint<16> usedLength = ((ap_uint<16>) txSar.not_ackd - txSar.ackd);
					// min_window, is the min(txSar.recv_window, txSar.cong_window)
					if (txSar.min_window > usedLength) {
						usableWindow = txSar.min_window - usedLength;
					}
					else {
						usableWindow = 0;
					}
					if (usableWindow < ml_curEvent.length) {
						txEng2timer_setProbeTimer.write(ml_curEvent.sessionID);
					}

					meta.length = ml_curEvent.length;

					//TODO some checking
					txSar.not_ackd += ml_curEvent.length;

					txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID, txSar.not_ackd, 1));
					ml_FsmState = 0;


					/*if (meta.length != 0)
					{
						//txBufferReadCmd.write(mmCmd(pkgAddr, meta.length));
					}*/
					// Send a packet only if there is data or we want to send an empty probing message
					if (meta.length != 0) {// || ml_curEvent.retransmit) //TODO retransmit boolean currently not set, should be removed
					
						txEng_ipMetaFifoOut.write(meta.length);
						txEng_tcpMetaFifoOut.write(meta);
						txEng_isLookUpFifoOut.write(true);
						txEng_isDDRbypass.write(true);
						txEng2sLookup_rev_req.write(ml_curEvent.sessionID);

						// Only set RT timer if we actually send sth, TODO only set if we change state and sent sth
						txEng2timer_setRetransmitTimer.write(txRetransmitTimerSet(ml_curEvent.sessionID));
					}//TODO if probe send msg length 1
					ml_sarLoaded = true;
				}

				break;
#else
			case TX:
				// Sends everyting between txSar.not_ackd and txSar.app
				if ((!rxSar2txEng_rsp.empty() && !txSar2txEng_upd_rsp.empty()) || ml_sarLoaded) {
					if (!ml_sarLoaded) {
						rxSar2txEng_rsp.read(rxSar);
						txSar2txEng_upd_rsp.read(txSar);
					}

					//Compute our space, Advertise at least a quarter/half, otherwise 0
					windowSize = (rxSar.appd - ((ap_uint<16>)rxSar.recvd)) - 1; // This works even for wrap around
					meta.ackNumb = rxSar.recvd;
					meta.seqNumb = txSar.not_ackd;
					meta.window_size = windowSize;
					meta.ack = 1; // ACK is always set when established
					meta.rst = 0;
					meta.syn = 0;
					meta.fin = 0;
					meta.length = 0;

					currLength = (txSar.app - ((ap_uint<16>)txSar.not_ackd));
					ap_uint<16> usedLength = ((ap_uint<16>) txSar.not_ackd - txSar.ackd);
					// min_window, is the min(txSar.recv_window, txSar.cong_window)
					if (txSar.min_window > usedLength)
					{
						usableWindow = txSar.min_window - usedLength;
					}
					else
					{
						usableWindow = 0;
					}
					// Construct address before modifying txSar.not_ackd
					ap_uint<32> pkgAddr;
					pkgAddr(31, 30) = 0x01;
					pkgAddr(29, 16) = ml_curEvent.sessionID(13, 0);
					pkgAddr(15, 0) = txSar.not_ackd(15, 0); //ml_curEvent.address;

					// Check length, if bigger than Usable Window or MMS
					if (currLength <= usableWindow) {
						if (currLength >= MSS) { //TODO change to >= MSS, use maxSegmentCount
							// We stay in this state and sent immediately another packet
							txSar.not_ackd += MSS;
							meta.length = MSS;
						}
						else {
							// If we sent all data, there might be a fin we have to sent too
							if (txSar.finReady && (txSar.ackd == txSar.not_ackd || currLength == 0)) {
								ml_curEvent.type = FIN;
							}
							else {
								ml_FsmState = 0;
							}
							// Check if small segment and if unacknowledged data in pipe (Nagle)
							if (txSar.ackd == txSar.not_ackd) {
								txSar.not_ackd += currLength;
								meta.length = currLength;
							}
							else {
								txEng2timer_setProbeTimer.write(ml_curEvent.sessionID);
							}
							// Write back txSar not_ackd pointer
							txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID, txSar.not_ackd, 1));
						}
					}
					else {
						// code duplication, but better timing..
						if (usableWindow >= MSS) {
							// We stay in this state and sent immediately another packet
							txSar.not_ackd += MSS;
							meta.length = MSS;
						}
						else {
							// Check if we sent >= MSS data
							if (txSar.ackd == txSar.not_ackd) {
								txSar.not_ackd += usableWindow;
								meta.length = usableWindow;
							}
							// Set probe Timer to try again later
							txEng2timer_setProbeTimer.write(ml_curEvent.sessionID);
							txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID, txSar.not_ackd, 1));
							ml_FsmState = 0;
						}
					}

					if (meta.length != 0) {
						txBufferReadCmd.write(mmCmd(pkgAddr, meta.length));
					}
					// Send a packet only if there is data or we want to send an empty probing message
					if (meta.length != 0) { // || ml_curEvent.retransmit) //TODO retransmit boolean currently not set, should be removed
						txEng_ipMetaFifoOut.write(meta.length);
						txEng_tcpMetaFifoOut.write(meta);
						txEng_isLookUpFifoOut.write(true);
						txEng2sLookup_rev_req.write(ml_curEvent.sessionID);
						// Only set RT timer if we actually send sth, TODO only set if we change state and sent sth
						txEng2timer_setRetransmitTimer.write(txRetransmitTimerSet(ml_curEvent.sessionID));
					}//TODO if probe send msg length 1
					ml_sarLoaded = true;
				}
				break;
	#endif
			case RT:
				if ((!rxSar2txEng_rsp.empty() && !txSar2txEng_upd_rsp.empty()) || ml_sarLoaded) {
					if (!ml_sarLoaded) {
						rxSar2txEng_rsp.read(rxSar);
						txSar2txEng_upd_rsp.read(txSar);
					}

					// Compute our window size
					windowSize = (rxSar.appd - ((ap_uint<16>)rxSar.recvd)) - 1; // This works even for wrap around
					if (!txSar.finSent) { //no FIN sent
						currLength = ((ap_uint<16>) txSar.not_ackd - txSar.ackd);
					}
					else {//FIN already sent
						currLength = ((ap_uint<16>) txSar.not_ackd - txSar.ackd)-1;
					}

					meta.ackNumb = rxSar.recvd;
					meta.seqNumb = txSar.ackd;
					meta.window_size = windowSize;
					meta.ack = 1; // ACK is always set when session is established
					meta.rst = 0;
					meta.syn = 0;
					meta.fin = 0;

					// Construct address before modifying txSar.ackd
					ap_uint<32> pkgAddr;
					pkgAddr(31, 30) = 0x01;
					pkgAddr(29, 16) = ml_curEvent.sessionID(13, 0);
					pkgAddr(15, 0) = txSar.ackd(15, 0); //ml_curEvent.address;

					// Decrease Slow Start Threshold, only on first RT from retransmitTimer
					if (!ml_sarLoaded && (ml_curEvent.rt_count == 1)) {
						if (currLength > (4*MSS)) {// max( FlightSize/2, 2*MSS) RFC:5681
							slowstart_threshold = currLength/2;
						}
						else {
							slowstart_threshold = (2 * MSS);
						}
						txEng2txSar_upd_req.write(txTxSarRtQuery(ml_curEvent.sessionID, slowstart_threshold));
					}

					// Since we are retransmitting from txSar.ackd to txSar.not_ackd, this data is already inside the usableWindow
					// => no check is required
					// Only check if length is bigger than MMS
					if (currLength > MSS) {
						// We stay in this state and sent immediately another packet
						meta.length = MSS;
						txSar.ackd += MSS;
						// TODO replace with dynamic count, remove this
						if (ml_segmentCount == 3) {
							// Should set a probe or sth??
							//txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID, txSar.not_ackd, 1));
							ml_FsmState = 0;
						}
						ml_segmentCount++;
					}
					else {
						meta.length = currLength;
						if (txSar.finSent) {
							ml_curEvent.type = FIN;
						}
						else {
							// set RT here???
							ml_FsmState = 0;
						}
					}

					// Only send a packet if there is data
					if (meta.length != 0) {
						txBufferReadCmd.write(mmCmd(pkgAddr, meta.length));
						txEng_ipMetaFifoOut.write(meta.length);
						txEng_tcpMetaFifoOut.write(meta);
						txEng_isLookUpFifoOut.write(true);
#if (TCP_NODELAY)
						txEng_isDDRbypass.write(false);
#endif
						txEng2sLookup_rev_req.write(ml_curEvent.sessionID);
						// Only set RT timer if we actually send sth
						txEng2timer_setRetransmitTimer.write(txRetransmitTimerSet(ml_curEvent.sessionID));
					}
					ml_sarLoaded = true;
				}
				break;
			case ACK:
			case ACK_NODELAY:
				if (!rxSar2txEng_rsp.empty() && !txSar2txEng_upd_rsp.empty()) {
					rxSar2txEng_rsp.read(rxSar);
					txSar2txEng_upd_rsp.read(txSar);
					windowSize = (rxSar.appd - ((ap_uint<16>)rxSar.recvd)) - 1;
					meta.ackNumb = rxSar.recvd;
					meta.seqNumb = txSar.not_ackd; //Always send SEQ
					meta.window_size = windowSize;
					meta.length = 0;
					meta.ack = 1;
					meta.rst = 0;
					meta.syn = 0;
					meta.fin = 0;
					txEng_ipMetaFifoOut.write(meta.length);
					txEng_tcpMetaFifoOut.write(meta);
					txEng_isLookUpFifoOut.write(true);
					txEng2sLookup_rev_req.write(ml_curEvent.sessionID);
					ml_FsmState = 0;
				}
				break;
			case SYN:
				if (((ml_curEvent.rt_count != 0) && !txSar2txEng_upd_rsp.empty()) || (ml_curEvent.rt_count == 0)) {
					if (ml_curEvent.rt_count != 0) {
						txSar2txEng_upd_rsp.read(txSar);
						meta.seqNumb = txSar.ackd;
					}
					else {
						txSar.not_ackd = ml_randomValue; // FIXME better rand()
						ml_randomValue = (ml_randomValue* 8) xor ml_randomValue;
						meta.seqNumb = txSar.not_ackd;
						txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID, txSar.not_ackd+1, 1, 1));
					}
					meta.ackNumb = 0;
					//meta.seqNumb = txSar.not_ackd;
					meta.window_size = 0xFFFF;
					meta.length = 4; // For MSS Option, 4 bytes
					meta.ack = 0;
					meta.rst = 0;
					meta.syn = 1;
					meta.fin = 0;

					txEng_ipMetaFifoOut.write(4); //length
					txEng_tcpMetaFifoOut.write(meta);
					txEng_isLookUpFifoOut.write(true);
					txEng2sLookup_rev_req.write(ml_curEvent.sessionID);
					// set retransmit timer
					txEng2timer_setRetransmitTimer.write(txRetransmitTimerSet(ml_curEvent.sessionID, SYN));
					ml_FsmState = 0;
				}
				break;
			case SYN_ACK:
				if (!rxSar2txEng_rsp.empty() && !txSar2txEng_upd_rsp.empty()) {
					rxSar2txEng_rsp.read(rxSar);
					txSar2txEng_upd_rsp.read(txSar);

					// construct SYN_ACK message
					meta.ackNumb = rxSar.recvd;
					meta.window_size = 0xFFFF;
					meta.length = 4; // For MSS Option, 4 bytes
					meta.ack = 1;
					meta.rst = 0;
					meta.syn = 1;
					meta.fin = 0;
					if (ml_curEvent.rt_count != 0) {
						meta.seqNumb = txSar.ackd;
					}
					else {
						txSar.not_ackd = ml_randomValue; // FIXME better rand();
						ml_randomValue = (ml_randomValue* 8) xor ml_randomValue;
						meta.seqNumb = txSar.not_ackd;
						txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID, txSar.not_ackd+1, 1, 1));
					}

					txEng_ipMetaFifoOut.write(4); // length
					txEng_tcpMetaFifoOut.write(meta);
					txEng_isLookUpFifoOut.write(true);
					txEng2sLookup_rev_req.write(ml_curEvent.sessionID);

					// set retransmit timer
					txEng2timer_setRetransmitTimer.write(txRetransmitTimerSet(ml_curEvent.sessionID, SYN_ACK));
					ml_FsmState = 0;
				}
				break;
			case FIN:
				if ((!rxSar2txEng_rsp.empty() && !txSar2txEng_upd_rsp.empty()) || ml_sarLoaded) {
					if (!ml_sarLoaded) {
						rxSar2txEng_rsp.read(rxSar);
						txSar2txEng_upd_rsp.read(txSar);
					}

					//construct FIN message
					windowSize = (rxSar.appd - ((ap_uint<16>)rxSar.recvd)) - 1;
					meta.ackNumb = rxSar.recvd;
					//meta.seqNumb = txSar.not_ackd;
					meta.window_size = windowSize;
					meta.length = 0;
					meta.ack = 1; // has to be set for FIN message as well
					meta.rst = 0;
					meta.syn = 0;
					meta.fin = 1;

					// Check if retransmission, in case of RT, we have to reuse not_ackd number
					if (ml_curEvent.rt_count != 0) {
						meta.seqNumb = txSar.not_ackd-1; //Special case, or use ackd?
					}
					else {
						meta.seqNumb = txSar.not_ackd;
						// Check if all data is sent, otherwise we have to delay FIN message
						// Set fin flag, such that probeTimer is informed
						if (txSar.app == txSar.not_ackd(15, 0)) {
							txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID, txSar.not_ackd+1, 1, 0, true, true));
						}
						else {
							txEng2txSar_upd_req.write(txTxSarQuery(ml_curEvent.sessionID, txSar.not_ackd, 1, 0, true, false));
						}
					}

					// Check if there is a FIN to be sent //TODO maybe restruce this
					if (meta.seqNumb(15, 0) == txSar.app) {
						txEng_ipMetaFifoOut.write(meta.length);
						txEng_tcpMetaFifoOut.write(meta);
						txEng_isLookUpFifoOut.write(true);
						txEng2sLookup_rev_req.write(ml_curEvent.sessionID);
						// set retransmit timer
						//txEng2timer_setRetransmitTimer.write(txRetransmitTimerSet(ml_curEvent.sessionID, FIN));
						txEng2timer_setRetransmitTimer.write(txRetransmitTimerSet(ml_curEvent.sessionID));
					}

					ml_FsmState = 0;
				}
				break;
			case RST:
				// Assumption RST length == 0
				resetEvent = ml_curEvent;
				if (!resetEvent.hasSessionID()) {
					txEng_ipMetaFifoOut.write(0);
					txEng_tcpMetaFifoOut.write(tx_engine_meta(0, resetEvent.getAckNumb(), 1, 1, 0, 0));
					txEng_isLookUpFifoOut.write(false);
					txEng_tupleShortCutFifoOut.write(ml_curEvent.tuple);
					ml_FsmState = 0;
				}
				else if (!txSar2txEng_upd_rsp.empty()) {
					txSar2txEng_upd_rsp.read(txSar);
					txEng_ipMetaFifoOut.write(0);
					txEng_isLookUpFifoOut.write(true);
					txEng2sLookup_rev_req.write(resetEvent.sessionID); //there is no sessionID??
					//if (resetEvent.getAckNumb() != 0)
					//{
						txEng_tcpMetaFifoOut.write(tx_engine_meta(txSar.not_ackd, resetEvent.getAckNumb(), 1, 1, 0, 0));
					/*}
					/else
					{
						metaDataFifoOut.write(tx_engine_meta(txSar.not_ackd, rxSar.recvd, 1, 1, 0, 0));
					}*/
						ml_FsmState = 0;
				}
				break;
			} //switch
			break;
	} //switch
}

/** @ingroup tx_engine
 *  Forwards the incoming tuple from the SmartCam or RX Engine to the 2 header construction modules
 *  @param[in]	sLookup2txEng_rev_rsp
 *  @param[in]	txEng_tupleShortCutFifoIn
 *  @param[in]	txEng_isLookUpFifoIn
 *  @param[out]	txEng_ipTupleFifoOut
 *  @param[out]	txEng_tcpTupleFifoOut
 */
void tupleSplitter(	stream<fourTuple>&		sLookup2txEng_rev_rsp,
					stream<fourTuple>&		txEng_tupleShortCutFifoIn,
					stream<bool>&			txEng_isLookUpFifoIn,
					stream<twoTuple>&		txEng_ipTupleFifoOut,
					stream<fourTuple>&		txEng_tcpTupleFifoOut)
{
//#pragma HLS INLINE off
#pragma HLS pipeline II=1
	static bool ts_getMeta = true;
	static bool ts_isLookUp;

	fourTuple tuple;

	if (ts_getMeta) {
		if (!txEng_isLookUpFifoIn.empty()) {
			txEng_isLookUpFifoIn.read(ts_isLookUp);
			ts_getMeta = false;
		}
	}
	else {
		if (!sLookup2txEng_rev_rsp.empty() && ts_isLookUp) {
			sLookup2txEng_rev_rsp.read(tuple);
			txEng_ipTupleFifoOut.write(twoTuple(tuple.srcIp, tuple.dstIp));
			txEng_tcpTupleFifoOut.write(tuple);
			ts_getMeta = true;
		}
		else if(!txEng_tupleShortCutFifoIn.empty() && !ts_isLookUp) {
			txEng_tupleShortCutFifoIn.read(tuple);
			txEng_ipTupleFifoOut.write(twoTuple(tuple.srcIp, tuple.dstIp));
			txEng_tcpTupleFifoOut.write(tuple);
			ts_getMeta = true;
		}
	}
}

/** @ingroup tx_engine
 * 	Reads the IP header metadata and the IP addresses. From this data it generates the IP header and streams it out.
 *  @param[in]		txEng_ipMetaDataFifoIn
 *  @param[in]		txEng_ipTupleFifoIn
 *  @param[out]		txEng_ipHeaderBufferOut
 */
void ipHeaderConstruction(
							stream<ap_uint<16> >&			txEng_ipMetaDataFifoIn,
							stream<twoTuple>&				txEng_ipTupleFifoIn,
							stream<axiWord>&				txEng_ipHeaderBufferOut)
{
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	static twoTuple ihc_tuple;

	axiWord sendWord = axiWord(0,0,1);
	ap_uint<16> length = 0;

	if (!txEng_ipMetaDataFifoIn.empty() && !txEng_ipTupleFifoIn.empty()){
		txEng_ipMetaDataFifoIn.read(length);
		txEng_ipTupleFifoIn.read(ihc_tuple);
		length = length + 40;						// TODO: it has to be change if TCP options are implemented

		// Compose the IP header
		sendWord.data(  7,  0) = 0x45;
		sendWord.data( 15,  8) = 0;
		sendWord.data( 23, 16) = length(15, 8); 	//length
		sendWord.data( 31, 24) = length(7, 0);
		sendWord.data( 47, 32) = 0;
		sendWord.data( 50, 48) = 0; 				//Flags
		sendWord.data( 63, 51) = 0x0;				//Fragment Offset
		sendWord.data( 71, 64) = 0x40;
		sendWord.data( 79, 72) = 0x06; 				// TCP
		sendWord.data( 95, 80) = 0; 				// IP header checksum 	
		sendWord.data(127, 96) = ihc_tuple.srcIp; 	// srcIp
		sendWord.data(159,128) = ihc_tuple.dstIp; 	// dstIp
		
		sendWord.last 		   = 1;
		sendWord.keep = 0xFFFFF;

		txEng_ipHeaderBufferOut.write(sendWord);

	}

}

/** @ingroup tx_engine
 * 	Reads the TCP header metadata and the IP tuples. From this data it generates the TCP pseudo header and streams it out.
 *  @param[in]		tcpMetaDataFifoIn
 *  @param[in]		tcpTupleFifoIn
 *  @param[out]		dataOut
 *  @TODO this should be better, cleaner
 */

void pseudoHeaderConstruction(
								stream<tx_engine_meta>&		tcpMetaDataFifoIn,
								stream<fourTuple>&			tcpTupleFifoIn,
								stream<axiWord>&			dataOut)
{
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	static ap_uint<3> phc_currWord = 0;
	axiWord sendWord = axiWord(0,0,0);
	static tx_engine_meta phc_meta;
	static fourTuple phc_tuple;
	//static bool phc_done = true;
	ap_uint<16> length = 0;

	if (!tcpTupleFifoIn.empty() && !tcpMetaDataFifoIn.empty()){
		tcpTupleFifoIn.read(phc_tuple);
		tcpMetaDataFifoIn.read(phc_meta);
		length = phc_meta.length + 0x14;  // 20 bytes for the header
		
		// Generate pseudoheader
		sendWord.data( 31,  0) = phc_tuple.srcIp;
		sendWord.data( 63, 32) = phc_tuple.dstIp;

		sendWord.data( 79, 64) = 0x0600; // TCP

		sendWord.data( 95, 80) = (length(7, 0),length(15, 8));
		sendWord.data(111, 96) = phc_tuple.srcPort; // srcPort
		sendWord.data(127,112) = phc_tuple.dstPort; // dstPort

		// Insert SEQ number
		sendWord.data(135,128) = phc_meta.seqNumb(31, 24);
		sendWord.data(143,136) = phc_meta.seqNumb(23, 16);
		sendWord.data(151,144) = phc_meta.seqNumb(15, 8);
		sendWord.data(159,152) = phc_meta.seqNumb(7, 0);
		// Insert ACK number
		sendWord.data(167,160) = phc_meta.ackNumb(31, 24);
		sendWord.data(175,168) = phc_meta.ackNumb(23, 16);
		sendWord.data(183,176) = phc_meta.ackNumb(15, 8);
		sendWord.data(191,184) = phc_meta.ackNumb(7, 0);


		sendWord.data(195,193) = 0; // reserved
		sendWord.data(199,196) = (0x5 + phc_meta.syn); //data offset
		/* Control bits:
		 * [8] == FIN
		 * [9] == SYN
		 * [10] == RST
		 * [11] == PSH
		 * [12] == ACK
		 * [13] == URG
		 */
		sendWord.data.bit(192) = 0; //NS bit
		sendWord.data.bit(200) = phc_meta.fin; //control bits
		sendWord.data.bit(201) = phc_meta.syn;
		sendWord.data.bit(202) = phc_meta.rst;
		sendWord.data.bit(203) = 0;
		sendWord.data.bit(204) = phc_meta.ack;
		sendWord.data(207, 205) = 0; //some other bits
		sendWord.data.range(223, 208) = (phc_meta.window_size(7, 0) , phc_meta.window_size(15, 8)); // TODO if window size is in option this must be verified
		sendWord.data.range(255, 224) = 0; //urgPointer & checksum

		if (phc_meta.syn) {
			sendWord.data(263, 256) = 0x02; // Option Kind
			sendWord.data(271, 264) = 0x04; // Option length
			sendWord.data(287, 272) = 0xB405; // 0x05B4 = 1460
			sendWord.data(319, 288) = 0;
			sendWord.keep = 0xFFFFFFFFFF;
		}
		else {
			sendWord.keep = 0xFFFFFFFF;
		}

		sendWord.last=1;

		dataOut.write(sendWord);
	}
}

/** @ingroup tx_engine
 *	Reads in the TCP pseudo header stream and appends the corresponding payload stream.
 *	@param[in]		txEng_tcpHeaderBufferIn, incoming TCP pseudo header stream
 *	@param[in]		txBufferReadData, incoming payload stream
 *	@param[out]		dataOut, outgoing data stream
 */
void tx_pseudo_header_pkt_stitcher(
					stream<axiWord>&		txEng_tcpHeaderBufferIn,
					stream<axiWord>&		txBufferReadData,
#if (TCP_NODELAY)
					stream<bool>&			txEng_isDDRbypass,
					stream<axiWord>&		txApp2txEng_data_stream,
#endif
					stream<axiWord>&		txEng_tcpSegOut,
					stream<ap_uint<1> > &memAccessBreakdown2txPkgStitcher)
{
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	static ap_uint<3> 	ps_wordCount = 0;
	static ap_uint<3>	tps_state = 0;
	static axiWord 		currWord = axiWord(0, 0, 0);
	static ap_uint<1> 	txPkgStitcherAccBreakDown = 0;
	static ap_uint<4> 	shiftBuffer = 0;
	static bool 		txEngBrkDownReadIn = false;

	bool isShortCutData = false;

	switch (tps_state)
	{
	case 0: // Read Header
		if (!txEng_tcpHeaderBufferIn.empty())
		{
			txEng_tcpHeaderBufferIn.read(currWord);
			txEng_tcpSegOut.write(currWord);
			txEngBrkDownReadIn = false;
			if (ps_wordCount == 3) {
				if (currWord.data[9] == 1) // is a SYN packet
				{
					tps_state = 1;
				}
				else
				{
#if (TCP_NODELAY)
					tps_state = 7;
#else
					tps_state = 2;
#endif
				}
				ps_wordCount = 0;
			}
			else {
				ps_wordCount++;
			}
			if (currWord.last)
			{
				tps_state = 0;
				ps_wordCount = 0;
			}
		}
		break;
	case 1: // Read one more word for MSS Option
		if (!txEng_tcpHeaderBufferIn.empty())
		{
			txEng_tcpHeaderBufferIn.read(currWord);
			txEng_tcpSegOut.write(currWord);
			tps_state = 7;
			if (currWord.last)
			{
				tps_state = 0;
			}
			ps_wordCount = 0;
		}
		break;
#if (TCP_NODELAY)
	case 7:
		if (!txEng_isDDRbypass.empty())
		{
			txEng_isDDRbypass.read(isShortCutData);
			if (isShortCutData)
			{
				tps_state = 6;
			}
			else
			{
				tps_state = 2;
			}
		}
		break;
#endif
	case 2:
		if (!txBufferReadData.empty() && !txEng_tcpSegOut.full() &&  (txEngBrkDownReadIn == true || (txEngBrkDownReadIn == false && !memAccessBreakdown2txPkgStitcher.empty()))) {    // Read and output the first mem. access for the segment
			if (txEngBrkDownReadIn == false) {
				txEngBrkDownReadIn = true;
				txPkgStitcherAccBreakDown = memAccessBreakdown2txPkgStitcher.read();
			}
			txBufferReadData.read(currWord);
			//txPktCounter++;
			//std::cerr <<  std::dec << cycleCounter << " - " << std::hex << currWord.data << " - " << currWord.keep << " - " << currWord.last << std::endl;
			if (currWord.last) {								// When this mem. access is finished...
				if (txPkgStitcherAccBreakDown == 0)	{			// Check if it was broken down in two. If not...
					tps_state = 0;							// go back to the init state and wait for the next segment.
					txEng_tcpSegOut.write(currWord);
				}
				else if (txPkgStitcherAccBreakDown == 1) {		// If yes, several options present themselves:
					shiftBuffer = keepToLen(currWord.keep);
					txPkgStitcherAccBreakDown = 0;
					currWord.last = 0;
					if (currWord.keep != 0xFF)				// If the last word is complete, this means that the data are aligned correctly & nothing else needs to be done. If not we need to align them.
						tps_state = 3;						// Go to the next state to do just that
					else
						txEng_tcpSegOut.write(currWord);
				}
			}
			else
				txEng_tcpSegOut.write(currWord);
		}
		break;
	case 3: // 0x8F908348249AB4F8
		if (!txBufferReadData.empty() && !txEng_tcpSegOut.full()) {    // Read the first word of a non_aligned second mem. access
			axiWord outputWord = axiWord(currWord.data, 0xFF, 0);
			currWord = txBufferReadData.read();
			outputWord.data(63, (shiftBuffer * 8)) = currWord.data(((8 - shiftBuffer) * 8) - 1, 0);
			ap_uint<4> keepCounter = keepToLen(currWord.keep);
			if (keepCounter < 8 - shiftBuffer) {	// If the entirety of the 2nd mem. access fits in this data word..
				outputWord.keep = lenToKeep(keepCounter + shiftBuffer);
				outputWord.last = 1;
				tps_state = 0;	// then go back to idle
			}
			else if (currWord.last == 1)
				tps_state = 5;
			else
				tps_state = 4;
			txEng_tcpSegOut.write(outputWord);
			//std::cerr <<  std::dec << cycleCounter << " - " << std::hex << outputWord.data << " - " << outputWord.keep << " - " << outputWord.last << std::endl;
		}
		break;
	case 4: //6
		if (!txBufferReadData.empty() && !txEng_tcpSegOut.full()) {    // Read the first word of a non_aligned second mem. access
			axiWord outputWord = axiWord(0, 0xFF, 0);
			outputWord.data((shiftBuffer * 8) - 1, 0) = currWord.data(63, (8 - shiftBuffer) * 8);
			currWord = txBufferReadData.read();
			outputWord.data(63, (8 * shiftBuffer)) = currWord.data(((8 - shiftBuffer) * 8) - 1, 0);
			ap_uint<4> keepCounter = keepToLen(currWord.keep);
			if (keepCounter < 8 - shiftBuffer) {	// If the entirety of the 2nd mem. access fits in this data word..
				outputWord.keep = lenToKeep(keepCounter + shiftBuffer);
				outputWord.last = 1;
				tps_state = 0;	// then go back to idle
			}
			else if (currWord.last == 1)
				tps_state = 5;
			txEng_tcpSegOut.write(outputWord);
			//std::cerr <<  std::dec << cycleCounter << " - " << std::hex << outputWord.data << " - " << outputWord.keep << " - " << outputWord.last << std::endl;
		}
		break;
	case 5:
		if (!txEng_tcpSegOut.full()) {
			ap_uint<4> keepCounter = keepToLen(currWord.keep) - (8 - shiftBuffer);							// This is how many bits are valid in this word
			axiWord outputWord = axiWord(0, lenToKeep(keepCounter), 1);
			outputWord.data((shiftBuffer * 8) - 1, 0) = currWord.data(63, (8 - shiftBuffer) * 8);
			txEng_tcpSegOut.write(outputWord);
			//std::cerr <<  std::dec << cycleCounter << " - " << std::hex << outputWord.data << " - " << outputWord.keep << " - " << outputWord.last << std::endl;
			tps_state = 0;
		}
		break;
#if (TCP_NODELAY)
	case 6:
		if (!txApp2txEng_data_stream.empty() && !txEng_tcpSegOut.full())
		{
			txApp2txEng_data_stream.read(currWord);
			txEng_tcpSegOut.write(currWord);
			if (currWord.last)
			{
				tps_state = 0;
			}
		}
		break;
#endif
	} // switch
}

/** @ingroup tx_engine
 *  Computes the TCP checksum and writes it into @param txEng_pseudo_tcp_checksum
 *	@param[in]		dataIn, incoming data stream
 *	@param[out]		dataOut, outgoing data stream
 *	@param[out]		txEng_pseudo_tcp_checksum, the computed checksum are stored into this FIFO
 */
void tx_compute_pseudo_tcp_checksum(	
									stream<axiWord>&			dataIn,
									stream<ap_uint<16> >&		txEng_pseudo_tcp_checksum)
{
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	axiWord 				currWord;
	static ap_uint<1> 		compute_checksum=0;

	static ap_uint<16> word_sum[32]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

	ap_uint<16> tmp;
	ap_uint<17> tmp1;
	ap_uint<17> tmp2;

	static ap_uint<17> ip_sums_L1[16];
	static ap_uint<18> ip_sums_L2[8];
	static ap_uint<19> ip_sums_L3[4] = {0, 0, 0, 0};
	static ap_uint<20> ip_sums_L4[2];
	static ap_uint<21> ip_sums_L5;
	ap_uint<17> final_sum_r; // real add
	ap_uint<17> final_sum_o; // overflowed add
	ap_uint<16> res_checksum;

	if (!dataIn.empty() && !compute_checksum){
		dataIn.read(currWord);

		first_level_sum : for (int i=0 ; i < 32 ; i++ ){
#pragma HLS UNROLL

			tmp(7,0) 	= currWord.data((((i*2)+1)*8)+7,((i*2)+1)*8);
			tmp(15,8) 	= currWord.data(((i*2)*8)+7,(i*2)*8);

			tmp1 		= word_sum[i] + tmp;
			tmp2 		= word_sum[i] + tmp + 1;

			if (tmp1.bit(16)) 				// one's complement adder
				word_sum[i] = tmp2(15,0);
			else
				word_sum[i] = tmp1(15,0);


		}

		if(currWord.last){
			compute_checksum = 1;
		}

	}
	else if(compute_checksum) {

		//adder tree
		second_level_sum : for (int i = 0; i < 16; i++) {
		#pragma HLS unroll
			ip_sums_L1[i] = word_sum[i*2] + word_sum[i*2+1];
			word_sum[i*2]   = 0; // clear adder variable
			word_sum[i*2+1] = 0;
		}

		//adder tree L2
		third_level_sum : for (int i = 0; i < 8; i++) {
		#pragma HLS unroll
			ip_sums_L2[i] = ip_sums_L1[i*2+1] + ip_sums_L1[i*2];
		}

		//adder tree L3
		fourth_level_sum : for (int i = 0; i < 4; i++) {
		#pragma HLS unroll
			ip_sums_L3[i] = ip_sums_L2[i*2+1] + ip_sums_L2[i*2];
		}

		ip_sums_L4[0] = ip_sums_L3[1] + ip_sums_L3[0];
		ip_sums_L4[1] = ip_sums_L3[3] + ip_sums_L3[2];
		ip_sums_L5 = ip_sums_L4[1] + ip_sums_L4[0];

		final_sum_r = ip_sums_L5.range(15,0) + ip_sums_L5.range(20,16);
		final_sum_o = ip_sums_L5.range(15,0) + ip_sums_L5.range(20,16) + 1;

		if (final_sum_r.bit(16))
			res_checksum = ~(final_sum_o.range(15,0));
		else
			res_checksum = ~(final_sum_r.range(15,0));

		compute_checksum = 0;
		
		txEng_pseudo_tcp_checksum.write(res_checksum);

	}

}



/** @ingroup tx_engine
 *  Reads the IP header stream and the payload stream. It also inserts TCP checksum
 *  The complete packet is then streamed out of the TCP engine. 
 *  The IP checksum must be computed and inserted after
 *  @param[in]		headerIn
 *  @param[in]		payloadIn
 *  @param[in]		ipChecksumFifoIn
 *  @param[in]		tcpChecksumFifoIn
 *  @param[out]		dataOut
 */
void tx_ip_pkt_stitcher(	
					stream<axiWord>& 		txEng_ipHeaderBufferIn,
					stream<axiWord>& 		payloadIn,
					stream<ap_uint<16> >& 	txEng_tcpChecksumFifoIn,
					stream<axiWord>& 		ipTxDataOut)
{
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	axiWord ip_word;
	axiWord payload;
	axiWord sendWord= axiWord(0,0,0);
	static axiWord prevWord;
	
	ap_uint<16> tcp_checksum;

	static bool writing_payload=false;
	static bool writing_extra=false;

	if (writing_extra){
		sendWord.data(159,  0) = prevWord.data(159,  0);
		sendWord.keep( 19,  0) = prevWord.data( 19,  0);
		sendWord.last 	= 1;
		writing_extra   = false;
		ipTxDataOut.write(sendWord);
	}
	else if (writing_payload){
		payloadIn.read(payload);
		sendWord.data(159,  0) = prevWord.data(159,  0);
		sendWord.keep( 19,  0) = prevWord.data( 19,  0);
		sendWord.data(511,160) = payload.data(351,  0);
		sendWord.keep( 63, 20) = payload.keep( 43,  0);
		sendWord.last 	= payload.last;

		if (payload.last){
			if (payload.keep.bit(44)){
				sendWord.last 	= 0;
				writing_extra 	= true;
			}
			writing_payload = false;
		}
		
		prevWord = payload;
		ipTxDataOut.write(sendWord);
	}
	else if (!txEng_ipHeaderBufferIn.empty() && !payloadIn.empty() && !txEng_tcpChecksumFifoIn.empty()) {
		txEng_ipHeaderBufferIn.read(ip_word);
		payloadIn.read(payload);
		txEng_tcpChecksumFifoIn.read(tcp_checksum);

		sendWord.data(159,  0) = ip_word.data(159,  0); 			// TODO: no IP options supported
		sendWord.keep( 19,  0) = 0xFFFFF;
		sendWord.data(511,160) = payload.data(351,  0);
		sendWord.data(304,288) = (tcp_checksum(7,0),tcp_checksum(15,8)); 	// insert checksum
		sendWord.keep( 63, 20) = payload.keep( 43,  0);

		sendWord.last 	= 0;

		if (payload.last){
			if (payload.keep.bit(44))
				writing_extra 	= true;
			else
				sendWord.last 	= 1;
		}
		else
			writing_payload 	= true;

		prevWord = payload;

		ipTxDataOut.write(sendWord);
	}

}

void txEngMemAccessBreakdown(stream<mmCmd> &inputMemAccess, stream<mmCmd> &outputMemAccess, stream<ap_uint<1> > &memAccessBreakdown2txPkgStitcher) {
#pragma HLS pipeline II=1
#pragma HLS INLINE off
	static bool txEngBreakdown = false;
	static mmCmd txEngTempCmd;
	static uint16_t txEngBreakTemp = 0;
	static uint16_t txPktCounter = 0;

	if (txEngBreakdown == false) {
		if (!inputMemAccess.empty() && !outputMemAccess.full()) {
			txEngTempCmd = inputMemAccess.read();
			mmCmd tempCmd = txEngTempCmd;
			if ((txEngTempCmd.saddr.range(15, 0) + txEngTempCmd.bbt) > 65536) {
				txEngBreakTemp = 65536 - txEngTempCmd.saddr;
				tempCmd = mmCmd(txEngTempCmd.saddr, txEngBreakTemp);
				txEngBreakdown = true;
			}
			outputMemAccess.write(tempCmd);
			memAccessBreakdown2txPkgStitcher.write(txEngBreakdown);
			txPktCounter++;
			//std::cerr << std::dec << "MemCmd: " << cycleCounter << " - " << txPktCounter << " - " << std::hex << " - " << tempCmd.saddr << " - " << tempCmd.bbt << std::endl;
		}
	}
	else if (txEngBreakdown == true) {
		if (!outputMemAccess.full()) {
			txEngTempCmd.saddr.range(15, 0) = 0;
			//std::cerr << std::dec << "MemCmd: " << cycleCounter << " - " << std::hex << " - " << txEngTempCmd.saddr << " - " << txEngTempCmd.bbt - txEngBreakTemp << std::endl;
			outputMemAccess.write(mmCmd(txEngTempCmd.saddr, txEngTempCmd.bbt - txEngBreakTemp));
			txEngBreakdown = false;
		}
	}
}

void txDataBroadcast(
					stream<axiWord>& in, 
					stream<axiWord>& out1, 
					stream<axiWord>& out2)
{
#pragma HLS PIPELINE II=1
#pragma HLS INLINE off

	axiWord currWord;

	if (!in.empty()) {
		in.read(currWord);
		out1.write(currWord);
		out2.write(currWord);
	}
}

/** @ingroup tx_engine
 *  @param[in]		eventEng2txEng_event
 *  @param[in]		rxSar2txEng_upd_rsp
 *  @param[in]		txSar2txEng_upd_rsp
 *  @param[in]		txBufferReadData
 *  @param[in]		sLookup2txEng_rev_rsp
 *  @param[out]		txEng2rxSar_upd_req
 *  @param[out]		txEng2txSar_upd_req
 *  @param[out]		txEng2timer_setRetransmitTimer
 *  @param[out]		txEng2timer_setProbeTimer
 *  @param[out]		txBufferReadCmd
 *  @param[out]		txEng2sLookup_rev_req
 *  @param[out]		ipTxData
 */
void tx_engine(	stream<extendedEvent>&			eventEng2txEng_event,
				stream<rxSarEntry_rsp>&			rxSar2txEng_rsp,
				stream<txTxSarReply>&			txSar2txEng_upd_rsp,
				stream<axiWord>&				txBufferReadData,
#if (TCP_NODELAY)
				stream<axiWord>&				txApp2txEng_data_stream,
#endif
				stream<fourTuple>&				sLookup2txEng_rev_rsp,
				stream<ap_uint<16> >&			txEng2rxSar_req,
				stream<txTxSarQuery>&			txEng2txSar_upd_req,
				stream<txRetransmitTimerSet>&	txEng2timer_setRetransmitTimer,
				stream<ap_uint<16> >&			txEng2timer_setProbeTimer,
				stream<mmCmd>&					txBufferReadCmd,
				stream<ap_uint<16> >&			txEng2sLookup_rev_req,
				stream<axiWord>&				ipTxData,
				stream<ap_uint<1> >&			readCountFifo)
{
#pragma HLS DATAFLOW
#pragma HLS INTERFACE ap_ctrl_none port=return
//#pragma HLS PIPELINE II=1
//#pragma HLS INLINE //off

	// Memory Read delay around 76 cycles, 10 cycles/packet, so keep meta of at least 8 packets
	static stream<tx_engine_meta>		txEng_metaDataFifo("txEng_metaDataFifo");
	static stream<ap_uint<16> >			txEng_ipMetaFifo("txEng_ipMetaFifo");
	static stream<tx_engine_meta>		txEng_tcpMetaFifo("txEng_tcpMetaFifo");
	#pragma HLS stream variable=txEng_metaDataFifo depth=16
	#pragma HLS stream variable=txEng_ipMetaFifo depth=16
	#pragma HLS stream variable=txEng_tcpMetaFifo depth=16
	#pragma HLS DATA_PACK variable=txEng_metaDataFifo
	//#pragma HLS DATA_PACK variable=txEng_ipMetaFifo
	#pragma HLS DATA_PACK variable=txEng_tcpMetaFifo

	static stream<axiWord>		txEng_ipHeaderBuffer("txEng_ipHeaderBuffer");
	static stream<axiWord>		txEng_tcpHeaderBuffer("txEng_tcpHeaderBuffer");
	static stream<axiWord>		tx_Eng_pseudo_pkt("tx_Eng_pseudo_pkt");

	static stream<axiWord>		tx_Eng_pseudo_pkt_2_buff("tx_Eng_pseudo_pkt_2_buff");
	static stream<axiWord>		txEng_tcpPkgBuffer2("txEng_tcpPkgBuffer2");
	#pragma HLS stream variable=txEng_ipHeaderBuffer depth=32 // Ip header is 3 words, keep at least 8 headers
	#pragma HLS stream variable=txEng_tcpHeaderBuffer depth=32 // TCP pseudo header is 4 words, keep at least 8 headers
	#pragma HLS stream variable=tx_Eng_pseudo_pkt_2_buff depth=16   // is forwarded immediately, size is not critical
	#pragma HLS stream variable=txEng_tcpPkgBuffer2 depth=256  // critical, has to keep complete packet for checksum computation
	#pragma HLS DATA_PACK variable=txEng_ipHeaderBuffer
	#pragma HLS DATA_PACK variable=txEng_tcpHeaderBuffer
	#pragma HLS DATA_PACK variable=tx_Eng_pseudo_pkt_2_buff
	#pragma HLS DATA_PACK variable=txEng_tcpPkgBuffer2
	
	static stream<axiWord>		tx_Eng_pseudo_pkt_2_checksum("tx_Eng_pseudo_pkt_2_checksum");
	#pragma HLS stream variable=tx_Eng_pseudo_pkt_2_checksum depth=16   
	#pragma HLS DATA_PACK variable=tx_Eng_pseudo_pkt_2_checksum

	static stream<subSums>				txEng_subChecksumsFifo("txEng_subChecksumsFifo");
	static stream<ap_uint<16> >			txEng_tcpChecksumFifo("txEng_tcpChecksumFifo");
	#pragma HLS stream variable=txEng_subChecksumsFifo depth=2
	#pragma HLS stream variable=txEng_tcpChecksumFifo depth=4
	#pragma HLS DATA_PACK variable=txEng_subChecksumsFifo

	static stream<fourTuple> 		txEng_tupleShortCutFifo("txEng_tupleShortCutFifo");
	static stream<bool>				txEng_isLookUpFifo("txEng_isLookUpFifo");
	static stream<twoTuple>			txEng_ipTupleFifo("txEng_ipTupleFifo");
	static stream<fourTuple>		txEng_tcpTupleFifo("txEng_tcpTupleFifo");
	#pragma HLS stream variable=txEng_tupleShortCutFifo depth=2
	#pragma HLS stream variable=txEng_isLookUpFifo depth=4
	#pragma HLS stream variable=txEng_ipTupleFifo depth=4
	#pragma HLS stream variable=txEng_tcpTupleFifo depth=4
	#pragma HLS DATA_PACK variable=txEng_tupleShortCutFifo
	#pragma HLS DATA_PACK variable=txEng_ipTupleFifo
	#pragma HLS DATA_PACK variable=txEng_tcpTupleFifo

	static stream<mmCmd> txMetaloader2memAccessBreakdown("txMetaloader2memAccessBreakdown");
	#pragma HLS stream variable=txMetaloader2memAccessBreakdown depth=32
	#pragma HLS DATA_PACK variable=txMetaloader2memAccessBreakdown
	static stream<ap_uint<1> > memAccessBreakdown2txPkgStitcher("memAccessBreakdown2txPkgStitcher");
	#pragma HLS stream variable=memAccessBreakdown2txPkgStitcher depth=32
	
	static stream<bool> txEng_isDDRbypass("txEng_isDDRbypass");
	#pragma HLS stream variable=txEng_isDDRbypass depth=32


	metaLoader(	eventEng2txEng_event,
				rxSar2txEng_rsp,
				txSar2txEng_upd_rsp,
				txEng2rxSar_req,
				txEng2txSar_upd_req,
				txEng2timer_setRetransmitTimer,
				txEng2timer_setProbeTimer,
				txEng_ipMetaFifo,
				txEng_tcpMetaFifo,
				txMetaloader2memAccessBreakdown,
				txEng2sLookup_rev_req,
				txEng_isLookUpFifo,
#if (TCP_NODELAY)
				txEng_isDDRbypass,
#endif
				txEng_tupleShortCutFifo,
				readCountFifo);
	txEngMemAccessBreakdown(txMetaloader2memAccessBreakdown, txBufferReadCmd, memAccessBreakdown2txPkgStitcher);

	tupleSplitter(	sLookup2txEng_rev_rsp,
					txEng_tupleShortCutFifo,
					txEng_isLookUpFifo,
					txEng_ipTupleFifo,
					txEng_tcpTupleFifo);

	ipHeaderConstruction(txEng_ipMetaFifo, txEng_ipTupleFifo, txEng_ipHeaderBuffer);

	pseudoHeaderConstruction(txEng_tcpMetaFifo, txEng_tcpTupleFifo, txEng_tcpHeaderBuffer);

	tx_pseudo_header_pkt_stitcher(	txEng_tcpHeaderBuffer,
					txBufferReadData,
#if (TCP_NODELAY)
					txEng_isDDRbypass,
					txApp2txEng_data_stream,
#endif
					tx_Eng_pseudo_pkt,
					memAccessBreakdown2txPkgStitcher);


	txDataBroadcast(tx_Eng_pseudo_pkt,tx_Eng_pseudo_pkt_2_buff,tx_Eng_pseudo_pkt_2_checksum);

	tx_compute_pseudo_tcp_checksum(tx_Eng_pseudo_pkt_2_checksum, txEng_tcpChecksumFifo);

	tx_ip_pkt_stitcher(txEng_ipHeaderBuffer, tx_Eng_pseudo_pkt_2_buff, txEng_tcpChecksumFifo, ipTxData);
}
