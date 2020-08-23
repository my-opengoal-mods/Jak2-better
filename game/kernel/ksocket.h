/*!
 * @file ksocket.h
 * GOAL Socket connection to listener using DECI2/DSNET
 */

#ifndef JAK_KSOCKET_H
#define JAK_KSOCKET_H

#include "common/common_types.h"
#include "kmachine.h"
#include "Ptr.h"

/*!
 * Update GOAL message header after receiving and verify message is ok.
 * Return the size of the message in bytes (not including DECI or GOAL headers)
 * Return -1 on error.
 * The buffer parameter is unused.
 */
u32 ReceiveToBuffer(char* buff);

/*!
 * Do a DECI2 send and block until it is complete.
 * The message type is OUTPUT
 */
s32 SendFromBuffer(char* buff, s32 size);

/*!
 * Just prepare the Ack buffer, doesn't actually connect.
 * Must be called before attempting to use the socket connection.
 */
void InitListenerConnect();

/*!
 * Does nothing.
 */
void InitCheckListener();

/*!
 * Doesn't actually wait for a message, just checks if there's currently a message.
 * Doesn't actually send an ack either.
 * More accurate name would be "CheckForMessage"
 * Returns pointer to the message.
 */
Ptr<char> WaitForMessageAndAck();

/*!
 * Doesn't close anything, just print a closed message.
 */
void CloseListener();

#endif  // JAK_KSOCKET_H
