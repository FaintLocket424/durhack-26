//---------------------------------------------------------------------------------
// handlers.h - the compute side. Knows nothing about sockets or the Wii.
//---------------------------------------------------------------------------------
#ifndef HANDLERS_H
#define HANDLERS_H

#include "protocol.h"

// Looks up the handler for a request type and runs it.
//   in / in_len : request payload, only valid for the duration of the call
//   out         : filled in by the handler (data must be malloc()ed)
// Returns PROTO_OK, or a negative proto_status_t which the caller turns into MSG_ERROR.
int dispatch_request(uint16_t type, const uint8_t *in, uint32_t in_len, response_t *out);

#endif
