// TinyFrame <-> ISerialTransport glue (design.md §9).
//
// TinyFrame is plain C and emits bytes through a single link-time hook,
// TF_WriteImpl(); tf_glue.cpp is the ONE definition of that hook in the whole
// program. It forwards to whatever ISerialTransport is bound to the TinyFrame
// instance, so the same framing code runs over the real UART (firmware) and an
// in-memory pipe (tests). Full framing/dispatch is backlog A1 — this only
// carries bytes out.
#pragma once

#include "ISerialTransport.h"

extern "C" {
#include "TinyFrame/TinyFrame.h" // no C++ guards upstream; C linkage matches TinyFrame.c
}

namespace protocol {

// Point tf's output at transport (stored in tf.userdata). The transport must
// outlive tf. TF_WriteImpl drops bytes while no transport is bound.
void bindTransport(TinyFrame &tf, ISerialTransport &transport);

} // namespace protocol
