// SPDX-License-Identifier: GPL-3.0-or-later
// mbedTLS's error text, short: the error's number instead of its sentence.
//
// mbedtls_strerror() pulls in a table of every error message mbedTLS has (its
// error.c, about 15 KB of flash), for the one place a standalone shows one: a
// failed update or download's "TLS: ..." line. Boards whose app slot is full link
// with -Wl,--wrap=mbedtls_strerror and -DWADA_SHORT_TLS_ERRORS, which sends every
// call here: "TLS: mbedTLS -0x7880" (look the number up in mbedTLS's error list)
// instead of "TLS: SSL - The peer notified us that the connection is going to be
// closed". The table is then never linked.

#if defined(WADA_SHORT_TLS_ERRORS)
#include <stddef.h>
#include <stdio.h>

extern "C" void __wrap_mbedtls_strerror(int ret, char* buf, size_t buflen) {
  if (!buf || !buflen) return;
  const unsigned code = (unsigned)(ret < 0 ? -ret : ret);
  snprintf(buf, buflen, "mbedTLS -0x%04X", code);
}
#endif
