/* vendor/testcerts/test_chain.c -- chapter 112b.
 *
 * Re-exports BearSSL 0.6's sample localhost RSA-2048 cert chain
 * and private key (vendor/bearssl/samples/chain-rsa.h, key-rsa.h,
 * MIT-licensed by Thomas Pornin) as extern symbols other TUs can
 * link against.
 *
 * The sample headers define everything as `static const` so they
 * can be #included into many sample programs without colliding;
 * we lift that restriction by importing both and exporting
 * pointers to the static data through this single translation
 * unit.  The cert bytes themselves stay in .rodata of THIS object
 * file -- no duplication when both httpsd.o and tlstest.o link
 * against it.
 *
 * The chain is (end-entity for CN=localhost, intermediate CA).
 * Validity window 2010-01-01..2037-12-31 (signed under SHA-256
 * with PKCS#1 v1.5 over a 2048-bit modulus); fine for in-guest
 * regression use where wall clock = 0 anyway (see cstring.c's
 * time() stub).
 *
 * For the client side of the 112b handshake we don't validate
 * the chain at all -- the client uses a br_x509_knownkey context
 * pinned to CERT0's public key, which it extracts at runtime from
 * the first chain entry via br_x509_decoder_*.  See
 * userspace/tlstest/tlstest.c for the wiring.
 */

#include "bearssl.h"

/* BearSSL samples ship as headers full of `static const` arrays.
 * Including them HERE (and nowhere else) means the bytes land in
 * test_chain.o's .rodata exactly once. */
#include "chain-rsa.h"
#include "key-rsa.h"

const br_x509_certificate *const test_server_chain     = CHAIN;
const size_t                     test_server_chain_len = CHAIN_LEN;
const br_rsa_private_key  *const test_server_key       = &RSA;
